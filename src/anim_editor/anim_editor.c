// ============================================================================
//  anim_editor.c  -  visual authoring for the anim.* animation model
//
//  Draw()  (game space) : live preview of the working AnimDoc at the scrubber
//                         playhead - authored fractions land exactly where they
//                         will in game.
//  Gui()   (screen space): raygui tool UI over the preview:
//    - LEFT   element list + add Text/Shape/Global + delete (multi-select)
//    - RIGHT  inspector: base fields of the selection + its tracks/keyframes
//    - BOTTOM timeline scrubber (draggable playhead + keyframe diamonds) and
//             the signals row; a top toolbar has New/Load/Save/Undo/Redo/Back.
//
//  Undo is a fixed ring of whole-document snapshots (AnimDoc is a plain value).
//  raygui's implementation is compiled once in main_menu.c; we only include it.
// ============================================================================

#include "raylib.h"
#include "raygui.h"
#include "anim_editor.h"
#include "../app_state/app_state.h"
#include "../screen_state/screen_state.h"
#include "../audio_state/audio_state.h"
#include "../anim/anim.h"
#include "../anim/anim_io.h"
#include "../anim/anim_library.h"
#include "../signal/signal.h"
#include "../signal/anim_signal.h"
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>          // remove() for deleting anim files off disk
#include <math.h>

// Forward declares (house pattern).
static void Enter();
static void Exit();
static void Update();
static void Draw();
static void Gui();

                            /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_state_anim_editor = {Enter, Exit, Update, Draw, Gui, "AnimEditor"};

// ---------------------------------------------------------------------------
//  Editor state (file-scope singleton - persists across frames like the menu).
// ---------------------------------------------------------------------------
// Animations live as one .cfg each under ANIM_DIR (CWD-relative, writable -
// NOT RESOURCES_PATH, which is read-only/preloaded on release+web). The dropdown
// enumerates this dir; each file's basename (no extension) is one animation.
#define ANIM_DIR      "anims"
#define ANIM_EXT      ".cfg"
#define ANIM_LIST_MAX 64
#define LEGACY_PATH   "anim_doc.cfg"   // pre-dir single file; migrated in on first run
// element library: '_'-prefixed so RescanAnims() skips it (it is not an anim).
#define LIB_PATH      ANIM_DIR "/_library" ANIM_EXT
#define UNDO_MAX  16
#define AUTOKEY_EPS 0.02f          // playhead-to-key snap when auto-keying (s)

static AnimDoc doc;                 // the working document
// A fired signal runs as an OVERRIDE on top of the timeline: the playhead keeps
// meaning what it always did, and the signal transiently drives its targets.
static AnimSignalPlayer preview;

// animation library (files in ANIM_DIR) + which one `doc` came from.
static char animList[ANIM_LIST_MAX][ANIM_NAME_MAX];  // basenames, no extension
static int  animCount = 0;
static int  animCurrent = -1;       // index into animList of the loaded anim (-1 unsaved)
static bool docDirty = false;       // `doc` has edits not written to its file

// Animation open when the editor was last left, so re-entering resumes there.
// Session-scoped by design: a static, not a file, so a fresh launch starts on
// the default animation again. Kept by NAME - RescanAnims() shifts indices.
static char lastOpened[ANIM_NAME_MAX] = {0};

// Per-animation element selection, remembered for this run only (same
// session-scoped, no-disk rationale as lastOpened). Keyed by anim NAME
// (rescan shifts indices) -> element INDEX: element names are NOT unique
// (every new shape is "shape"), so a name key would be ambiguous.
static char lastSelAnim[ANIM_LIST_MAX][ANIM_NAME_MAX];
static int  lastSelElem[ANIM_LIST_MAX];
static int  lastSelCount = 0;

// animation-switch dropdown (header in toolbar, list drawn as overlay like ease)
static bool animSwitchOpen = false;
static Rectangle animSwitchRect;
static bool animSwitchVisible = false;

// modal prompts driven by the switch dropdown (drawn topmost, block other input).
typedef enum { PROMPT_NONE, PROMPT_SAVE_THEN_SWITCH, PROMPT_CONFIRM_DELETE,
               PROMPT_NEW_NAME, PROMPT_LIBRARY, PROMPT_LIB_SAVE_NAME,
               PROMPT_LIB_RENAME } PromptKind;
static PromptKind prompt = PROMPT_NONE;
static int  promptTargetIdx = -1;   // anim to switch-to / delete
static char nameBuf[ANIM_NAME_MAX]; // New... name input
static bool edNameBuf = false;      // name textbox edit flag

// element library: one shelf shared by every animation, loaded once in Enter()
// and written back on every mutation (small file, same eager style as Save).
static AnimLibrary library;
static float libScroll = 0.0f;      // library modal list scroll
static int   libTargetIdx = -1;     // entry being renamed / deleted

// signal modal: which signal is open (-1 = none). Unlike the PROMPT_* modals
// this one SURVIVES playback (shrunk to Fire/Close) so a signal can be tested
// mid-animation, which is the whole point of a signal.
static int   sigModalIdx = -1;
static float sigScroll = 0.0f;      // modal's target/key list scroll
static int   sigPropDrop = -1;      // target row whose property dropdown is open
static int   sigKeyEaseDrop = -1;   // packed target*256+key whose ease list is open
static float sigLastU = 1.0f;       // last u the user set on ANY signal key; new
                                    // keys seed from it so placing a beat across
                                    // several tracks doesn't mean retyping it

static bool guiLocked = false;      // mirrors GuiLock() for this frame: raygui
                                    // exposes no query, and PanelScroll must
                                    // not GuiUnlock() an outer modal's lock
static int  selElem = -1;           // primary selected element (-1 = none)
static bool multiSel[ANIM_ELEMS_MAX];   // additional selected elements

// timeline / playback
static float playhead = 0.0f;       // scrubber time, 0..doc.duration
static bool  playing  = false;      // timeline play/pause
static bool  loopPlay = true;

// playback UI slide: 0 = editing (panels shown) .. 1 = playing (panels hidden,
// timeline collapsed to a thin clickable strip). Eased in Gui().
static float panelAnim = 0.0f;
static bool  prevPlaybackUi = false;

// A scrolling panel view: content taller than the panel scrolls under the
// wheel, clipped by a scissor. Three panels use this (elements, signals,
// inspector) so the bookkeeping lives in one place - see PanelScroll().
typedef struct {
    float scroll;       // <= 0; how far the content is pulled up
    float contentH;     // measured by the draw callback last frame
} PanelView;

static PanelView elemView, sigView, inspView;

// inspector wheel-scroll (content can overflow the panel; no visible bar)
static Rectangle inspPanelRect;     // this frame's inspector panel (scissor)
static bool scrollToSelKey = false; // scroll the selected key row into view

// add-track dropdown header rect (list drawn last as an overlay, like ease)
static Rectangle addTrackRect;
static bool addTrackVisible = false;

// undo ring (snapshots of the whole doc)
static AnimDoc undoBuf[UNDO_MAX];
static int     undoHead = 0;        // next write slot
static int     undoCount = 0;       // valid snapshots behind head
static int     redoCount = 0;       // snapshots ahead (after undo)

// edit-mode flags for raygui controls that need one (only one active at a time)
static bool edName = false, edText = false;
static int  edSigIdx = -1;          // signal whose name textbox is in edit mode
static int  addTrackDrop = -1;      // element whose "add track" dropdown is open
static int  addTrackSel = 0;

// key selection: the timeline diamond whose t/value/ease the inspector edits
static int  selKeyElem = -1, selKeyTrack = -1, selKeyIdx = -1;
static bool edKeyTime = false;      // key time textbox edit flag
static char keyTimeBuf[16];         // key time textbox buffer
static int  keyEaseSel = 0;         // key ease dropdown selection
static bool keyEaseDropOpen = false;
static Rectangle keyEaseRect;       // where the dropdown goes (drawn LAST in Gui)
static bool keyEaseVisible = false; // key inspector on screen this frame

// auto-key: inspector sliders on TRACKED props write a key at the playhead
static bool autoKey = false;

// one UndoPush per slider drag gesture (cleared on mouse release in Gui())
static bool sliderGestureOpen = false;

// precise slider input: double-click the value label to type an exact value,
// or Ctrl+wheel over a slider to step it in fixed increments.
static bool      edSliderActive = false;    // a slider textbox is open
static Rectangle edSliderRect = {0};        // rect of the slider that owns it
static char      edSliderBuf[16];
static double    lastSliderClick = 0.0;     // double-click timing
static Rectangle lastSliderClickRect = {0};

// dragging on the timeline
static bool dragPlayhead = false;
static int  dragKeyElem = -1, dragKeyTrack = -1, dragKeyIdx = -1;
static bool dragIntro = false, dragOutro = false;   // the trim triangles

// ---------------------------------------------------------------------------
//  Undo helpers: snapshot the doc before a mutating edit.
// ---------------------------------------------------------------------------
static void UndoPush()
{
    undoBuf[undoHead] = doc;                    // value copy of the whole doc
    undoHead = (undoHead + 1) % UNDO_MAX;
    if (undoCount < UNDO_MAX - 1) undoCount++;
    redoCount = 0;                              // a new edit invalidates redo
    docDirty = true;                            // every mutating edit routes here
}

static void ClearKeySelection()
{
    selKeyElem = selKeyTrack = selKeyIdx = -1;
    edKeyTime = false; keyEaseDropOpen = false;
}

// Select a timeline key and sync its inspector widgets (time buffer, ease).
static void SelectKey(int elem, int track, int idx)
{
    selKeyElem = elem; selKeyTrack = track; selKeyIdx = idx;
    const AnimKey *k = &doc.elems[elem].tracks[track].keys[idx];
    TextCopy(keyTimeBuf, TextFormat("%.2f", k->t));
    keyEaseSel = k->ease;
    edKeyTime = false;
}

static void ClampSelection()
{
    if (selElem >= doc.elemCount) selElem = doc.elemCount - 1;
    for (int i = 0; i < ANIM_ELEMS_MAX; i++)
        if (i >= doc.elemCount) multiSel[i] = false;

    // the key selection must survive undo/redo and deletes: revalidate it.
    if (selKeyElem >= 0)
    {
        if (selKeyElem >= doc.elemCount ||
            selKeyTrack >= doc.elems[selKeyElem].trackCount ||
            selKeyIdx   >= doc.elems[selKeyElem].tracks[selKeyTrack].keyCount)
            ClearKeySelection();
    }

    // an undo/redo or doc switch can shrink signalCount under the open modal
    if (sigModalIdx >= doc.signalCount)
    {
        sigModalIdx = -1; edSigIdx = -1;
        sigPropDrop = -1; sigKeyEaseDrop = -1;
    }
}

// Bindings capture a signal's name/dir/section by value at register time, so
// any change to doc.signals (or an undo swapping the doc) needs a re-register.
static void ReRegisterSignals()
{
    AnimSignalUnregister(&doc, &preview);
    AnimSignalRegister(&doc, &preview, &playhead);
}

static void UndoApply(int delta)   // delta -1 = undo, +1 = redo
{
    if (delta < 0 ? undoCount == 0 : redoCount == 0) return;
    AnimSignalUnregister(&doc, &preview);   // bindings match the OLD doc
    if (delta < 0)
    {
        // push current onto redo side, step head back.
        undoHead = (undoHead - 1 + UNDO_MAX) % UNDO_MAX;
        AnimDoc prev = undoBuf[undoHead];
        undoBuf[undoHead] = doc;                // store current for redo
        doc = prev;
        undoCount--; redoCount++;
    }
    else
    {
        AnimDoc next = undoBuf[undoHead];
        undoBuf[undoHead] = doc;
        doc = next;
        undoHead = (undoHead + 1) % UNDO_MAX;
        undoCount++; redoCount--;
    }
    AnimSignalRegister(&doc, &preview, &playhead);
    ClampSelection();
}

// ---------------------------------------------------------------------------
//  A tiny starter document so the editor opens on something visible.
// ---------------------------------------------------------------------------
static void MakeStarterDoc()
{
    AnimDocInit(&doc);
    TextCopy(doc.name, "new_anim");
    doc.duration = 2.0f;

    AnimElem *t = AnimDocAddElem(&doc, AE_TEXT);
    TextCopy(t->name, "title");
    TextCopy(t->text, "HELLO");
    t->posFrac  = (Vector2){ 0.5f, 0.10f };
    t->sizeFrac = (Vector2){ 0.12f, 0.12f };
    AnimTrack *a = AnimElemAddTrack(t, AP_T_ALPHA);
    AnimTrackAddKey(a, 0.0f, 0.0f, ANIM_EASE_LINEAR);
    AnimTrackAddKey(a, 0.6f, 1.0f, ANIM_EASE_SINE_OUT);

    // one signal so there is something to open and fill in straight away.
    TextCopy(doc.signals[0].name, "enter");
    doc.signals[0].length      = 1.0f;
    doc.signals[0].targetCount = 0;
    doc.signalCount = 1;

    selElem = 0;
}

// ---------------------------------------------------------------------------
//  Animation library: files under ANIM_DIR, one .cfg per animation.
// ---------------------------------------------------------------------------

// Path to an animation file by basename (rotating TextFormat buffer - use now).
static const char *AnimPath(const char *name)
{
    return TextFormat("%s/%s%s", ANIM_DIR, name, ANIM_EXT);
}

// Remember `elem` as the selected element of animation `anim` (upsert). Full
// table (>ANIM_LIST_MAX anims touched) just drops the write - the selection
// restore is a convenience, not state anything depends on.
static void RememberSelElem(const char *anim, int elem)
{
    if (!anim || !anim[0]) return;
    for (int i = 0; i < lastSelCount; i++)
        if (TextIsEqual(lastSelAnim[i], anim)) { lastSelElem[i] = elem; return; }
    if (lastSelCount >= ANIM_LIST_MAX) return;
    TextCopy(lastSelAnim[lastSelCount], anim);
    lastSelElem[lastSelCount++] = elem;
}

// Element index remembered for `anim`, or -1 if none. Caller bounds-checks it
// against the loaded doc: elements may have been deleted since.
static int RecallSelElem(const char *anim)
{
    if (!anim || !anim[0]) return -1;
    for (int i = 0; i < lastSelCount; i++)
        if (TextIsEqual(lastSelAnim[i], anim)) return lastSelElem[i];
    return -1;
}

// Drop the remembered selection for `anim` (its file is gone), so a later
// animation reusing the name doesn't inherit a stale index.
static void ForgetSelElem(const char *anim)
{
    for (int i = 0; i < lastSelCount; i++)
        if (TextIsEqual(lastSelAnim[i], anim))
        {
            // order doesn't matter here: fill the hole with the last entry.
            lastSelCount--;
            TextCopy(lastSelAnim[i], lastSelAnim[lastSelCount]);
            lastSelElem[i] = lastSelElem[lastSelCount];
            return;
        }
}

// Index of an animation in animList by name, or -1.
static int AnimFind(const char *name)
{
    for (int i = 0; i < animCount; i++)
        if (TextIsEqual(animList[i], name)) return i;
    return -1;
}

// (Re)build animList from disk. Creates ANIM_DIR if missing and, on first run,
// migrates the legacy single anim_doc.cfg into it so old work isn't lost.
static void RescanAnims()
{
    if (!DirectoryExists(ANIM_DIR)) MakeDirectory(ANIM_DIR);

    // one-time migration: legacy file present, new dir still empty.
    FilePathList probe = LoadDirectoryFilesEx(ANIM_DIR, ANIM_EXT, false);
    bool empty = probe.count == 0;
    UnloadDirectoryFiles(probe);
    if (empty && FileExists(LEGACY_PATH))
    {
        AnimDoc tmp;
        if (AnimDocLoad(&tmp, LEGACY_PATH))
        {
            const char *nm = tmp.name[0] ? tmp.name : "main";
            AnimDocSave(&tmp, AnimPath(nm));
        }
    }

    animCount = 0;
    FilePathList fl = LoadDirectoryFilesEx(ANIM_DIR, ANIM_EXT, false);
    for (unsigned int i = 0; i < fl.count && animCount < ANIM_LIST_MAX; i++)
    {
        // '_'-prefixed files are editor data, not animations (the element
        // library lives here too) - keep them out of the switcher.
        const char *base = GetFileNameWithoutExt(fl.paths[i]);
        if (base[0] == '_') continue;
        TextCopy(animList[animCount++], base);
    }
    UnloadDirectoryFiles(fl);
}

// Load animList[idx] into `doc`, making it the current (clean) animation.
static void LoadAnimByIndex(int idx)
{
    if (idx < 0 || idx >= animCount) return;
    UndoPush();                                 // switching is undoable
    // stash the OUTGOING anim's selection before `doc` is overwritten.
    if (animCurrent >= 0) RememberSelElem(animList[animCurrent], selElem);
    AnimSignalUnregister(&doc, &preview);       // bindings match the OLD doc
    if (!AnimDocLoad(&doc, AnimPath(animList[idx]))) MakeStarterDoc();
    AnimSignalRegister(&doc, &preview, &playhead);
    animCurrent = idx;
    docDirty = false;                           // freshly loaded == clean
    // restore this anim's own selection; stale/absent falls back to the first.
    int wantSel = RecallSelElem(animList[idx]);
    selElem = (wantSel >= 0 && wantSel < doc.elemCount) ? wantSel
            : (doc.elemCount > 0 ? 0 : -1);
    ClearKeySelection();
    // the new doc's signals are unrelated to the old one's: close the modal and
    // reset the panel scrolls rather than pointing them at a different signal.
    sigModalIdx = -1; edSigIdx = -1;
    sigPropDrop = -1; sigKeyEaseDrop = -1;
    elemView.scroll = sigView.scroll = inspView.scroll = 0.0f;

    preview = (AnimSignalPlayer){0};            // bindings pointed at the OLD doc
    ClampSelection();
    playhead = 0.0f; playing = false;
}

// Save `doc` to its current file (or under doc.name if never saved), refresh list.
static void SaveCurrent()
{
    const char *nm = (animCurrent >= 0) ? animList[animCurrent]
                   : (doc.name[0] ? doc.name : "untitled");
    AnimDocSave(&doc, AnimPath(nm));
    RescanAnims();
    animCurrent = AnimFind(nm);
    docDirty = false;
}

// ===========================================================================
//  State lifecycle
// ===========================================================================
static void Enter()
{
    // Enumerate the animation library (creates the dir + migrates legacy file).
    RescanAnims();

    // the element shelf (absent file -> empty library, which is fine).
    AnimLibraryLoad(&library, LIB_PATH);
    libScroll = 0.0f; libTargetIdx = -1;

    // Reopen the animation last left (this run only), else the first saved one,
    // else start on a demo. A remembered anim deleted since simply misses.
    animCurrent = -1;
    int openIdx = lastOpened[0] ? AnimFind(lastOpened) : -1;
    if (openIdx < 0) openIdx = 0;
    if (animCount > 0 && AnimDocLoad(&doc, AnimPath(animList[openIdx])) && doc.elemCount > 0)
        animCurrent = openIdx;
    else
        MakeStarterDoc();
    docDirty = false;

    // restore the element last selected in THIS anim (bounds-checked: it may
    // have been deleted, or the file edited on disk, since we saw it).
    int wantSel = (animCurrent >= 0) ? RecallSelElem(animList[animCurrent]) : -1;
    selElem  = (wantSel >= 0 && wantSel < doc.elemCount) ? wantSel
             : (doc.elemCount > 0 ? 0 : -1);
    for (int i = 0; i < ANIM_ELEMS_MAX; i++) multiSel[i] = false;
    ClearKeySelection();
    edSigIdx = -1; sliderGestureOpen = false; edSliderActive = false;
    animSwitchOpen = false; prompt = PROMPT_NONE; edNameBuf = false;
    sigModalIdx = -1; sigScroll = 0.0f;
    sigPropDrop = -1; sigKeyEaseDrop = -1;
    elemView = sigView = inspView = (PanelView){0};
    playhead = 0.0f; playing = false;
    panelAnim = 0.0f; prevPlaybackUi = false;
    undoCount = redoCount = 0; undoHead = 0;

    // register the doc's signals so the "Fire" buttons drive the preview player.
    preview = (AnimSignalPlayer){0};
    AnimSignalRegister(&doc, &preview, &playhead);
}

static void Exit()
{
    // remember where the user was; unsaved scratch doc has nothing to reopen.
    if (animCurrent >= 0)
    {
        TextCopy(lastOpened, animList[animCurrent]);
        RememberSelElem(animList[animCurrent], selElem);
    }
    else lastOpened[0] = '\0';

    AnimSignalUnregister(&doc, &preview);
}

static void Update()
{
    float dt = GetFrameTime();

    // Timeline playback (independent of signal-fired preview).
    // Playback runs over the TRIMMED section [0..outroStart]; the intro is a
    // one-shot lead-in, so looping restarts at introEnd, not at 0.
    if (playing)
    {
        float inEnd = AnimDocIntroEnd(&doc), outStart = AnimDocOutroStart(&doc);
        playhead += dt;
        if (playhead >= outStart)
        {
            if (loopPlay)
            {
                float cycle = outStart - inEnd;
                playhead = (cycle > 0.0f) ? inEnd + fmodf(playhead - outStart, cycle)
                                          : inEnd;
            }
            else playhead = outStart;
        }
    }

    // A fired signal runs on its OWN clock as an override on top of the
    // timeline - it does not move the playhead (the whole point is that it
    // transitions away from wherever the playhead has the scene posed).
    if (!AnimSignalPlayerDone(&preview)) AnimSignalPlayerUpdate(&preview, dt);

    // ESC closes whatever is open, innermost first, and only leaves the editor
    // once nothing is (so it can't discard work behind a modal by accident).
    if (IsKeyPressed(KEY_ESCAPE))
    {
        if (sigPropDrop != -1 || sigKeyEaseDrop >= 0)
        { sigPropDrop = -1; sigKeyEaseDrop = -1; }
        else if (prompt != PROMPT_NONE)   { prompt = PROMPT_NONE; edNameBuf = false; }
        else if (sigModalIdx >= 0)        { sigModalIdx = -1; edSigIdx = -1; }
        else if (animSwitchOpen)          animSwitchOpen = false;
        else if (keyEaseDropOpen || addTrackDrop >= 0)
        { keyEaseDropOpen = false; addTrackDrop = -1; }
        else AppStateTransition(&app_state_main_menu);
    }

    // Ctrl+Z / Ctrl+Y undo-redo.
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl && IsKeyPressed(KEY_Z)) UndoApply(-1);
    if (ctrl && IsKeyPressed(KEY_Y)) UndoApply(+1);

    // Space = play/pause, Ctrl+Space = play-from-start/stop (playhead to 0).
    // Ignored while any textbox is capturing typing.
    bool typing = edName || edText || edKeyTime || edSigIdx >= 0 || edSliderActive;
    if (!typing && IsKeyPressed(KEY_SPACE))
    {
        if (ctrl)
        {
            if (playing) { playing = false; playhead = 0.0f; }
            else         { playhead = 0.0f; playing = true; }
        }
        else playing = !playing;
        preview.playing = false;
    }

    // Slide the tool panels away while anything is playing, back when paused.
    bool playbackUi = playing || !AnimSignalPlayerDone(&preview);
    if (playbackUi && !prevPlaybackUi)
    {
        // hidden widgets must not keep capturing input. The signal modal is
        // deliberately NOT closed (it shrinks to Fire/Close so a signal can be
        // triggered mid-playback), but its dropdowns do go away with it.
        edName = edText = edKeyTime = false; edSigIdx = -1; edSliderActive = false;
        addTrackDrop = -1; keyEaseDropOpen = false;
        sigPropDrop = -1; sigKeyEaseDrop = -1;
    }
    prevPlaybackUi = playbackUi;
    float step = dt / 0.25f;
    if (playbackUi) { panelAnim += step; if (panelAnim > 1.0f) panelAnim = 1.0f; }
    else            { panelAnim -= step; if (panelAnim < 0.0f) panelAnim = 0.0f; }
}

// ===========================================================================
//  Draw: live preview in GAME space, at the current playhead.
// ===========================================================================
static void Draw()
{
    Vector2 game = ScreenStateTargetSize();
    // subtle checker/backdrop so text/alpha is legible against the preview.
    ClearBackground((Color){ 30, 32, 38, 255 });
    DrawRectangleLines(0, 0, (int)game.x, (int)game.y, (Color){ 70, 74, 84, 255 });

    // a fired signal layers over the timeline pose (NULL override when idle)
    AnimDocDrawEx(&doc, playhead, &preview);

    // Highlight the selected element's anchor so it's easy to see what's picked.
    if (selElem >= 0 && selElem < doc.elemCount)
    {
        const AnimElem *e = &doc.elems[selElem];
        float cx = game.x * AnimElemProp(e, e->kind == AE_TEXT ? AP_T_POS_X : AP_S_POS_X, playhead);
        float cy = game.y * AnimElemProp(e, e->kind == AE_TEXT ? AP_T_POS_Y : AP_S_POS_Y, playhead);
        if (e->kind != AE_GLOBAL)
            DrawCircleLines((int)cx, (int)cy, 8.0f, (Color){ 255, 210, 90, 255 });
    }
}

// ===========================================================================
//  Gui helpers
// ===========================================================================
// A labelled float slider that snapshots the doc ONCE per drag gesture: the
// UndoPush lands before the first change is written, and the gesture stays
// open until the mouse is released (cleared at the top of Gui()).
// same slider rect two frames running -> same widget (positions are stable)
static bool SameRect(Rectangle a, Rectangle b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

// step size for Ctrl+wheel / Ctrl-drag snapping, keyed on the value range:
// fine for fractional 0..1/0..2 sliders, coarser for duration/time.
static float SliderStep(float lo, float hi) { return (hi - lo <= 2.0f) ? 0.01f : 0.1f; }

static float ClampF(float v, float lo, float hi)
{
    if (v < lo) return lo; if (v > hi) return hi; return v;
}

static bool EditSlider(Rectangle r, const char *label, float *v, float lo, float hi)
{
    // a disabled slider (auto-key off) gets no precise input either: fall through
    // to the plain draw so it can't sneak a value/keyframe write.
    if (GuiGetState() == STATE_DISABLED)
    {
        float tmp = *v;
        GuiSlider(r, label, TextFormat("%.2f", tmp), &tmp, lo, hi);
        return false;
    }

    // value label sits just right of the bar (raygui draws it there too).
    float pad = (float)GuiGetStyle(SLIDER, TEXT_PADDING);
    Rectangle lbl = { r.x + r.width + pad, r.y, 44, r.height };
    Vector2 mouse = GetMousePosition();

    // --- textbox mode: this slider owns the open value editor ------------------
    if (edSliderActive && SameRect(edSliderRect, r))
    {
        GuiSlider(r, label, "", &(float){ *v }, lo, hi);   // draw bar, ignore drag
        bool commit = false, cancel = false;
        // Esc cancels; Enter or a click outside the box commits.
        if (IsKeyPressed(KEY_ESCAPE)) cancel = true;
        else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) commit = true;
        else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                 !CheckCollisionPointRec(mouse, lbl)) commit = true;

        GuiTextBox(lbl, edSliderBuf, sizeof(edSliderBuf), true);

        if (commit || cancel)
        {
            edSliderActive = false;
            if (commit)
            {
                float nv = ClampF((float)atof(edSliderBuf), lo, hi);
                if (nv != *v) { UndoPush(); *v = nv; return true; }
            }
        }
        return false;
    }

    // --- open the textbox on a double-click of the value label -----------------
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, lbl))
    {
        if (GetTime() - lastSliderClick < 0.3 && SameRect(lastSliderClickRect, r))
        {
            edSliderActive = true; edSliderRect = r;
            TextCopy(edSliderBuf, TextFormat("%.2f", *v));
            lastSliderClick = 0.0;
            return false;
        }
        lastSliderClick = GetTime(); lastSliderClickRect = r;
    }

    // --- Ctrl+wheel steps the value in fixed increments ------------------------
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    float wheel = GetMouseWheelMove();
    if (ctrl && wheel != 0.0f && CheckCollisionPointRec(mouse, r))
    {
        float step = SliderStep(lo, hi);
        float nv = ClampF(*v + wheel * step, lo, hi);
        if (nv != *v) { UndoPush(); *v = nv; return true; }
        return false;
    }

    // --- normal drag -----------------------------------------------------------
    float tmp = *v;
    GuiSlider(r, label, TextFormat("%.2f", tmp), &tmp, lo, hi);
    if (ctrl && tmp != *v)                       // Ctrl-drag snaps to increments
    {
        float step = SliderStep(lo, hi);
        tmp = ClampF(roundf(tmp / step) * step, lo, hi);
    }
    if (tmp == *v) return false;
    if (!sliderGestureOpen) { UndoPush(); sliderGestureOpen = true; }
    *v = tmp;
    return true;
}

// Every track keeps a key at t=0: the element's START state. Called before
// writing a key later on the clock so the animation has a defined beginning.
static void EnsureZeroKey(AnimElem *e, AnimTrack *tr)
{
    if (tr->keyCount == 0)
        AnimTrackAddKey(tr, 0.0f, AnimElemProp(e, tr->prop, 0.0f), ANIM_EASE_LINEAR);
}

static void EnsureZeroColorKey(AnimElem *e, AnimTrack *tr)
{
    // seed from the track's OWN property (a shape has fill AND outline colour)
    if (tr->keyCount == 0)
        AnimTrackAddColorKey(tr, 0.0f, AnimElemColorProp(e, tr->prop, 0.0f),
                             ANIM_EASE_LINEAR);
}

// The colour property for an element kind (each kind has exactly one).
static int ColorPropFor(int kind)
{
    switch (kind)
    {
        case AE_TEXT:  return AP_T_COLOR;
        case AE_SHAPE: return AP_S_COLOR;
        default:       return AP_G_COLOR;
    }
}

// Small colour preview square with an outline (used by the colour editors).
static void DrawSwatch(Rectangle r, Color c)
{
    DrawRectangleRec(r, c);
    DrawRectangleLinesEx(r, 1.0f, (Color){ 70, 74, 84, 255 });
}

// Slider for an ANIMATABLE property. No track -> edits the base field (rest
// pose), exactly like before. With a track -> shows the value evaluated at the
// playhead; auto-key ON writes/updates a key there, auto-key OFF disables the
// slider (edit through the key inspector instead).
static void PropSlider(Rectangle r, AnimElem *e, int prop, float *baseField)
{
    float lo = AnimPropMin(prop), hi = AnimPropMax(prop);
    AnimTrack *tr = AnimElemFindTrack(e, prop);
    if (!tr) { EditSlider(r, "", baseField, lo, hi); return; }

    float v = AnimElemProp(e, prop, playhead);
    if (!autoKey) GuiSetState(STATE_DISABLED);
    if (EditSlider(r, "", &v, lo, hi))
    {
        if (playhead > AUTOKEY_EPS) EnsureZeroKey(e, tr);   // keep a start key
        AnimTrackWriteKeyAt(tr, playhead, v, AUTOKEY_EPS);
    }
    if (!autoKey) GuiSetState(STATE_NORMAL);
}

// RGB slider rows for ONE colour property (fill or outline). Tracked -> the
// sliders follow the playhead and auto-key writes colour keys there (mirrors
// PropSlider); untracked -> they edit the base colour. Returns the new y.
static float ColorRGBRows(float x, float y, float w, AnimElem *e, int prop,
                          Color *base, const char *label)
{
    float rh = 24.0f;
    AnimTrack *ctr = AnimElemFindTrack(e, prop);
    Color cc = AnimElemColorProp(e, prop, playhead);
    GuiLabel((Rectangle){ x, y, w-24, rh },
             TextFormat(ctr ? "%s (rgb, keyed)" : "%s (rgb)", label));
    DrawSwatch((Rectangle){ x+w-20, y+3, 18, 18 }, cc);
    y += rh;
    float cr=cc.r, cg=cc.g, cb=cc.b;
    bool changed = false;
    if (ctr && !autoKey) GuiSetState(STATE_DISABLED);
    if (EditSlider((Rectangle){ x+16, y, w-16-50, 16 }, "R", &cr, 0,255)) changed=true; y+=18;
    if (EditSlider((Rectangle){ x+16, y, w-16-50, 16 }, "G", &cg, 0,255)) changed=true; y+=18;
    if (EditSlider((Rectangle){ x+16, y, w-16-50, 16 }, "B", &cb, 0,255)) changed=true; y+=18;
    if (ctr && !autoKey) GuiSetState(STATE_NORMAL);
    if (changed)
    {
        Color nc = { (unsigned char)cr,(unsigned char)cg,(unsigned char)cb, 255 };
        if (!ctr) { base->r = nc.r; base->g = nc.g; base->b = nc.b; }
        else
        {
            if (playhead > AUTOKEY_EPS) EnsureZeroColorKey(e, ctr);
            AnimTrackWriteColorKeyAt(ctr, playhead, nc, AUTOKEY_EPS);
        }
    }
    return y;
}

static bool IsSelected(int i)
{
    return i == selElem || (i >= 0 && i < ANIM_ELEMS_MAX && multiSel[i]);
}

// ---------------------------------------------------------------------------
//  Left panel: element list + add/delete.
// ---------------------------------------------------------------------------
static float DrawElementList(float x, float y, float w)   // returns content height
{
    float y0 = y, rh = 26.0f, gap = 4.0f;
    GuiLabel((Rectangle){ x, y, w, 20 }, "ELEMENTS"); y += 22;

    // reorder/duplicate are deferred past the loop: mutating doc.elems mid-draw
    // shifts the array and re-draws the next row's buttons on the same rect this
    // frame, which re-fires (same hazard as the track `del` button).
    int pendingMove = -1, pendingMoveDelta = 0, pendingDup = -1;
    float btnW = 20.0f;                       // ^ / v / dup strip on the right
    for (int i = 0; i < doc.elemCount; i++)
    {
        Rectangle nameR = { x, y, w - 3*btnW - 2, rh };
        const char *tag = AnimElemKindName(doc.elems[i].kind);
        bool pressed = GuiButton(nameR, TextFormat("%s  [%s]", doc.elems[i].name, tag));

        // order controls: disabled at the ends so they read as unavailable.
        if (i == 0) GuiDisable();
        if (GuiButton((Rectangle){ x + w - 3*btnW, y, btnW, rh }, "#121#"))
        { AudioPlayButton(); pendingMove = i; pendingMoveDelta = -1; }
        if (i == 0) GuiEnable();

        if (i == doc.elemCount - 1) GuiDisable();
        if (GuiButton((Rectangle){ x + w - 2*btnW, y, btnW, rh }, "#120#"))
        { AudioPlayButton(); pendingMove = i; pendingMoveDelta = +1; }
        if (i == doc.elemCount - 1) GuiEnable();

        if (doc.elemCount >= ANIM_ELEMS_MAX) GuiDisable();
        if (GuiButton((Rectangle){ x + w - btnW, y, btnW, rh }, "#016#"))
        { AudioPlayButton(); pendingDup = i; }
        if (doc.elemCount >= ANIM_ELEMS_MAX) GuiEnable();

        // selection marker AFTER the button - raygui paints its own background,
        // so a tint drawn first would be invisible.
        if (i == selElem)
            DrawRectangleRec(nameR, (Color){ 90, 140, 220, 90 });
        else if (IsSelected(i))
            DrawRectangleRec(nameR, (Color){ 60, 90, 140, 70 });
        if (pressed)
        {
            AudioPlayButton();
            bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            if (ctrl && i != selElem) multiSel[i] = !multiSel[i];   // toggle extra
            else { selElem = i; for (int k=0;k<ANIM_ELEMS_MAX;k++) multiSel[k]=false; }
        }
        y += rh + gap;
    }

    if (pendingMove >= 0)
    {
        UndoPush();
        int to = pendingMove + pendingMoveDelta;
        AnimDocMoveElem(&doc, pendingMove, pendingMoveDelta);
        // follow the moved element with the selection + key selection, and keep
        // any ctrl-selected companion rows pointing at the same elements.
        if      (selElem == pendingMove) selElem = to;
        else if (selElem == to)          selElem = pendingMove;
        bool a = multiSel[pendingMove], b = multiSel[to];
        multiSel[pendingMove] = b; multiSel[to] = a;
        if      (selKeyElem == pendingMove) selKeyElem = to;
        else if (selKeyElem == to)          selKeyElem = pendingMove;
        ClampSelection();
    }
    else if (pendingDup >= 0)
    {
        UndoPush();
        if (AnimDocDuplicateElem(&doc, pendingDup))
        {
            // the copy is inserted at pendingDup+1: anything at or after that
            // slot shifted down one, so trailing selections must follow.
            for (int i = ANIM_ELEMS_MAX - 1; i > pendingDup + 1; i--)
                multiSel[i] = multiSel[i-1];
            multiSel[pendingDup + 1] = false;
            if (selElem    > pendingDup) selElem++;
            if (selKeyElem > pendingDup) selKeyElem++;
            selElem = pendingDup + 1;               // select the new copy
            ClampSelection();
        }
    }

    y += 6;
    float bw = (w - 8) / 3.0f;
    if (GuiButton((Rectangle){ x, y, bw, rh }, "+Text"))
    { AudioPlayButton(); UndoPush(); AnimElem *e=AnimDocAddElem(&doc, AE_TEXT); if(e) selElem=doc.elemCount-1; }
    if (GuiButton((Rectangle){ x+bw+4, y, bw, rh }, "+Shape"))
    { AudioPlayButton(); UndoPush(); AnimElem *e=AnimDocAddElem(&doc, AE_SHAPE); if(e) selElem=doc.elemCount-1; }
    if (GuiButton((Rectangle){ x+2*bw+8, y, bw, rh }, "+Global"))
    { AudioPlayButton(); UndoPush(); AnimElem *e=AnimDocAddElem(&doc, AE_GLOBAL); if(e) selElem=doc.elemCount-1; }
    y += rh + gap;

    // shelve the primary selection (base props + all its tracks) for reuse
    if (selElem >= 0 && selElem < doc.elemCount)
    {
        if (GuiButton((Rectangle){ x, y, w, rh }, "Save to library"))
        {
            AudioPlayButton();
            TextCopy(nameBuf, doc.elems[selElem].name);
            edNameBuf = true; libTargetIdx = -1; prompt = PROMPT_LIB_SAVE_NAME;
        }
        y += rh + gap;
    }

    if (selElem >= 0 && GuiButton((Rectangle){ x, y, w, rh }, "Delete selected"))
    {
        AudioPlayButton(); UndoPush();
        // remove ALL selected elements (primary + ctrl-clicked), high to low so
        // the pending indices stay valid while we shift.
        for (int i = doc.elemCount - 1; i >= 0; i--)
            if (IsSelected(i)) AnimDocRemoveElem(&doc, i);
        selElem = doc.elemCount > 0 ? 0 : -1;
        for (int i = 0; i < ANIM_ELEMS_MAX; i++) multiSel[i] = false;
        ClearKeySelection();
        ClampSelection();
    }
    y += rh + gap;

    return (y + 4) - y0;
}

// ---------------------------------------------------------------------------
//  Right panel: inspector for the primary selection.
// ---------------------------------------------------------------------------
static float DrawInspector(float x, float y, float w)   // returns content height
{
    float y0 = y;
    GuiLabel((Rectangle){ x, y, w, 20 }, "INSPECTOR"); y += 24;
    if (selElem < 0 || selElem >= doc.elemCount)
    {
        GuiLabel((Rectangle){ x, y, w, 20 }, "(no element selected)");
        return (y + 20) - y0;
    }
    AnimElem *e = &doc.elems[selElem];
    float rh = 24.0f, gap = 6.0f;

    // name
    GuiLabel((Rectangle){ x, y, 40, rh }, "name");
    if (GuiTextBox((Rectangle){ x+44, y, w-44, rh }, e->name, ANIM_NAME_MAX, edName))
    { if(!edName) UndoPush(); edName = !edName; }
    y += rh + gap;

    if (e->kind == AE_TEXT)
    {
        GuiLabel((Rectangle){ x, y, 40, rh }, "text");
        if (GuiTextBox((Rectangle){ x+44, y, w-44, rh }, e->text, ANIM_TEXT_LEN_MAX, edText))
        { if(!edText) UndoPush(); edText = !edText; }
        y += rh + gap;
    }
    if (e->kind == AE_SHAPE)
    {
        // 2x3 grid of kind toggles (a single GuiToggleGroup can't wrap rows)
        GuiLabel((Rectangle){ x, y, 40, rh }, "shape");
        float bw3 = (w - 44 - 8) / 3.0f;
        for (int si = 0; si < SHAPE_KIND_COUNT; si++)
        {
            int rowI = si / 3, colI = si % 3;
            Rectangle rr = { x + 44 + (float)colI * (bw3 + 4),
                             y + (float)rowI * (rh + 4), bw3, rh };
            bool on = (e->shapeKind == si);
            GuiToggle(rr, AnimShapeKindName(si), &on);
            if (on && e->shapeKind != si) { UndoPush(); e->shapeKind = si; }
        }
        y += 2*(rh + 4) + gap - 4;
    }

    // position / size: tracked props follow the playhead (see PropSlider)
    if (e->kind != AE_GLOBAL)
    {
        bool isText = e->kind == AE_TEXT;
        PropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, isText ? AP_T_POS_X : AP_S_POS_X, &e->posFrac.x);
        GuiLabel((Rectangle){ x, y, 44, rh }, "posX"); y += rh + gap;
        PropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, isText ? AP_T_POS_Y : AP_S_POS_Y, &e->posFrac.y);
        GuiLabel((Rectangle){ x, y, 44, rh }, "posY"); y += rh + gap;
        PropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, isText ? AP_T_SIZE : AP_S_W, &e->sizeFrac.x);
        GuiLabel((Rectangle){ x, y, 44, rh }, isText ? "size" : "w"); y += rh + gap;
        if (e->kind == AE_SHAPE)
        {
            PropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, AP_S_H, &e->sizeFrac.y);
            GuiLabel((Rectangle){ x, y, 44, rh }, "h"); y += rh + gap;
            // uniform multiplier over w/h: the single control for growing a
            // shape without keeping the two axes in proportion by hand.
            PropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, AP_S_SCALE, &e->scaleFrac);
            GuiLabel((Rectangle){ x, y, 44, rh }, "scale"); y += rh + gap;
        }
    }

    // colour (RGB sliders keep it compact and stable across raygui versions).
    // With a colour track the sliders follow the playhead and auto-key writes
    // colour keys there (mirrors PropSlider); without one they edit the base.
    // RGB is animatable via the colour track; alpha stays separate (the base
    // channel here, the alpha track for animation) so it is never keyed along.
    y = ColorRGBRows(x, y, w, e, ColorPropFor(e->kind), &e->color, "color");
    float ca = e->color.a;
    if (EditSlider((Rectangle){ x+16, y, w-16-50, 16 }, "A", &ca, 0,255))
        e->color.a = (unsigned char)ca;
    y+=22;

    // background: the scene fill behind every element. Its own colour track
    // plus an alpha track, both independent of the fade colour above.
    if (e->kind == AE_GLOBAL)
    {
        GuiLine((Rectangle){ x, y, w, 8 }, "background"); y += 12;
        y = ColorRGBRows(x, y, w, e, AP_G_BG_COLOR, &e->bgColor, "background");
        // alpha doubles as the on/off control (0 = no background). Matches the
        // fill/outline alpha rows: the base channel here, AP_G_BG_ALPHA for
        // animation.
        float ba = e->bgColor.a;
        if (EditSlider((Rectangle){ x+16, y, w-16-50, 16 }, "A", &ba, 0,255))
            e->bgColor.a = (unsigned char)ba;
        y += 22;
    }

    // outline: thickness + its own colour track (fill and outline are separate
    // colour PROPERTIES, each with its own base + optional track).
    if (e->kind == AE_SHAPE)
    {
        GuiLine((Rectangle){ x, y, w, 8 }, "outline"); y += 12;
        PropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, AP_S_OUTLINE, &e->outlineFrac);
        GuiLabel((Rectangle){ x, y, 44, rh }, "thick"); y += rh + gap;
        y = ColorRGBRows(x, y, w, e, AP_S_OUTLINE_COLOR, &e->outlineColor, "outline");
        float oa = e->outlineColor.a;
        if (EditSlider((Rectangle){ x+16, y, w-16-50, 16 }, "A", &oa, 0,255))
            e->outlineColor.a = (unsigned char)oa;
        y += 22;
    }

    // --- tracks list: every track and ALL of its keys, always visible --------
    GuiLine((Rectangle){ x, y, w, 8 }, "tracks"); y += 12;
    // deletion is deferred past the loop: removing mid-draw shifts the array
    // and re-draws the next track's del button on the same rect this frame,
    // which re-fires and cascade-deletes.
    int pendingTrackDel = -1;
    for (int j = 0; j < e->trackCount; j++)
    {
        AnimTrack *tr = &e->tracks[j];
        GuiLabel((Rectangle){ x, y, w-106, rh },
                 TextFormat("%s (%d)", AnimPropName(tr->prop), tr->keyCount));
        // explicit keying path (works with auto-key off): key at the playhead.
        if (GuiButton((Rectangle){ x+w-102, y, 50, rh }, "+key"))
        {
            AudioPlayButton(); UndoPush();
            AnimKey *k;
            if (AnimPropIsColor(tr->prop))
            {
                Color c = AnimElemColorProp(e, tr->prop, playhead);
                if (playhead > AUTOKEY_EPS) EnsureZeroColorKey(e, tr);
                k = AnimTrackWriteColorKeyAt(tr, playhead, c, AUTOKEY_EPS);
            }
            else
            {
                float v = AnimElemProp(e, tr->prop, playhead);
                if (playhead > AUTOKEY_EPS) EnsureZeroKey(e, tr);
                k = AnimTrackWriteKeyAt(tr, playhead, v, AUTOKEY_EPS);
            }
            if (k) SelectKey(selElem, j, (int)(k - tr->keys));
        }
        if (GuiButton((Rectangle){ x+w-48, y, 48, rh }, "del"))
        {
            AudioPlayButton(); UndoPush();
            pendingTrackDel = j;
        }
        y += rh + 2;

        // key rows: "t  value  ease" - click to select (same as the timeline).
        // The two keys bracketing the playhead (= the ones being lerped right
        // now) carry a warm tint so it's clear which keys are in effect.
        int segA = -1, segB = -1;
        AnimTrackSegment(tr, playhead, &segA, &segB);
        for (int k = 0; k < tr->keyCount; k++)
        {
            bool sel = (selKeyElem == selElem && selKeyTrack == j && selKeyIdx == k);
            bool lerping = (k == segA || k == segB);
            Rectangle kr = { x+12, y, w-12, 18 };
            const char *label = AnimPropIsColor(tr->prop)
                ? TextFormat("%.2f   #%02X%02X%02X   %s", tr->keys[k].t,
                             tr->keys[k].cval.r, tr->keys[k].cval.g,
                             tr->keys[k].cval.b,
                             AnimEaseName(tr->keys[k].ease))
                : TextFormat("%.2f   %.2f   %s", tr->keys[k].t,
                             tr->keys[k].value, AnimEaseName(tr->keys[k].ease));
            bool pressed = GuiButton(kr, label);
            // markers AFTER the button (raygui paints its own background)
            if (sel)     DrawRectangleRec(kr, (Color){ 60, 90, 140, 120 });
            if (lerping)
            {
                DrawRectangleRec(kr, (Color){ 255, 210, 90, 40 });
                DrawRectangleRec((Rectangle){ kr.x, kr.y, 3, kr.height },
                                 (Color){ 255, 210, 90, 255 });
            }
            if (AnimPropIsColor(tr->prop))
            {
                Color sc = tr->keys[k].cval; sc.a = 255;
                DrawSwatch((Rectangle){ kr.x + kr.width - 18, kr.y + 2, 14, 14 }, sc);
            }
            if (pressed)
            {
                AudioPlayButton();
                SelectKey(selElem, j, k);
            }
            // a timeline click asked to reveal this key: nudge the scroll so
            // the row sits inside the panel (applies next frame).
            if (sel && scrollToSelKey)
            {
                float top = inspPanelRect.y + 24.0f;
                float bot = inspPanelRect.y + inspPanelRect.height - 24.0f;
                // nudge the OWNING view (PanelScroll re-clamps it next frame);
                // writing a mirror copy here would just be overwritten.
                if (kr.y < top)                    inspView.scroll += top - kr.y;
                else if (kr.y + kr.height > bot)   inspView.scroll -= (kr.y + kr.height) - bot;
                scrollToSelKey = false;
            }
            y += 20;
        }
        y += 4;
    }
    if (pendingTrackDel >= 0)
    {
        AnimElemRemoveTrack(e, pendingTrackDel);
        // keep the key selection pointing at the same track after the shift
        if (selKeyElem == selElem)
        {
            if (selKeyTrack == pendingTrackDel)     ClearKeySelection();
            else if (selKeyTrack > pendingTrackDel) selKeyTrack--;
        }
        ClampSelection();
    }

    // add-track dropdown (built from the element kind's valid props)
    y += 4;
    int propCount = AnimPropCountFor(e->kind);
    if (addTrackSel >= propCount) addTrackSel = 0;   // kinds differ in count
    Rectangle addR = { x, y, w-56, rh };
    if (GuiButton((Rectangle){ x+w-52, y, 52, rh }, "+track"))
    {
        AudioPlayButton(); UndoPush();
        int prop = AnimPropAt(e->kind, addTrackSel);
        // seed the START key at t=0, and - when the scrubber sits later on the
        // clock - a second key right at the playhead, ready to edit.
        AnimTrack *tr = AnimElemAddTrack(e, prop);
        if (tr && AnimPropIsColor(prop))
        {
            Color c = AnimElemColorProp(e, prop, playhead);
            EnsureZeroColorKey(e, tr);
            if (playhead > AUTOKEY_EPS)
            {
                AnimKey *k = AnimTrackWriteColorKeyAt(tr, playhead, c, AUTOKEY_EPS);
                if (k) SelectKey(selElem, e->trackCount - 1, (int)(k - tr->keys));
            }
        }
        else if (tr)
        {
            float v = AnimElemProp(e, prop, playhead);
            EnsureZeroKey(e, tr);
            if (playhead > AUTOKEY_EPS)
            {
                AnimKey *k = AnimTrackWriteKeyAt(tr, playhead, v, AUTOKEY_EPS);
                if (k) SelectKey(selElem, e->trackCount - 1, (int)(k - tr->keys));
            }
        }
    }
    // dropdown HEADER only - the open list is drawn last as an overlay so it
    // can flip above the header instead of being culled at the screen bottom.
    if (GuiButton(addR, TextFormat("%s  v",
                  AnimPropName(AnimPropAt(e->kind, addTrackSel)))))
    {
        AudioPlayButton();
        addTrackDrop = (addTrackDrop == selElem) ? -1 : selElem;
        keyEaseDropOpen = false;
    }
    addTrackRect = addR; addTrackVisible = true;
    y += rh + 10;

    // --- key inspector: the selected timeline diamond's t / value / ease ----
    // (skipped while the add-track dropdown is open - it would draw over us)
    keyEaseVisible = false;
    if (addTrackDrop != selElem && selKeyElem == selElem &&
        selKeyTrack >= 0 && selKeyTrack < e->trackCount &&
        selKeyIdx   >= 0 && selKeyIdx   < e->tracks[selKeyTrack].keyCount)
    {
        AnimTrack *tr = &e->tracks[selKeyTrack];
        GuiLine((Rectangle){ x, y, w, 8 },
                TextFormat("key  %s #%d", AnimPropName(tr->prop), selKeyIdx));
        y += 12;

        // time: textbox commits on toggle-off (parse, clamp, resort)
        GuiLabel((Rectangle){ x, y, 44, rh }, "time");
        if (GuiTextBox((Rectangle){ x+44, y, w-44, rh }, keyTimeBuf,
                       sizeof(keyTimeBuf), edKeyTime))
        {
            if (!edKeyTime) edKeyTime = true;
            else
            {
                float nt = (float)atof(keyTimeBuf);
                if (nt < 0) nt = 0; if (nt > doc.duration) nt = doc.duration;
                UndoPush();
                selKeyIdx = AnimTrackSetKeyTime(tr, selKeyIdx, nt);
                SelectKey(selKeyElem, selKeyTrack, selKeyIdx);
            }
        }
        y += rh + gap;

        // value: colour keys get RGBA sliders + swatch, scalar keys one slider
        AnimKey *k = &tr->keys[selKeyIdx];
        if (AnimPropIsColor(tr->prop))
        {
            GuiLabel((Rectangle){ x, y, w-24, rh }, "value (rgb)");
            DrawSwatch((Rectangle){ x+w-20, y+3, 18, 18 },
                       (Color){ k->cval.r, k->cval.g, k->cval.b, 255 });
            y += rh;
            float kr=k->cval.r, kg=k->cval.g, kb=k->cval.b;
            bool ch = false;
            if (EditSlider((Rectangle){ x+16, y, w-16-50, 16 }, "R", &kr, 0,255)) ch=true; y+=18;
            if (EditSlider((Rectangle){ x+16, y, w-16-50, 16 }, "G", &kg, 0,255)) ch=true; y+=18;
            if (EditSlider((Rectangle){ x+16, y, w-16-50, 16 }, "B", &kb, 0,255)) ch=true; y+=22;
            if (ch) k->cval = (Color){ (unsigned char)kr,(unsigned char)kg,
                                       (unsigned char)kb, 255 };
        }
        else
        {
            GuiLabel((Rectangle){ x, y, 44, rh }, "value");
            float v = k->value;
            if (EditSlider((Rectangle){ x+44, y, w-44-50, rh }, "", &v,
                           AnimPropMin(tr->prop), AnimPropMax(tr->prop)))
                k->value = v;
            y += rh + gap;
        }

        // ease dropdown header; its open list is drawn last as an overlay that
        // flips above the header when there is no room below.
        GuiLabel((Rectangle){ x, y, 44, rh }, "ease");
        keyEaseRect = (Rectangle){ x+44, y, w-44-54, rh };
        keyEaseVisible = true;
        if (GuiButton(keyEaseRect, TextFormat("%s  v", AnimEaseName(keyEaseSel))))
        {
            AudioPlayButton();
            keyEaseDropOpen = !keyEaseDropOpen;
            addTrackDrop = -1;
        }
        if (GuiButton((Rectangle){ x+w-50, y, 50, rh }, "del"))
        {
            AudioPlayButton(); UndoPush();
            AnimTrackRemoveKey(tr, selKeyIdx);
            ClearKeySelection();
            ClampSelection();
        }
        y += rh;
    }
    return (y + 12) - y0;
}

// The open dropdown LIST (ease or add-track), drawn AFTER every other widget
// so it overlays them (immediate mode: last drawn wins). Unlike raygui's
// GuiDropdownBox - which always opens downward and gets culled at the screen
// bottom - the list flips ABOVE the header when there is no room below.
static void DrawDropdownOverlays()
{
    bool easeMode  = keyEaseDropOpen && keyEaseVisible && selKeyElem >= 0;
    bool trackMode = !easeMode && addTrackVisible && addTrackDrop == selElem &&
                     selElem >= 0 && selElem < doc.elemCount;
    if (!easeMode && !trackMode) return;

    ScreenState *ss = ScreenStateGet();
    float H = (float)ss->height;
    Rectangle hdr = easeMode ? keyEaseRect : addTrackRect;
    int count = easeMode ? AnimEaseCount() : AnimPropCountFor(doc.elems[selElem].kind);
    int cur   = easeMode ? keyEaseSel : addTrackSel;

    float ih = 20.0f, listH = ih * count;
    float ly = (hdr.y + hdr.height + listH <= H - 4.0f)
             ? hdr.y + hdr.height          // fits below
             : hdr.y - listH;              // flip above the header
    if (ly < 4.0f) ly = 4.0f;              // clamp to the screen top
    Rectangle bg = { hdr.x, ly, hdr.width, listH };
    DrawRectangleRec(bg, (Color){ 32, 34, 40, 255 });
    DrawRectangleLinesEx(bg, 1.0f, (Color){ 70, 74, 84, 255 });

    int picked = -1;
    for (int i = 0; i < count; i++)
    {
        Rectangle rr = { bg.x, bg.y + i*ih, bg.width, ih };
        const char *nm = easeMode ? AnimEaseName(i)
                                  : AnimPropName(AnimPropAt(doc.elems[selElem].kind, i));
        if (GuiButton(rr, nm)) picked = i;
        if (i == cur) DrawRectangleRec(rr, (Color){ 90, 140, 220, 60 });
    }

    if (picked >= 0)
    {
        AudioPlayButton();
        if (easeMode)
        {
            UndoPush();
            keyEaseSel = picked;
            doc.elems[selKeyElem].tracks[selKeyTrack].keys[selKeyIdx].ease = picked;
            keyEaseDropOpen = false;
        }
        else { addTrackSel = picked; addTrackDrop = -1; }
    }
    else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
             !CheckCollisionPointRec(GetMousePosition(), bg) &&
             !CheckCollisionPointRec(GetMousePosition(), hdr))
    { keyEaseDropOpen = false; addTrackDrop = -1; }    // click-away closes
}

// Request switching to animList[idx]: guard unsaved edits with a save prompt,
// otherwise load immediately. Closes the dropdown either way.
static void RequestSwitch(int idx)
{
    animSwitchOpen = false;
    if (idx == animCurrent) return;             // already on it
    if (docDirty) { prompt = PROMPT_SAVE_THEN_SWITCH; promptTargetIdx = idx; }
    else          LoadAnimByIndex(idx);
}

// The animation-switch dropdown: one row per saved anim (name switches, X
// deletes) plus a "+ New..." row. Drawn as a top overlay like the ease list.
static void DrawAnimSwitchOverlay()
{
    if (!animSwitchOpen || !animSwitchVisible) return;

    ScreenState *ss = ScreenStateGet();
    float H = (float)ss->height;
    Rectangle hdr = animSwitchRect;
    int rows = animCount + 1;                    // +1 for the "New..." row
    float ih = 22.0f, listH = ih * rows;
    float ly = (hdr.y + hdr.height + listH <= H - 4.0f)
             ? hdr.y + hdr.height : hdr.y - listH;
    if (ly < 4.0f) ly = 4.0f;
    Rectangle bg = { hdr.x, ly, hdr.width, listH };
    DrawRectangleRec(bg, (Color){ 32, 34, 40, 255 });
    DrawRectangleLinesEx(bg, 1.0f, (Color){ 70, 74, 84, 255 });

    int reqSwitch = -1, reqDelete = -1;
    bool reqNew = false;
    float xw = 22.0f;                            // width of the X delete button
    for (int i = 0; i < animCount; i++)
    {
        Rectangle rr = { bg.x, bg.y + i*ih, bg.width, ih };
        Rectangle nameR = { rr.x, rr.y, rr.width - xw, ih };
        Rectangle delR  = { rr.x + rr.width - xw, rr.y, xw, ih };
        if (GuiButton(nameR, animList[i])) reqSwitch = i;
        if (GuiButton(delR, "#143#")) reqDelete = i;   // trash icon
        if (i == animCurrent) DrawRectangleRec(nameR, (Color){ 90, 140, 220, 60 });
    }
    Rectangle newR = { bg.x, bg.y + animCount*ih, bg.width, ih };
    if (GuiButton(newR, "+ New...")) reqNew = true;

    if (reqNew)
    {
        AudioPlayButton(); animSwitchOpen = false;
        nameBuf[0] = '\0'; edNameBuf = true; prompt = PROMPT_NEW_NAME;
    }
    else if (reqDelete >= 0)
    {
        AudioPlayButton(); animSwitchOpen = false;
        prompt = PROMPT_CONFIRM_DELETE; promptTargetIdx = reqDelete;
    }
    else if (reqSwitch >= 0) { AudioPlayButton(); RequestSwitch(reqSwitch); }
    else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
             !CheckCollisionPointRec(GetMousePosition(), bg) &&
             !CheckCollisionPointRec(GetMousePosition(), hdr))
        animSwitchOpen = false;                  // click-away closes
}

// Actually delete animList[idx] from disk, refresh the list, and if it was the
// current animation fall back to another (or a fresh starter doc).
static void DeleteAnim(int idx)
{
    if (idx < 0 || idx >= animCount) return;
    bool wasCurrent = (idx == animCurrent);
    // remember the current anim's NAME - rescan may reorder/shift indices.
    char curName[ANIM_NAME_MAX] = {0};
    if (animCurrent >= 0) TextCopy(curName, animList[animCurrent]);

    ForgetSelElem(animList[idx]);   // while animList[idx] still names it
    remove(AnimPath(animList[idx]));
    RescanAnims();

    if (wasCurrent)
    {
        animCurrent = -1;
        if (animCount > 0) LoadAnimByIndex(0);
        else { UndoPush(); AnimSignalUnregister(&doc,&preview); MakeStarterDoc();
               AnimSignalRegister(&doc, &preview, &playhead); docDirty = false; ClampSelection(); }
    }
    else animCurrent = AnimFind(curName);        // follow the current anim by name
}

// Create a fresh named animation from nameBuf, save it, make it current.
static void CreateAnim(const char *name)
{
    UndoPush();
    AnimSignalUnregister(&doc, &preview);
    MakeStarterDoc();
    TextCopy(doc.name, name);
    AnimSignalRegister(&doc, &preview, &playhead);
    AnimDocSave(&doc, AnimPath(name));
    RescanAnims();
    animCurrent = AnimFind(name);
    docDirty = false;
    ClampSelection();
    playhead = 0.0f; playing = false;
}

// A small centered modal for the switch/delete/new flows. Drawn topmost.
static void DrawPromptModal()
{
    if (prompt == PROMPT_NONE) return;

    ScreenState *ss = ScreenStateGet();
    float W = (float)ss->width, H = (float)ss->height;
    float mw = 320, mh = 120;
    Rectangle m = { (W-mw)/2, (H-mh)/2, mw, mh };
    DrawRectangle(0, 0, (int)W, (int)H, (Color){ 0, 0, 0, 120 });   // dim behind
    DrawRectangleRec(m, (Color){ 40, 42, 48, 255 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 90, 94, 104, 255 });

    float bw = 90, bh = 28, by = m.y + mh - bh - 12;
    Rectangle msg = { m.x+16, m.y+14, mw-32, 40 };

    if (prompt == PROMPT_SAVE_THEN_SWITCH)
    {
        GuiLabel(msg, TextFormat("Save changes to \"%s\" before switching?",
                                 animCurrent >= 0 ? animList[animCurrent] : "*unsaved"));
        if (GuiButton((Rectangle){ m.x+16, by, bw, bh }, "Save"))
        {   // saving rescans (list may reorder), so re-find the target by name.
            AudioPlayButton();
            char target[ANIM_NAME_MAX];
            TextCopy(target, animList[promptTargetIdx]);
            SaveCurrent();
            prompt = PROMPT_NONE;
            LoadAnimByIndex(AnimFind(target));
        }
        if (GuiButton((Rectangle){ m.x+(mw-bw)/2, by, bw, bh }, "Discard"))
        { AudioPlayButton(); docDirty = false; int t = promptTargetIdx; prompt = PROMPT_NONE;
          LoadAnimByIndex(t); }
        if (GuiButton((Rectangle){ m.x+mw-bw-16, by, bw, bh }, "Cancel"))
        { AudioPlayButton(); prompt = PROMPT_NONE; }
    }
    else if (prompt == PROMPT_CONFIRM_DELETE)
    {
        const char *nm = (promptTargetIdx >= 0 && promptTargetIdx < animCount)
                       ? animList[promptTargetIdx] : "?";
        GuiLabel(msg, TextFormat("Delete animation \"%s\"? This cannot be undone.", nm));
        if (GuiButton((Rectangle){ m.x+mw-2*bw-24, by, bw, bh }, "Delete"))
        { AudioPlayButton(); DeleteAnim(promptTargetIdx); prompt = PROMPT_NONE; }
        if (GuiButton((Rectangle){ m.x+mw-bw-16, by, bw, bh }, "Cancel"))
        { AudioPlayButton(); prompt = PROMPT_NONE; }
    }
    else if (prompt == PROMPT_LIB_SAVE_NAME || prompt == PROMPT_LIB_RENAME)
    {
        bool renaming = (prompt == PROMPT_LIB_RENAME);
        GuiLabel(msg, renaming ? "Rename library entry:"
                               : "Save element to library as:");
        Rectangle tb = { m.x+16, m.y+44, mw-32, 26 };
        if (GuiTextBox(tb, nameBuf, ANIM_NAME_MAX, edNameBuf)) edNameBuf = !edNameBuf;

        // a name may reuse an existing entry only when SAVING (overwrite); a
        // rename onto another entry's name is rejected by AnimLibraryRename.
        int clash = AnimLibraryFind(&library, nameBuf);
        bool valid = nameBuf[0] && !(renaming && clash >= 0 && clash != libTargetIdx);
        if (!valid) GuiDisable();
        if (GuiButton((Rectangle){ m.x+mw-2*bw-24, by, bw, bh },
                      renaming ? "Rename" : (clash >= 0 ? "Overwrite" : "Save")))
        {
            AudioPlayButton();
            if (renaming) AnimLibraryRename(&library, libTargetIdx, nameBuf);
            else if (selElem >= 0 && selElem < doc.elemCount)
                AnimLibraryAdd(&library, nameBuf, &doc.elems[selElem]);
            AnimLibrarySave(&library, LIB_PATH);
            edNameBuf = false; libTargetIdx = -1;
            // back to the shelf so the result is visible straight away
            prompt = PROMPT_LIBRARY;
        }
        if (!valid) GuiEnable();
        if (GuiButton((Rectangle){ m.x+mw-bw-16, by, bw, bh }, "Cancel"))
        { AudioPlayButton(); edNameBuf = false; libTargetIdx = -1;
          prompt = renaming ? PROMPT_LIBRARY : PROMPT_NONE; }
    }
    else if (prompt == PROMPT_NEW_NAME)
    {
        GuiLabel(msg, "New animation name:");
        Rectangle tb = { m.x+16, m.y+44, mw-32, 26 };
        if (GuiTextBox(tb, nameBuf, ANIM_NAME_MAX, edNameBuf)) edNameBuf = !edNameBuf;
        bool valid = nameBuf[0] && AnimFind(nameBuf) < 0;
        if (!valid) GuiDisable();
        if (GuiButton((Rectangle){ m.x+mw-2*bw-24, by, bw, bh }, "Create"))
        { AudioPlayButton(); CreateAnim(nameBuf); prompt = PROMPT_NONE; edNameBuf = false; }
        if (!valid) GuiEnable();
        if (GuiButton((Rectangle){ m.x+mw-bw-16, by, bw, bh }, "Cancel"))
        { AudioPlayButton(); prompt = PROMPT_NONE; edNameBuf = false; }
    }
}

// ---------------------------------------------------------------------------
//  Element library modal: insert a shelved element into the doc, rename or
//  delete entries. Bigger than DrawPromptModal's fixed box (it needs a
//  scrolling list), so it gets its own drawing routine.
// ---------------------------------------------------------------------------
static void DrawLibraryModal()
{
    if (prompt != PROMPT_LIBRARY) return;

    ScreenState *ss = ScreenStateGet();
    float W = (float)ss->width, H = (float)ss->height;
    float mw = 420, mh = 340;
    Rectangle m = { (W-mw)/2, (H-mh)/2, mw, mh };
    DrawRectangle(0, 0, (int)W, (int)H, (Color){ 0, 0, 0, 120 });
    DrawRectangleRec(m, (Color){ 40, 42, 48, 255 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 90, 94, 104, 255 });

    GuiLabel((Rectangle){ m.x+16, m.y+10, mw-32, 20 }, "ELEMENT LIBRARY");
    if (library.count == 0)
        GuiLabel((Rectangle){ m.x+16, m.y+34, mw-32, 20 },
                 "(empty - use \"Save to library\" on a selected element)");

    // scrolling entry list
    Rectangle list = { m.x+12, m.y+34, mw-24, mh-34-48 };
    if (CheckCollisionPointRec(GetMousePosition(), list))
        libScroll += GetMouseWheelMove() * 24.0f;

    float rh = 26.0f, gap = 4.0f;
    float contentH = library.count * (rh + gap);
    float maxScroll = contentH - list.height;
    if (maxScroll < 0) maxScroll = 0;
    if (libScroll < -maxScroll) libScroll = -maxScroll;
    if (libScroll > 0) libScroll = 0;

    int reqInsert = -1, reqRename = -1, reqDelete = -1;
    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    float ly = list.y + libScroll;
    for (int i = 0; i < library.count; i++)
    {
        const AnimLibEntry *en = &library.entries[i];
        float bw2 = 60.0f;
        Rectangle nameR = { list.x, ly, list.width - 2*bw2 - 8, rh };
        if (GuiButton(nameR, TextFormat("%s   [%s]", en->name,
                                        AnimElemKindName(en->elem.kind))))
            reqInsert = i;
        if (GuiButton((Rectangle){ list.x + list.width - 2*bw2 - 4, ly, bw2, rh }, "rename"))
            reqRename = i;
        if (GuiButton((Rectangle){ list.x + list.width - bw2, ly, bw2, rh }, "delete"))
            reqDelete = i;
        ly += rh + gap;
    }
    EndScissorMode();

    // footer
    float bh = 28, by = m.y + mh - bh - 12;
    GuiLabel((Rectangle){ m.x+16, by, mw-120, bh }, "click an entry to add it here");
    if (GuiButton((Rectangle){ m.x+mw-90, by, 78, bh }, "Close"))
    { AudioPlayButton(); prompt = PROMPT_NONE; }

    if (reqInsert >= 0)
    {
        AudioPlayButton();
        if (doc.elemCount < ANIM_ELEMS_MAX)
        {
            UndoPush();
            // add a slot of the right kind, then overwrite it with the shelved
            // element (a plain value, so tracks and keys come along).
            AnimElem *dst = AnimDocAddElem(&doc, library.entries[reqInsert].elem.kind);
            if (dst)
            {
                *dst = library.entries[reqInsert].elem;
                AnimDocUniquifyElemName(&doc, doc.elemCount - 1);
                selElem = doc.elemCount - 1;
                for (int k = 0; k < ANIM_ELEMS_MAX; k++) multiSel[k] = false;
                ClearKeySelection();
                ClampSelection();
            }
        }
    }
    else if (reqRename >= 0)
    {
        AudioPlayButton();
        libTargetIdx = reqRename;
        TextCopy(nameBuf, library.entries[reqRename].name);
        edNameBuf = true; prompt = PROMPT_LIB_RENAME;
    }
    else if (reqDelete >= 0)
    {
        AudioPlayButton();
        AnimLibraryRemove(&library, reqDelete);
        AnimLibrarySave(&library, LIB_PATH);
    }
}

// ---------------------------------------------------------------------------
//  Signals list (left panel, below the elements). Deliberately minimal: create,
//  open, fire, delete. Everything about a signal's contents - its targets and
//  their keyframes - lives in the modal, so this list stays readable.
//  Returns the content height (for the panel's scrollbar maths).
// ---------------------------------------------------------------------------
static float DrawSignalList(float x, float y, float w)
{
    float y0 = y, rh = 26.0f, gap = 4.0f;
    GuiLabel((Rectangle){ x, y, w, 20 }, "SIGNALS"); y += 22;

    int pendingDel = -1;
    float bw2 = 22.0f;
    for (int i = 0; i < doc.signalCount; i++)
    {
        AnimSignal *sg = &doc.signals[i];
        Rectangle openR = { x, y, w - 2*bw2 - 4, rh };
        if (GuiButton(openR, TextFormat("%s  (%d)", sg->name, sg->targetCount)))
        { AudioPlayButton(); sigModalIdx = i; sigScroll = 0.0f; }
        if (i == sigModalIdx) DrawRectangleRec(openR, (Color){ 90, 140, 220, 90 });

        if (GuiButton((Rectangle){ x + w - 2*bw2 - 2, y, bw2, rh }, "#131#"))
        { AudioPlayButton(); SignalEmit(sg->name, NULL); }          // fire (play icon)
        if (GuiButton((Rectangle){ x + w - bw2, y, bw2, rh }, "#143#"))
        { AudioPlayButton(); pendingDel = i; }                // trash icon
        y += rh + gap;
    }

    if (doc.signalCount < ANIM_SIGNALS_MAX)
    {
        if (GuiButton((Rectangle){ x, y, w, rh }, "+signal"))
        {
            AudioPlayButton(); UndoPush();
            AnimSignal *sg = &doc.signals[doc.signalCount++];
            TextCopy(sg->name, TextFormat("sig%d", doc.signalCount));
            sg->length = 1.0f; sg->targetCount = 0;
            sigModalIdx = doc.signalCount - 1; sigScroll = 0.0f;
            ReRegisterSignals();
        }
        y += rh + gap;
    }

    // deferred: deleting mid-loop shifts the array under the next row's buttons
    if (pendingDel >= 0)
    {
        UndoPush();
        for (int m = pendingDel; m < doc.signalCount - 1; m++)
            doc.signals[m] = doc.signals[m+1];
        doc.signalCount--;
        if      (sigModalIdx == pendingDel) sigModalIdx = -1;   // its modal closes
        else if (sigModalIdx >  pendingDel) sigModalIdx--;
        if      (edSigIdx == pendingDel) edSigIdx = -1;
        else if (edSigIdx >  pendingDel) edSigIdx--;
        ReRegisterSignals();
    }

    return (y + 4) - y0;
}

// ---------------------------------------------------------------------------
//  Signal modal: everything about ONE signal - its length and its targets,
//  each an (element, property) pair with its own keyframes.
//
//  Two forms:
//    FULL    (editing)  the whole editor for the signal
//    SHRUNK  (playing)  a small bar with just Fire + Close, so the signal can
//                       be triggered at any point during playback to see how
//                       it blends from the live scene. It deliberately does
//                       NOT close on playback, unlike the PROMPT_* modals.
// ---------------------------------------------------------------------------
static void DrawSignalModal()
{
    if (sigModalIdx < 0 || sigModalIdx >= doc.signalCount) { sigModalIdx = -1; return; }
    AnimSignal *sg = &doc.signals[sigModalIdx];

    ScreenState *ss = ScreenStateGet();
    float W = (float)ss->width, H = (float)ss->height;
    bool playbackUi = playing || !AnimSignalPlayerDone(&preview);

    // --- shrunk form ------------------------------------------------------
    if (playbackUi)
    {
        float mw = 240, mh = 40;
        Rectangle m = { W - mw - 12, 52, mw, mh };      // out of the preview's way
        DrawRectangleRec(m, (Color){ 40, 42, 48, 235 });
        DrawRectangleLinesEx(m, 1.0f, (Color){ 90, 94, 104, 255 });
        GuiLabel((Rectangle){ m.x+10, m.y+10, mw-140, 20 }, sg->name);
        if (GuiButton((Rectangle){ m.x+mw-116, m.y+7, 52, 26 }, "Fire"))
        { AudioPlayButton(); SignalEmit(sg->name, NULL); }
        if (GuiButton((Rectangle){ m.x+mw-60, m.y+7, 52, 26 }, "Close"))
        { AudioPlayButton(); sigModalIdx = -1; }
        return;
    }

    // --- full form --------------------------------------------------------
    float mw = 520, mh = 420;
    Rectangle m = { (W-mw)/2, (H-mh)/2, mw, mh };
    DrawRectangle(0, 0, (int)W, (int)H, (Color){ 0, 0, 0, 120 });
    DrawRectangleRec(m, (Color){ 40, 42, 48, 255 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 90, 94, 104, 255 });

    float rh = 24.0f, gap = 6.0f;
    float x = m.x + 14, w = mw - 28, y = m.y + 12;

    // One of this modal's own dropdowns is expanded over these rows: lock them
    // so a click lands on the overlay list only. Without this the row widget
    // underneath the expanded list consumes the click too (picking a target's
    // element would fire whatever button sat beneath the list item).
    bool sigDropOpen = sigPropDrop != -1 || sigKeyEaseDrop >= 0;
    if (sigDropOpen) GuiLock();

    GuiLabel((Rectangle){ x, y, 60, rh }, "signal");
    if (GuiTextBox((Rectangle){ x+60, y, 150, rh }, sg->name, ANIM_NAME_MAX,
                   edSigIdx == sigModalIdx))
    {
        if (edSigIdx != sigModalIdx) { UndoPush(); edSigIdx = sigModalIdx; }
        else { edSigIdx = -1; ReRegisterSignals(); }   // name change = rebind
    }
    GuiLabel((Rectangle){ x+222, y, 46, rh }, "length");
    if (EditSlider((Rectangle){ x+270, y, w-270-52, rh }, "", &sg->length, 0.0f, 10.0f))
    { /* normalized keys rescale automatically - nothing else to do */ }
    y += rh + 4;
    GuiLabel((Rectangle){ x, y, w-220, 18 },
             sg->length <= 0.0f ? "0.00 = instant snap to the final keys"
                                : "keys are fractions of the length (0..1)");

    // Terminal: an authored END marker. It changes nothing in this preview (the
    // playhead is the user's here) - it tells the in-game player (anim_stage.h)
    // to stop the animation once this signal has run its full length, so a loop
    // winds down through the transition instead of being cut off.
    // (snapshot BEFORE writing the new value, like the shape-kind toggle above)
    bool terminal = sg->terminal;
    GuiCheckBox((Rectangle){ x+w-206, y, 16, 16 }, "terminal (ends playback)",
                &terminal);
    if (terminal != sg->terminal)
    {
        AudioPlayButton();
        UndoPush();
        sg->terminal = terminal;
    }
    y += 20;
    GuiLine((Rectangle){ x, y, w, 8 }, "targets"); y += 12;

    // --- scrolling target list -------------------------------------------
    Rectangle list = { x, y, w, m.y + mh - 48 - y };
    bool ctrlDown = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (!ctrlDown && CheckCollisionPointRec(GetMousePosition(), list))
        sigScroll += GetMouseWheelMove() * 24.0f;

    int pendingTgtDel = -1, pendingKeyDel = -1, pendingKeyTgt = -1;

    // Same trap as PanelScroll: scissor hides the rows scrolled past the top of
    // `list` but leaves them clickable, so hitting the signal name/length row
    // above also fired whatever row was hidden under the cursor.
    bool inList = CheckCollisionPointRec(GetMousePosition(), list);
    if (!inList && !sigDropOpen) GuiLock();

    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    float ly = list.y + sigScroll;

    // The list is driven by the DOCUMENT's elements, not by the signal's
    // targets: every element is always shown, and its targets are the tracks
    // nested under it. Adding a track is therefore a per-element action and
    // the element itself never has to be picked from a dropdown.
    for (int e = 0; e < doc.elemCount; e++)
    {
        // element header: name + a track adder scoped to this element
        DrawRectangleRec((Rectangle){ list.x, ly, list.width, rh },
                         (Color){ 52, 55, 62, 255 });
        GuiLabel((Rectangle){ list.x+6, ly, 180, rh }, doc.elems[e].name);

        // Show the adder greyed rather than hidden when the target pool is
        // full: a missing button read as "this element can't have tracks".
        bool sigFull = sg->targetCount >= ANIM_SIG_TARGETS_MAX;
        if (sigFull) GuiSetState(STATE_DISABLED);
        bool addHit = GuiButton((Rectangle){ list.x+list.width-86, ly, 82, rh },
                                sigFull ? "full" : "+ track");
        if (sigFull) GuiSetState(STATE_NORMAL);
        if (!sigFull && addHit)
        {
            // sigPropDrop identifies a target row; for the adder there is no
            // row yet, so it is encoded as a negative element ref (-e-2) and
            // the overlay creates the target once a property is chosen.
            // -e-2, not -e-1: -1 is the "nothing open" sentinel, and element 0
            // would collide with it (opening any other dropdown then resolved
            // as "adder on element 0").
            AudioPlayButton();
            sigPropDrop = (sigPropDrop == -e-2) ? -1 : -e-2;
            sigKeyEaseDrop = -1;
        }
        ly += rh + 2;

        for (int t = 0; t < sg->targetCount; t++)
        {
        AnimSigTarget *tg = &sg->targets[t];
        if (tg->elemIdx != e) continue;      // only this element's tracks

        // property picker (custom overlay dropdown, drawn later)
        if (GuiButton((Rectangle){ list.x+14, ly, 130, rh },
                      TextFormat("%s  v", AnimPropName(tg->prop))))
        { AudioPlayButton(); sigPropDrop = (sigPropDrop == t) ? -1 : t; }

        if (GuiButton((Rectangle){ list.x+152, ly, 46, rh }, "+key"))
        {
            AudioPlayButton();
            if (tg->keyCount < ANIM_SIG_KEYS_MAX)
            {
                UndoPush();
                // Seed u from the last one the user set on ANY signal key, not
                // 1.0: the common case is placing the same beat across several
                // tracks, and 1.0 forced a retype every time.
                AnimKey *k = &tg->keys[tg->keyCount++];
                k->t = sigLastU; k->ease = ANIM_EASE_SINE_OUT;
                if (tg->elemIdx >= 0 && tg->elemIdx < doc.elemCount)
                {
                    const AnimElem *el = &doc.elems[tg->elemIdx];
                    k->value = AnimElemProp(el, tg->prop, playhead);
                    k->cval  = AnimElemColorProp(el, tg->prop, playhead);
                }
                else { k->value = 0.0f; k->cval = (Color){0,0,0,255}; }
            }
        }
        if (GuiButton((Rectangle){ list.x+list.width-24, ly, 24, rh }, "#143#"))
        { AudioPlayButton(); pendingTgtDel = t; }
        ly += rh + 2;

        // key rows: u / value / ease
        for (int k = 0; k < tg->keyCount; k++)
        {
            AnimKey *kk = &tg->keys[k];
            GuiLabel((Rectangle){ list.x+14, ly, 16, 20 }, "u");
            float u = kk->t;
            if (EditSlider((Rectangle){ list.x+30, ly, 96, 20 }, "", &u, 0.0f, 1.0f))
            { kk->t = u; sigLastU = u; }    // remember for the next +key

            if (AnimPropIsColor(tg->prop))
            {
                float cr=kk->cval.r, cg=kk->cval.g, cb=kk->cval.b;
                bool ch=false;
                if (EditSlider((Rectangle){ list.x+180, ly, 60, 20 }, "R", &cr, 0,255)) ch=true;
                if (EditSlider((Rectangle){ list.x+250, ly, 60, 20 }, "G", &cg, 0,255)) ch=true;
                if (EditSlider((Rectangle){ list.x+320, ly, 60, 20 }, "B", &cb, 0,255)) ch=true;
                if (ch) kk->cval = (Color){ (unsigned char)cr,(unsigned char)cg,
                                            (unsigned char)cb, 255 };
                DrawSwatch((Rectangle){ list.x+158, ly+2, 16, 16 },
                           (Color){ kk->cval.r, kk->cval.g, kk->cval.b, 255 });
            }
            else
            {
                float v = kk->value;
                if (EditSlider((Rectangle){ list.x+180, ly, 150, 20 }, "", &v,
                               AnimPropMin(tg->prop), AnimPropMax(tg->prop)))
                    kk->value = v;
            }

            if (GuiButton((Rectangle){ list.x+list.width-104, ly, 76, 20 },
                          AnimEaseName(kk->ease)))
            {
                AudioPlayButton();
                int packed = t*256 + k;
                sigKeyEaseDrop = (sigKeyEaseDrop == packed) ? -1 : packed;
            }
            if (GuiButton((Rectangle){ list.x+list.width-24, ly, 24, 20 }, "x"))
            { AudioPlayButton(); pendingKeyDel = k; pendingKeyTgt = t; }
            ly += 22;
        }
        ly += 4;
        }       // targets of this element
        ly += gap;
    }           // elements
    EndScissorMode();
    if (!inList && !sigDropOpen) GuiUnlock();   // footer/header stay live

    // measured content height -> clamp the scroll
    float contentH = (ly - (list.y + sigScroll));
    float maxScroll = contentH - list.height;
    if (maxScroll < 0) maxScroll = 0;
    if (sigScroll < -maxScroll) sigScroll = -maxScroll;
    if (sigScroll > 0) sigScroll = 0;

    // deferred deletions (same reason as everywhere else: mid-draw array
    // shifts re-fire the next row's button on the same rect this frame)
    if (pendingTgtDel >= 0)
    {
        UndoPush();
        for (int mI = pendingTgtDel; mI < sg->targetCount - 1; mI++)
            sg->targets[mI] = sg->targets[mI+1];
        sg->targetCount--;
        sigPropDrop = -1; sigKeyEaseDrop = -1;
    }
    else if (pendingKeyDel >= 0)
    {
        UndoPush();
        AnimSigTarget *tg = &sg->targets[pendingKeyTgt];
        for (int mI = pendingKeyDel; mI < tg->keyCount - 1; mI++)
            tg->keys[mI] = tg->keys[mI+1];
        tg->keyCount--;
        sigKeyEaseDrop = -1;
    }

    // --- footer -----------------------------------------------------------
    float bh = 28, by = m.y + mh - bh - 12;
    if (GuiButton((Rectangle){ x, by, 70, bh }, "Fire"))
    { AudioPlayButton(); SignalEmit(sg->name, NULL); }
    GuiLabel((Rectangle){ x+80, by, mw-200, bh }, "fires from the CURRENT pose");
    if (GuiButton((Rectangle){ m.x+mw-84, by, 70, bh }, "Close"))
    { AudioPlayButton(); sigModalIdx = -1; edSigIdx = -1;
      sigPropDrop = -1; sigKeyEaseDrop = -1; }

    if (sigDropOpen) GuiUnlock();   // the overlay list itself draws unlocked
}

// The signal modal's own dropdown overlays (element / property / key ease).
// Drawn after the modal so they sit on top of it, mirroring how
// DrawDropdownOverlays relates to the main inspector.
static void DrawSignalModalOverlays()
{
    if (sigModalIdx < 0 || sigModalIdx >= doc.signalCount) return;
    if (playing || !AnimSignalPlayerDone(&preview)) return;    // shrunk: no lists
    if (sigPropDrop == -1 && sigKeyEaseDrop < 0) return;

    AnimSignal *sg = &doc.signals[sigModalIdx];
    ScreenState *ss = ScreenStateGet();
    float H = (float)ss->height;

    // Recompute the row geometry the modal used (same constants).
    float W = (float)ss->width;
    float mw = 520, mh = 420;
    Rectangle m = { (W-mw)/2, (H-mh)/2, mw, mh };
    float rh = 24.0f;
    float x = m.x + 14, w = mw - 28;
    float y = m.y + 12 + rh + 4 + 20 + 12;
    Rectangle list = { x, y, w, m.y + mh - 48 - y };

    int   count = 0;
    Rectangle hdr = {0};
    int   mode = 0;    // 2 = property (existing row), 3 = ease, 4 = new track
    int   tIdx = -1, kIdx = -1, addElem = -1;

    // walk the rows to find the open dropdown's header rect. This MUST mirror
    // the element-grouped layout DrawSignalModal lays out above.
    float ly = list.y + sigScroll;
    for (int e = 0; e < doc.elemCount && !mode; e++)
    {
        // element header row, carrying the "+ track" adder
        if (sigPropDrop == -e-2)
        { mode = 4; addElem = e;
          hdr = (Rectangle){ list.x+list.width-86, ly, 82, rh };
          count = AnimPropCountFor(doc.elems[e].kind); }
        ly += rh + 2;

        for (int t = 0; t < sg->targetCount && !mode; t++)
        {
            AnimSigTarget *tg = &sg->targets[t];
            if (tg->elemIdx != e) continue;

            if (sigPropDrop == t)
            { mode = 2; tIdx = t; hdr = (Rectangle){ list.x+14, ly, 130, rh };
              count = AnimPropCountFor(doc.elems[e].kind); }
            ly += rh + 2;
            for (int k = 0; k < tg->keyCount && !mode; k++)
            {
                if (sigKeyEaseDrop == t*256 + k)
                { mode = 3; tIdx = t; kIdx = k;
                  hdr = (Rectangle){ list.x+list.width-104, ly, 76, 20 };
                  count = AnimEaseCount(); }
                ly += 22;
            }
            if (!mode) ly += 4;
        }
        if (!mode) ly += 6.0f;      // == `gap` in DrawSignalModal
    }
    if (!mode || count <= 0) return;

    float ih = 20.0f, listH = ih * count;
    float ly2 = (hdr.y + hdr.height + listH <= H - 4.0f)
              ? hdr.y + hdr.height : hdr.y - listH;
    if (ly2 < 4.0f) ly2 = 4.0f;
    Rectangle bg = { hdr.x, ly2, hdr.width, listH };
    DrawRectangleRec(bg, (Color){ 32, 34, 40, 255 });
    DrawRectangleLinesEx(bg, 1.0f, (Color){ 70, 74, 84, 255 });

    int picked = -1;
    for (int i = 0; i < count; i++)
    {
        Rectangle rr = { bg.x, bg.y + i*ih, bg.width, ih };
        const char *nm = (mode == 2) ? AnimPropName(AnimPropAt(
                             doc.elems[sg->targets[tIdx].elemIdx].kind, i))
                       : (mode == 4) ? AnimPropName(AnimPropAt(doc.elems[addElem].kind, i))
                       : AnimEaseName(i);
        if (GuiButton(rr, nm)) picked = i;
    }

    if (picked >= 0)
    {
        AudioPlayButton(); UndoPush();
        if (mode == 2)
        {
            AnimSigTarget *tg = &sg->targets[tIdx];
            int np = AnimPropAt(doc.elems[tg->elemIdx].kind, picked);
            // scalar <-> colour keys are not interchangeable: reset on a switch
            if (AnimPropIsColor(np) != AnimPropIsColor(tg->prop)) tg->keyCount = 0;
            tg->prop = np;
            sigPropDrop = -1;
        }
        else if (mode == 4)
        {
            // adder: create the target on this element with the chosen property
            int np = AnimPropAt(doc.elems[addElem].kind, picked);
            if (sg->targetCount < ANIM_SIG_TARGETS_MAX)
            {
                AnimSigTarget *tg = &sg->targets[sg->targetCount++];
                tg->elemIdx = addElem; tg->prop = np; tg->keyCount = 0;
            }
            sigPropDrop = -1;
        }
        else { sg->targets[tIdx].keys[kIdx].ease = picked; sigKeyEaseDrop = -1; }
    }
    else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
             !CheckCollisionPointRec(GetMousePosition(), bg) &&
             !CheckCollisionPointRec(GetMousePosition(), hdr))
    { sigPropDrop = -1; sigKeyEaseDrop = -1; }
}

// A filled diamond. DrawTriangle culls back faces: vertices must be counter-
// clockwise ON SCREEN (y-down), i.e. top -> left -> right; the old clockwise
// order made every keyframe diamond invisible.
static void DrawDiamond(float cx, float cy, float r, Color c)
{
    DrawTriangle((Vector2){cx,cy-r},(Vector2){cx-r,cy},(Vector2){cx+r,cy}, c);
    DrawTriangle((Vector2){cx-r,cy},(Vector2){cx,cy+r},(Vector2){cx+r,cy}, c);
}

// A trim marker handle: apex ON the timeline at (cx,cy), body hanging away
// from it. `down` = apex points down (the intro handle, sitting on the top
// edge); otherwise it points up (the outro handle, on the bottom edge).
// Same counter-clockwise winding rule as DrawDiamond.
static void DrawMarkerTriangle(float cx, float cy, float r, bool down, Color c)
{
    if (down) DrawTriangle((Vector2){cx-r,cy-r},(Vector2){cx,cy},(Vector2){cx+r,cy-r}, c);
    else      DrawTriangle((Vector2){cx,cy},(Vector2){cx-r,cy+r},(Vector2){cx+r,cy+r}, c);
}

// Dotted vertical line (raylib has no dashed-line primitive).
static void DrawDottedV(float x, float y0, float y1, Color c)
{
    for (float yy = y0; yy < y1; yy += 8.0f)
    {
        float seg = (yy + 4.0f < y1) ? yy + 4.0f : y1;
        DrawLine((int)x, (int)yy, (int)x, (int)seg, c);
    }
}

// ---------------------------------------------------------------------------
//  Timeline scrubber: playhead + keyframe diamonds for the selected element.
//  Below `thin` height (playback mode) only the ticks + playhead remain; the
//  whole bar stays clickable so a click pauses and scrubs.
// ---------------------------------------------------------------------------
static void DrawTimeline(float x, float y, float w, float h)
{
    bool thin = h < 60.0f;
    Rectangle bar = { x, y, w, h };
    DrawRectangleRec(bar, (Color){ 24, 26, 30, 255 });
    DrawRectangleLinesEx(bar, 1.0f, (Color){ 70, 74, 84, 255 });

    float dur = doc.duration > 0 ? doc.duration : 1.0f;
    // left gutter holds the per-track property labels so t=0 diamonds are not
    // buried under the text.
    float gutter = 56.0f, padR = 8.0f;
    float trackLeft = x + gutter, trackW = w - gutter - padR;

    // time->x and x->time.
    #define T2X(t) (trackLeft + (trackW) * ((t)/dur))
    #define X2T(px) (((px) - trackLeft)/trackW * dur)

    // second grid ticks
    for (float s = 0; s <= dur + 0.001f; s += 0.5f)
    {
        float tx = T2X(s);
        DrawLine((int)tx, (int)y+2, (int)tx, (int)(y+h-16), (Color){ 50,54,62,255 });
        if (fmodf(s,1.0f) < 0.01f)
            DrawText(TextFormat("%.0f", s), (int)tx+2, (int)(y+h-14), 10, (Color){110,116,128,255});
    }

    // Raw-mouse input is suppressed while the ease dropdown is open (raygui's
    // GuiLock only covers gui widgets, not this hand-drawn timeline).
    Vector2 mouse = GetMousePosition();
    bool press  = !keyEaseDropOpen && addTrackDrop < 0 &&
                  IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    bool keyHit = false;

    // --- intro / outro trim: shaded dead zones behind everything else -------
    float inEnd = AnimDocIntroEnd(&doc), outStart = AnimDocOutroStart(&doc);
    float inX = T2X(inEnd), outX = T2X(outStart);
    if (inEnd > 0.0f)
        DrawRectangleRec((Rectangle){ trackLeft, y+1, inX-trackLeft, h-2 },
                         (Color){ 90, 140, 200, 26 });      // intro: skipped on loop
    if (outStart < dur)
        DrawRectangleRec((Rectangle){ outX, y+1, x+w-padR-outX, h-2 },
                         (Color){ 0, 0, 0, 120 });          // outro: trimmed away

    if (!thin && selElem >= 0 && selElem < doc.elemCount)
    {
        AnimElem *e = &doc.elems[selElem];
        if (e->trackCount == 0)
            DrawText("(no tracks - add one in the inspector to key frames)",
                     (int)(x + w*0.5f - 130), (int)(y + h*0.5f - 5), 10,
                     (Color){ 110, 116, 128, 255 });
        float rowH = (h - 24) / (float)(e->trackCount > 0 ? e->trackCount : 1);
        for (int j = 0; j < e->trackCount; j++)
        {
            AnimTrack *tr = &e->tracks[j];
            float ry = y + 4 + j*rowH + rowH*0.5f;
            // alternating lane backgrounds + a baseline so tracks read as rows
            if (j & 1)
                DrawRectangleRec((Rectangle){ x+1, y+4 + j*rowH, w-2, rowH },
                                 (Color){ 255, 255, 255, 6 });
            DrawLine((int)trackLeft, (int)ry, (int)(x+w-padR), (int)ry,
                     (Color){ 45, 48, 56, 255 });
            DrawText(AnimPropName(tr->prop), (int)x+2, (int)ry-5, 10, (Color){130,136,148,255});
            for (int k = 0; k < tr->keyCount; k++)
            {
                float kx = T2X(tr->keys[k].t);
                // generous hit box - diamonds are small targets otherwise.
                Rectangle hit = { kx-10, ry-10, 20, 20 };
                bool hot = CheckCollisionPointRec(mouse, hit);
                bool sel = (selKeyElem == selElem && selKeyTrack == j && selKeyIdx == k);
                float r = hot ? 9.0f : 7.0f;
                // outline ring underneath: amber = selected, white = hover
                Color ring = sel ? (Color){255,210,90,255}
                           : hot ? (Color){255,255,255,255} : (Color){15,16,20,255};
                Color fill = AnimPropIsColor(tr->prop)
                           ? (Color){ tr->keys[k].cval.r, tr->keys[k].cval.g,
                                      tr->keys[k].cval.b, 255 }
                           : sel ? (Color){255,255,255,255} : (Color){120,180,240,255};
                DrawDiamond(kx, ry, r + 2.0f, ring);
                DrawDiamond(kx, ry, r, fill);

                if (hot && press)
                {
                    UndoPush();                       // once per drag gesture
                    dragKeyElem = selElem; dragKeyTrack = j; dragKeyIdx = k;
                    SelectKey(selElem, j, k);
                    scrollToSelKey = true;            // reveal it in the inspector
                    keyHit = true;
                }
            }
        }
    }
    else if (!thin)
        DrawText("(select an element to see its keys)",
                 (int)(x + w*0.5f - 90), (int)(y + h*0.5f - 5), 10,
                 (Color){ 110, 116, 128, 255 });

    // playhead line + grab handle.
    float phx = T2X(playhead);
    DrawLine((int)phx, (int)y, (int)phx, (int)(y+h), (Color){255,90,90,255});
    Rectangle phHandle = { phx-6, y-2, 12, 10 };
    DrawRectangleRec(phHandle, (Color){255,90,90,255});

    // --- trim markers: dotted lines + grab triangles ------------------------
    // Drawn over the lanes so they read as boundaries, and hit-tested BEFORE
    // the bar scrub below so grabbing a handle never moves the playhead.
    Color introCol = (Color){ 120, 190, 255, 255 };
    Color outroCol = (Color){ 255, 160, 90, 255 };
    if (inEnd > 0.0f)    DrawDottedV(inX,  y+2, y+h-2, introCol);
    if (outStart < dur)  DrawDottedV(outX, y+2, y+h-2, outroCol);

    Rectangle introHit = { inX-9,  y,      18, 14 };
    Rectangle outroHit = { outX-9, y+h-14, 18, 14 };
    bool introHot = CheckCollisionPointRec(mouse, introHit);
    bool outroHot = CheckCollisionPointRec(mouse, outroHit);
    DrawMarkerTriangle(inX,  y+1.0f,   introHot ? 9.0f : 7.0f, true,  introCol);
    DrawMarkerTriangle(outX, y+h-1.0f, outroHot ? 9.0f : 7.0f, false, outroCol);

    if (press && (introHot || outroHot))
    {
        UndoPush();                                 // once per drag gesture
        if (introHot) dragIntro = true; else dragOutro = true;
        keyHit = true;                              // suppress the scrub below
    }
    if ((dragIntro || dragOutro) && IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        float nt = X2T(mouse.x);
        if (nt < 0) nt = 0; if (nt > dur) nt = dur;
        // intro and outro may not cross: each clamps against the other.
        if (dragIntro) doc.introEnd   = (nt > outStart) ? outStart : nt;
        else           doc.outroStart = (nt < inEnd)    ? inEnd    : nt;
    }

    // --- input -------------------------------------------------------------
    // pressing anywhere on the bar that is NOT a key scrubs the playhead there
    // (and deselects); no tiny grab strip to aim for.
    if (press && !keyHit &&
        CheckCollisionPointRec(mouse, (Rectangle){ x, y-4, w, h+4 }))
    {
        dragPlayhead = true;
        ClearKeySelection();
    }

    if (dragKeyElem >= 0 && IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        // continuous drag: the key keeps following the mouse; SetKeyTime keeps
        // the track sorted and hands back the key's new index every frame.
        AnimTrack *tr = &doc.elems[dragKeyElem].tracks[dragKeyTrack];
        float nt = X2T(mouse.x);
        if (nt < 0) nt = 0; if (nt > dur) nt = dur;
        dragKeyIdx = AnimTrackSetKeyTime(tr, dragKeyIdx, nt);
        selKeyIdx  = dragKeyIdx;
        TextCopy(keyTimeBuf, TextFormat("%.2f", nt));
    }
    else if (dragPlayhead && IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        float nt = X2T(mouse.x);
        if (nt < 0) nt = 0; if (nt > dur) nt = dur;
        playhead = nt; playing = false; preview.playing = false;
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
    { dragPlayhead = false; dragKeyElem = -1; dragKeyTrack = -1; dragKeyIdx = -1;
      dragIntro = false; dragOutro = false; }

    #undef T2X
    #undef X2T
}

// ---------------------------------------------------------------------------
//  Toolbar: New / Load / Save / Undo / Redo / play controls / Back.
// ---------------------------------------------------------------------------
// Width a raygui label/button needs to show `text` without squeezing: the
// glyphs at the CURRENT style text size (which follows the global gui scale)
// plus the style's own left/right text padding and a little slack.
static float TextW(const char *text, float extra)
{
    float fs = (float)GuiGetStyle(DEFAULT, TEXT_SIZE);
    float sp = fs/(float)GuiGetFont().baseSize;      // raygui's glyph spacing
    return MeasureTextEx(GuiGetFont(), text, fs, sp).x + extra;
}

// Draws the bar and returns the HEIGHT it used, so the panels below can sit
// under it: at larger gui scales the controls no longer fit on one row and the
// clock group wraps to a second one.
static float DrawToolbar(float x, float y, float w)
{
    // Everything here scales with the global gui-scale setting: raygui's
    // TEXT_SIZE is set from it by whichever state drew last, so widget sizes
    // are derived from the font rather than hardcoded, or big text overflows
    // small boxes. `s` is that size relative to the 10px base font.
    float fs = (float)GuiGetStyle(DEFAULT, TEXT_SIZE);
    float s  = fs / 10.0f;

    // gap = space between related buttons, grp = between groups. The bar is
    // laid out left to right by advancing gx; keep the two spacings distinct
    // so the groups (file / history / playback / clock) still read apart.
    float rh = 26*s, gx = x, gap = 8*s, grp = 20*s;
    float bw = TextW("Library", 20*s);      // widest of the plain buttons
    // Undo/Redo are short words: sizing them off "Library" leaves a lot of dead
    // space, so give them their own (narrower) width.
    float uw = TextW("Undo", 16*s);
    // EditSlider parks its value label just right of the bar, so any slider
    // must reserve that much room before the next widget or the two numbers
    // run into each other.
    float valW = (float)GuiGetStyle(SLIDER, TEXT_PADDING) + 44.0f;
    float rowY = y;                         // first row (Back stays pinned here)

    // animation switcher: header shows current anim (or "*unsaved") + dropdown.
    // The list itself is drawn as an overlay in DrawDropdownOverlays().
    const char *label = (animCurrent >= 0) ? animList[animCurrent] : "*unsaved";
    float dw = TextW(TextFormat("%s  v", label), 24*s);
    Rectangle switchR = { gx, y, dw, rh };
    if (GuiButton(switchR, TextFormat("%s  v", label)))
    { AudioPlayButton(); animSwitchOpen = !animSwitchOpen;
      keyEaseDropOpen = false; addTrackDrop = -1; }
    animSwitchRect = switchR; animSwitchVisible = true;
    gx += dw+gap;
    if (GuiButton((Rectangle){ gx, y, bw, rh }, "Save"))
    { AudioPlayButton(); SaveCurrent(); }
    gx += bw+gap;
    if (GuiButton((Rectangle){ gx, y, bw, rh }, "Library"))
    { AudioPlayButton(); libScroll = 0.0f; prompt = PROMPT_LIBRARY; }
    gx += bw+grp;
    if (GuiButton((Rectangle){ gx, y, uw, rh }, "Undo")) { AudioPlayButton(); UndoApply(-1); }
    gx += uw+gap;
    if (GuiButton((Rectangle){ gx, y, uw, rh }, "Redo")) { AudioPlayButton(); UndoApply(+1); }
    gx += uw+grp;
    if (GuiButton((Rectangle){ gx, y, bw, rh }, playing ? "Pause" : "Play"))
    { AudioPlayButton(); playing = !playing; preview.playing = false; }
    gx += bw+gap;
    // checkboxes: the box is square (row height) and raygui writes the label to
    // its RIGHT, so each advance is box + measured label + a gap.
    float cb = 18*s;
    GuiCheckBox((Rectangle){ gx, y+(rh-cb)*0.5f, cb, cb }, "loop", &loopPlay);
    gx += cb + TextW("loop", 8*s) + gap;
    GuiCheckBox((Rectangle){ gx, y+(rh-cb)*0.5f, cb, cb }, "autokey", &autoKey);
    // TextW's `extra` already pads past the glyphs, so a full group gap here
    // reads as a gaping hole - the plain gap is enough separation.
    gx += cb + TextW("autokey", 4*s) + gap;

    // The clock group (dur slider + length readout) is the widest block, and
    // "Back" is pinned to the right edge. If they would collide, wrap the group
    // onto a second row instead of letting it run under the button.
    float dlw = TextW("dur", 6*s);
    float clockW = dlw + gap + 120*s + valW + grp + TextW("60.00 (60.00)", 10*s);
    float rows = 1.0f;
    if (gx + clockW > x + w - bw - grp)
    { gx = x; y += rh + gap; rows = 2.0f; }

    // duration editor (never below the last keyframe - keys must stay on clock)
    GuiLabel((Rectangle){ gx, y, dlw, rh }, "dur"); gx += dlw + gap;
    if (EditSlider((Rectangle){ gx, y, 120*s, rh }, "", &doc.duration, 0.2f, 60.0f))
    {
        float minDur = AnimDocMaxKeyTime(&doc);
        if (doc.duration < minDur) doc.duration = minDur;
        // shrinking the clock must not strand the trim markers past its end
        if (doc.outroStart > doc.duration) doc.outroStart = doc.duration;
        if (doc.introEnd   > doc.outroStart) doc.introEnd = doc.outroStart;
    }
    gx += 120*s + valW + grp;

    // Played length, with the FULL timeline length in brackets: the outro is
    // trimmed, so these differ whenever the outro marker has been moved in.
    // Measured at the WIDEST value it can ever show, so the label neither
    // clips nor jitters as the numbers change width while dragging.
    const char *lenTxt = TextFormat("%.2f (%.2f)", AnimDocPlayLen(&doc), doc.duration);
    float lenW = TextW("60.00 (60.00)", 10*s);
    GuiLabel((Rectangle){ gx, y, lenW, rh }, lenTxt);
    gx += lenW + grp;

    // Back to menu: pinned to the right edge of the FIRST row (y0), which the
    // wrap above deliberately keeps clear.
    if (GuiButton((Rectangle){ x + w - bw, rowY, bw, rh }, "Back"))
    { AudioPlayButton(); AppStateTransition(&app_state_main_menu); }

    return rows*rh + (rows-1.0f)*gap;
}

// Draw one scrolling panel: wheel over it scrolls (unless Ctrl is held, which
// is reserved for stepping the hovered slider), a scissor clips the content to
// the panel, and `draw` reports the content height for the scroll clamp.
// Returns that height so the caller can store it back.
static float PanelScroll(PanelView *v, Rectangle panel,
                         float (*draw)(float x, float y, float w))
{
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (!ctrl && CheckCollisionPointRec(GetMousePosition(), panel))
        v->scroll += GetMouseWheelMove() * 30.0f;

    float maxScroll = v->contentH - (panel.height - 16);
    if (maxScroll < 0) maxScroll = 0;
    if (v->scroll < -maxScroll) v->scroll = -maxScroll;
    if (v->scroll > 0) v->scroll = 0;

    // Scissor clips PIXELS, not hit-testing: a row scrolled out of the panel is
    // invisible but still live, so a click meant for whatever is drawn outside
    // (a header, another panel) also fired the hidden widget under the cursor.
    // Lock the gui whenever the mouse is outside this panel - the rows then
    // draw normally but refuse input, which is exactly the clipped region.
    bool wasLocked = guiLocked;
    bool inPanel   = CheckCollisionPointRec(GetMousePosition(), panel);
    if (!inPanel && !wasLocked) GuiLock();

    BeginScissorMode((int)panel.x+1, (int)panel.y+1,
                     (int)panel.width-2, (int)panel.height-2);
    float h = draw(panel.x+8, panel.y+8 + v->scroll, panel.width-16);
    EndScissorMode();

    if (!inPanel && !wasLocked) GuiUnlock();
    return h;
}

// ===========================================================================
//  Gui: assemble the panels (screen space).
// ===========================================================================
static void Gui()
{
    ScreenState *ss = ScreenStateGet();
    float W = (float)ss->width, H = (float)ss->height;

    float pad = 10.0f;
    float toolbarH;             // set by DrawToolbar below (it can wrap)
    float leftW = 220.0f;
    float rightW = 320.0f;
    float bottomH = 180.0f;
    float thinH = 26.0f;        // timeline strip height while playing

    // a slider drag gesture (one undo snapshot) ends when the button comes up.
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) sliderGestureOpen = false;

    // while a dropdown list or modal prompt is open every other widget is
    // locked; the overlay itself is drawn (unlocked) last so it sits on top.
    addTrackVisible = false;
    animSwitchVisible = false;
    // The signal modal locks the editor behind it too - EXCEPT in its shrunk
    // playback form, which is a small bar the user is meant to work around.
    bool sigModalBlocking = sigModalIdx >= 0 &&
                            !(playing || !AnimSignalPlayerDone(&preview));
    guiLocked = keyEaseDropOpen || addTrackDrop >= 0 || animSwitchOpen ||
                prompt != PROMPT_NONE || sigModalBlocking;
    if (guiLocked) GuiLock();

    // the bar reports its own height (it wraps to two rows at large gui scales)
    toolbarH = DrawToolbar(pad, pad, W - 2*pad) + 8.0f;

    // playback slide: panels move off the sides, the bottom block drops until
    // only a thin timeline strip remains. Skip the panels entirely once hidden
    // so off-screen widgets can't hit-test or capture input.
    float k = AnimEaseApply(ANIM_EASE_SINE_INOUT, panelAnim);
    bool uiHidden = k >= 0.999f;

    // left + right panels flank the central preview (which Draw() already drew).
    Rectangle leftPanel = { pad - (leftW + 2*pad)*k, toolbarH+pad,
                            leftW, H - toolbarH - bottomH - 3*pad };
    Rectangle rightPanel = { W - rightW - pad + (rightW + 2*pad)*k, toolbarH+pad,
                             rightW, H - toolbarH - bottomH - 3*pad };
    // the left column is split: ELEMENTS on top, SIGNALS below. Both scroll.
    float leftGap = 6.0f;
    float leftElemH = (leftPanel.height - leftGap) * 0.60f;
    Rectangle elemPanel = { leftPanel.x, leftPanel.y, leftPanel.width, leftElemH };
    Rectangle sigPanel  = { leftPanel.x, leftPanel.y + leftElemH + leftGap,
                            leftPanel.width, leftPanel.height - leftElemH - leftGap };

    if (!uiHidden)
    {
        GuiPanel(elemPanel, NULL);
        GuiPanel(sigPanel, NULL);
        GuiPanel(rightPanel, NULL);

        // Ctrl+wheel is reserved for stepping the hovered slider, not scrolling.
        elemView.contentH = PanelScroll(&elemView, elemPanel, DrawElementList);
        sigView.contentH  = PanelScroll(&sigView,  sigPanel,  DrawSignalList);

        inspPanelRect = rightPanel;                 // key-reveal scrolling uses it
        inspView.contentH = PanelScroll(&inspView, rightPanel, DrawInspector);
    }
    else keyEaseVisible = false;    // inspector skipped: no ease dropdown

    // bottom: the timeline alone now (signals moved to the left panel), so it
    // gets the whole block. It slides down by k until only the thin strip shows.
    float by = H - bottomH - pad + k * (bottomH - thinH);
    float tlH = H - pad - by;
    if (tlH < thinH) { tlH = thinH; by = H - pad - thinH; }
    DrawTimeline(pad, by, W - 2*pad, tlH);

    GuiUnlock();
    DrawDropdownOverlays();
    DrawAnimSwitchOverlay();    // animation list (over widgets, under the modal)
    DrawLibraryModal();         // element shelf (its own scrolling list)
    DrawSignalModal();          // one signal's targets/keys (survives playback)
    DrawSignalModalOverlays();  // its element/prop/ease lists, over the modal
    DrawPromptModal();          // topmost: save-before-switch / delete / new / lib names
}
