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
#define UNDO_MAX  24

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
static bool edName = false, edText = false, edDur = false;
static bool edSigName = false;
static int  easeDropElem = -1, easeDropTrack = -1, easeDropKey = -1; // open dropdown
static bool easeDropOpen = false;
static int  addTrackDrop = -1;      // element whose "add track" dropdown is open
static int  addTrackSel = 0;

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

static void ClampSelection()
{
    if (selElem >= doc.elemCount) selElem = doc.elemCount - 1;
    for (int i = 0; i < ANIM_ELEMS_MAX; i++)
        if (i >= doc.elemCount) multiSel[i] = false;
}

static void UndoApply(int delta)   // delta -1 = undo, +1 = redo
{
    if (delta < 0)
    {
        if (undoCount == 0) return;
        // push current onto redo side, step head back.
        undoHead = (undoHead - 1 + UNDO_MAX) % UNDO_MAX;
        AnimDoc prev = undoBuf[undoHead];
        undoBuf[undoHead] = doc;                // store current for redo
        doc = prev;
        undoCount--; redoCount++;
    }
    else
    {
        if (redoCount == 0) return;
        AnimDoc next = undoBuf[undoHead];
        undoBuf[undoHead] = doc;
        doc = next;
        undoHead = (undoHead + 1) % UNDO_MAX;
        undoCount++; redoCount--;
    }
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
    AnimTrackAddKey(a, 0.0f, 0.0f, NULL);
    AnimTrackAddKey(a, 0.6f, 1.0f, sineEaseOutf);

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
// A labelled float editor as a slider (keeps the UI compact; timeline handles t).
static bool FloatSlider(Rectangle r, const char *label, float *v, float lo, float hi)
{
    float before = *v;
    GuiSlider(r, label, TextFormat("%.2f", *v), v, lo, hi);
    return *v != before;
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
        // selected rows get a filled marker.
        if (IsSelected(i))
            DrawRectangleRec(rr, (Color){ 60, 90, 140, 120 });
        if (GuiButton(rr, TextFormat("%s  [%s]", doc.elems[i].name, tag)))
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
        // shift elements down over the removed one.
        for (int i = selElem; i < doc.elemCount - 1; i++) doc.elems[i] = doc.elems[i+1];
        doc.elemCount--;
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

    // base position / size (fractions)
    if (e->kind != AE_GLOBAL)
    {
        if (FloatSlider((Rectangle){ x+44, y, w-44, rh }, "posX", &e->posFrac.x, 0.0f, 1.0f)) {}
        GuiLabel((Rectangle){ x, y, 44, rh }, "posX"); y += rh + gap;
        if (FloatSlider((Rectangle){ x+44, y, w-44, rh }, "posY", &e->posFrac.y, 0.0f, 1.0f)) {}
        GuiLabel((Rectangle){ x, y, 44, rh }, "posY"); y += rh + gap;
        const char *l1 = e->kind == AE_TEXT ? "size" : "w";
        FloatSlider((Rectangle){ x+44, y, w-44, rh }, l1, &e->sizeFrac.x, 0.0f, 1.0f);
        GuiLabel((Rectangle){ x, y, 44, rh }, l1); y += rh + gap;
        if (e->kind == AE_SHAPE)
        {
            FloatSlider((Rectangle){ x+44, y, w-44, rh }, "h", &e->sizeFrac.y, 0.0f, 1.0f);
            GuiLabel((Rectangle){ x, y, 44, rh }, "h"); y += rh + gap;
        }
    }

    // colour (RGBA sliders keep it compact and stable across raygui versions)
    GuiLabel((Rectangle){ x, y, w, rh }, "color (rgba)"); y += rh;
    float cr=e->color.r, cg=e->color.g, cb=e->color.b, ca=e->color.a;
    GuiSlider((Rectangle){ x+16, y, w-16, 16 }, "R", TextFormat("%d",e->color.r), &cr, 0,255); y+=18;
    GuiSlider((Rectangle){ x+16, y, w-16, 16 }, "G", TextFormat("%d",e->color.g), &cg, 0,255); y+=18;
    GuiSlider((Rectangle){ x+16, y, w-16, 16 }, "B", TextFormat("%d",e->color.b), &cb, 0,255); y+=18;
    GuiSlider((Rectangle){ x+16, y, w-16, 16 }, "A", TextFormat("%d",e->color.a), &ca, 0,255); y+=22;
    e->color = (Color){ (unsigned char)cr,(unsigned char)cg,(unsigned char)cb,(unsigned char)ca };

    // --- tracks list -------------------------------------------------------
    GuiLine((Rectangle){ x, y, w, 8 }, "tracks"); y += 12;
    for (int j = 0; j < e->trackCount; j++)
    {
        AnimTrack *tr = &e->tracks[j];
        GuiLabel((Rectangle){ x, y, w-52, rh },
                 TextFormat("%s  (%d keys)", AnimPropName(tr->prop), tr->keyCount));
        if (GuiButton((Rectangle){ x+w-48, y, 48, rh }, "del"))
        {
            AudioPlayButton(); UndoPush();
            for (int m=j; m<e->trackCount-1; m++) e->tracks[m]=e->tracks[m+1];
            e->trackCount--; j--; continue;
        }
        y += rh + 2;
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
        AnimTrack *tr = AnimElemAddTrack(e, prop);
        if (tr) { AnimTrackAddKey(tr, 0.0f, AnimElemProp(e, prop, 0.0f), NULL);
                  AnimTrackAddKey(tr, doc.duration, AnimElemProp(e, prop, 0.0f), NULL); }
    }
    if (GuiDropdownBox(addR, opts, &addTrackSel, addTrackDrop == selElem))
        addTrackDrop = (addTrackDrop == selElem) ? -1 : selElem;
}

// ---------------------------------------------------------------------------
//  Signals row.
// ---------------------------------------------------------------------------
static void DrawSignals(float x, float y, float w)
{
    float rh = 24.0f;
    GuiLabel((Rectangle){ x, y, 80, rh }, "SIGNALS");
    float sx = x + 84;
    for (int i = 0; i < doc.signalCount; i++)
    {
        AnimSignal *sg = &doc.signals[i];
        Rectangle nr = { sx, y, 90, rh };
        GuiTextBox(nr, sg->name, ANIM_NAME_MAX, false);
        int dir = sg->dir;
        if (GuiToggleGroup((Rectangle){ sx+94, y, 44, rh }, "fwd;rev", &dir)) sg->dir = dir;
        if (GuiButton((Rectangle){ sx+188, y, 44, rh }, "Fire"))
        { AudioPlayButton(); SignalEmit(sg->name); }
        sx += 240;
    }
    if (doc.signalCount < ANIM_SIGNALS_MAX &&
        GuiButton((Rectangle){ sx, y, 60, rh }, "+signal"))
    {
        AudioPlayButton(); UndoPush();
        AnimSignal *sg = &doc.signals[doc.signalCount++];
        TextCopy(sg->name, TextFormat("sig%d", doc.signalCount));
        sg->dir = ANIM_FWD; sg->sectionStart = 0.0f; sg->sectionEnd = doc.duration;
        // re-register so the new signal fires the preview.
        AnimSignalUnregister(&doc, &preview);
        AnimSignalRegister(&doc, &preview);
    }
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
    float pad = 8.0f;
    float trackLeft = x + pad, trackW = w - 2*pad;

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
    Vector2 mouse = GetMousePosition();
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
                Rectangle hit = { kx-6, ry-6, 12, 12 };
                bool hot = CheckCollisionPointRec(mouse, hit);
                Color c = hot ? (Color){255,210,90,255} : (Color){120,180,240,255};
                // diamond
                DrawTriangle((Vector2){kx,ry-6},(Vector2){kx+6,ry},(Vector2){kx-6,ry}, c);
                DrawTriangle((Vector2){kx-6,ry},(Vector2){kx+6,ry},(Vector2){kx,ry+6}, c);

                if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                { dragKeyElem=selElem; dragKeyTrack=j; dragKeyIdx=k; UndoPush(); }
            }
        }
    }

    // playhead line + grab handle.
    float phx = T2X(playhead);
    DrawLine((int)phx, (int)y, (int)phx, (int)(y+h), (Color){255,90,90,255});
    Rectangle phHandle = { phx-6, y-2, 12, 10 };
    DrawRectangleRec(phHandle, (Color){255,90,90,255});

    // --- dragging ----------------------------------------------------------
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        CheckCollisionPointRec(mouse, (Rectangle){ x, y-4, w, 14 }))
        dragPlayhead = true;

    if (dragKeyElem >= 0 && IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        AnimTrack *tr = &doc.elems[dragKeyElem].tracks[dragKeyTrack];
        float nt = X2T(mouse.x);
        if (nt < 0) nt = 0; if (nt > dur) nt = dur;
        tr->keys[dragKeyIdx].t = nt;
        AnimTrackSortKeys(tr);
        // after a sort the dragged key may move; re-find it by value+time next frame
        dragKeyIdx = -1; dragKeyElem = -1; dragKeyTrack = -1;   // one-shot per press-move
    }
    else if (dragPlayhead && IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        float nt = X2T(mouse.x);
        if (nt < 0) nt = 0; if (nt > dur) nt = dur;
        playhead = nt; playing = false; preview.playing = false;
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
    { dragPlayhead = false; dragKeyElem = -1; }

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

    // duration editor
    GuiLabel((Rectangle){ gx, y, 30, rh }, "dur"); gx += 32;
    if (FloatSlider((Rectangle){ gx, y, 120, rh }, "", &doc.duration, 0.2f, 10.0f)) {}
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
    float bottomH = 150.0f;

    DrawToolbar(pad, pad, W - 2*pad);

    // left + right panels flank the central preview (which Draw() already drew).
    Rectangle leftPanel = { pad, toolbarH+pad, leftW, H - toolbarH - bottomH - 3*pad };
    Rectangle rightPanel = { W - rightW - pad, toolbarH+pad, rightW, H - toolbarH - bottomH - 3*pad };
    GuiPanel(leftPanel, NULL);
    GuiPanel(rightPanel, NULL);
    DrawElementList(leftPanel.x+8, leftPanel.y+8, leftW-16);
    DrawInspector(rightPanel.x+8, rightPanel.y+8, rightW-16);

    // bottom: signals row then the timeline.
    float by = H - bottomH - pad;
    DrawSignals(pad, by, W - 2*pad);
    DrawTimeline(pad, by + 30, W - 2*pad, bottomH - 30);
}
