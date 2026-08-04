// ============================================================================
//  zen_view.c  -  the Zen editor's main viewport
//
//  While EDITING the document is drawn zoomed out (ZEN_ZOOM_EDIT) so elements
//  animating in from off-screen stay visible and grabbable; the real screen
//  is marked with a dotted rectangle. Pressing play first zooms to 1:1 with
//  a sineIn over ZEN_ZOOM_TIME, and ONLY THEN starts the timeline clock
//  (zen.playPending -> zen.playing hand-off happens here in ZenViewUpdate).
//
//  Draw() runs inside the game-space render target (see main.c), so the zoom
//  is a Camera2D around the target's center. ZenScreenToDoc() applies the
//  same camera in reverse so later phases can pick/drag elements.
// ============================================================================

#include "raylib.h"
#include "raygui.h"
#include "zen_internal.h"
#include "../screen_state/screen_state.h"
#include <math.h>

// ---------------------------------------------------------------------------
//  Camera: pinned to the target center; only the zoom animates.
// ---------------------------------------------------------------------------
static float ZoomNow(void)
{
    float editZoom = zen.zoomFull ? 1.0f : ZEN_ZOOM_EDIT;
    float e = AnimEaseApply(ANIM_EASE_SINE_IN, zen.zoomT);
    return editZoom + (1.0f - editZoom) * e;
}

static Camera2D ViewCamera(void)
{
    Vector2 game = ScreenStateTargetSize();
    Camera2D cam = { 0 };
    cam.target = (Vector2){ game.x * 0.5f, game.y * 0.5f };
    cam.offset = cam.target;
    cam.zoom = ZoomNow();
    return cam;
}

// ---------------------------------------------------------------------------
//  Picking / dragging state (viewport-local).
// ---------------------------------------------------------------------------
static Vector2 lastPickPos = {0};   // doc-space point of the last pick click
static int     pickCycle = 0;       // which of the overlapping hits is next

static bool    ctxOpen = false;     // right-click track menu
static Vector2 ctxPos = {0};        // its screen position

static bool    dragActive = false;  // Ctrl-drag of the selected element
static Vector2 dragStartDoc = {0};  // doc-space mouse at drag start
static Vector2 dragStartFrac = {0}; // element position (fractions) at start
static bool    dragAxisLock = false;// Ctrl+Shift: constrained to an axis
static int     dragAxisIdx = 0;     // 0 H, 1 V, 2 diag \, 3 diag /

// Doc-space AABB of an element at the playhead (approximate for rotated
// shapes - good enough to pick). Globals return an empty rect: they cover
// everything, so picking them by click would shadow every other element.
static Rectangle ElemRectDoc(const AnimElem *e)
{
    Vector2 game = ScreenStateTargetSize();
    float t = zen.playhead;
    if (e->kind == AE_GLOBAL) return (Rectangle){0};

    if (e->kind == AE_TEXT)
    {
        float sizeF = AnimElemProp(e, AP_T_SIZE, t);
        float sizePx = e->sizeAbsolute ? sizeF : game.y * sizeF;
        if (sizePx < 1.0f) sizePx = 1.0f;
        Vector2 box = MeasureTextEx(GetFontDefault(), e->text, sizePx, sizePx / 10.0f);
        if (box.x < 12.0f) box.x = 12.0f;
        float cx = game.x * AnimElemProp(e, AP_T_POS_X, t);
        float cy = game.y * AnimElemProp(e, AP_T_POS_Y, t);
        return (Rectangle){ cx - box.x*0.5f, cy - box.y*0.5f, box.x, box.y };
    }

    // shape: extents in element units x the unit reference x scale.
    float uW = e->sizeAbsolute ? 1.0f : game.x;
    float uH = e->sizeAbsolute ? 1.0f : game.y;
    float sc = AnimElemProp(e, AP_S_SCALE, t);
    float wPx = AnimElemProp(e, AP_S_W, t) * uW * sc;
    float hPx = AnimElemProp(e, AP_S_H, t) * uH * sc;
    if (e->shapeKind == SHAPE_CIRCLE || e->shapeKind == SHAPE_SQUARE ||
        e->shapeKind == SHAPE_RHOMBUS) hPx = wPx;   // one-extent shapes
    if (wPx < 12.0f) wPx = 12.0f;
    if (hPx < 12.0f) hPx = 12.0f;
    float cx = game.x * AnimElemProp(e, AP_S_POS_X, t);
    float cy = game.y * AnimElemProp(e, AP_S_POS_Y, t);
    return (Rectangle){ cx - wPx*0.5f, cy - hPx*0.5f, wPx, hPx };
}

// Write the selected element's position, honoring tracks + auto-key exactly
// like the inspector sliders: tracked + auto-key ON keys at the playhead,
// tracked + auto-key OFF refuses, untracked writes the base pose.
static void ViewWritePosAxis(AnimElem *e, int prop, float *base, float v)
{
    AnimTrack *tr = AnimElemFindTrack(e, prop);
    if (!tr) { *base = v; return; }
    if (!zen.autoKey) return;
    if (zen.playhead > ZEN_AUTOKEY_EPS) ZenEnsureZeroKey(e, tr);
    AnimTrackWriteKeyAt(tr, zen.playhead, v, ZEN_AUTOKEY_EPS);
    // dragging calls this every frame; the helper no-ops once it's showing.
    ZenAutoKeyFocus(zen.selElem, prop, zen.playhead);
}

static void ViewWritePos(AnimElem *e, float fx, float fy)
{
    bool text = e->kind == AE_TEXT;
    ViewWritePosAxis(e, text ? AP_T_POS_X : AP_S_POS_X, &e->posFrac.x, fx);
    ViewWritePosAxis(e, text ? AP_T_POS_Y : AP_S_POS_Y, &e->posFrac.y, fy);
}

// The four constraint axes in doc pixels (diagonals stay 45 deg ON SCREEN).
static Vector2 AxisDir(int idx)
{
    const float d = 0.70710678f;
    switch (idx)
    {
        case 0:  return (Vector2){ 1.0f, 0.0f };
        case 1:  return (Vector2){ 0.0f, 1.0f };
        case 2:  return (Vector2){ d,  d };
        default: return (Vector2){ d, -d };
    }
}

static void ViewInput(void)
{
    // The viewport listens only while EDITING with the mouse actually on it.
    if (zen.playing || zen.playPending) { dragActive = false; return; }
    // guiLocked also goes up while another layer owns a drag, so ask about the
    // viewport's own claim instead: a drag that started here keeps running
    // even once the cursor has wandered over the chrome.
    if (!ZenLayerActive(ZEN_LAYER_VIEWPORT)) return;
    if (!dragActive && (zen.uiHover || zen.guiLocked || ctxOpen)) return;

    Vector2 mouse = GetMousePosition();
    Vector2 dm = ZenScreenToDoc(mouse);
    Vector2 game = ScreenStateTargetSize();
    bool ctrl  = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT)   || IsKeyDown(KEY_RIGHT_SHIFT);

    // --- Ctrl+drag: reposition the selected element -------------------------
    if (dragActive)
    {
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT) ||
            zen.selElem < 0 || zen.selElem >= zen.doc.elemCount)
        { dragActive = false; dragAxisLock = false; return; }

        AnimElem *e = &zen.doc.elems[zen.selElem];
        Vector2 dpx = { dm.x - dragStartDoc.x, dm.y - dragStartDoc.y };

        dragAxisLock = shift;
        if (dragAxisLock)
        {
            // project the pixel delta on each axis; the longest wins.
            float best = 0.0f;
            for (int i = 0; i < 4; i++)
            {
                Vector2 a = AxisDir(i);
                float p = dpx.x*a.x + dpx.y*a.y;
                if (fabsf(p) > fabsf(best)) { best = p; dragAxisIdx = i; }
            }
            Vector2 a = AxisDir(dragAxisIdx);
            dpx = (Vector2){ a.x * best, a.y * best };
        }

        ViewWritePos(e, dragStartFrac.x + dpx.x / game.x,
                        dragStartFrac.y + dpx.y / game.y);
        return;
    }

    if (ctrl && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        zen.selElem >= 0 && zen.selElem < zen.doc.elemCount &&
        zen.doc.elems[zen.selElem].kind != AE_GLOBAL)
    {
        AnimElem *e = &zen.doc.elems[zen.selElem];
        bool text = e->kind == AE_TEXT;
        ZenUndoPush();                      // one snapshot per drag gesture
        dragActive = true; dragAxisLock = false;
        dragStartDoc = dm;
        dragStartFrac = (Vector2){
            AnimElemProp(e, text ? AP_T_POS_X : AP_S_POS_X, zen.playhead),
            AnimElemProp(e, text ? AP_T_POS_Y : AP_S_POS_Y, zen.playhead) };
        return;
    }

    // --- left click: pick (repeat clicks cycle overlapping elements) --------
    if (!ctrl && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        int hits[ANIM_ELEMS_MAX], hn = 0;
        // topmost first: later elements draw later, so walk backwards.
        for (int i = zen.doc.elemCount - 1; i >= 0; i--)
            if (CheckCollisionPointRec(dm, ElemRectDoc(&zen.doc.elems[i])))
                hits[hn++] = i;
        if (hn > 0)
        {
            float moved = fabsf(dm.x - lastPickPos.x) + fabsf(dm.y - lastPickPos.y);
            if (moved > 6.0f) pickCycle = 0;       // new spot: start at the top
            lastPickPos = dm;
            int pick = hits[pickCycle % hn];
            pickCycle++;
            if (pick != zen.selElem)
            {
                ZenSelClear();
                zen.selElem = pick;
                for (int k = 0; k < ANIM_ELEMS_MAX; k++) zen.multiSel[k] = false;
            }
        }
        return;
    }

    // --- right click: context menu of the selected element's tracks ---------
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
    {
        // pick under the cursor first so the menu matches what was clicked.
        for (int i = zen.doc.elemCount - 1; i >= 0; i--)
            if (CheckCollisionPointRec(dm, ElemRectDoc(&zen.doc.elems[i])))
            {
                if (i != zen.selElem)
                {
                    ZenSelClear();
                    zen.selElem = i;
                    for (int k = 0; k < ANIM_ELEMS_MAX; k++) zen.multiSel[k] = false;
                }
                break;
            }
        if (zen.selElem >= 0 && zen.selElem < zen.doc.elemCount)
        { ctxOpen = true; ctxPos = mouse; }
    }
}

void ZenViewUpdate(float dt)
{
    // Zoom toward 1:1 whenever playback is running OR armed; back out to the
    // editing zoom otherwise. sineIn shaping happens in ZoomNow(), the T
    // itself moves linearly.
    bool wantIn = zen.playing || zen.playPending;
    float step = dt / ZEN_ZOOM_TIME;
    zen.zoomT += wantIn ? step : -step;
    if (zen.zoomT > 1.0f) zen.zoomT = 1.0f;
    if (zen.zoomT < 0.0f) zen.zoomT = 0.0f;

    // The play hand-off: the clock starts only once the zoom has landed, so
    // the zoom never overlaps the animation.
    if (zen.playPending && zen.zoomT >= 1.0f)
    {
        zen.playPending = false;
        zen.playing = true;
    }

    ViewInput();
}

// ---------------------------------------------------------------------------
//  Right-click context menu: the selected element's tracked groups; a row
//  click selects the track and opens the track modal. Screen space (Gui).
// ---------------------------------------------------------------------------
bool ZenViewCtxOpen(void)  { return ctxOpen; }
void ZenViewCtxClose(void) { ctxOpen = false; }

void ZenViewContextGui(void)
{
    if (!ctxOpen) return;
    if (zen.playing || zen.playPending ||
        zen.selElem < 0 || zen.selElem >= zen.doc.elemCount)
    { ctxOpen = false; return; }

    AnimElem *e = &zen.doc.elems[zen.selElem];
    int grps[ANIM_TRACKS_MAX], gn = 0;
    int grpN = AnimGroupCountFor(e->kind);
    for (int gi = 0; gi < grpN && gn < ANIM_TRACKS_MAX; gi++)
        if (ZenGroupHasTrack(e, gi)) grps[gn++] = gi;

    const float rowH = 24.0f;
    float w = 140.0f;
    for (int i = 0; i < gn; i++)
    {
        float lw = ZenTextW(AnimGroupAt(e->kind, grps[i])->name) + 32.0f;
        if (lw > w) w = lw;
    }
    int rows = gn > 0 ? gn : 1;
    Rectangle bg = { ctxPos.x, ctxPos.y, w, rows * rowH + 8 };
    ScreenState *ss = ScreenStateGet();
    if (bg.x + bg.width  > ss->width)  bg.x = ss->width  - bg.width;
    if (bg.y + bg.height > ss->height) bg.y = ss->height - bg.height;

    DrawRectangleRec(bg, (Color){ 32, 34, 40, 250 });
    DrawRectangleLinesEx(bg, 1.0f, (Color){ 70, 74, 84, 255 });

    float y = bg.y + 4;
    if (gn == 0)
    {
        GuiDisable();
        GuiButton((Rectangle){ bg.x + 4, y, w - 8, rowH }, "no tracks");
        GuiEnable();
    }
    for (int i = 0; i < gn; i++)
    {
        Rectangle r = { bg.x + 4, y, w - 8, rowH };
        if (GuiButton(r, AnimGroupAt(e->kind, grps[i])->name))
        {
            ZenSelTrack(zen.selElem, grps[i]);
            ZenTrackModalOpen(zen.selElem, grps[i]);
            ctxOpen = false;
        }
        y += rowH;
    }

    if (ctxOpen && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        !CheckCollisionPointRec(GetMousePosition(), bg))
        ctxOpen = false;                            // click-away closes
}

// ---------------------------------------------------------------------------
//  Dotted primitives (screen-bounds marker now, drag-axis guides later).
// ---------------------------------------------------------------------------
void ZenDrawDottedLine(Vector2 a, Vector2 b, Color c)
{
    const float dash = 6.0f, gap = 5.0f;
    float dx = b.x - a.x, dy = b.y - a.y;
    float len = sqrtf(dx*dx + dy*dy);
    if (len <= 0.0f) return;
    float ux = dx/len, uy = dy/len;
    for (float t = 0.0f; t < len; t += dash + gap)
    {
        float e = t + dash; if (e > len) e = len;
        DrawLineV((Vector2){ a.x + ux*t, a.y + uy*t },
                  (Vector2){ a.x + ux*e, a.y + uy*e }, c);
    }
}

void ZenDrawDottedRect(Rectangle r, Color c)
{
    Vector2 tl = { r.x, r.y };
    Vector2 tr = { r.x + r.width, r.y };
    Vector2 br = { r.x + r.width, r.y + r.height };
    Vector2 bl = { r.x, r.y + r.height };
    ZenDrawDottedLine(tl, tr, c);
    ZenDrawDottedLine(tr, br, c);
    ZenDrawDottedLine(br, bl, c);
    ZenDrawDottedLine(bl, tl, c);
}

// ---------------------------------------------------------------------------
//  Mouse mapping for later picking phases: raw screen -> doc/game pixels.
//  Screen2Target undoes the letterbox; the camera inverse undoes the zoom.
// ---------------------------------------------------------------------------
Vector2 ZenScreenToDoc(Vector2 screenPos)
{
    Vector2 p = Screen2Target(screenPos);
    Camera2D cam = ViewCamera();
    return (Vector2){ (p.x - cam.offset.x) / cam.zoom + cam.target.x,
                      (p.y - cam.offset.y) / cam.zoom + cam.target.y };
}

// ---------------------------------------------------------------------------
//  Draw
// ---------------------------------------------------------------------------
void ZenViewDraw(void)
{
    Vector2 game = ScreenStateTargetSize();
    ClearBackground((Color){ 24, 26, 31, 255 });

    Camera2D cam = ViewCamera();
    BeginMode2D(cam);

    // The actual screen: filled slightly lighter so on-screen vs off-screen
    // reads instantly, dotted border marks the exact bounds.
    DrawRectangle(0, 0, (int)game.x, (int)game.y, (Color){ 30, 32, 38, 255 });

    // A fired signal layers over the timeline pose (NULL override when idle);
    // the smooth-loop blend shows only while ACTUALLY looping, so hand
    // scrubbing edits the authored tail.
    AnimDocDrawLoop(&zen.doc, zen.playhead, &zen.preview,
                    zen.playing && zen.loopPlay);

    // Ctrl+Shift drag: dotted guide lines through the drag start, all four
    // axes faint with the currently winning one bright.
    if (dragActive && dragAxisLock)
    {
        float len = 2.0f * (game.x + game.y);
        for (int i = 0; i < 4; i++)
        {
            Vector2 a = AxisDir(i);
            Color c = i == dragAxisIdx ? (Color){ 255, 210, 90, 200 }
                                       : (Color){ 160, 170, 190, 60 };
            ZenDrawDottedLine(
                (Vector2){ dragStartDoc.x - a.x*len, dragStartDoc.y - a.y*len },
                (Vector2){ dragStartDoc.x + a.x*len, dragStartDoc.y + a.y*len }, c);
        }
    }

    // Overlays fade away as the view zooms into play.
    float fade = 1.0f - zen.zoomT;
    if (fade > 0.0f)
    {
        Color line = { 120, 190, 255, (unsigned char)(170 * fade) };
        ZenDrawDottedRect((Rectangle){ 0, 0, game.x, game.y }, line);

        // Selected element anchor ring, so it's easy to see what's picked.
        if (zen.selElem >= 0 && zen.selElem < zen.doc.elemCount)
        {
            const AnimElem *e = &zen.doc.elems[zen.selElem];
            if (e->kind != AE_GLOBAL)
            {
                bool text = e->kind == AE_TEXT;
                float cx = game.x * AnimElemProp(e, text ? AP_T_POS_X : AP_S_POS_X, zen.playhead);
                float cy = game.y * AnimElemProp(e, text ? AP_T_POS_Y : AP_S_POS_Y, zen.playhead);
                DrawCircleLines((int)cx, (int)cy, 8.0f,
                                (Color){ 255, 210, 90, (unsigned char)(255 * fade) });
            }
        }
    }

    EndMode2D();
}
