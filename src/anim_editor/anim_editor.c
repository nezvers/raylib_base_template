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
#include "../anim/signal.h"
#include "../anim/anim_signal.h"
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>          // remove() for deleting anim files off disk
#include <string.h>         // strlen/strncat for group-key value summaries
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

// Which INSTANCE the preview stands in for. In game the same document is often
// played several times at once, each with its own sequence number, and a signal
// fans those copies apart by it (the "--sequence--" section, AnimSignal.usesSeq).
// The editor plays exactly one copy, so the number it pretends to be is a control
// - otherwise a fan-out could only be checked by launching the game.
static int previewSeq = 0;

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
static float sigLastU = 1.0f;       // last u the user set on ANY signal key; new
                                    // keys seed from it so placing a beat across
                                    // several tracks doesn't mean retyping it

// The selected signal GROUP key: (element, group, u). Targets are authored in
// property groups exactly like timeline tracks, so the selection names a group
// key rather than one target's key. sigSelElem < 0 = nothing selected.
static int   sigSelElem = -1, sigSelGroup = -1;
static float sigSelU = 0.0f;

// Selected key in the "--params--" (Mouse Position) section: which binding
// (element + slot) and which of its keys (by u). sigPosSelElem < 0 = none.
static int   sigPosSelElem = -1, sigPosSelSlot = 0;
static float sigPosSelU = 0.0f;
// Selected key in the "--sequence--" section (by u). < 0 = none.
static float sigSeqSelU = -1.0f;

// The ONE dropdown the signal modal can have open, recorded while drawing so
// the overlay pass needs no layout maths (same pattern as addTrackRect /
// keyEaseRect for the main inspector).
enum { SIGDROP_NONE = 0, SIGDROP_ADD, SIGDROP_EASE };
static int       sigDropMode = SIGDROP_NONE;
static int       sigDropElem = -1;      // SIGDROP_ADD: element being added to
static Rectangle sigDropHdr;            // header rect the list hangs off

static void SigCloseDrops(void)  { sigDropMode = SIGDROP_NONE; sigDropElem = -1; }
static void SigClearKeySel(void) { sigSelElem = -1; sigSelGroup = -1; sigSelU = 0.0f;
                                   sigPosSelElem = -1; sigSeqSelU = -1.0f; }

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
    int   alphaMode;    // background opacity: 0 opaque, 1 semi, 2 faint
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

// Shift-fine drag: which slider owns the current mouse press, and whether it
// has entered sticky fine mode (relative nudging). Cleared on release in Gui().
static Rectangle finePressRect = {0};
static bool      finePressActive = false;
static bool      fineSticky = false;

// timeline background opacity, toggled by its own glyph (mirrors PanelView.alphaMode)
static int timelineAlphaMode = 0;

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
// a timeline drag now moves a whole GROUP key (all its member keys) by time.
static int  dragKeyGroup = -1;
static float dragKeyTime = 0.0f;
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
        SigCloseDrops(); SigClearKeySel();
    }

    // the selected signal group key can vanish under an undo/redo (its targets
    // or their keys removed) - drop the selection rather than edit a stale one.
    if (sigSelElem >= 0 && sigSelElem >= doc.elemCount) SigClearKeySel();
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
    SigCloseDrops(); SigClearKeySel();
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
    SigCloseDrops(); SigClearKeySel();
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
        if (sigDropMode != SIGDROP_NONE)  SigCloseDrops();
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
        SigCloseDrops();
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

    // A fired signal layers over the timeline pose (NULL override when idle).
    // The smooth-loop blend is shown only while the preview is ACTUALLY looping:
    // scrubbing by hand must show the authored pose at the playhead, or the tail
    // of the timeline could never be edited.
    // (The gizmos below read AnimElemProp outside this call, so during the blend
    //  window they track the authored pose rather than the blended one.)
    AnimDocDrawLoop(&doc, playhead, &preview, playing && loopPlay);

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

    // --- Shift = fine relative drag --------------------------------------------
    // Remember which slider a fresh press landed on, so only that slider reacts.
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, r))
    {
        finePressRect = r; finePressActive = true; fineSticky = false;
    }
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (finePressActive && SameRect(finePressRect, r) &&
        IsMouseButtonDown(MOUSE_BUTTON_LEFT) && (shift || fineSticky))
    {
        // Once Shift is seen during a press, stay in fine mode until release so
        // letting go of Shift mid-drag doesn't snap the bar to the mouse x.
        if (!fineSticky) { UndoPush(); fineSticky = true; }
        GuiSlider(r, label, TextFormat("%.2f", *v), &(float){ *v }, lo, hi);
        float fstep = (hi - lo) * 0.0015f;
        float nv = ClampF(*v + GetMouseDelta().x * fstep, lo, hi);
        if (nv != *v) { *v = nv; return true; }
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

// Panel background opacity for a given toggle mode (0 opaque, 1 semi, 2 faint).
static unsigned char PanelAlpha(int mode)
{
    return mode == 0 ? 255 : mode == 1 ? 140 : 75;
}

// Draw a panel background (replacing GuiPanel) with per-panel transparency:
// only the fill fades, so widgets/text drawn afterward stay fully readable.
static void DrawPanelBG(Rectangle r, int mode)
{
    Color bg = GetColor(GuiGetStyle(DEFAULT, BACKGROUND_COLOR));
    Color ln = GetColor(GuiGetStyle(DEFAULT, LINE_COLOR));
    DrawRectangleRec(r, Fade(bg, PanelAlpha(mode) / 255.0f));
    DrawRectangleLinesEx(r, 1.0f, ln);
}

// A small window-glyph toggle at a panel's top-left. Clicking cycles the given
// alpha mode opaque -> semi -> faint. The glyph brightens as it gets more
// transparent so the current state is legible at a glance. Returns the icon rect.
static Rectangle DrawPanelAlphaToggle(float x, float y, int *mode, bool locked)
{
    Rectangle ico = { x, y, 13.0f, 13.0f };
    if (!locked && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        CheckCollisionPointRec(GetMousePosition(), ico))
        *mode = (*mode + 1) % 3;

    // brighter frame as transparency increases; filled title-bar shows state.
    unsigned char b = (unsigned char)(120 + 55 * (*mode));
    Color edge = { b, b, (unsigned char)(b + 20 > 255 ? 255 : b + 20), 255 };
    DrawRectangleLinesEx(ico, 1.0f, edge);
    DrawRectangleRec((Rectangle){ ico.x + 1, ico.y + 1, ico.width - 2, 3 }, edge);
    return ico;
}

// Canvas reference dimension for a SIZE property's axis (width-like -> game.x,
// height-like -> game.y). 0 for props that are not size-in-units (position,
// alpha, rotation, ...), which therefore never switch units.
static float SizePropRef(int prop, Vector2 game)
{
    switch (prop)
    {
        case AP_S_W:                                    return game.x;
        case AP_T_SIZE: case AP_S_H: case AP_S_OUTLINE: return game.y;
        default:                                        return 0.0f;
    }
}

// Slider [lo,hi] for a property in the element's CURRENT units: fractional
// bounds from AnimProp{Min,Max}, scaled to pixels when the element is absolute.
static void PropRange(const AnimElem *e, int prop, float *lo, float *hi)
{
    *lo = AnimPropMin(prop);
    *hi = AnimPropMax(prop);
    if (e->sizeAbsolute)
    {
        float ref = SizePropRef(prop, ScreenStateTargetSize());
        if (ref > 0.0f) { *lo *= ref; *hi *= ref; }
    }
}

// Rescale an element's size base fields AND every size-track key value between
// canvas-fraction and absolute-pixel units, so toggling the unit leaves the
// shape looking identical. toAbsolute = frac->px (multiply by the reference
// dimension), else px->frac (divide).
static void ConvertSizeUnits(AnimElem *e, bool toAbsolute)
{
    Vector2 game = ScreenStateTargetSize();
    if (e->kind == AE_TEXT)
        e->sizeFrac.x = toAbsolute ? e->sizeFrac.x*game.y : e->sizeFrac.x/game.y;
    else if (e->kind == AE_SHAPE)
    {
        e->sizeFrac.x  = toAbsolute ? e->sizeFrac.x*game.x  : e->sizeFrac.x/game.x;
        e->sizeFrac.y  = toAbsolute ? e->sizeFrac.y*game.y  : e->sizeFrac.y/game.y;
        e->outlineFrac = toAbsolute ? e->outlineFrac*game.y : e->outlineFrac/game.y;
    }
    for (int i = 0; i < e->trackCount; i++)
    {
        AnimTrack *tr = &e->tracks[i];
        float ref = SizePropRef(tr->prop, game);
        if (ref <= 0.0f) continue;
        for (int k = 0; k < tr->keyCount; k++)
            tr->keys[k].value = toAbsolute ? tr->keys[k].value*ref
                                           : tr->keys[k].value/ref;
    }
}

// The base (rest-pose) field a scalar geometry prop writes into when it has no
// track. NULL for props with no base field of their own.
static float *ElemBaseField(AnimElem *e, int prop)
{
    switch (prop)
    {
        case AP_T_POS_X: case AP_S_POS_X: return &e->posFrac.x;
        case AP_T_POS_Y: case AP_S_POS_Y: return &e->posFrac.y;
        case AP_T_SIZE:  case AP_S_W:     return &e->sizeFrac.x;
        case AP_S_H:                      return &e->sizeFrac.y;
        case AP_T_ROT:   case AP_S_ROT:   return &e->rotBase;
        case AP_S_SCALE:                  return &e->scaleFrac;
        case AP_S_OUTLINE:                return &e->outlineFrac;
        default:                          return NULL;
    }
}

// Write a scalar value to `prop` the same way PropSlider commits: keyframe it at
// the playhead when a track exists (auto-key), otherwise set the base field.
// Used by corner-mode editing, which drives several props from one gesture.
static void WritePropValue(AnimElem *e, int prop, float v, float *baseField)
{
    AnimTrack *tr = AnimElemFindTrack(e, prop);
    if (!tr) { if (baseField) *baseField = v; return; }
    if (!autoKey) return;                    // keyed edits go via the key inspector
    if (playhead > AUTOKEY_EPS) EnsureZeroKey(e, tr);
    AnimTrackWriteKeyAt(tr, playhead, v, AUTOKEY_EPS);
}

// Read/write binding for the geometry props a corner-mode gesture drives. The
// same corner maths serves two very different stores - the element's timeline
// (values at the playhead) and one signal group key (values at its u) - so the
// rows below take the access, not the store.
typedef struct {
    float (*get)(void *ctx, int prop);
    void  (*set)(void *ctx, int prop, float v);
    void   *ctx;
} PropAccess;

static float ElemPropGet(void *ctx, int prop)
{ return AnimElemProp((const AnimElem *)ctx, prop, playhead); }

static void ElemPropSet(void *ctx, int prop, float v)
{ AnimElem *e = (AnimElem *)ctx; WritePropValue(e, prop, v, ElemBaseField(e, prop)); }

// PropAccess over ONE timeline group key: props with a key at t read/write it,
// props without fall back to (and, on write, get) a key at that time - a corner
// drag drives position AND size, so both have to land on the same key.
typedef struct { AnimElem *e; float t; int ease; } TrackKeyCtx;

static float TrackKeyPropGet(void *ctx, int prop)
{
    TrackKeyCtx *c = (TrackKeyCtx *)ctx;
    return AnimElemProp(c->e, prop, c->t);
}

static void TrackKeyPropSet(void *ctx, int prop, float v)
{
    TrackKeyCtx *c = (TrackKeyCtx *)ctx;
    AnimTrack *tr = AnimElemFindTrack(c->e, prop);
    if (!tr) tr = AnimElemAddTrack(c->e, prop);
    if (!tr) return;
    if (c->t > AUTOKEY_EPS) EnsureZeroKey(c->e, tr);
    AnimKey *k = AnimTrackWriteKeyAt(tr, c->t, v, AUTOKEY_EPS);
    if (k && tr->keyCount == 1) k->ease = c->ease;   // brand-new track
}

// Slider for an ANIMATABLE property. No track -> edits the base field (rest
// pose), exactly like before. With a track -> shows the value evaluated at the
// playhead; auto-key ON writes/updates a key there, auto-key OFF disables the
// slider (edit through the key inspector instead).
static void PropSlider(Rectangle r, AnimElem *e, int prop, float *baseField)
{
    float lo, hi; PropRange(e, prop, &lo, &hi);
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
    GuiLabel((Rectangle){ x+16, y, w-16, 20 }, "ELEMENTS"); y += 22;

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
//  Track GROUPS: the inspector/timeline present a few logical targets (Position,
//  Color, Outline, ...) instead of the raw per-prop tracks. These helpers keep a
//  group's member tracks in lockstep at shared key times, so ONE group keyframe
//  drives every member (pos_x AND pos_y, colour AND alpha, ...). Storage stays
//  per-prop; this is purely coordinated editing over the existing tracks.
// ---------------------------------------------------------------------------
#define GROUP_TIMES_MAX (ANIM_KEYS_MAX * ANIM_GROUP_PROPS)

// Union of a group's member key times, ascending and de-duped within eps.
static int GroupKeyTimes(AnimElem *e, int gi, float *out)
{
    const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
    int n = 0;
    if (!g) return 0;
    for (int m = 0; m < g->propCount; m++)
    {
        AnimTrack *tr = AnimElemFindTrack(e, g->props[m]);
        if (!tr) continue;
        for (int k = 0; k < tr->keyCount; k++)
        {
            float t = tr->keys[k].t;
            bool dup = false;
            for (int i = 0; i < n; i++)
                if (fabsf(out[i] - t) <= AUTOKEY_EPS) { dup = true; break; }
            if (!dup && n < GROUP_TIMES_MAX) out[n++] = t;
        }
    }
    for (int i = 1; i < n; i++)          // insertion sort ascending
    { float v = out[i]; int j = i-1; while (j>=0 && out[j]>v){out[j+1]=out[j];j--;} out[j+1]=v; }
    return n;
}

// True if any member of the group has a track.
static bool GroupHasTrack(AnimElem *e, int gi)
{
    const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
    if (!g) return false;
    for (int m = 0; m < g->propCount; m++)
        if (AnimElemFindTrack(e, g->props[m])) return true;
    return false;
}

// Write a group key at time t: every member gets a key there, seeded from the
// element's current value at t (creating member tracks + a zero key as needed).
static void GroupWriteKey(AnimElem *e, int gi, float t)
{
    const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
    if (!g) return;
    for (int m = 0; m < g->propCount; m++)
    {
        int prop = g->props[m];
        AnimTrack *tr = AnimElemFindTrack(e, prop);
        if (!tr) tr = AnimElemAddTrack(e, prop);
        if (!tr) continue;
        if (AnimPropIsColor(prop))
        {
            Color c = AnimElemColorProp(e, prop, t);
            if (t > AUTOKEY_EPS) EnsureZeroColorKey(e, tr);
            AnimTrackWriteColorKeyAt(tr, t, c, AUTOKEY_EPS);
        }
        else
        {
            float v = AnimElemProp(e, prop, t);
            if (t > AUTOKEY_EPS) EnsureZeroKey(e, tr);
            AnimTrackWriteKeyAt(tr, t, v, AUTOKEY_EPS);
        }
    }
}

// Remove every member track of a group.
static void GroupDeleteTracks(AnimElem *e, int gi)
{
    const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
    if (!g) return;
    for (int m = 0; m < g->propCount; m++)
    {
        for (int i = 0; i < e->trackCount; i++)
            if (e->tracks[i].prop == g->props[m]) { AnimElemRemoveTrack(e, i); break; }
    }
}

// Remove the group key at time t (each member's key near t).
static void GroupDeleteKeyAt(AnimElem *e, int gi, float t)
{
    const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
    if (!g) return;
    for (int m = 0; m < g->propCount; m++)
    {
        AnimTrack *tr = AnimElemFindTrack(e, g->props[m]);
        if (!tr) continue;
        for (int k = tr->keyCount - 1; k >= 0; k--)
            if (fabsf(tr->keys[k].t - t) <= AUTOKEY_EPS) AnimTrackRemoveKey(tr, k);
    }
}

// Move the group key from oldT to newT across every member.
static void GroupMoveKeyTo(AnimElem *e, int gi, float oldT, float newT)
{
    const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
    if (!g) return;
    for (int m = 0; m < g->propCount; m++)
    {
        AnimTrack *tr = AnimElemFindTrack(e, g->props[m]);
        if (!tr) continue;
        for (int k = 0; k < tr->keyCount; k++)
            if (fabsf(tr->keys[k].t - oldT) <= AUTOKEY_EPS)
            { AnimTrackSetKeyTime(tr, k, newT); break; }
    }
}

// Set the segment ease of a group key at time t on every member.
static void GroupSetEaseAt(AnimElem *e, int gi, float t, int ease)
{
    const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
    if (!g) return;
    for (int m = 0; m < g->propCount; m++)
    {
        AnimTrack *tr = AnimElemFindTrack(e, g->props[m]);
        if (!tr) continue;
        for (int k = 0; k < tr->keyCount; k++)
            if (fabsf(tr->keys[k].t - t) <= AUTOKEY_EPS) tr->keys[k].ease = ease;
    }
}

// Resolve a group key (group gi at time t) to a representative member (track
// index + key index): the first member with a key near t. False if none. Used
// to point the existing (selKeyTrack, selKeyIdx) selection at a group key.
static bool GroupRepKey(AnimElem *e, int gi, float t, int *trackIdx, int *keyIdx)
{
    const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
    if (!g) return false;
    for (int m = 0; m < g->propCount; m++)
        for (int i = 0; i < e->trackCount; i++)
        {
            if (e->tracks[i].prop != g->props[m]) continue;
            for (int k = 0; k < e->tracks[i].keyCount; k++)
                if (fabsf(e->tracks[i].keys[k].t - t) <= AUTOKEY_EPS)
                { *trackIdx = i; *keyIdx = k; return true; }
        }
    return false;
}

// The group index + time of the current key selection on element e, or gi=-1.
static int SelectedGroup(AnimElem *e, float *tOut)
{
    if (selKeyElem != selElem || selKeyTrack < 0 || selKeyTrack >= e->trackCount)
        return -1;
    if (selKeyIdx < 0 || selKeyIdx >= e->tracks[selKeyTrack].keyCount) return -1;
    if (tOut) *tOut = e->tracks[selKeyTrack].keys[selKeyIdx].t;
    return AnimGroupIndexOfProp(e->kind, e->tracks[selKeyTrack].prop);
}

// Point the key selection at the group key (gi, t) via its representative member.
static void SelectGroupKey(int elem, int gi, float t)
{
    AnimElem *e = &doc.elems[elem];
    int ti, ki;
    if (GroupRepKey(e, gi, t, &ti, &ki)) SelectKey(elem, ti, ki);
}

// Index of the key at time t on a track (within eps), or -1.
static int TrackKeyNear(const AnimTrack *tr, float t)
{
    if (!tr) return -1;
    for (int k = 0; k < tr->keyCount; k++)
        if (fabsf(tr->keys[k].t - t) <= AUTOKEY_EPS) return k;
    return -1;
}

// Compact one-line summary of a group key at time t: "t   v0,v1,..   ease".
// Scalar members show their value; colour members a #RRGGBB hex.
static const char *GroupKeyLabel(AnimElem *e, int gi, float t)
{
    const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
    char vals[80]; vals[0] = 0;
    for (int m = 0; g && m < g->propCount; m++)
    {
        int prop = g->props[m];
        const char *one;
        if (AnimPropIsColor(prop))
        {
            Color c = AnimElemColorProp(e, prop, t);
            one = TextFormat("#%02X%02X%02X", c.r, c.g, c.b);
        }
        else one = TextFormat("%.2f", AnimElemProp(e, prop, t));
        if (m) strncat(vals, ",", sizeof(vals)-strlen(vals)-1);
        strncat(vals, one, sizeof(vals)-strlen(vals)-1);
    }
    int ti, ki, ease = 0;
    if (GroupRepKey(e, gi, t, &ti, &ki)) ease = e->tracks[ti].keys[ki].ease;
    return TextFormat("%.2f   %s   %s", t, vals, AnimEaseName(ease));
}

// The colour member prop of a group (fill/outline/fade colour), or -1.
static int GroupColorProp(int kind, int gi)
{
    const AnimPropGroup *g = AnimGroupAt(kind, gi);
    for (int m = 0; g && m < g->propCount; m++)
        if (AnimPropIsColor(g->props[m])) return g->props[m];
    return -1;
}

// ---------------------------------------------------------------------------
//  Signal target GROUPS: the same coordinated editing as the track groups
//  above, applied to a signal's (element, property) targets. One authored track
//  on a signal IS a group - picking `position` creates the pos_x and pos_y
//  targets, and every key written afterwards lands on both at the same u.
//  Storage (and the .cfg) stays one target per property.
//
//  Times here are a signal key's NORMALIZED u (0..1), a fraction of the signal
//  length, so they need their own epsilon: AUTOKEY_EPS is a playhead tolerance
//  in SECONDS.
// ---------------------------------------------------------------------------
#define SIG_U_EPS     0.001f
#define SIG_TIMES_MAX (ANIM_SIG_KEYS_MAX * ANIM_GROUP_PROPS)

static AnimSigTarget *SigFindTarget(AnimSignal *sg, int elemIdx, int prop)
{
    for (int i = 0; i < sg->targetCount; i++)
        if (sg->targets[i].elemIdx == elemIdx && sg->targets[i].prop == prop)
            return &sg->targets[i];
    return NULL;
}

// True if any member of the group already has a target on this element.
static bool SigGroupHasTarget(AnimSignal *sg, int elemIdx, int gi)
{
    const AnimPropGroup *g = AnimGroupAt(doc.elems[elemIdx].kind, gi);
    if (!g) return false;
    for (int m = 0; m < g->propCount; m++)
        if (SigFindTarget(sg, elemIdx, g->props[m])) return true;
    return false;
}

// Free target slots vs the members this group still needs: a 3-prop group can
// be refused while a 1-prop one still fits.
static bool SigGroupFits(AnimSignal *sg, int elemIdx, int gi)
{
    const AnimPropGroup *g = AnimGroupAt(doc.elems[elemIdx].kind, gi);
    if (!g) return false;
    int need = 0;
    for (int m = 0; m < g->propCount; m++)
        if (!SigFindTarget(sg, elemIdx, g->props[m])) need++;
    return sg->targetCount + need <= ANIM_SIG_TARGETS_MAX;
}

// Union of a group's member key u values, ascending and de-duped within eps.
static int SigGroupKeyTimes(AnimSignal *sg, int elemIdx, int gi, float *out)
{
    const AnimPropGroup *g = AnimGroupAt(doc.elems[elemIdx].kind, gi);
    int n = 0;
    if (!g) return 0;
    for (int m = 0; m < g->propCount; m++)
    {
        AnimSigTarget *tg = SigFindTarget(sg, elemIdx, g->props[m]);
        if (!tg) continue;
        for (int k = 0; k < tg->keyCount; k++)
        {
            float u = tg->keys[k].t;
            bool dup = false;
            for (int i = 0; i < n; i++)
                if (fabsf(out[i] - u) <= SIG_U_EPS) { dup = true; break; }
            if (!dup && n < SIG_TIMES_MAX) out[n++] = u;
        }
    }
    for (int i = 1; i < n; i++)          // insertion sort ascending
    { float v = out[i]; int j = i-1; while (j>=0 && out[j]>v){out[j+1]=out[j];j--;} out[j+1]=v; }
    return n;
}

// Index of the key at u on a target (within eps), or -1.
static int SigTargetKeyNear(const AnimSigTarget *tg, float u)
{
    if (!tg) return -1;
    for (int k = 0; k < tg->keyCount; k++)
        if (fabsf(tg->keys[k].t - u) <= SIG_U_EPS) return k;
    return -1;
}

// Keys ascending in u, like AnimTrackSortKeys does for timeline tracks - the
// player walks them in order, so an out-of-order insert would strand a segment.
static void SigTargetSortKeys(AnimSigTarget *tg)
{
    for (int i = 1; i < tg->keyCount; i++)
    {
        AnimKey v = tg->keys[i]; int j = i-1;
        while (j >= 0 && tg->keys[j].t > v.t) { tg->keys[j+1] = tg->keys[j]; j--; }
        tg->keys[j+1] = v;
    }
}

// Pick a u for a NEW key given the times (sorted ascending) already present.
// Prefer `pref` (sigLastU - the "same beat" case); if that slot is taken, drop
// the new key into the middle of the largest free gap in [0,1] so every +key
// press spawns a distinct key instead of re-selecting the existing one.
static float SigFreeU(const float *times, int nt, float pref)
{
    if (nt == 0) return pref;

    bool taken = false;
    for (int i = 0; i < nt; i++)
        if (fabsf(times[i] - pref) <= SIG_U_EPS) { taken = true; break; }
    if (!taken) return pref;                              // pref is free

    // Largest gap among the endpoints [0, k0, k1, ..., 1]; return its midpoint.
    float lo = 0.0f, hi = times[0], gap = times[0];
    for (int i = 0; i + 1 < nt; i++)
        if (times[i+1] - times[i] > gap)
        { gap = times[i+1] - times[i]; lo = times[i]; hi = times[i+1]; }
    if (1.0f - times[nt-1] > gap) { lo = times[nt-1]; hi = 1.0f; }
    return (lo + hi) * 0.5f;
}

// Free u for a new group key across every member of the group.
static float SigGroupFreeU(AnimSignal *sg, int elemIdx, int gi, float pref)
{
    float times[SIG_TIMES_MAX];
    int nt = SigGroupKeyTimes(sg, elemIdx, gi, times);   // sorted ascending
    return SigFreeU(times, nt, pref);
}

// Write a group key at u: every member target gets a key there (creating the
// target as needed), seeded from the element's CURRENT pose - a signal key is
// an absolute destination, and the pose under the playhead is what the user is
// looking at while authoring.
static void SigGroupWriteKey(AnimSignal *sg, int elemIdx, int gi, float u)
{
    const AnimElem *el = &doc.elems[elemIdx];
    const AnimPropGroup *g = AnimGroupAt(el->kind, gi);
    if (!g) return;
    for (int m = 0; m < g->propCount; m++)
    {
        int prop = g->props[m];
        AnimSigTarget *tg = SigFindTarget(sg, elemIdx, prop);
        if (!tg)
        {
            if (sg->targetCount >= ANIM_SIG_TARGETS_MAX) continue;
            tg = &sg->targets[sg->targetCount++];
            *tg = (AnimSigTarget){0};
            tg->elemIdx = elemIdx; tg->prop = prop;
        }
        int k = SigTargetKeyNear(tg, u);
        if (k < 0)
        {
            if (tg->keyCount >= ANIM_SIG_KEYS_MAX) continue;
            k = tg->keyCount++;
            tg->keys[k].ease = ANIM_EASE_SINE_OUT;
        }
        tg->keys[k].t     = u;
        tg->keys[k].value = AnimElemProp(el, prop, playhead);
        tg->keys[k].cval  = AnimElemColorProp(el, prop, playhead);
        SigTargetSortKeys(tg);
    }
}

// Create a group's missing member targets without keying anything (the adder).
static void SigGroupAddTargets(AnimSignal *sg, int elemIdx, int gi)
{
    const AnimPropGroup *g = AnimGroupAt(doc.elems[elemIdx].kind, gi);
    if (!g) return;
    for (int m = 0; m < g->propCount; m++)
    {
        if (SigFindTarget(sg, elemIdx, g->props[m])) continue;
        if (sg->targetCount >= ANIM_SIG_TARGETS_MAX) return;
        AnimSigTarget *tg = &sg->targets[sg->targetCount++];
        *tg = (AnimSigTarget){0};
        tg->elemIdx = elemIdx; tg->prop = g->props[m];
    }
}

// Remove every member target of a group (descending, so the shift-down can't
// skip a member).
static void SigGroupDeleteTargets(AnimSignal *sg, int elemIdx, int gi)
{
    const AnimPropGroup *g = AnimGroupAt(doc.elems[elemIdx].kind, gi);
    if (!g) return;
    for (int i = sg->targetCount - 1; i >= 0; i--)
    {
        if (sg->targets[i].elemIdx != elemIdx) continue;
        bool member = false;
        for (int m = 0; m < g->propCount; m++)
            if (sg->targets[i].prop == g->props[m]) { member = true; break; }
        if (!member) continue;
        for (int j = i; j < sg->targetCount - 1; j++) sg->targets[j] = sg->targets[j+1];
        sg->targetCount--;
    }
}

// Remove the group key at u (each member's key near u).
static void SigGroupDeleteKeyAt(AnimSignal *sg, int elemIdx, int gi, float u)
{
    const AnimPropGroup *g = AnimGroupAt(doc.elems[elemIdx].kind, gi);
    if (!g) return;
    for (int m = 0; m < g->propCount; m++)
    {
        AnimSigTarget *tg = SigFindTarget(sg, elemIdx, g->props[m]);
        if (!tg) continue;
        for (int k = tg->keyCount - 1; k >= 0; k--)
            if (fabsf(tg->keys[k].t - u) <= SIG_U_EPS)
            {
                for (int j = k; j < tg->keyCount - 1; j++) tg->keys[j] = tg->keys[j+1];
                tg->keyCount--;
            }
    }
}

// Move the group key from oldU to newU across every member.
static void SigGroupMoveKeyTo(AnimSignal *sg, int elemIdx, int gi,
                              float oldU, float newU)
{
    const AnimPropGroup *g = AnimGroupAt(doc.elems[elemIdx].kind, gi);
    if (!g) return;
    for (int m = 0; m < g->propCount; m++)
    {
        AnimSigTarget *tg = SigFindTarget(sg, elemIdx, g->props[m]);
        int k = SigTargetKeyNear(tg, oldU);
        if (k < 0) continue;
        tg->keys[k].t = newU;
        SigTargetSortKeys(tg);
    }
}

// Set the segment ease of a group key at u on every member.
static void SigGroupSetEaseAt(AnimSignal *sg, int elemIdx, int gi, float u, int ease)
{
    const AnimPropGroup *g = AnimGroupAt(doc.elems[elemIdx].kind, gi);
    if (!g) return;
    for (int m = 0; m < g->propCount; m++)
    {
        AnimSigTarget *tg = SigFindTarget(sg, elemIdx, g->props[m]);
        int k = SigTargetKeyNear(tg, u);
        if (k >= 0) tg->keys[k].ease = ease;
    }
}

// Compact one-line summary of a signal group key: "u   v0,v1,..   ease".
// Mirrors GroupKeyLabel, but reads the TARGET keys (absolute destinations)
// rather than evaluating the element.
static const char *SigGroupKeyLabel(AnimSignal *sg, int elemIdx, int gi, float u)
{
    const AnimPropGroup *g = AnimGroupAt(doc.elems[elemIdx].kind, gi);
    char vals[80]; vals[0] = 0;
    int ease = 0; bool haveEase = false;
    for (int m = 0; g && m < g->propCount; m++)
    {
        AnimSigTarget *tg = SigFindTarget(sg, elemIdx, g->props[m]);
        int k = SigTargetKeyNear(tg, u);
        if (k < 0) continue;                     // ragged: member has no key here
        if (!haveEase) { ease = tg->keys[k].ease; haveEase = true; }
        const char *one = AnimPropIsColor(g->props[m])
            ? TextFormat("#%02X%02X%02X", tg->keys[k].cval.r, tg->keys[k].cval.g,
                                          tg->keys[k].cval.b)
            : TextFormat("%.2f", tg->keys[k].value);
        if (vals[0]) strncat(vals, ",", sizeof(vals)-strlen(vals)-1);
        strncat(vals, one, sizeof(vals)-strlen(vals)-1);
    }
    return TextFormat("%.2f   %s   %s", u, vals, AnimEaseName(ease));
}

// ---------------------------------------------------------------------------
//  Mouse-Position bindings ("--params--" section) authoring helpers
// ---------------------------------------------------------------------------
static int SigPosFind(AnimSignal *sg, int elemIdx, int slot)
{
    for (int i = 0; i < sg->posParamCount; i++)
        if (sg->posParams[i].elemIdx == elemIdx && sg->posParams[i].slot == slot)
            return i;
    return -1;
}
static void SigPosSortKeys(AnimSigPosParam *pp)
{
    for (int i = 1; i < pp->keyCount; i++)     // insertion sort (<= 8 keys)
    {
        AnimPosKey k = pp->keys[i]; int j = i - 1;
        while (j >= 0 && pp->keys[j].t > k.t) { pp->keys[j+1] = pp->keys[j]; j--; }
        pp->keys[j+1] = k;
    }
}
static AnimSigPosParam *SigPosAdd(AnimSignal *sg, int elemIdx, int slot)
{
    if (SigPosFind(sg, elemIdx, slot) >= 0) return NULL;
    if (sg->posParamCount >= ANIM_SIG_POS_MAX) return NULL;
    AnimSigPosParam *pp = &sg->posParams[sg->posParamCount++];
    *pp = (AnimSigPosParam){0};
    pp->elemIdx = elemIdx; pp->slot = slot;
    // seed one key so a fresh binding does something: ease onto the mouse at u=1
    pp->keys[0] = (AnimPosKey){ 1.0f, 0.0f, 0.0f, ANIM_EASE_SINE_OUT };
    pp->keyCount = 1;
    return pp;
}
static void SigPosRemoveAt(AnimSignal *sg, int idx)
{
    for (int i = idx; i < sg->posParamCount - 1; i++)
        sg->posParams[i] = sg->posParams[i+1];
    sg->posParamCount--;
}
static int SigPosKeyNear(const AnimSigPosParam *pp, float u)
{
    for (int i = 0; pp && i < pp->keyCount; i++)
        if (fabsf(pp->keys[i].t - u) <= SIG_U_EPS) return i;
    return -1;
}
static void SigPosWriteKey(AnimSigPosParam *pp, float u)
{
    if (SigPosKeyNear(pp, u) >= 0 || pp->keyCount >= ANIM_SIG_KEYS_MAX) return;
    pp->keys[pp->keyCount++] = (AnimPosKey){ u, 0.0f, 0.0f, ANIM_EASE_SINE_OUT };
    SigPosSortKeys(pp);
}
// Free u for a new key on a position slot (keys are kept sorted ascending).
static float SigPosFreeU(const AnimSigPosParam *pp, float pref)
{
    float times[ANIM_SIG_KEYS_MAX];
    for (int i = 0; i < pp->keyCount; i++) times[i] = pp->keys[i].t;
    return SigFreeU(times, pp->keyCount, pref);
}
static void SigPosRemoveKeyAt(AnimSigPosParam *pp, int idx)
{
    for (int i = idx; i < pp->keyCount - 1; i++) pp->keys[i] = pp->keys[i+1];
    pp->keyCount--;
}

// ---------------------------------------------------------------------------
//  Sequence offset ("--sequence--" section) authoring helpers
// ---------------------------------------------------------------------------
static int SigSeqTargetFind(AnimSignal *sg, int elemIdx, int prop)
{
    for (int i = 0; i < sg->seqTargetCount; i++)
        if (sg->seqTargets[i].elemIdx == elemIdx && sg->seqTargets[i].prop == prop)
            return i;
    return -1;
}
static void SigSeqTargetRemoveAt(AnimSignal *sg, int idx)
{
    for (int i = idx; i < sg->seqTargetCount - 1; i++)
        sg->seqTargets[i] = sg->seqTargets[i+1];
    sg->seqTargetCount--;
}
// Is EVERY scalar member of group gi (on elem) a sequence target? A group is
// toggled as a unit so w/h fan together (a colour member is never a seq target).
static bool SigSeqGroupOn(AnimSignal *sg, int elemIdx, int kind, int gi)
{
    const AnimPropGroup *g = AnimGroupAt(kind, gi);
    int scal = 0, have = 0;
    for (int m = 0; g && m < g->propCount; m++)
    {
        if (AnimPropIsColor(g->props[m])) continue;
        scal++;
        if (SigSeqTargetFind(sg, elemIdx, g->props[m]) >= 0) have++;
    }
    return scal > 0 && have == scal;
}
static bool SigSeqGroupHasScalar(int kind, int gi)
{
    const AnimPropGroup *g = AnimGroupAt(kind, gi);
    for (int m = 0; g && m < g->propCount; m++)
        if (!AnimPropIsColor(g->props[m])) return true;
    return false;
}
static void SigSeqGroupSet(AnimSignal *sg, int elemIdx, int kind, int gi, bool on)
{
    const AnimPropGroup *g = AnimGroupAt(kind, gi);
    for (int m = 0; g && m < g->propCount; m++)
    {
        if (AnimPropIsColor(g->props[m])) continue;
        int idx = SigSeqTargetFind(sg, elemIdx, g->props[m]);
        if (on && idx < 0 && sg->seqTargetCount < ANIM_SIG_SEQ_TARGETS)
        {
            AnimSigSeqTarget *st = &sg->seqTargets[sg->seqTargetCount++];
            st->elemIdx = elemIdx; st->prop = g->props[m];
        }
        else if (!on && idx >= 0) SigSeqTargetRemoveAt(sg, idx);
    }
}
static int SigSeqKeyNear(const AnimSignal *sg, float u)
{
    for (int i = 0; i < sg->seqKeyCount; i++)
        if (fabsf(sg->seqKeys[i].t - u) <= SIG_U_EPS) return i;
    return -1;
}
static void SigSeqSortKeys(AnimSignal *sg)
{
    for (int i = 1; i < sg->seqKeyCount; i++)
    {
        AnimSeqKey k = sg->seqKeys[i]; int j = i - 1;
        while (j >= 0 && sg->seqKeys[j].t > k.t) { sg->seqKeys[j+1] = sg->seqKeys[j]; j--; }
        sg->seqKeys[j+1] = k;
    }
}
static void SigSeqWriteKey(AnimSignal *sg, float u)
{
    if (SigSeqKeyNear(sg, u) >= 0 || sg->seqKeyCount >= ANIM_SIG_SEQ_KEYS) return;
    sg->seqKeys[sg->seqKeyCount++] = (AnimSeqKey){ u, 1.0f, ANIM_EASE_SINE_OUT };
    SigSeqSortKeys(sg);
}
// Free u for a new sequence-envelope key (seqKeys are kept sorted ascending).
static float SigSeqFreeU(const AnimSignal *sg, float pref)
{
    float times[ANIM_SIG_SEQ_KEYS];
    for (int i = 0; i < sg->seqKeyCount; i++) times[i] = sg->seqKeys[i].t;
    return SigFreeU(times, sg->seqKeyCount, pref);
}
static void SigSeqRemoveKeyAt(AnimSignal *sg, int idx)
{
    for (int i = idx; i < sg->seqKeyCount - 1; i++) sg->seqKeys[i] = sg->seqKeys[i+1];
    sg->seqKeyCount--;
}

// PropAccess over ONE signal group key: a prop the signal drives reads/writes
// its key at u, and a prop it does not drive reads the element's live value
// (which is exactly what the signal player leaves alone).
typedef struct { AnimSignal *sg; int elemIdx; float u; int ease; } SigKeyCtx;

static float SigKeyPropGet(void *ctx, int prop)
{
    SigKeyCtx *c = (SigKeyCtx *)ctx;
    AnimSigTarget *tg = SigFindTarget(c->sg, c->elemIdx, prop);
    int k = SigTargetKeyNear(tg, c->u);
    if (k >= 0) return tg->keys[k].value;
    return AnimElemProp(&doc.elems[c->elemIdx], prop, playhead);
}

// A corner drag moves AND resizes, so the size members have to become part of
// the signal for the result to replay: create the target/key when it is missing
// rather than silently dropping half the gesture.
static void SigKeyPropSet(void *ctx, int prop, float v)
{
    SigKeyCtx *c = (SigKeyCtx *)ctx;
    AnimSigTarget *tg = SigFindTarget(c->sg, c->elemIdx, prop);
    if (!tg)
    {
        if (c->sg->targetCount >= ANIM_SIG_TARGETS_MAX) return;
        tg = &c->sg->targets[c->sg->targetCount++];
        *tg = (AnimSigTarget){0};
        tg->elemIdx = c->elemIdx; tg->prop = prop;
    }
    int k = SigTargetKeyNear(tg, c->u);
    if (k < 0)
    {
        if (tg->keyCount >= ANIM_SIG_KEYS_MAX) return;
        k = tg->keyCount++;
        tg->keys[k] = (AnimKey){ c->u, v, (Color){0,0,0,255}, c->ease };
        SigTargetSortKeys(tg);
        return;
    }
    tg->keys[k].value = v;
}

// Corner-mode geometry rows for a shape: two opposite corners (P0/P1), or for a
// line its two endpoints (Start/End). Values are canvas FRACTIONS (like
// position) in % mode and absolute pixels in px mode; on edit they are converted
// back to the stored center+size (and, for a line, length + rotation) through
// `pa`, so the same four sliders serve the inspector (writing the element's
// tracks at the playhead) and the signal key editor (writing one group key).
// Returns the new y.
static float DrawCornerRows(float x, float y, float w, float rh, float gap,
                            const AnimElem *e, PropAccess pa)
{
    Vector2 game = ScreenStateTargetSize();
    float lo = -3.0f, hi = 3.0f;                 // generous off-canvas reach for endpoints
    // Two side-by-side x/y sliders, each with room for its value label so the
    // first slider's value/double-click region doesn't overlap the second.
    float valW = 48.0f;                          // TEXT_PADDING(4) + 44 value slot
    float half = (w - 44 - gap - 2*valW) * 0.5f;
    float x2   = x + 44 + half + valW + gap;     // second slider's x origin
    float cx = pa.get(pa.ctx, AP_S_POS_X);
    float cy = pa.get(pa.ctx, AP_S_POS_Y);
    float wv = pa.get(pa.ctx, AP_S_W);
    float wUnit = e->sizeAbsolute ? 1.0f : game.x;
    // The renderer multiplies every extent by AP_S_SCALE, so the on-screen
    // endpoints reflect the SCALED size. Show/edit those scaled positions
    // (WYSIWYG) and divide the scale back out when storing the base size.
    float sc = pa.get(pa.ctx, AP_S_SCALE);
    float invSc = sc > 1e-6f ? 1.0f/sc : 1.0f;

    // Endpoints follow the element's size units: canvas FRACTIONS in % mode
    // (resize-invariant, like position), absolute PIXELS in px mode. Internally
    // position is always a fraction, so we display frac*scale and divide back.
    float sx = e->sizeAbsolute ? game.x : 1.0f;
    float sy = e->sizeAbsolute ? game.y : 1.0f;
    float loX = lo*sx, hiX = hi*sx, loY = lo*sy, hiY = hi*sy;

    if (e->shapeKind == SHAPE_LINE)
    {
        // endpoints = center +/- half-length, projected per-axis (x via the width
        // reference, y via the height reference) so they are true canvas fractions
        // in % mode and absolute pixels in px mode - matching DrawShapeElem exactly.
        float hUnit = e->sizeAbsolute ? 1.0f : game.y;
        float rot = pa.get(pa.ctx, AP_S_ROT) * DEG2RAD;
        float wEff = wv * sc;                                  // rendered (scaled) length
        float ox = (wUnit * wEff * 0.5f) * cosf(rot) / game.x; // endpoint x frac offset
        float oy = (hUnit * wEff * 0.5f) * sinf(rot) / game.y; // endpoint y frac offset
        float ax = (cx - ox)*sx, ay = (cy - oy)*sy, bx = (cx + ox)*sx, by = (cy + oy)*sy;
        bool ch = false;
        GuiLabel((Rectangle){ x, y, 44, rh }, "start");
        if (EditSlider((Rectangle){ x+44, y, half, rh }, "x", &ax, loX, hiX)) ch = true;
        if (EditSlider((Rectangle){ x2, y, half, rh }, "y", &ay, loY, hiY)) ch = true;
        y += rh + gap;
        GuiLabel((Rectangle){ x, y, 44, rh }, "end");
        if (EditSlider((Rectangle){ x+44, y, half, rh }, "x", &bx, loX, hiX)) ch = true;
        if (EditSlider((Rectangle){ x2, y, half, rh }, "y", &by, loY, hiY)) ch = true;
        y += rh + gap;
        if (ch)
        {
            float axF = ax/sx, ayF = ay/sy, bxF = bx/sx, byF = by/sy;
            // unit-space deltas: fractions in % mode, pixels in px mode.
            float dux = (bxF - axF) * game.x / wUnit;
            float duy = (byF - ayF) * game.y / hUnit;
            float lenFull = hypotf(dux, duy);        // rendered length in element units
            pa.set(pa.ctx, AP_S_POS_X, (axF + bxF) * 0.5f);
            pa.set(pa.ctx, AP_S_POS_Y, (ayF + byF) * 0.5f);
            pa.set(pa.ctx, AP_S_W, lenFull * invSc);  // base = rendered / scale
            pa.set(pa.ctx, AP_S_ROT, atan2f(duy, dux) * RAD2DEG);
        }
    }
    else
    {
        float hv = pa.get(pa.ctx, AP_S_H);
        float hUnit = e->sizeAbsolute ? 1.0f : game.y;
        // scaled (rendered) half-extents, so the corner handles sit on screen.
        float hxF = (wUnit*wv*sc/game.x) * 0.5f, hyF = (hUnit*hv*sc/game.y) * 0.5f;
        float x0 = (cx-hxF)*sx, y0 = (cy-hyF)*sy, x1 = (cx+hxF)*sx, y1 = (cy+hyF)*sy;
        bool ch = false;
        GuiLabel((Rectangle){ x, y, 44, rh }, "P0");
        if (EditSlider((Rectangle){ x+44, y, half, rh }, "x", &x0, loX, hiX)) ch = true;
        if (EditSlider((Rectangle){ x2, y, half, rh }, "y", &y0, loY, hiY)) ch = true;
        y += rh + gap;
        GuiLabel((Rectangle){ x, y, 44, rh }, "P1");
        if (EditSlider((Rectangle){ x+44, y, half, rh }, "x", &x1, loX, hiX)) ch = true;
        if (EditSlider((Rectangle){ x2, y, half, rh }, "y", &y1, loY, hiY)) ch = true;
        y += rh + gap;
        if (ch)
        {
            float x0F = x0/sx, y0F = y0/sy, x1F = x1/sx, y1F = y1/sy;
            float wF = fabsf(x1F-x0F), hF = fabsf(y1F-y0F);
            pa.set(pa.ctx, AP_S_POS_X, (x0F+x1F)*0.5f);
            pa.set(pa.ctx, AP_S_POS_Y, (y0F+y1F)*0.5f);
            pa.set(pa.ctx, AP_S_W, (e->sizeAbsolute ? wF*game.x : wF) * invSc);
            pa.set(pa.ctx, AP_S_H, (e->sizeAbsolute ? hF*game.y : hF) * invSc);
        }
    }
    return y;
}

// ---------------------------------------------------------------------------
//  Right panel: inspector for the primary selection.
// ---------------------------------------------------------------------------
static float DrawInspector(float x, float y, float w)   // returns content height
{
    float y0 = y;
    GuiLabel((Rectangle){ x+16, y, w-16, 20 }, "INSPECTOR"); y += 24;
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

        // units: size in canvas fractions (default) vs absolute pixels. Flipping
        // rescales the base fields + size keys so the shape looks unchanged.
        GuiLabel((Rectangle){ x, y, 44, rh }, "units");
        float uw = (w - 44 - 4) / 2.0f;
        bool wantFrac = !e->sizeAbsolute, wantAbs = e->sizeAbsolute;
        GuiToggle((Rectangle){ x+44, y, uw, rh }, "% canvas", &wantFrac);
        GuiToggle((Rectangle){ x+44+uw+4, y, uw, rh }, "px abs", &wantAbs);
        if (e->sizeAbsolute && wantFrac)      // px -> fraction
        { UndoPush(); ConvertSizeUnits(e, false); e->sizeAbsolute = false; }
        else if (!e->sizeAbsolute && wantAbs) // fraction -> px
        { UndoPush(); ConvertSizeUnits(e, true);  e->sizeAbsolute = true; }
        y += rh + gap;

        // anchor mode (shapes only): author by center+size or by two corners.
        if (e->kind == AE_SHAPE)
        {
            GuiLabel((Rectangle){ x, y, 44, rh }, "anchor");
            float aw = (w - 44 - 4) / 2.0f;
            bool wantCtr = !e->cornerMode, wantCor = e->cornerMode;
            GuiToggle((Rectangle){ x+44, y, aw, rh }, "center+size", &wantCtr);
            GuiToggle((Rectangle){ x+44+aw+4, y, aw, rh }, "corners", &wantCor);
            if (e->cornerMode && wantCtr)      { UndoPush(); e->cornerMode = false; }
            else if (!e->cornerMode && wantCor){ UndoPush(); e->cornerMode = true; }
            y += rh + gap;
        }

        // position + geometry: corner mode swaps the pos/size rows for the two
        // corner (or line-endpoint) rows; storage stays center+size either way.
        if (e->kind == AE_SHAPE && e->cornerMode)
        {
            y = DrawCornerRows(x, y, w, rh, gap, e,
                               (PropAccess){ ElemPropGet, ElemPropSet, e });
            if (e->shapeKind != SHAPE_LINE)
            {
                PropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, AP_S_SCALE, &e->scaleFrac);
                GuiLabel((Rectangle){ x, y, 44, rh }, "scale"); y += rh + gap;
            }
        }
        else
        {
            PropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, isText ? AP_T_POS_X : AP_S_POS_X, &e->posFrac.x);
            GuiLabel((Rectangle){ x, y, 44, rh }, "posX"); y += rh + gap;
            PropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, isText ? AP_T_POS_Y : AP_S_POS_Y, &e->posFrac.y);
            GuiLabel((Rectangle){ x, y, 44, rh }, "posY"); y += rh + gap;
            PropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, isText ? AP_T_SIZE : AP_S_W, &e->sizeFrac.x);
            GuiLabel((Rectangle){ x, y, 44, rh }, isText ? "size" : (e->shapeKind==SHAPE_LINE?"length":"w")); y += rh + gap;
            if (e->kind == AE_SHAPE)
            {
                PropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, AP_S_H, &e->sizeFrac.y);
                GuiLabel((Rectangle){ x, y, 44, rh }, e->shapeKind==SHAPE_LINE?"thick":"h"); y += rh + gap;
                // uniform multiplier over w/h: the single control for growing a
                // shape without keeping the two axes in proportion by hand.
                PropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, AP_S_SCALE, &e->scaleFrac);
                GuiLabel((Rectangle){ x, y, 44, rh }, "scale"); y += rh + gap;
            }
        }

        // thickness row for a line in corner mode (its 'h'), which the corner
        // rows do not cover.
        if (e->kind == AE_SHAPE && e->cornerMode && e->shapeKind == SHAPE_LINE)
        {
            PropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, AP_S_H, &e->sizeFrac.y);
            GuiLabel((Rectangle){ x, y, 44, rh }, "thick"); y += rh + gap;
        }

        // rest-pose rotation (both text and shapes). In line corner mode the
        // rotation is implied by the endpoints, so the row is hidden there.
        if (!(e->kind == AE_SHAPE && e->cornerMode && e->shapeKind == SHAPE_LINE))
        {
            PropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, isText ? AP_T_ROT : AP_S_ROT, &e->rotBase);
            GuiLabel((Rectangle){ x, y, 44, rh }, "rot"); y += rh + gap;
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

        // outline style (circle only): crisp ring vs faceted polygon ("crawling")
        if (e->shapeKind == SHAPE_CIRCLE)
        {
            GuiLabel((Rectangle){ x, y, 44, rh }, "style");
            float sw = (w - 44 - 4) / 2.0f;
            bool wantCrawl = !e->outlineCrisp, wantCrisp = e->outlineCrisp;
            GuiToggle((Rectangle){ x+44, y, sw, rh }, "crawling", &wantCrawl);
            GuiToggle((Rectangle){ x+44+sw+4, y, sw, rh }, "crisp", &wantCrisp);
            if (e->outlineCrisp && wantCrawl)      { UndoPush(); e->outlineCrisp = false; }
            else if (!e->outlineCrisp && wantCrisp){ UndoPush(); e->outlineCrisp = true; }
            y += rh + gap;
        }
    }

    // --- tracks list: one row per GROUP (Position, Color, ...) with ALL of its
    // keys, always visible. A group key spans every member prop at that time.
    GuiLine((Rectangle){ x, y, w, 8 }, "tracks"); y += 12;
    // which group + time is selected (derived from the representative member key)
    float selT = 0.0f;
    int   selG = SelectedGroup(e, &selT);

    int pendingGroupDel = -1;
    int grpN = AnimGroupCountFor(e->kind);
    for (int gi = 0; gi < grpN; gi++)
    {
        if (!GroupHasTrack(e, gi)) continue;       // only groups that have tracks
        const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
        float times[GROUP_TIMES_MAX];
        int   nt = GroupKeyTimes(e, gi, times);

        GuiLabel((Rectangle){ x, y, w-106, rh }, TextFormat("%s (%d)", g->name, nt));
        // +key: one keyframe across every member of the group at the playhead.
        if (GuiButton((Rectangle){ x+w-102, y, 50, rh }, "+key"))
        {
            AudioPlayButton(); UndoPush();
            GroupWriteKey(e, gi, playhead);
            SelectGroupKey(selElem, gi, playhead);
        }
        if (GuiButton((Rectangle){ x+w-48, y, 48, rh }, "del"))
        { AudioPlayButton(); UndoPush(); pendingGroupDel = gi; }
        y += rh + 2;

        int colorProp = GroupColorProp(e->kind, gi);
        for (int i = 0; i < nt; i++)
        {
            float t = times[i];
            bool sel = (selG == gi && fabsf(selT - t) <= AUTOKEY_EPS);
            Rectangle kr = { x+12, y, w-12, 18 };
            bool pressed = GuiButton(kr, GroupKeyLabel(e, gi, t));
            if (sel) DrawRectangleRec(kr, (Color){ 60, 90, 140, 120 });
            if (colorProp >= 0)
            {
                Color sc = AnimElemColorProp(e, colorProp, t); sc.a = 255;
                DrawSwatch((Rectangle){ kr.x + kr.width - 18, kr.y + 2, 14, 14 }, sc);
            }
            if (pressed) { AudioPlayButton(); SelectGroupKey(selElem, gi, t); }
            if (sel && scrollToSelKey)
            {
                float top = inspPanelRect.y + 24.0f;
                float bot = inspPanelRect.y + inspPanelRect.height - 24.0f;
                if (kr.y < top)                    inspView.scroll += top - kr.y;
                else if (kr.y + kr.height > bot)   inspView.scroll -= (kr.y + kr.height) - bot;
                scrollToSelKey = false;
            }
            y += 20;
        }
        y += 4;
    }
    if (pendingGroupDel >= 0)
    {
        if (selG == pendingGroupDel) ClearKeySelection();
        GroupDeleteTracks(e, pendingGroupDel);
        ClampSelection();
    }

    // add-track dropdown, now GROUP based: adding creates all member tracks.
    y += 4;
    int addCount = AnimGroupCountFor(e->kind);
    if (addTrackSel >= addCount) addTrackSel = 0;   // kinds differ in count
    Rectangle addR = { x, y, w-56, rh };
    if (GuiButton((Rectangle){ x+w-52, y, 52, rh }, "+track"))
    {
        AudioPlayButton(); UndoPush();
        GroupWriteKey(e, addTrackSel, playhead);    // seeds zero + playhead keys
        SelectGroupKey(selElem, addTrackSel, playhead);
    }
    // dropdown HEADER only - the open list is drawn last as an overlay so it
    // can flip above the header instead of being culled at the screen bottom.
    const AnimPropGroup *addG = AnimGroupAt(e->kind, addTrackSel);
    if (GuiButton(addR, TextFormat("%s  v", addG ? addG->name : "?")))
    {
        AudioPlayButton();
        addTrackDrop = (addTrackDrop == selElem) ? -1 : selElem;
        keyEaseDropOpen = false;
    }
    addTrackRect = addR; addTrackVisible = true;
    y += rh + 10;

    // --- key inspector: the selected GROUP key's time / per-member values / ease
    // (skipped while the add-track dropdown is open - it would draw over us)
    keyEaseVisible = false;
    float kt = 0.0f;
    int   kg = (addTrackDrop != selElem) ? SelectedGroup(e, &kt) : -1;
    if (kg >= 0)
    {
        const AnimPropGroup *g = AnimGroupAt(e->kind, kg);
        GuiLine((Rectangle){ x, y, w, 8 }, TextFormat("key  %s  @%.2f", g->name, kt));
        y += 12;

        // time: moves the WHOLE group key (every member) on commit.
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
                GroupMoveKeyTo(e, kg, kt, nt);
                SelectGroupKey(selElem, kg, nt);
            }
        }
        y += rh + gap;

        // A corner-mode element is AUTHORED by its two ends, so its keys are
        // edited the same way: the position and size groups show the four
        // endpoint sliders, writing this key's position AND size members
        // (plus rotation for a line) instead of the raw per-prop rows.
        int posG  = AnimGroupIndexOfProp(e->kind, AP_S_POS_X);
        int sizeG = AnimGroupIndexOfProp(e->kind, AP_S_W);
        bool cornerKey = (e->kind == AE_SHAPE && e->cornerMode &&
                          (kg == posG || kg == sizeG));
        TrackKeyCtx tkc = { e, kt, keyEaseSel };
        if (cornerKey)
            y = DrawCornerRows(x, y, w, rh, gap, e,
                               (PropAccess){ TrackKeyPropGet, TrackKeyPropSet, &tkc });

        // one editor per member prop (Position -> x & y; Color -> RGB + alpha).
        for (int m = 0; !cornerKey && m < g->propCount; m++)
        {
            int prop = g->props[m];
            AnimTrack *tr = AnimElemFindTrack(e, prop);
            int ki = TrackKeyNear(tr, kt);
            if (ki < 0) continue;                    // ragged: member has no key here
            AnimKey *k = &tr->keys[ki];
            if (AnimPropIsColor(prop))
            {
                GuiLabel((Rectangle){ x, y, w-24, rh },
                         TextFormat("%s (rgb)", AnimPropName(prop)));
                DrawSwatch((Rectangle){ x+w-20, y+3, 18, 18 },
                           (Color){ k->cval.r, k->cval.g, k->cval.b, 255 });
                y += rh;
                float kr=k->cval.r, kg2=k->cval.g, kb=k->cval.b; bool ch=false;
                if (EditSlider((Rectangle){ x+16, y, w-16-50, 16 }, "R", &kr, 0,255)) ch=true; y+=18;
                if (EditSlider((Rectangle){ x+16, y, w-16-50, 16 }, "G", &kg2,0,255)) ch=true; y+=18;
                if (EditSlider((Rectangle){ x+16, y, w-16-50, 16 }, "B", &kb, 0,255)) ch=true; y+=20;
                if (ch) k->cval = (Color){ (unsigned char)kr,(unsigned char)kg2,
                                           (unsigned char)kb, 255 };
            }
            else
            {
                GuiLabel((Rectangle){ x, y, 44, rh }, AnimPropName(prop));
                float v = k->value, klo, khi; PropRange(e, prop, &klo, &khi);
                if (EditSlider((Rectangle){ x+44, y, w-44-50, rh }, "", &v, klo, khi))
                    k->value = v;
                y += rh + gap;
            }
        }

        // ease dropdown header (applies to every member); its list is an overlay.
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
            GroupDeleteKeyAt(e, kg, kt);
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
    int count = easeMode ? AnimEaseCount() : AnimGroupCountFor(doc.elems[selElem].kind);
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
        const AnimPropGroup *gg = easeMode ? NULL : AnimGroupAt(doc.elems[selElem].kind, i);
        const char *nm = easeMode ? AnimEaseName(i) : (gg ? gg->name : "?");
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
            // apply the ease to every member of the selected group key.
            AnimElem *se = &doc.elems[selKeyElem];
            float et; int eg = SelectedGroup(se, &et);
            if (eg >= 0) GroupSetEaseAt(se, eg, et, picked);
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

// Fire a signal the way the GAME would fire it. A signal authored to consume
// the position parameter (AnimSignal.usesPos) is emitted AT THE MOUSE, in canvas
// fractions - the same thing main_menu.c does on a click - so the re-anchoring
// can be seen in the editor at all; one that does not is emitted bare. The
// preview's instance number is refreshed here so a re-fire always reflects the
// spinner without having to restart anything.
static void FireSignal(const AnimSignal *sg)
{
    preview.seq = previewSeq;

    if (!sg->usesPos) { SignalEmit(sg->name, NULL); return; }

    Vector2 game = ScreenStateTargetSize();
    Vector2 px   = Screen2Target(GetMousePosition());
    SignalParams p = { .pos = { px.x/game.x, px.y/game.y }, .hasPos = true };
    SignalEmit(sg->name, &p);
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
    GuiLabel((Rectangle){ x+16, y, w-16, 20 }, "SIGNALS"); y += 22;

    int pendingDel = -1;
    float bw2 = 22.0f;
    for (int i = 0; i < doc.signalCount; i++)
    {
        AnimSignal *sg = &doc.signals[i];
        Rectangle openR = { x, y, w - 2*bw2 - 4, rh };
        if (GuiButton(openR, TextFormat("%s  (%d)", sg->name, sg->targetCount)))
        { AudioPlayButton(); sigModalIdx = i; sigScroll = 0.0f;
          SigClearKeySel(); SigCloseDrops(); }   // selection belonged to the old one
        if (i == sigModalIdx) DrawRectangleRec(openR, (Color){ 90, 140, 220, 90 });

        if (GuiButton((Rectangle){ x + w - 2*bw2 - 2, y, bw2, rh }, "#131#"))
        { AudioPlayButton(); FireSignal(sg); }          // fire (play icon)
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
            // every field explicitly: the slot may hold a deleted signal's data
            sg->length = 1.0f; sg->targetCount = 0;
            sg->terminal = false; sg->usesPos = false;
            sg->usesSeq = false; sg->seqMult = 0.0f;
            sg->posParamCount = 0; sg->seqTargetCount = 0; sg->seqKeyCount = 0;
            sigModalIdx = doc.signalCount - 1; sigScroll = 0.0f;
            SigClearKeySel(); SigCloseDrops();
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
        SigClearKeySel(); SigCloseDrops();
        if      (sigModalIdx == pendingDel) sigModalIdx = -1;   // its modal closes
        else if (sigModalIdx >  pendingDel) sigModalIdx--;
        if      (edSigIdx == pendingDel) edSigIdx = -1;
        else if (edSigIdx >  pendingDel) edSigIdx--;
        ReRegisterSignals();
    }

    return (y + 4) - y0;
}

// ---------------------------------------------------------------------------
//  Signal modal: everything about ONE signal - its length and its targets.
//
//  Targets are authored in property GROUPS, the same ones the inspector's
//  tracks use (position, color, outline, ...): a group row owns the keys of all
//  its member (element, property) targets at shared u values, and the key
//  editor below the list edits the selected group key across every member.
//
//  Two forms:
//    FULL    (editing)  the whole editor for the signal
//    SHRUNK  (playing)  a small bar with just Fire + Close, so the signal can
//                       be triggered at any point during playback to see how
//                       it blends from the live scene. It deliberately does
//                       NOT close on playback, unlike the PROMPT_* modals.
// ---------------------------------------------------------------------------
// Position slots an element exposes to a Mouse-Position binding: a text/normal
// shape has one (its center); a corners-mode shape has two (its corners); a
// global has none. Fills `names` with the slot labels and returns the count.
static int SigElemSlots(const AnimElem *e, const char *names[2])
{
    if (e->kind == AE_GLOBAL) return 0;
    if (e->kind == AE_SHAPE && e->cornerMode)
    { names[0] = "P0"; names[1] = "P1"; return 2; }
    names[0] = "center"; return 1;
}

// The "--params--" section: one row per position slot of every element. A bound
// slot shows its keys (each easing the slot to mouse + per-key offset); an
// unbound one shows "+ bind". Drawn inside the modal's scrolled list; returns
// the new y.
static float DrawSigParamsSection(AnimSignal *sg, Rectangle list, float ly,
                                  float rh, float gap)
{
    DrawRectangleRec((Rectangle){ list.x, ly, list.width, rh },
                     (Color){ 44, 52, 60, 255 });
    GuiLabel((Rectangle){ list.x+6, ly, list.width-12, rh },
             "params  (mouse position -> slot + offset)");
    ly += rh + 2;

    int delBind = -1, delKeyBind = -1, delKeyIdx = -1;

    for (int e = 0; e < doc.elemCount; e++)
    {
        const char *sn[2]; int ns = SigElemSlots(&doc.elems[e], sn);
        for (int s = 0; s < ns; s++)
        {
            int bi = SigPosFind(sg, e, s);
            GuiLabel((Rectangle){ list.x+14, ly, list.width-160, rh },
                     TextFormat("%s . %s", doc.elems[e].name, sn[s]));
            if (bi < 0)
            {
                bool full = sg->posParamCount >= ANIM_SIG_POS_MAX;
                if (full) GuiSetState(STATE_DISABLED);
                if (GuiButton((Rectangle){ list.x+list.width-70, ly, 66, rh },
                              full ? "full" : "+ bind") && !full)
                { AudioPlayButton(); UndoPush(); SigPosAdd(sg, e, s);
                  sigPosSelElem = e; sigPosSelSlot = s; sigPosSelU = 1.0f; }
                if (full) GuiSetState(STATE_NORMAL);
                ly += rh + 2;
                continue;
            }
            AnimSigPosParam *pp = &sg->posParams[bi];
            if (GuiButton((Rectangle){ list.x+list.width-104, ly, 50, rh }, "+key"))
            { AudioPlayButton(); UndoPush();
              float nu = SigPosFreeU(pp, sigLastU); SigPosWriteKey(pp, nu);
              sigPosSelElem = e; sigPosSelSlot = s; sigPosSelU = nu; }
            if (GuiButton((Rectangle){ list.x+list.width-50, ly, 50, rh }, "del"))
            { AudioPlayButton(); UndoPush(); delBind = bi; }
            ly += rh + 2;

            for (int k = 0; k < pp->keyCount; k++)
            {
                float u = pp->keys[k].t;
                bool sel = (sigPosSelElem == e && sigPosSelSlot == s &&
                            fabsf(sigPosSelU - u) <= SIG_U_EPS);
                Rectangle kr = { list.x+26, ly, list.width-26, 20 };
                if (GuiButton(kr, TextFormat("%.2f   +(%.2f, %.2f)   %s", u,
                              pp->keys[k].offX, pp->keys[k].offY,
                              AnimEaseName(pp->keys[k].ease))))
                { AudioPlayButton(); sigPosSelElem = e; sigPosSelSlot = s; sigPosSelU = u; }
                if (sel) DrawRectangleRec(kr, (Color){ 60, 90, 140, 120 });
                ly += 22;
            }

            if (sigPosSelElem == e && sigPosSelSlot == s)
            {
                int k = SigPosKeyNear(pp, sigPosSelU);
                if (k >= 0)
                {
                    AnimPosKey *kk = &pp->keys[k];
                    float u = kk->t, ox = kk->offX, oy = kk->offY;
                    GuiLabel((Rectangle){ list.x+14, ly, 40, rh }, "u");
                    if (EditSlider((Rectangle){ list.x+58, ly, list.width-58-56, rh },
                                   "", &u, 0.0f, 1.0f))
                    { kk->t = u; SigPosSortKeys(pp); sigPosSelU = u; sigLastU = u; }
                    ly += rh + gap;
                    GuiLabel((Rectangle){ list.x+14, ly, 40, rh }, "off x");
                    if (EditSlider((Rectangle){ list.x+58, ly, list.width-58-56, rh },
                                   "", &ox, -1.0f, 1.0f)) kk->offX = ox;
                    ly += rh + gap;
                    GuiLabel((Rectangle){ list.x+14, ly, 40, rh }, "off y");
                    if (EditSlider((Rectangle){ list.x+58, ly, list.width-58-56, rh },
                                   "", &oy, -1.0f, 1.0f)) kk->offY = oy;
                    ly += rh + gap;
                    GuiLabel((Rectangle){ list.x+14, ly, 40, rh }, "ease");
                    if (GuiButton((Rectangle){ list.x+58, ly, 120, rh },
                                  AnimEaseName(kk->ease)))
                    { AudioPlayButton(); UndoPush();
                      kk->ease = (kk->ease + 1) % AnimEaseCount(); }
                    if (GuiButton((Rectangle){ list.x+list.width-50, ly, 50, rh }, "del"))
                    { AudioPlayButton(); UndoPush(); delKeyBind = bi; delKeyIdx = k; }
                    ly += rh + gap;
                }
            }
        }
    }
    ly += gap;

    if (delKeyBind >= 0)
    { SigPosRemoveKeyAt(&sg->posParams[delKeyBind], delKeyIdx); }
    else if (delBind >= 0)
    { SigPosRemoveAt(sg, delBind); sigPosSelElem = -1; }
    return ly;
}

// The "--sequence--" section: a per-instance multiplier, a checkbox per scalar
// property GROUP marking it a sequence target, and an envelope of keys. The
// offset added to each target is seq * mult * envelope(u). Returns the new y.
static float DrawSigSequenceSection(AnimSignal *sg, Rectangle list, float ly,
                                    float rh, float gap)
{
    DrawRectangleRec((Rectangle){ list.x, ly, list.width, rh },
                     (Color){ 44, 52, 60, 255 });
    GuiLabel((Rectangle){ list.x+6, ly, 200, rh }, "sequence  (seq x mult)");
    GuiLabel((Rectangle){ list.x+list.width-210, ly, 34, rh }, "mult");
    EditSlider((Rectangle){ list.x+list.width-172, ly+2, 168, rh-4 }, "",
               &sg->seqMult, -30.0f, 30.0f);
    ly += rh + 2;

    // which properties the offset adds to - one checkbox per scalar group
    for (int e = 0; e < doc.elemCount; e++)
    {
        int kind = doc.elems[e].kind, grpN = AnimGroupCountFor(kind);
        for (int gi = 0; gi < grpN; gi++)
        {
            if (!SigSeqGroupHasScalar(kind, gi)) continue;
            const AnimPropGroup *g = AnimGroupAt(kind, gi);
            bool on = SigSeqGroupOn(sg, e, kind, gi), want = on;
            GuiCheckBox((Rectangle){ list.x+20, ly+2, 16, 16 },
                        TextFormat("%s . %s", doc.elems[e].name, g->name), &want);
            if (want != on)
            { AudioPlayButton(); UndoPush(); SigSeqGroupSet(sg, e, kind, gi, want); }
            ly += rh;
        }
    }

    // the envelope keys
    if (GuiButton((Rectangle){ list.x+14, ly, 60, rh }, "+ key"))
    { AudioPlayButton(); UndoPush();
      float nu = SigSeqFreeU(sg, sigLastU); SigSeqWriteKey(sg, nu); sigSeqSelU = nu; }
    ly += rh + 2;

    int delKey = -1;
    for (int k = 0; k < sg->seqKeyCount; k++)
    {
        float u = sg->seqKeys[k].t;
        bool sel = fabsf(sigSeqSelU - u) <= SIG_U_EPS;
        Rectangle kr = { list.x+26, ly, list.width-26, 20 };
        if (GuiButton(kr, TextFormat("%.2f   amt %.2f   %s", u, sg->seqKeys[k].amt,
                                     AnimEaseName(sg->seqKeys[k].ease))))
        { AudioPlayButton(); sigSeqSelU = u; }
        if (sel) DrawRectangleRec(kr, (Color){ 60, 90, 140, 120 });
        ly += 22;
    }
    int sk = SigSeqKeyNear(sg, sigSeqSelU);
    if (sk >= 0)
    {
        AnimSeqKey *kk = &sg->seqKeys[sk];
        float u = kk->t, amt = kk->amt;
        GuiLabel((Rectangle){ list.x+14, ly, 40, rh }, "u");
        if (EditSlider((Rectangle){ list.x+58, ly, list.width-58-56, rh }, "",
                       &u, 0.0f, 1.0f))
        { kk->t = u; SigSeqSortKeys(sg); sigSeqSelU = u; sigLastU = u; }
        ly += rh + gap;
        GuiLabel((Rectangle){ list.x+14, ly, 40, rh }, "amt");
        if (EditSlider((Rectangle){ list.x+58, ly, list.width-58-56, rh }, "",
                       &amt, 0.0f, 1.0f)) kk->amt = amt;
        ly += rh + gap;
        GuiLabel((Rectangle){ list.x+14, ly, 40, rh }, "ease");
        if (GuiButton((Rectangle){ list.x+58, ly, 120, rh }, AnimEaseName(kk->ease)))
        { AudioPlayButton(); UndoPush(); kk->ease = (kk->ease + 1) % AnimEaseCount(); }
        if (GuiButton((Rectangle){ list.x+list.width-50, ly, 50, rh }, "del"))
        { AudioPlayButton(); UndoPush(); delKey = sk; }
        ly += rh + gap;
    }
    ly += gap;
    if (delKey >= 0) { SigSeqRemoveKeyAt(sg, delKey); sigSeqSelU = -1.0f; }
    return ly;
}

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
        { AudioPlayButton(); FireSignal(sg); }
        if (GuiButton((Rectangle){ m.x+mw-60, m.y+7, 52, 26 }, "Close"))
        { AudioPlayButton(); sigModalIdx = -1; }
        return;
    }

    // --- full form --------------------------------------------------------
    float mw = 620, mh = 460;
    Rectangle m = { (W-mw)/2, (H-mh)/2, mw, mh };
    DrawRectangle(0, 0, (int)W, (int)H, (Color){ 0, 0, 0, 120 });
    DrawRectangleRec(m, (Color){ 40, 42, 48, 255 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 90, 94, 104, 255 });

    float rh = 24.0f, gap = 6.0f;
    float x = m.x + 14, w = mw - 28, y = m.y + 12;

    // One of this modal's own dropdowns is expanded over these rows: lock them
    // so a click lands on the overlay list only. Without this the row widget
    // underneath the expanded list consumes the click too (picking a group
    // would fire whatever button sat beneath the list item).
    bool sigDropOpen = sigDropMode != SIGDROP_NONE;
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

    // Position parameter: does THIS signal consume the emit's location? Placing
    // a transition where it fired only makes sense for some animations (a ripple
    // at the mouse, yes; a screen wipe, no), so it is authored here rather than
    // assumed. When on, the "--params--" section below binds mouse position to
    // element position slots, and the Fire buttons emit at the mouse.
    bool usesPos = sg->usesPos;
    GuiCheckBox((Rectangle){ x+w-206, y, 16, 16 }, "uses position param (fire at mouse)",
                &usesPos);
    if (usesPos != sg->usesPos)
    {
        AudioPlayButton();
        UndoPush();
        sg->usesPos = usesPos;
    }
    // Sequence: does THIS signal consume the instance's sequence number? When on
    // the "--sequence--" section below offsets chosen properties by seq * mult.
    bool usesSeq = sg->usesSeq;
    GuiCheckBox((Rectangle){ x, y, 16, 16 }, "uses sequence", &usesSeq);
    if (usesSeq != sg->usesSeq)
    { AudioPlayButton(); UndoPush(); sg->usesSeq = usesSeq; }
    y += 20;

    // Instance number this preview stands in for, feeding the sequence offset.
    // Plain -/+ buttons: it is a preview control, not authored data, so it is
    // deliberately outside undo.
    GuiLabel((Rectangle){ x, y, 90, 18 }, TextFormat("instance %d", previewSeq));
    if (GuiButton((Rectangle){ x+92, y, 22, 18 }, "-"))
    { AudioPlayButton(); if (previewSeq > 0) previewSeq--; preview.seq = previewSeq; }
    if (GuiButton((Rectangle){ x+116, y, 22, 18 }, "+"))
    { AudioPlayButton(); previewSeq++; preview.seq = previewSeq; }
    y += 22;
    GuiLine((Rectangle){ x, y, w, 8 }, "targets"); y += 12;

    // --- scrolling target list -------------------------------------------
    Rectangle list = { x, y, w, m.y + mh - 48 - y };
    bool ctrlDown = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (!ctrlDown && CheckCollisionPointRec(GetMousePosition(), list))
        sigScroll += GetMouseWheelMove() * 24.0f;

    int  pendingDelElem = -1, pendingDelGroup = -1;  // group whose targets go
    bool pendingKeyDel = false;                      // selected group key goes

    // Same trap as PanelScroll: scissor hides the rows scrolled past the top of
    // `list` but leaves them clickable, so hitting the signal name/length row
    // above also fired whatever row was hidden under the cursor.
    bool inList = CheckCollisionPointRec(GetMousePosition(), list);
    if (!inList && !sigDropOpen) GuiLock();

    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    float ly = list.y + sigScroll;

    // Authored parameter sections sit ABOVE the plain targets (they scroll with
    // the list). Each is only shown when its checkbox opts the signal in.
    if (sg->usesPos) ly = DrawSigParamsSection(sg, list, ly, rh, gap);
    if (sg->usesSeq) ly = DrawSigSequenceSection(sg, list, ly, rh, gap);
    if (sg->usesPos || sg->usesSeq)
    { GuiLine((Rectangle){ list.x, ly, list.width, 8 }, "targets"); ly += 12; }

    // The list is driven by the DOCUMENT's elements, not by the signal's
    // targets: every element is always shown, and its authored tracks - one row
    // per property GROUP, exactly like the inspector's - are nested under it.
    // Adding a track is therefore a per-element action and the element itself
    // never has to be picked from a dropdown.
    for (int e = 0; e < doc.elemCount; e++)
    {
        int kind = doc.elems[e].kind;

        // element header: name + a track adder scoped to this element
        DrawRectangleRec((Rectangle){ list.x, ly, list.width, rh },
                         (Color){ 52, 55, 62, 255 });
        GuiLabel((Rectangle){ list.x+6, ly, 180, rh }, doc.elems[e].name);

        // Show the adder greyed rather than hidden when the target pool is
        // full: a missing button read as "this element can't have tracks".
        bool sigFull = sg->targetCount >= ANIM_SIG_TARGETS_MAX;
        Rectangle addR = { list.x+list.width-86, ly, 82, rh };
        if (sigDropMode == SIGDROP_ADD && sigDropElem == e) sigDropHdr = addR;
        if (sigFull) GuiSetState(STATE_DISABLED);
        bool addHit = GuiButton(addR, sigFull ? "full" : "+ track");
        if (sigFull) GuiSetState(STATE_NORMAL);
        if (!sigFull && addHit)
        {
            AudioPlayButton();
            if (sigDropMode == SIGDROP_ADD && sigDropElem == e) SigCloseDrops();
            else { sigDropMode = SIGDROP_ADD; sigDropElem = e; sigDropHdr = addR; }
        }
        ly += rh + 2;

        int grpN = AnimGroupCountFor(kind);
        for (int gi = 0; gi < grpN; gi++)
        {
            if (!SigGroupHasTarget(sg, e, gi)) continue;   // only authored ones
            const AnimPropGroup *g = AnimGroupAt(kind, gi);
            float times[SIG_TIMES_MAX];
            int   nt = SigGroupKeyTimes(sg, e, gi, times);

            GuiLabel((Rectangle){ list.x+14, ly, list.width-160, rh },
                     TextFormat("%s (%d)", g->name, nt));

            // +key: one key across every member of the group. u seeds from the
            // last one the user set on ANY signal key, not 1.0: the common case
            // is placing the same beat across several tracks.
            if (GuiButton((Rectangle){ list.x+list.width-104, ly, 50, rh }, "+key"))
            {
                AudioPlayButton(); UndoPush();
                float nu = SigGroupFreeU(sg, e, gi, sigLastU);
                SigGroupWriteKey(sg, e, gi, nu);
                sigSelElem = e; sigSelGroup = gi; sigSelU = nu;
            }
            if (GuiButton((Rectangle){ list.x+list.width-50, ly, 50, rh }, "del"))
            { AudioPlayButton(); UndoPush(); pendingDelElem = e; pendingDelGroup = gi; }
            ly += rh + 2;

            int colorProp = GroupColorProp(kind, gi);
            for (int i = 0; i < nt; i++)
            {
                float u = times[i];
                bool sel = (sigSelElem == e && sigSelGroup == gi &&
                            fabsf(sigSelU - u) <= SIG_U_EPS);
                Rectangle kr = { list.x+26, ly, list.width-26, 20 };
                bool pressed = GuiButton(kr, SigGroupKeyLabel(sg, e, gi, u));
                if (sel) DrawRectangleRec(kr, (Color){ 60, 90, 140, 120 });
                if (colorProp >= 0)
                {
                    AnimSigTarget *ct = SigFindTarget(sg, e, colorProp);
                    int ck = SigTargetKeyNear(ct, u);
                    if (ck >= 0)
                    {
                        Color sc = ct->keys[ck].cval; sc.a = 255;
                        DrawSwatch((Rectangle){ kr.x+kr.width-20, kr.y+2, 16, 16 }, sc);
                    }
                }
                if (pressed)
                { AudioPlayButton(); sigSelElem = e; sigSelGroup = gi; sigSelU = u; }
                ly += 22;
            }
            ly += 4;
        }
        ly += gap;
    }

    // --- the selected group key: u / per-member values / shared ease -------
    // Drawn inside the same scrolled content as the rows above, the way the
    // inspector puts its key editor under the track list.
    if (sigSelElem >= 0 && sigSelElem < doc.elemCount && sigSelGroup >= 0)
    {
        AnimElem *el = &doc.elems[sigSelElem];
        const AnimPropGroup *g = AnimGroupAt(el->kind, sigSelGroup);
        if (!g || !SigGroupHasTarget(sg, sigSelElem, sigSelGroup)) SigClearKeySel();
        else
        {
            GuiLine((Rectangle){ list.x, ly, list.width, 8 },
                    TextFormat("key  %s.%s  @%.2f", el->name, g->name, sigSelU));
            ly += 12;

            // u moves the WHOLE group key (every member) at once
            GuiLabel((Rectangle){ list.x+14, ly, 54, rh }, "u");
            float u = sigSelU;
            if (EditSlider((Rectangle){ list.x+68, ly, list.width-68-56, rh }, "",
                           &u, 0.0f, 1.0f))
            {
                SigGroupMoveKeyTo(sg, sigSelElem, sigSelGroup, sigSelU, u);
                sigSelU = u; sigLastU = u;      // remember for the next +key
            }
            ly += rh + gap;

            // ease: read off the first member that has this key, applied to all
            int ease = 0;
            for (int mI = 0; mI < g->propCount; mI++)
            {
                AnimSigTarget *rt = SigFindTarget(sg, sigSelElem, g->props[mI]);
                int rk = SigTargetKeyNear(rt, sigSelU);
                if (rk >= 0) { ease = rt->keys[rk].ease; break; }
            }

            // A corner-mode element is AUTHORED by its two ends, so its keys are
            // edited the same way: the position and size groups both show the
            // four endpoint sliders, writing this key's position AND size
            // members (plus rotation for a line) through SigKeyPropSet.
            int posG  = AnimGroupIndexOfProp(el->kind, AP_S_POS_X);
            int sizeG = AnimGroupIndexOfProp(el->kind, AP_S_W);
            SigKeyCtx skc = { sg, sigSelElem, sigSelU, ease };
            if (el->kind == AE_SHAPE && el->cornerMode &&
                (sigSelGroup == posG || sigSelGroup == sizeG))
            {
                ly = DrawCornerRows(list.x+14, ly, list.width-28, rh, gap, el,
                                    (PropAccess){ SigKeyPropGet, SigKeyPropSet, &skc });
            }
            else
            {
                // one editor per member prop (position -> x & y; color -> RGB + alpha)
                for (int mI = 0; mI < g->propCount; mI++)
                {
                    int prop = g->props[mI];
                    AnimSigTarget *tg = SigFindTarget(sg, sigSelElem, prop);
                    int k = SigTargetKeyNear(tg, sigSelU);
                    if (k < 0) continue;             // ragged: member has no key here
                    AnimKey *kk = &tg->keys[k];
                    if (AnimPropIsColor(prop))
                    {
                        GuiLabel((Rectangle){ list.x+14, ly, list.width-40, rh },
                                 TextFormat("%s (rgb)", AnimPropName(prop)));
                        DrawSwatch((Rectangle){ list.x+list.width-24, ly+3, 18, 18 },
                                   (Color){ kk->cval.r, kk->cval.g, kk->cval.b, 255 });
                        ly += rh;
                        float cr=kk->cval.r, cg=kk->cval.g, cb=kk->cval.b, sw=list.width-30-50;
                        bool ch=false;
                        if (EditSlider((Rectangle){ list.x+30, ly, sw, 16 }, "R", &cr, 0,255)) ch=true; ly+=18;
                        if (EditSlider((Rectangle){ list.x+30, ly, sw, 16 }, "G", &cg, 0,255)) ch=true; ly+=18;
                        if (EditSlider((Rectangle){ list.x+30, ly, sw, 16 }, "B", &cb, 0,255)) ch=true; ly+=20;
                        if (ch) kk->cval = (Color){ (unsigned char)cr,(unsigned char)cg,
                                                    (unsigned char)cb, 255 };
                    }
                    else
                    {
                        GuiLabel((Rectangle){ list.x+14, ly, 54, rh }, AnimPropName(prop));
                        // the element's CURRENT units, not raw fractions: a
                        // px-absolute element gets pixel bounds here too.
                        float v = kk->value, klo, khi; PropRange(el, prop, &klo, &khi);
                        if (EditSlider((Rectangle){ list.x+68, ly, list.width-68-56, rh },
                                       "", &v, klo, khi))
                            kk->value = v;
                        ly += rh + gap;
                    }
                }
            }

            GuiLabel((Rectangle){ list.x+14, ly, 54, rh }, "ease");
            Rectangle easeR = { list.x+68, ly, list.width-68-110, rh };
            if (sigDropMode == SIGDROP_EASE) sigDropHdr = easeR;
            if (GuiButton(easeR, TextFormat("%s  v", AnimEaseName(ease))))
            {
                AudioPlayButton();
                if (sigDropMode == SIGDROP_EASE) SigCloseDrops();
                else { sigDropMode = SIGDROP_EASE; sigDropHdr = easeR; }
            }
            if (GuiButton((Rectangle){ list.x+list.width-50, ly, 50, rh }, "del"))
            { AudioPlayButton(); UndoPush(); pendingKeyDel = true; }
            ly += rh + gap;
        }
    }
    // the ease list hangs off the key editor: it cannot outlive the selection
    if (sigDropMode == SIGDROP_EASE && sigSelElem < 0) SigCloseDrops();

    EndScissorMode();
    if (!inList && !sigDropOpen) GuiUnlock();   // footer/header stay live

    // measured content height -> clamp the scroll
    float contentH = (ly - (list.y + sigScroll));
    float maxScroll = contentH - list.height;
    if (maxScroll < 0) maxScroll = 0;
    if (sigScroll < -maxScroll) sigScroll = -maxScroll;
    if (sigScroll > 0) sigScroll = 0;

    // deferred edits (same reason as everywhere else: mid-draw array shifts
    // re-fire the next row's button on the same rect this frame)
    if (pendingDelGroup >= 0)
    {
        SigGroupDeleteTargets(sg, pendingDelElem, pendingDelGroup);
        if (sigSelElem == pendingDelElem && sigSelGroup == pendingDelGroup)
            SigClearKeySel();
        SigCloseDrops();
    }
    else if (pendingKeyDel)
    {
        SigGroupDeleteKeyAt(sg, sigSelElem, sigSelGroup, sigSelU);
        SigClearKeySel();
        SigCloseDrops();
    }

    // --- footer -----------------------------------------------------------
    float bh = 28, by = m.y + mh - bh - 12;
    if (GuiButton((Rectangle){ x, by, 70, bh }, "Fire"))
    { AudioPlayButton(); FireSignal(sg); }
    GuiLabel((Rectangle){ x+80, by, mw-200, bh }, "fires from the CURRENT pose");
    if (GuiButton((Rectangle){ m.x+mw-84, by, 70, bh }, "Close"))
    { AudioPlayButton(); sigModalIdx = -1; edSigIdx = -1;
      SigCloseDrops(); SigClearKeySel(); }

    if (sigDropOpen) GuiUnlock();   // the overlay list itself draws unlocked
}

// The signal modal's own dropdown overlay (group adder or key ease). Drawn
// after the modal so it sits on top of it, mirroring how DrawDropdownOverlays
// relates to the main inspector. The header rect is recorded while the modal
// draws (sigDropHdr), so nothing here has to re-derive the row layout.
static void DrawSignalModalOverlays()
{
    if (sigModalIdx < 0 || sigModalIdx >= doc.signalCount) return;
    if (playing || !AnimSignalPlayerDone(&preview)) return;    // shrunk: no lists
    if (sigDropMode == SIGDROP_NONE) return;

    AnimSignal *sg = &doc.signals[sigModalIdx];
    ScreenState *ss = ScreenStateGet();
    float H = (float)ss->height;

    bool addMode = (sigDropMode == SIGDROP_ADD);
    if (addMode && (sigDropElem < 0 || sigDropElem >= doc.elemCount))
    { SigCloseDrops(); return; }

    int kind  = addMode ? doc.elems[sigDropElem].kind : 0;
    int count = addMode ? AnimGroupCountFor(kind) : AnimEaseCount();
    if (count <= 0) return;

    Rectangle hdr = sigDropHdr;
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
        const AnimPropGroup *g = addMode ? AnimGroupAt(kind, i) : NULL;
        // a 3-prop group can be out of target slots while a 1-prop one still
        // fits, so the entries grey out individually rather than the adder
        bool fits = !addMode || SigGroupFits(sg, sigDropElem, i);
        if (!fits) GuiSetState(STATE_DISABLED);
        if (GuiButton(rr, addMode ? (g ? g->name : "?") : AnimEaseName(i)) && fits)
            picked = i;
        if (!fits) GuiSetState(STATE_NORMAL);
    }

    if (picked >= 0)
    {
        AudioPlayButton(); UndoPush();
        if (addMode)
        {
            // adding a track = creating every member target of the group; the
            // keys come later through the group's own +key.
            SigGroupAddTargets(sg, sigDropElem, picked);
            sigSelElem = sigDropElem; sigSelGroup = picked; sigSelU = sigLastU;
        }
        else SigGroupSetEaseAt(sg, sigSelElem, sigSelGroup, sigSelU, picked);
        SigCloseDrops();
    }
    else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
             !CheckCollisionPointRec(GetMousePosition(), bg) &&
             !CheckCollisionPointRec(GetMousePosition(), hdr))
    { SigCloseDrops(); }       // click-away closes
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
    DrawRectangleRec(bar, Fade((Color){ 24, 26, 30, 255 }, PanelAlpha(timelineAlphaMode) / 255.0f));
    DrawRectangleLinesEx(bar, 1.0f, (Color){ 70, 74, 84, 255 });
    // transparency toggle at the timeline's top-left corner
    if (!thin) DrawPanelAlphaToggle(x + 4, y + 4, &timelineAlphaMode, guiLocked);

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
        // one lane per GROUP that has tracks; a diamond per union key time.
        int vis[16], vn = 0, grpN = AnimGroupCountFor(e->kind);
        for (int gi = 0; gi < grpN && vn < 16; gi++)
            if (GroupHasTrack(e, gi)) vis[vn++] = gi;
        if (vn == 0)
            DrawText("(no tracks - add one in the inspector to key frames)",
                     (int)(x + w*0.5f - 130), (int)(y + h*0.5f - 5), 10,
                     (Color){ 110, 116, 128, 255 });
        float selT = 0.0f;
        int   selG = SelectedGroup(e, &selT);
        float rowH = (h - 24) / (float)(vn > 0 ? vn : 1);
        for (int r0 = 0; r0 < vn; r0++)
        {
            int gi = vis[r0];
            const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
            float ry = y + 4 + r0*rowH + rowH*0.5f;
            if (r0 & 1)
                DrawRectangleRec((Rectangle){ x+1, y+4 + r0*rowH, w-2, rowH },
                                 (Color){ 255, 255, 255, 6 });
            DrawLine((int)trackLeft, (int)ry, (int)(x+w-padR), (int)ry,
                     (Color){ 45, 48, 56, 255 });
            DrawText(g ? g->name : "?", (int)x+2, (int)ry-5, 10, (Color){130,136,148,255});

            int colorProp = GroupColorProp(e->kind, gi);
            float times[GROUP_TIMES_MAX];
            int   nt = GroupKeyTimes(e, gi, times);
            for (int i = 0; i < nt; i++)
            {
                float t = times[i];
                float kx = T2X(t);
                Rectangle hit = { kx-10, ry-10, 20, 20 };
                bool hot = CheckCollisionPointRec(mouse, hit);
                bool sel = (selG == gi && fabsf(selT - t) <= AUTOKEY_EPS);
                float r = hot ? 9.0f : 7.0f;
                Color ring = sel ? (Color){255,210,90,255}
                           : hot ? (Color){255,255,255,255} : (Color){15,16,20,255};
                Color fill;
                if (colorProp >= 0)
                { Color c = AnimElemColorProp(e, colorProp, t); fill = (Color){c.r,c.g,c.b,255}; }
                else fill = sel ? (Color){255,255,255,255} : (Color){120,180,240,255};
                DrawDiamond(kx, ry, r + 2.0f, ring);
                DrawDiamond(kx, ry, r, fill);

                if (hot && press)
                {
                    UndoPush();                       // once per drag gesture
                    dragKeyElem = selElem; dragKeyGroup = gi; dragKeyTime = t;
                    SelectGroupKey(selElem, gi, t);
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

    if (dragKeyElem >= 0 && dragKeyGroup >= 0 && IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        // continuous drag: the whole GROUP key follows the mouse - every member
        // key moves together so pos_x/pos_y (etc.) stay locked at one time.
        AnimElem *de = &doc.elems[dragKeyElem];
        float nt = X2T(mouse.x);
        if (nt < 0) nt = 0; if (nt > dur) nt = dur;
        GroupMoveKeyTo(de, dragKeyGroup, dragKeyTime, nt);
        dragKeyTime = nt;
        SelectGroupKey(dragKeyElem, dragKeyGroup, nt);
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
      dragKeyGroup = -1; dragIntro = false; dragOutro = false; }

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

    // The clock group (dur slider + length readout + the smooth-loop pair) is
    // the widest block, and "Back" is pinned to the right edge. If they would
    // collide, wrap the group onto a second row instead of letting it run under
    // the button. Measured at its WIDEST (blend slider shown) so the row does
    // not re-wrap the moment "smooth" is ticked.
    float dlw = TextW("dur", 6*s);
    float smw = cb + TextW("smooth", 6*s) + gap + 80*s + valW;
    float clockW = dlw + gap + 120*s + valW + grp + TextW("60.00 (60.00)", 10*s)
                 + grp + smw;
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

    // SMOOTH LOOP: how the cycle wraps. With it on, the last `blend` seconds of
    // every loop ease back into the pose at the loop start, so the last key
    // lerps into the first one instead of cutting. Only visible in the preview
    // while the timeline is actually playing AND looping (see Draw).
    // Snapshot before writing, like the other authoring toggles.
    bool smooth = doc.loopSmooth;
    GuiCheckBox((Rectangle){ gx, y+(rh-cb)*0.5f, cb, cb }, "smooth", &smooth);
    if (smooth != doc.loopSmooth)
    { AudioPlayButton(); UndoPush(); doc.loopSmooth = smooth; }
    gx += cb + TextW("smooth", 6*s) + gap;

    if (doc.loopSmooth)
    {
        // Clamped to the cycle at draw time too, but keeping the authored value
        // sane here means the number the user reads is the one that applies.
        if (EditSlider((Rectangle){ gx, y, 80*s, rh }, "", &doc.loopBlend, 0.0f, 2.0f))
        {
            float cycle = AnimDocPlayLen(&doc);
            if (doc.loopBlend > cycle) doc.loopBlend = cycle;
        }
    }
    gx += 80*s + valW + grp;

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
    float rightW = 380.0f;
    float bottomH = 180.0f;
    float thinH = 26.0f;        // timeline strip height while playing

    // a slider drag gesture (one undo snapshot) ends when the button comes up.
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
    {
        sliderGestureOpen = false;
        finePressActive = false; fineSticky = false; finePressRect = (Rectangle){0};
    }

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
        DrawPanelBG(elemPanel, elemView.alphaMode);
        DrawPanelBG(sigPanel,  sigView.alphaMode);
        DrawPanelBG(rightPanel, inspView.alphaMode);

        // Ctrl+wheel is reserved for stepping the hovered slider, not scrolling.
        elemView.contentH = PanelScroll(&elemView, elemPanel, DrawElementList);
        sigView.contentH  = PanelScroll(&sigView,  sigPanel,  DrawSignalList);

        inspPanelRect = rightPanel;                 // key-reveal scrolling uses it
        inspView.contentH = PanelScroll(&inspView, rightPanel, DrawInspector);

        // transparency toggles: drawn after content, pinned to each panel's
        // top-left corner (outside the scroll scissor) so they never scroll away.
        DrawPanelAlphaToggle(elemPanel.x + 4,  elemPanel.y + 4,  &elemView.alphaMode, guiLocked);
        DrawPanelAlphaToggle(sigPanel.x + 4,   sigPanel.y + 4,   &sigView.alphaMode,  guiLocked);
        DrawPanelAlphaToggle(rightPanel.x + 4, rightPanel.y + 4, &inspView.alphaMode, guiLocked);
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
