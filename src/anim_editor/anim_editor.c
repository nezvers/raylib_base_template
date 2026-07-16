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
#include "../signal/signal.h"
#include "../signal/anim_signal.h"
#include <stddef.h>
#include <stdlib.h>
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
#define SAVE_PATH "anim_doc.cfg"   // relative to CWD, like settings.cfg
#define UNDO_MAX  16
#define AUTOKEY_EPS 0.02f          // playhead-to-key snap when auto-keying (s)

static AnimDoc doc;                 // the working document
static AnimPlayer preview;          // plays the doc for signal-fire testing

static int  selElem = -1;           // primary selected element (-1 = none)
static bool multiSel[ANIM_ELEMS_MAX];   // additional selected elements

// timeline / playback
static float playhead = 0.0f;       // scrubber time, 0..doc.duration
static bool  playing  = false;      // timeline play/pause
static bool  loopPlay = true;

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
static bool autoKey = true;

// one UndoPush per slider drag gesture (cleared on mouse release in Gui())
static bool sliderGestureOpen = false;

// dragging on the timeline
static bool dragPlayhead = false;
static int  dragKeyElem = -1, dragKeyTrack = -1, dragKeyIdx = -1;

// ---------------------------------------------------------------------------
//  Undo helpers: snapshot the doc before a mutating edit.
// ---------------------------------------------------------------------------
static void UndoPush()
{
    undoBuf[undoHead] = doc;                    // value copy of the whole doc
    undoHead = (undoHead + 1) % UNDO_MAX;
    if (undoCount < UNDO_MAX - 1) undoCount++;
    redoCount = 0;                              // a new edit invalidates redo
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
}

// Bindings capture a signal's name/dir/section by value at register time, so
// any change to doc.signals (or an undo swapping the doc) needs a re-register.
static void ReRegisterSignals()
{
    AnimSignalUnregister(&doc, &preview);
    AnimSignalRegister(&doc, &preview);
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
    AnimSignalRegister(&doc, &preview);
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

    // one signal so the doc is playable straight away.
    TextCopy(doc.signals[0].name, "enter");
    doc.signals[0].dir = ANIM_FWD;
    doc.signals[0].sectionStart = 0.0f;
    doc.signals[0].sectionEnd   = doc.duration;
    doc.signalCount = 1;

    selElem = 0;
}

// ===========================================================================
//  State lifecycle
// ===========================================================================
static void Enter()
{
    // Load a previously saved doc if present, else start on a demo.
    if (!AnimDocLoad(&doc, SAVE_PATH) || doc.elemCount == 0)
        MakeStarterDoc();

    selElem  = doc.elemCount > 0 ? 0 : -1;
    for (int i = 0; i < ANIM_ELEMS_MAX; i++) multiSel[i] = false;
    ClearKeySelection();
    edSigIdx = -1; sliderGestureOpen = false;
    playhead = 0.0f; playing = false;
    undoCount = redoCount = 0; undoHead = 0;

    // register the doc's signals so the "Fire" buttons drive the preview player.
    preview = (AnimPlayer){0};
    AnimSignalRegister(&doc, &preview);
}

static void Exit()
{
    AnimSignalUnregister(&doc, &preview);
}

static void Update()
{
    float dt = GetFrameTime();

    // Timeline playback (independent of signal-fired preview).
    if (playing)
    {
        playhead += dt;
        if (playhead >= doc.duration)
            playhead = loopPlay ? fmodf(playhead, doc.duration > 0 ? doc.duration : 1.0f)
                                : doc.duration;
    }

    // The signal-fired preview player advances too; when it is playing it drives
    // the visible time so "Fire" shows the real signal playback.
    if (!AnimPlayerDone(&preview))
    {
        AnimPlayerUpdate(&preview, dt);
        playhead = AnimPlayerSampleTime(&preview);
    }

    // ESC returns to the menu.
    if (IsKeyPressed(KEY_ESCAPE))
        AppStateTransition(&app_state_main_menu);

    // Ctrl+Z / Ctrl+Y undo-redo.
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl && IsKeyPressed(KEY_Z)) UndoApply(-1);
    if (ctrl && IsKeyPressed(KEY_Y)) UndoApply(+1);

    // Space = play/pause, Ctrl+Space = play-from-start/stop (playhead to 0).
    // Ignored while any textbox is capturing typing.
    bool typing = edName || edText || edKeyTime || edSigIdx >= 0;
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

    AnimDocDraw(&doc, playhead);

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
static bool EditSlider(Rectangle r, const char *label, float *v, float lo, float hi)
{
    float tmp = *v;
    GuiSlider(r, label, TextFormat("%.2f", tmp), &tmp, lo, hi);
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

static bool IsSelected(int i)
{
    return i == selElem || (i >= 0 && i < ANIM_ELEMS_MAX && multiSel[i]);
}

// ---------------------------------------------------------------------------
//  Left panel: element list + add/delete.
// ---------------------------------------------------------------------------
static void DrawElementList(float x, float y, float w)
{
    float rh = 26.0f, gap = 4.0f;
    GuiLabel((Rectangle){ x, y, w, 20 }, "ELEMENTS"); y += 22;

    for (int i = 0; i < doc.elemCount; i++)
    {
        Rectangle rr = { x, y, w, rh };
        const char *tag = AnimElemKindName(doc.elems[i].kind);
        bool pressed = GuiButton(rr, TextFormat("%s  [%s]", doc.elems[i].name, tag));
        // selection marker AFTER the button - raygui paints its own background,
        // so a tint drawn first would be invisible.
        if (i == selElem)
            DrawRectangleRec(rr, (Color){ 90, 140, 220, 90 });
        else if (IsSelected(i))
            DrawRectangleRec(rr, (Color){ 60, 90, 140, 70 });
        if (pressed)
        {
            AudioPlayButton();
            bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            if (ctrl && i != selElem) multiSel[i] = !multiSel[i];   // toggle extra
            else { selElem = i; for (int k=0;k<ANIM_ELEMS_MAX;k++) multiSel[k]=false; }
        }
        y += rh + gap;
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
}

// ---------------------------------------------------------------------------
//  Right panel: inspector for the primary selection.
// ---------------------------------------------------------------------------
static void DrawInspector(float x, float y, float w)
{
    GuiLabel((Rectangle){ x, y, w, 20 }, "INSPECTOR"); y += 24;
    if (selElem < 0 || selElem >= doc.elemCount)
    {
        GuiLabel((Rectangle){ x, y, w, 20 }, "(no element selected)");
        return;
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
        int sk = e->shapeKind;
        GuiLabel((Rectangle){ x, y, 40, rh }, "shape");
        if (GuiToggleGroup((Rectangle){ x+44, y, (w-44)/2-2, rh }, "Rect;Circle", &sk))
        { UndoPush(); e->shapeKind = sk; }
        y += rh + gap;
    }

    // position / size: tracked props follow the playhead (see PropSlider)
    if (e->kind != AE_GLOBAL)
    {
        bool isText = e->kind == AE_TEXT;
        PropSlider((Rectangle){ x+44, y, w-44, rh }, e, isText ? AP_T_POS_X : AP_S_POS_X, &e->posFrac.x);
        GuiLabel((Rectangle){ x, y, 44, rh }, "posX"); y += rh + gap;
        PropSlider((Rectangle){ x+44, y, w-44, rh }, e, isText ? AP_T_POS_Y : AP_S_POS_Y, &e->posFrac.y);
        GuiLabel((Rectangle){ x, y, 44, rh }, "posY"); y += rh + gap;
        PropSlider((Rectangle){ x+44, y, w-44, rh }, e, isText ? AP_T_SIZE : AP_S_W, &e->sizeFrac.x);
        GuiLabel((Rectangle){ x, y, 44, rh }, isText ? "size" : "w"); y += rh + gap;
        if (e->kind == AE_SHAPE)
        {
            PropSlider((Rectangle){ x+44, y, w-44, rh }, e, AP_S_H, &e->sizeFrac.y);
            GuiLabel((Rectangle){ x, y, 44, rh }, "h"); y += rh + gap;
        }
    }

    // colour (RGBA sliders keep it compact and stable across raygui versions)
    GuiLabel((Rectangle){ x, y, w, rh }, "color (rgba)"); y += rh;
    float cr=e->color.r, cg=e->color.g, cb=e->color.b, ca=e->color.a;
    EditSlider((Rectangle){ x+16, y, w-16, 16 }, "R", &cr, 0,255); y+=18;
    EditSlider((Rectangle){ x+16, y, w-16, 16 }, "G", &cg, 0,255); y+=18;
    EditSlider((Rectangle){ x+16, y, w-16, 16 }, "B", &cb, 0,255); y+=18;
    EditSlider((Rectangle){ x+16, y, w-16, 16 }, "A", &ca, 0,255); y+=22;
    e->color = (Color){ (unsigned char)cr,(unsigned char)cg,(unsigned char)cb,(unsigned char)ca };

    // --- tracks list: every track and ALL of its keys, always visible --------
    GuiLine((Rectangle){ x, y, w, 8 }, "tracks"); y += 12;
    for (int j = 0; j < e->trackCount; j++)
    {
        AnimTrack *tr = &e->tracks[j];
        GuiLabel((Rectangle){ x, y, w-106, rh },
                 TextFormat("%s (%d)", AnimPropName(tr->prop), tr->keyCount));
        // explicit keying path (works with auto-key off): key at the playhead.
        if (GuiButton((Rectangle){ x+w-102, y, 50, rh }, "+key"))
        {
            AudioPlayButton(); UndoPush();
            float v = AnimElemProp(e, tr->prop, playhead);
            if (playhead > AUTOKEY_EPS) EnsureZeroKey(e, tr);
            AnimKey *k = AnimTrackWriteKeyAt(tr, playhead, v, AUTOKEY_EPS);
            if (k) SelectKey(selElem, j, (int)(k - tr->keys));
        }
        if (GuiButton((Rectangle){ x+w-48, y, 48, rh }, "del"))
        {
            AudioPlayButton(); UndoPush();
            for (int m=j; m<e->trackCount-1; m++) e->tracks[m]=e->tracks[m+1];
            e->trackCount--; j--;
            ClearKeySelection();
            continue;
        }
        y += rh + 2;

        // key rows: "t  value  ease" - click to select (same as the timeline).
        for (int k = 0; k < tr->keyCount; k++)
        {
            bool sel = (selKeyElem == selElem && selKeyTrack == j && selKeyIdx == k);
            Rectangle kr = { x+12, y, w-12, 18 };
            if (sel) DrawRectangleRec(kr, (Color){ 60, 90, 140, 120 });
            if (GuiButton(kr, TextFormat("%.2f   %.2f   %s", tr->keys[k].t,
                          tr->keys[k].value, AnimEaseName(tr->keys[k].ease))))
            {
                AudioPlayButton();
                SelectKey(selElem, j, k);
            }
            y += 20;
        }
        y += 4;
    }

    // add-track dropdown (built from the element kind's valid props)
    y += 4;
    int propCount = AnimPropCountFor(e->kind);
    // build a ';'-joined option string for the dropdown.
    char opts[128]; opts[0]=0; int pos=0;
    for (int i=0;i<propCount;i++)
    {
        const char *nm = AnimPropName(AnimPropAt(e->kind,i));
        if (i) TextAppend(opts, ";", &pos);
        TextAppend(opts, nm, &pos);
    }
    Rectangle addR = { x, y, w-56, rh };
    if (GuiButton((Rectangle){ x+w-52, y, 52, rh }, "+track"))
    {
        AudioPlayButton(); UndoPush();
        int prop = AnimPropAt(e->kind, addTrackSel);
        // seed the START key at t=0, and - when the scrubber sits later on the
        // clock - a second key right at the playhead, ready to edit.
        AnimTrack *tr = AnimElemAddTrack(e, prop);
        if (tr)
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
    if (GuiDropdownBox(addR, opts, &addTrackSel, addTrackDrop == selElem))
        addTrackDrop = (addTrackDrop == selElem) ? -1 : selElem;
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

        // value (range depends on the property)
        AnimKey *k = &tr->keys[selKeyIdx];
        GuiLabel((Rectangle){ x, y, 44, rh }, "value");
        float v = k->value;
        if (EditSlider((Rectangle){ x+44, y, w-44, rh }, "", &v,
                       AnimPropMin(tr->prop), AnimPropMax(tr->prop)))
            k->value = v;
        y += rh + gap;

        // ease dropdown: only the RECT is reserved here; the widget is drawn
        // last in Gui() so its open list overlays the timeline (raygui z-order)
        GuiLabel((Rectangle){ x, y, 44, rh }, "ease");
        keyEaseRect = (Rectangle){ x+44, y, w-44-54, rh };
        keyEaseVisible = true;
        if (GuiButton((Rectangle){ x+w-50, y, 50, rh }, "del"))
        {
            AudioPlayButton(); UndoPush();
            AnimTrackRemoveKey(tr, selKeyIdx);
            ClearKeySelection();
        }
    }
}

// The key inspector's ease dropdown, drawn AFTER every other widget so its
// open list overlays them (immediate mode: last drawn wins).
static void DrawKeyEaseDropdown()
{
    if (!keyEaseVisible) return;

    static char opts[256]; static bool optsInit = false;   // "linear;sineIn;..."
    if (!optsInit)
    {
        int pos = 0;
        for (int i = 0; i < AnimEaseCount(); i++)
        {
            if (i) TextAppend(opts, ";", &pos);
            TextAppend(opts, AnimEaseName(i), &pos);
        }
        optsInit = true;
    }

    if (GuiDropdownBox(keyEaseRect, opts, &keyEaseSel, keyEaseDropOpen))
    {
        if (keyEaseDropOpen && selKeyElem >= 0)
        {
            UndoPush();
            doc.elems[selKeyElem].tracks[selKeyTrack].keys[selKeyIdx].ease = keyEaseSel;
        }
        keyEaseDropOpen = !keyEaseDropOpen;
    }
}

// ---------------------------------------------------------------------------
//  Signals row.
// ---------------------------------------------------------------------------
static float DrawSignals(float x, float y, float w)   // returns height used
{
    float rh = 24.0f;
    #define SIG_CELL 480.0f
    GuiLabel((Rectangle){ x, y, 80, rh }, "SIGNALS");
    float sx = x + 84, sy = y;
    bool changed = false;       // any edit -> one re-register at the end

    for (int i = 0; i < doc.signalCount; i++)
    {
        if (sx + SIG_CELL > x + w) { sx = x + 84; sy += rh + 4; }  // wrap row
        AnimSignal *sg = &doc.signals[i];

        // name (editable; bindings are refreshed on commit)
        if (GuiTextBox((Rectangle){ sx, sy, 90, rh }, sg->name, ANIM_NAME_MAX,
                       edSigIdx == i))
        {
            if (edSigIdx != i) { UndoPush(); edSigIdx = i; }
            else               { edSigIdx = -1; changed = true; }
        }

        int dir = sg->dir;
        if (GuiToggleGroup((Rectangle){ sx+94, sy, 44, rh }, "fwd;rev", &dir) &&
            dir != sg->dir)
        { UndoPush(); sg->dir = dir; changed = true; }

        // playback section [start,end] within the doc clock
        float s0 = sg->sectionStart, s1 = sg->sectionEnd;
        if (EditSlider((Rectangle){ sx+190, sy, 70, rh }, "", &s0, 0.0f, doc.duration))
        { sg->sectionStart = s0; if (sg->sectionEnd < s0) sg->sectionEnd = s0; changed = true; }
        if (EditSlider((Rectangle){ sx+266, sy, 70, rh }, "", &s1, 0.0f, doc.duration))
        { sg->sectionEnd = s1; if (sg->sectionStart > s1) sg->sectionStart = s1; changed = true; }

        if (GuiButton((Rectangle){ sx+342, sy, 44, rh }, "Fire"))
        { AudioPlayButton(); SignalEmit(sg->name); }
        if (GuiButton((Rectangle){ sx+390, sy, 24, rh }, "x"))
        {
            AudioPlayButton(); UndoPush();
            for (int m = i; m < doc.signalCount - 1; m++)
                doc.signals[m] = doc.signals[m+1];
            doc.signalCount--; i--;
            if (edSigIdx == i+1) edSigIdx = -1;
            changed = true;
            continue;
        }
        sx += SIG_CELL;
    }

    if (doc.signalCount < ANIM_SIGNALS_MAX)
    {
        if (sx + 60 > x + w) { sx = x + 84; sy += rh + 4; }
        if (GuiButton((Rectangle){ sx, sy, 60, rh }, "+signal"))
        {
            AudioPlayButton(); UndoPush();
            AnimSignal *sg = &doc.signals[doc.signalCount++];
            TextCopy(sg->name, TextFormat("sig%d", doc.signalCount));
            sg->dir = ANIM_FWD; sg->sectionStart = 0.0f; sg->sectionEnd = doc.duration;
            changed = true;
        }
    }

    if (changed) ReRegisterSignals();
    #undef SIG_CELL
    return (sy - y) + rh + 6;
}

// ---------------------------------------------------------------------------
//  Timeline scrubber: playhead + keyframe diamonds for the selected element.
// ---------------------------------------------------------------------------
static void DrawTimeline(float x, float y, float w, float h)
{
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

    // keyframe diamonds for the selected element's tracks (stacked rows).
    // Raw-mouse input is suppressed while the ease dropdown is open (raygui's
    // GuiLock only covers gui widgets, not this hand-drawn timeline).
    Vector2 mouse = GetMousePosition();
    bool press  = !keyEaseDropOpen && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    bool keyHit = false;
    if (selElem >= 0 && selElem < doc.elemCount)
    {
        AnimElem *e = &doc.elems[selElem];
        float rowH = (h - 24) / (float)(e->trackCount > 0 ? e->trackCount : 1);
        for (int j = 0; j < e->trackCount; j++)
        {
            AnimTrack *tr = &e->tracks[j];
            float ry = y + 4 + j*rowH + rowH*0.5f;
            DrawText(AnimPropName(tr->prop), (int)x+2, (int)ry-5, 10, (Color){130,136,148,255});
            for (int k = 0; k < tr->keyCount; k++)
            {
                float kx = T2X(tr->keys[k].t);
                // generous hit box - diamonds are small targets otherwise.
                Rectangle hit = { kx-10, ry-10, 20, 20 };
                bool hot = CheckCollisionPointRec(mouse, hit);
                bool sel = (selKeyElem == selElem && selKeyTrack == j && selKeyIdx == k);
                Color c = sel ? (Color){255,255,255,255}
                        : hot ? (Color){255,210,90,255} : (Color){120,180,240,255};
                // diamond
                DrawTriangle((Vector2){kx,ry-6},(Vector2){kx+6,ry},(Vector2){kx-6,ry}, c);
                DrawTriangle((Vector2){kx-6,ry},(Vector2){kx+6,ry},(Vector2){kx,ry+6}, c);

                if (hot && press)
                {
                    UndoPush();                       // once per drag gesture
                    dragKeyElem = selElem; dragKeyTrack = j; dragKeyIdx = k;
                    SelectKey(selElem, j, k);
                    keyHit = true;
                }
            }
        }
    }

    // playhead line + grab handle.
    float phx = T2X(playhead);
    DrawLine((int)phx, (int)y, (int)phx, (int)(y+h), (Color){255,90,90,255});
    Rectangle phHandle = { phx-6, y-2, 12, 10 };
    DrawRectangleRec(phHandle, (Color){255,90,90,255});

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
    { dragPlayhead = false; dragKeyElem = -1; dragKeyTrack = -1; dragKeyIdx = -1; }

    #undef T2X
    #undef X2T
}

// ---------------------------------------------------------------------------
//  Toolbar: New / Load / Save / Undo / Redo / play controls / Back.
// ---------------------------------------------------------------------------
static void DrawToolbar(float x, float y, float w)
{
    float bw = 70, rh = 26, gx = x;
    if (GuiButton((Rectangle){ gx, y, bw, rh }, "New"))
    { AudioPlayButton(); UndoPush(); MakeStarterDoc();
      AnimSignalUnregister(&doc,&preview); AnimSignalRegister(&doc,&preview); }
    gx += bw+4;
    if (GuiButton((Rectangle){ gx, y, bw, rh }, "Load"))
    { AudioPlayButton(); UndoPush(); AnimDocLoad(&doc, SAVE_PATH); ClampSelection();
      AnimSignalUnregister(&doc,&preview); AnimSignalRegister(&doc,&preview); }
    gx += bw+4;
    if (GuiButton((Rectangle){ gx, y, bw, rh }, "Save"))
    { AudioPlayButton(); AnimDocSave(&doc, SAVE_PATH); }
    gx += bw+8;
    if (GuiButton((Rectangle){ gx, y, bw, rh }, "Undo")) { AudioPlayButton(); UndoApply(-1); }
    gx += bw+4;
    if (GuiButton((Rectangle){ gx, y, bw, rh }, "Redo")) { AudioPlayButton(); UndoApply(+1); }
    gx += bw+8;
    if (GuiButton((Rectangle){ gx, y, bw, rh }, playing ? "Pause" : "Play"))
    { AudioPlayButton(); playing = !playing; preview.playing = false; }
    gx += bw+4;
    GuiCheckBox((Rectangle){ gx, y+4, 18, 18 }, "loop", &loopPlay);
    gx += 60;
    GuiCheckBox((Rectangle){ gx, y+4, 18, 18 }, "autokey", &autoKey);
    gx += 80;

    // duration editor (never below the last keyframe - keys must stay on clock)
    GuiLabel((Rectangle){ gx, y, 30, rh }, "dur"); gx += 32;
    if (EditSlider((Rectangle){ gx, y, 120, rh }, "", &doc.duration, 0.2f, 10.0f))
    {
        float minDur = AnimDocMaxKeyTime(&doc);
        if (doc.duration < minDur) doc.duration = minDur;
    }
    gx += 128;

    // Back to menu (right edge).
    if (GuiButton((Rectangle){ x + w - bw, y, bw, rh }, "Back"))
    { AudioPlayButton(); AppStateTransition(&app_state_main_menu); }
}

// ===========================================================================
//  Gui: assemble the panels (screen space).
// ===========================================================================
static void Gui()
{
    ScreenState *ss = ScreenStateGet();
    float W = (float)ss->width, H = (float)ss->height;

    float pad = 10.0f;
    float toolbarH = 34.0f;
    float leftW = 220.0f;
    float rightW = 260.0f;
    float bottomH = 180.0f;

    // a slider drag gesture (one undo snapshot) ends when the button comes up.
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) sliderGestureOpen = false;

    // while the key-ease dropdown is open every other widget is locked; the
    // dropdown itself is drawn (unlocked) last so its list overlays them.
    if (keyEaseDropOpen) GuiLock();

    DrawToolbar(pad, pad, W - 2*pad);

    // left + right panels flank the central preview (which Draw() already drew).
    Rectangle leftPanel = { pad, toolbarH+pad, leftW, H - toolbarH - bottomH - 3*pad };
    Rectangle rightPanel = { W - rightW - pad, toolbarH+pad, rightW, H - toolbarH - bottomH - 3*pad };
    GuiPanel(leftPanel, NULL);
    GuiPanel(rightPanel, NULL);
    DrawElementList(leftPanel.x+8, leftPanel.y+8, leftW-16);
    DrawInspector(rightPanel.x+8, rightPanel.y+8, rightW-16);

    // bottom: signal rows (wraps, reports its height) then the timeline.
    float by = H - bottomH - pad;
    float sigH = DrawSignals(pad, by, W - 2*pad);
    DrawTimeline(pad, by + sigH, W - 2*pad, bottomH - sigH);

    GuiUnlock();
    DrawKeyEaseDropdown();
}
