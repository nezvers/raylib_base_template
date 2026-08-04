// ============================================================================
//  zen_panels.c  -  the four Zen editor panels
//
//  ELEMENTS  (left, top)    clean selection list; ops live in the Element
//                           menu and (later) viewport right-click.
//  SIGNALS   (left, bottom) create / open / fire / delete; contents live in
//                           the signal modal.
//  INSPECTOR (right)        base fields of the selection + a COLLAPSIBLE
//                           track list. Key editing happens in the track
//                           modal, not inline - clicking a track header or a
//                           key row opens/updates it.
//  TIMELINE  (bottom)       group lanes with diamonds. Click selects + opens
//                           the track modal, Shift+click multi-selects,
//                           Ctrl while dragging snaps to the tick grid.
//
//  Every clipped label goes through ZenLabelTip so hovering always reveals
//  the full name. Panels slide away during playback exactly like the classic
//  editor.
// ============================================================================

#include "raylib.h"
#include "raygui.h"
#include "zen_internal.h"
#include "../screen_state/screen_state.h"
#include "../audio_state/audio_state.h"
#include "../anim/signal.h"
#include <math.h>

#define ZEN_TIMELINE_SNAP 0.1f     // Ctrl-drag key snap grid (seconds)
// How close the scrubbing playhead counts as "on" a key. Wider than
// ZEN_AUTOKEY_EPS so a sweep reliably catches keys at normal mouse speed, and
// wide enough that near-coincident keys are picked up together.
#define ZEN_SCRUB_KEY_EPS 0.04f

// ---------------------------------------------------------------------------
//  Scrolling panel wrapper (scissor + wheel + outside-lock), classic pattern.
// ---------------------------------------------------------------------------
static float PanelScroll(ZenPanelView *v, Rectangle panel,
                         float (*draw)(float x, float y, float w))
{
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (!ctrl && !zen.guiLocked && CheckCollisionPointRec(GetMousePosition(), panel))
        v->scroll += GetMouseWheelMove() * 30.0f;

    float maxScroll = v->contentH - (panel.height - 16);
    if (maxScroll < 0) maxScroll = 0;
    if (v->scroll < -maxScroll) v->scroll = -maxScroll;
    if (v->scroll > 0) v->scroll = 0;

    // Scissor clips pixels, not hit-testing: lock the gui while the mouse is
    // outside this panel so scrolled-away rows can't swallow clicks.
    bool wasLocked = zen.guiLocked;
    bool inPanel   = CheckCollisionPointRec(GetMousePosition(), panel);
    if (!inPanel && !wasLocked) GuiLock();

    BeginScissorMode((int)panel.x+1, (int)panel.y+1,
                     (int)panel.width-2, (int)panel.height-2);
    float h = draw(panel.x+8, panel.y+8 + v->scroll, panel.width-16);
    EndScissorMode();

    if (!inPanel && !wasLocked) GuiUnlock();
    return h;
}

// ---------------------------------------------------------------------------
//  ELEMENTS
// ---------------------------------------------------------------------------
static float DrawElementList(float x, float y, float w)
{
    float y0 = y, rh = 26.0f, gap = 4.0f;
    bool elemsFull = (zen.doc.elemCount >= ANIM_ELEMS_MAX);
    ZenLabelTip((Rectangle){ x+4, y, w-4, 20 },
                TextFormat("ELEMENTS  %d/%d%s", zen.doc.elemCount, ANIM_ELEMS_MAX,
                           elemsFull? "  FULL" : ""),
                "Click selects - Ctrl+click multi-selects. All operations live in the "
                "Element menu.  The count is this animation's element budget - when a "
                "block has finished its part, reuse it with Element > Clone from... "
                "instead of spending a new slot.");
    y += 22;

    for (int i = 0; i < zen.doc.elemCount; i++)
    {
        AnimElem *e = &zen.doc.elems[i];
        Rectangle nameR = { x, y, w, rh };
        // kind badge + name; the badge is part of the button label so the
        // whole row is one click target.
        const char *kindTag = e->kind == AE_TEXT ? "T" : e->kind == AE_SHAPE ? "S" : "G";
        bool pressed = GuiButton(nameR, "");
        ZenLabelTip((Rectangle){ x + 26, y + 4, w - 30, rh - 8 }, e->name,
                    TextFormat("%s element", AnimElemKindName(e->kind)));
        DrawRectangleLines((int)x + 4, (int)y + 5, 16, 16, (Color){ 110, 116, 128, 255 });
        DrawText(kindTag, (int)x + 9, (int)y + 8, 10, (Color){ 160, 166, 178, 255 });

        // selection tint AFTER the button (raygui paints its own background).
        if (i == zen.selElem)
            DrawRectangleRec(nameR, (Color){ 90, 140, 220, 90 });
        else if (ZenIsSelected(i))
            DrawRectangleRec(nameR, (Color){ 60, 90, 140, 70 });

        if (pressed)
        {
            AudioPlayButton();
            bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            if (ctrl && i != zen.selElem) zen.multiSel[i] = !zen.multiSel[i];
            else
            {
                if (zen.selElem != i) ZenSelClear();
                zen.selElem = i;
                for (int k = 0; k < ANIM_ELEMS_MAX; k++) zen.multiSel[k] = false;
            }
        }
        y += rh + gap;
    }

    // quick-add row (the menu has the same ops; this is the one-click path).
    y += 4;
    float bw = (w - 8) / 3.0f;
    if (zen.doc.elemCount >= ANIM_ELEMS_MAX) GuiDisable();
    if (GuiButton((Rectangle){ x, y, bw, rh }, "+text"))
    { AudioPlayButton(); ZenElemAdd(AE_TEXT); }
    if (GuiButton((Rectangle){ x+bw+4, y, bw, rh }, "+shape"))
    { AudioPlayButton(); ZenElemAdd(AE_SHAPE); }
    if (GuiButton((Rectangle){ x+2*bw+8, y, bw, rh }, "+global"))
    { AudioPlayButton(); ZenElemAdd(AE_GLOBAL); }
    if (zen.doc.elemCount >= ANIM_ELEMS_MAX) GuiEnable();
    y += rh + gap;

    return (y + 4) - y0;
}

// ---------------------------------------------------------------------------
//  SIGNALS
// ---------------------------------------------------------------------------
void ZenFireSignal(const AnimSignal *sg)
{
    zen.preview.seq = zen.previewSeq;

    if (!sg->usesPos) { SignalEmit(sg->name, NULL); return; }

    // a position-consuming signal is emitted AT THE MOUSE in canvas fractions,
    // exactly like the game does on a click.
    Vector2 game = ScreenStateTargetSize();
    Vector2 px   = Screen2Target(GetMousePosition());
    SignalParams p = { .pos = { px.x/game.x, px.y/game.y }, .hasPos = true };
    SignalEmit(sg->name, &p);
}

static float DrawSignalList(float x, float y, float w)
{
    float y0 = y, rh = 26.0f, gap = 4.0f;
    bool sigsFull = (zen.doc.signalCount >= ANIM_SIGNALS_MAX);
    ZenLabelTip((Rectangle){ x+4, y, w-4, 20 },
                TextFormat("SIGNALS  %d/%d%s", zen.doc.signalCount, ANIM_SIGNALS_MAX,
                           sigsFull? "  FULL" : ""),
                "Click opens the signal in its modal; the play glyph fires it live.  "
                "The count is this animation's signal budget.");
    y += 22;

    int pendingDel = -1;
    float bw2 = 22.0f;
    for (int i = 0; i < zen.doc.signalCount; i++)
    {
        AnimSignal *sg = &zen.doc.signals[i];
        Rectangle openR = { x, y, w - 2*bw2 - 4, rh };
        bool pressed = GuiButton(openR, "");
        ZenLabelTip((Rectangle){ x + 6, y + 4, openR.width - 10, rh - 8 },
                    TextFormat("%s  (%d)", sg->name, sg->targetCount),
                    "Open this signal's targets and keys");
        if (pressed)
        {
            AudioPlayButton();
            zen.sigModalIdx = i; zen.sigScroll = 0.0f;
            ZenSigClearKeySel(); ZenSigCloseDrops();
        }
        if (i == zen.sigModalIdx) DrawRectangleRec(openR, (Color){ 90, 140, 220, 90 });

        if (GuiButton((Rectangle){ x + w - 2*bw2 - 2, y, bw2, rh }, "#131#"))
        { AudioPlayButton(); ZenFireSignal(sg); }
        if (GuiButton((Rectangle){ x + w - bw2, y, bw2, rh }, "#143#"))
        { AudioPlayButton(); pendingDel = i; }
        y += rh + gap;
    }

    if (zen.doc.signalCount < ANIM_SIGNALS_MAX)
    {
        if (GuiButton((Rectangle){ x, y, w, rh }, "+signal"))
        {
            AudioPlayButton(); ZenUndoPush();
            AnimSignal *sg = &zen.doc.signals[zen.doc.signalCount++];
            TextCopy(sg->name, TextFormat("sig%d", zen.doc.signalCount));
            // every field explicitly: the slot may hold a deleted signal's data
            sg->length = 1.0f; sg->targetCount = 0;
            sg->terminal = false; sg->usesPos = false; sg->posAnchor = false;
            sg->replay = false;
            sg->usesSeq = false; sg->seqMult = 0.0f;
            sg->posParamCount = 0; sg->seqTargetCount = 0; sg->seqKeyCount = 0;
            zen.sigModalIdx = zen.doc.signalCount - 1; zen.sigScroll = 0.0f;
            ZenSigClearKeySel(); ZenSigCloseDrops();
            ZenReRegisterSignals();
        }
        y += rh + gap;
    }

    // deferred: deleting mid-loop shifts the array under the next row's buttons
    if (pendingDel >= 0)
    {
        ZenUndoPush();
        for (int m = pendingDel; m < zen.doc.signalCount - 1; m++)
            zen.doc.signals[m] = zen.doc.signals[m+1];
        zen.doc.signalCount--;
        ZenSigClearKeySel(); ZenSigCloseDrops();
        if      (zen.sigModalIdx == pendingDel) zen.sigModalIdx = -1;
        else if (zen.sigModalIdx >  pendingDel) zen.sigModalIdx--;
        if      (zen.edSigIdx == pendingDel) zen.edSigIdx = -1;
        else if (zen.edSigIdx >  pendingDel) zen.edSigIdx--;
        ZenReRegisterSignals();
    }

    return (y + 4) - y0;
}

// ---------------------------------------------------------------------------
//  INSPECTOR - base fields + collapsible track list. NO inline key editor:
//  the track modal owns that.
// ---------------------------------------------------------------------------

// The base (rest-pose) field a scalar geometry prop writes into.
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

// Inspector row label: the short caption the panel has room for, plus the
// property's plain-language explanation on hover ("rot" -> what rot means).
static void PropLabel(Rectangle r, const char *caption, int prop)
{
    GuiLabel(r, caption);
    ZenTip(r, ZenPropDesc(prop));
}

// Keyframe-or-base write, same semantics as ZenPropSlider commits.
static void WritePropValue(AnimElem *e, int prop, float v, float *baseField)
{
    AnimTrack *tr = AnimElemFindTrack(e, prop);
    if (!tr && zen.autoKey)
    {
        // auto-key starts the track here too, same as the sliders. Adding a
        // track is structural, so it gets its own undo step; t=0 is seeded
        // from the original pose before the base field moves.
        ZenUndoPush();
        tr = AnimElemAddTrack(e, prop);
        if (tr && zen.playhead > ZEN_AUTOKEY_EPS) ZenEnsureZeroKey(e, tr);
    }
    if (!tr) { if (baseField) *baseField = v; return; }
    if (!zen.autoKey) return;
    if (zen.playhead > ZEN_AUTOKEY_EPS) ZenEnsureZeroKey(e, tr);
    AnimTrackWriteKeyAt(tr, zen.playhead, v, ZEN_AUTOKEY_EPS);
    ZenAutoKeyFocus(zen.selElem, prop, zen.playhead);   // inspector edits the selection
}

// Corner-mode geometry rows (shape by two corners / line by endpoints);
// storage stays center+size. Ported semantics from the classic editor.
static float DrawCornerRows(float x, float y, float w, float rh, float gap,
                            AnimElem *e)
{
    Vector2 game = ScreenStateTargetSize();
    float lo = -3.0f, hi = 3.0f;
    float valW = 48.0f;
    float half = (w - 44 - gap - 2*valW) * 0.5f;
    float x2   = x + 44 + half + valW + gap;
    float cx = AnimElemProp(e, AP_S_POS_X, zen.playhead);
    float cy = AnimElemProp(e, AP_S_POS_Y, zen.playhead);
    float wv = AnimElemProp(e, AP_S_W, zen.playhead);
    float wUnit = e->sizeAbsolute ? 1.0f : game.x;
    float sc = AnimElemProp(e, AP_S_SCALE, zen.playhead);
    float invSc = sc > 1e-6f ? 1.0f/sc : 1.0f;

    float sx = e->sizeAbsolute ? game.x : 1.0f;
    float sy = e->sizeAbsolute ? game.y : 1.0f;
    float loX = lo*sx, hiX = hi*sx, loY = lo*sy, hiY = hi*sy;

    if (e->shapeKind == SHAPE_LINE)
    {
        float hUnit = e->sizeAbsolute ? 1.0f : game.y;
        float rot = AnimElemProp(e, AP_S_ROT, zen.playhead) * DEG2RAD;
        float wEff = wv * sc;
        float ox = (wUnit * wEff * 0.5f) * cosf(rot) / game.x;
        float oy = (hUnit * wEff * 0.5f) * sinf(rot) / game.y;
        float ax = (cx - ox)*sx, ay = (cy - oy)*sy, bx = (cx + ox)*sx, by = (cy + oy)*sy;
        bool ch = false;
        ZenLabelTip((Rectangle){ x, y, 44, rh }, "start",
                    "First endpoint of the line. Moving it rewrites the line's "
                    "center, length and rotation");
        if (ZenEditSlider((Rectangle){ x+44, y, half, rh }, "x", &ax, loX, hiX)) ch = true;
        if (ZenEditSlider((Rectangle){ x2, y, half, rh }, "y", &ay, loY, hiY)) ch = true;
        y += rh + gap;
        ZenLabelTip((Rectangle){ x, y, 44, rh }, "end",
                    "Second endpoint of the line. Moving it rewrites the line's "
                    "center, length and rotation");
        if (ZenEditSlider((Rectangle){ x+44, y, half, rh }, "x", &bx, loX, hiX)) ch = true;
        if (ZenEditSlider((Rectangle){ x2, y, half, rh }, "y", &by, loY, hiY)) ch = true;
        y += rh + gap;
        if (ch)
        {
            float axF = ax/sx, ayF = ay/sy, bxF = bx/sx, byF = by/sy;
            float dux = (bxF - axF) * game.x / wUnit;
            float duy = (byF - ayF) * game.y / hUnit;
            float lenFull = hypotf(dux, duy);
            WritePropValue(e, AP_S_POS_X, (axF + bxF) * 0.5f, &e->posFrac.x);
            WritePropValue(e, AP_S_POS_Y, (ayF + byF) * 0.5f, &e->posFrac.y);
            WritePropValue(e, AP_S_W, lenFull * invSc, &e->sizeFrac.x);
            WritePropValue(e, AP_S_ROT, atan2f(duy, dux) * RAD2DEG, &e->rotBase);
        }
    }
    else
    {
        float hv = AnimElemProp(e, AP_S_H, zen.playhead);
        float hUnit = e->sizeAbsolute ? 1.0f : game.y;
        float hxF = (wUnit*wv*sc/game.x) * 0.5f, hyF = (hUnit*hv*sc/game.y) * 0.5f;
        float x0 = (cx-hxF)*sx, y0 = (cy-hyF)*sy, x1 = (cx+hxF)*sx, y1 = (cy+hyF)*sy;
        bool ch = false;
        ZenLabelTip((Rectangle){ x, y, 44, rh }, "P0",
                    "Top-left corner. Dragging it rewrites the shape's center "
                    "and size instead of moving the whole shape");
        if (ZenEditSlider((Rectangle){ x+44, y, half, rh }, "x", &x0, loX, hiX)) ch = true;
        if (ZenEditSlider((Rectangle){ x2, y, half, rh }, "y", &y0, loY, hiY)) ch = true;
        y += rh + gap;
        ZenLabelTip((Rectangle){ x, y, 44, rh }, "P1",
                    "Bottom-right corner. Dragging it rewrites the shape's "
                    "center and size instead of moving the whole shape");
        if (ZenEditSlider((Rectangle){ x+44, y, half, rh }, "x", &x1, loX, hiX)) ch = true;
        if (ZenEditSlider((Rectangle){ x2, y, half, rh }, "y", &y1, loY, hiY)) ch = true;
        y += rh + gap;
        if (ch)
        {
            float x0F = x0/sx, y0F = y0/sy, x1F = x1/sx, y1F = y1/sy;
            float wF = fabsf(x1F-x0F), hF = fabsf(y1F-y0F);
            WritePropValue(e, AP_S_POS_X, (x0F+x1F)*0.5f, &e->posFrac.x);
            WritePropValue(e, AP_S_POS_Y, (y0F+y1F)*0.5f, &e->posFrac.y);
            WritePropValue(e, AP_S_W, (e->sizeAbsolute ? wF*game.x : wF) * invSc, &e->sizeFrac.x);
            WritePropValue(e, AP_S_H, (e->sizeAbsolute ? hF*game.y : hF) * invSc, &e->sizeFrac.y);
        }
    }
    return y;
}

// last click on the inspector's "text" label, for double-click detection
static double s_textLblClick = 0.0;

static float DrawInspector(float x, float y, float w)
{
    float y0 = y;
    ZenLabelTip((Rectangle){ x+4, y, w-4, 20 }, "INSPECTOR",
                "Base pose of the selected element; tracks collapse below.");
    y += 24;
    if (zen.selElem < 0 || zen.selElem >= zen.doc.elemCount)
    {
        GuiLabel((Rectangle){ x, y, w, 20 }, "(no element selected)");
        return (y + 20) - y0;
    }
    AnimElem *e = &zen.doc.elems[zen.selElem];
    float rh = 24.0f, gap = 6.0f;

    GuiLabel((Rectangle){ x, y, 40, rh }, "name");
    if (GuiTextBox((Rectangle){ x+44, y, w-44, rh }, e->name, ANIM_NAME_MAX, zen.edName))
    { if (!zen.edName) ZenUndoPush(); zen.edName = !zen.edName; }
    y += rh + gap;

    if (e->kind == AE_TEXT)
    {
        // The "text" label is a BUTTON in disguise: double-clicking it opens the
        // string pool, where this element's words can be pointed at a shared
        // entry (the only way text can change mid-animation). Tinted on hover so
        // it does not look like dead chrome.
        Rectangle lblR = { x, y, 40, rh };
        bool lblHot = !zen.guiLocked && CheckCollisionPointRec(GetMousePosition(), lblR);
        // Resolved AT THE PLAYHEAD, not from keys[0]: once the words change
        // over time, the first key is not what is on screen.
        int strIdx = -1;
        {
            AnimTrack *st = AnimElemFindTrack(e, AP_T_STRING);
            if (st && st->keyCount > 0)
                strIdx = (int)(AnimTrackEval(st, zen.playhead, -1.0f) + 0.5f);
        }
        bool pooled = (AnimDocStringAt(&zen.doc, strIdx) != NULL);
        DrawText("text", (int)lblR.x, (int)(lblR.y + 5), 10,
                 lblHot ? (Color){ 150, 200, 255, 255 }
                        : (pooled ? (Color){ 92, 158, 232, 255 }
                                  : (Color){ 160, 166, 178, 255 }));
        if (pooled)
            DrawText(TextFormat("%02d", strIdx), (int)lblR.x + 26, (int)(lblR.y + 5), 10,
                     (Color){ 92, 158, 232, 255 });

        if (lblHot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            double now = GetTime();
            if (now - s_textLblClick < 0.35) { AudioPlayButton(); ZenStringPoolShow(); }
            s_textLblClick = now;
        }

        // grows with the content up to 8 rows, then scrolls inside itself -
        // reading it all here is not the point, the viewport shows the text.
        float th = ZenTextAreaHeight(e->text, 8);
        if (th < rh) th = rh;
        if (ZenEditTextArea((Rectangle){ x+44, y, w-44, th },
                            e->text, ANIM_TEXT_LEN_MAX, zen.edText))
        {
            if (!zen.edText) ZenUndoPush();
            else
            {
                // COMMIT: the words just typed join the pool (deduped), so every
                // string in the document is reusable without extra ceremony. If
                // this element already points at a pool entry, the edit updates
                // THAT entry - the pool is the source of truth once assigned.
                if (pooled) TextCopy(zen.doc.strings[strIdx].text, e->text);
                else        AnimDocAddString(&zen.doc, e->text);
            }
            zen.edText = !zen.edText;
        }
        ZenTip(lblR, pooled
            ? "This element shows a pooled string - editing here changes the pool "
              "entry (and every element sharing it). Double-click 'text' to open "
              "the string pool and reassign."
            : "Shift+Enter adds a line, Enter commits. Double-click 'text' to open "
              "the string pool, where a string can be shared and animated.");
        y += th + gap;
    }
    if (e->kind == AE_SHAPE)
    {
        GuiLabel((Rectangle){ x, y, 40, rh }, "shape");
        float bw3 = (w - 44 - 8) / 3.0f;
        for (int si = 0; si < SHAPE_KIND_COUNT; si++)
        {
            int rowI = si / 3, colI = si % 3;
            Rectangle rr = { x + 44 + (float)colI * (bw3 + 4),
                             y + (float)rowI * (rh + 4), bw3, rh };
            bool on = (e->shapeKind == si);
            GuiToggle(rr, AnimShapeKindName(si), &on);
            if (on && e->shapeKind != si) { ZenUndoPush(); e->shapeKind = si; }
        }
        y += 2*(rh + 4) + gap - 4;
    }

    if (e->kind != AE_GLOBAL)
    {
        bool isText = e->kind == AE_TEXT;

        // units toggle (rescales base + keys so nothing visually moves).
        ZenLabelTip((Rectangle){ x, y, 44, rh }, "units",
                    "How sizes are stored: '% canvas' scales with the window, "
                    "'px abs' keeps a fixed pixel size. Switching rescales the "
                    "base pose and every key so nothing visually moves");
        float uw = (w - 44 - 4) / 2.0f;
        bool wantFrac = !e->sizeAbsolute, wantAbs = e->sizeAbsolute;
        GuiToggle((Rectangle){ x+44, y, uw, rh }, "% canvas", &wantFrac);
        GuiToggle((Rectangle){ x+44+uw+4, y, uw, rh }, "px abs", &wantAbs);
        if (e->sizeAbsolute && wantFrac)
        { ZenUndoPush(); ZenConvertSizeUnits(e, false); e->sizeAbsolute = false; }
        else if (!e->sizeAbsolute && wantAbs)
        { ZenUndoPush(); ZenConvertSizeUnits(e, true);  e->sizeAbsolute = true; }
        y += rh + gap;

        if (e->kind == AE_SHAPE)
        {
            ZenLabelTip((Rectangle){ x, y, 44, rh }, "anchor",
                        "How the shape is edited: by its center plus a size, or "
                        "by two opposite corners. Storage is the same either way");
            float aw = (w - 44 - 4) / 2.0f;
            bool wantCtr = !e->cornerMode, wantCor = e->cornerMode;
            GuiToggle((Rectangle){ x+44, y, aw, rh }, "center+size", &wantCtr);
            GuiToggle((Rectangle){ x+44+aw+4, y, aw, rh }, "corners", &wantCor);
            if (e->cornerMode && wantCtr)       { ZenUndoPush(); e->cornerMode = false; }
            else if (!e->cornerMode && wantCor) { ZenUndoPush(); e->cornerMode = true; }
            y += rh + gap;
        }

        if (e->kind == AE_SHAPE && e->cornerMode)
        {
            y = DrawCornerRows(x, y, w, rh, gap, e);
            if (e->shapeKind != SHAPE_LINE)
            {
                ZenPropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, AP_S_SCALE, &e->scaleFrac);
                PropLabel((Rectangle){ x, y, 44, rh }, "scale", AP_S_SCALE); y += rh + gap;
            }
        }
        else
        {
            int pX = isText ? AP_T_POS_X : AP_S_POS_X;
            int pY = isText ? AP_T_POS_Y : AP_S_POS_Y;
            int pW = isText ? AP_T_SIZE  : AP_S_W;
            ZenPropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, pX, &e->posFrac.x);
            PropLabel((Rectangle){ x, y, 44, rh }, "posX", pX); y += rh + gap;
            ZenPropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, pY, &e->posFrac.y);
            PropLabel((Rectangle){ x, y, 44, rh }, "posY", pY); y += rh + gap;
            ZenPropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, pW, &e->sizeFrac.x);
            PropLabel((Rectangle){ x, y, 44, rh },
                      isText ? "size" : (e->shapeKind==SHAPE_LINE?"length":"w"), pW); y += rh + gap;
            if (e->kind == AE_SHAPE)
            {
                ZenPropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, AP_S_H, &e->sizeFrac.y);
                PropLabel((Rectangle){ x, y, 44, rh },
                          e->shapeKind==SHAPE_LINE?"thick":"h", AP_S_H); y += rh + gap;
                ZenPropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, AP_S_SCALE, &e->scaleFrac);
                PropLabel((Rectangle){ x, y, 44, rh }, "scale", AP_S_SCALE); y += rh + gap;
            }
        }

        if (e->kind == AE_SHAPE && e->cornerMode && e->shapeKind == SHAPE_LINE)
        {
            ZenPropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, AP_S_H, &e->sizeFrac.y);
            PropLabel((Rectangle){ x, y, 44, rh }, "thick", AP_S_H); y += rh + gap;
        }

        if (!(e->kind == AE_SHAPE && e->cornerMode && e->shapeKind == SHAPE_LINE))
        {
            int pR = isText ? AP_T_ROT : AP_S_ROT;
            ZenPropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, pR, &e->rotBase);
            PropLabel((Rectangle){ x, y, 44, rh }, "rot", pR); y += rh + gap;
        }
    }

    y = ZenColorRGBRows(x, y, w, e, ZenColorPropFor(e->kind), &e->color, "color");
    float ca = e->color.a;
    Rectangle caR = { x+16, y, w-16-50, 16 };
    if (ZenEditSlider(caR, "A", &ca, 0,255)) e->color.a = (unsigned char)ca;
    ZenTip(caR, "Base opacity of this colour (0-255). This is the rest pose - "
                "the 'alpha' track is what animates opacity over time");
    y += 22;

    if (e->kind == AE_GLOBAL)
    {
        GuiLine((Rectangle){ x, y, w, 8 }, "background"); y += 12;
        y = ZenColorRGBRows(x, y, w, e, AP_G_BG_COLOR, &e->bgColor, "background");
        float ba = e->bgColor.a;
        Rectangle baR = { x+16, y, w-16-50, 16 };
        if (ZenEditSlider(baR, "A", &ba, 0,255)) e->bgColor.a = (unsigned char)ba;
        ZenTip(baR, "Base opacity of the scene background fill (0-255). The "
                    "'bg_alpha' track animates it over time");
        y += 22;
    }

    if (e->kind == AE_SHAPE)
    {
        GuiLine((Rectangle){ x, y, w, 8 }, "outline"); y += 12;
        ZenPropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, AP_S_OUTLINE, &e->outlineFrac);
        PropLabel((Rectangle){ x, y, 44, rh }, "thick", AP_S_OUTLINE); y += rh + gap;
        y = ZenColorRGBRows(x, y, w, e, AP_S_OUTLINE_COLOR, &e->outlineColor, "outline");
        float oa = e->outlineColor.a;
        Rectangle oaR = { x+16, y, w-16-50, 16 };
        if (ZenEditSlider(oaR, "A", &oa, 0,255)) e->outlineColor.a = (unsigned char)oa;
        ZenTip(oaR, "Base opacity of the outline (0-255), separate from the "
                    "fill. The 'outline_alpha' track animates it");
        y += 22;

        if (e->shapeKind == SHAPE_CIRCLE)
        {
            ZenLabelTip((Rectangle){ x, y, 44, rh }, "style",
                        "How the circle's outline is drawn: 'crawling' follows "
                        "the ring smoothly, 'crisp' keeps a hard edge");
            float sw = (w - 44 - 4) / 2.0f;
            bool wantCrawl = !e->outlineCrisp, wantCrisp = e->outlineCrisp;
            GuiToggle((Rectangle){ x+44, y, sw, rh }, "crawling", &wantCrawl);
            GuiToggle((Rectangle){ x+44+sw+4, y, sw, rh }, "crisp", &wantCrisp);
            if (e->outlineCrisp && wantCrawl)       { ZenUndoPush(); e->outlineCrisp = false; }
            else if (!e->outlineCrisp && wantCrisp) { ZenUndoPush(); e->outlineCrisp = true; }
            y += rh + gap;
        }
    }

    // --- collapsible tracks --------------------------------------------------
    GuiLine((Rectangle){ x, y, w, 8 }, "tracks"); y += 12;

    int pendingGroupDel = -1;
    int grpN = AnimGroupCountFor(e->kind);
    for (int gi = 0; gi < grpN; gi++)
    {
        if (!ZenGroupHasTrack(e, gi)) continue;
        const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
        float times[ZEN_GROUP_TIMES_MAX];
        int nt = ZenGroupKeyTimes(e, gi, times);
        bool expanded = (zen.trackExpand[zen.selElem] >> gi) & 1u;
        bool selTrack = (zen.selGroup == gi);

        // header row: expander glyph + name + key count + [+key] [del].
        Rectangle hdrR = { x, y, w - 104, rh };
        bool hdrPressed = GuiButton(hdrR, "");
        const char *gDesc = g->propCount > 0 ? ZenPropDesc(g->props[0]) : NULL;
        ZenLabelTip((Rectangle){ x + 22, y + 4, hdrR.width - 26, rh - 8 },
                    TextFormat("%s (%d)", g->name, nt),
                    gDesc ? TextFormat("%s. Click selects the track for bulk "
                                       "editing; the arrow expands its keys.", gDesc)
                          : "Click: select track (bulk edit in the modal). "
                            "Arrow expands the key list.");
        if (selTrack) DrawRectangleRec(hdrR, (Color){ 90, 140, 220, 70 });

        // expander arrow: its own small click zone on the left of the header.
        Rectangle arrowR = { x, y, 20, rh };
        DrawText(expanded ? "v" : ">", (int)x + 7, (int)y + 7, 10,
                 (Color){ 160, 166, 178, 255 });
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !zen.guiLocked &&
            CheckCollisionPointRec(GetMousePosition(), arrowR))
        {
            zen.trackExpand[zen.selElem] ^= 1u << gi;
            hdrPressed = false;
        }
        else if (hdrPressed)
        {
            AudioPlayButton();
            ZenSelTrack(zen.selElem, gi);
            ZenTrackModalOpen(zen.selElem, gi);
        }

        if (GuiButton((Rectangle){ x+w-102, y, 50, rh }, "+key"))
        {
            AudioPlayButton(); ZenUndoPush();
            ZenGroupWriteKey(e, gi, zen.playhead);
            ZenSelKey(zen.selElem, gi, zen.playhead, false);
            ZenTrackModalOpen(zen.selElem, gi);
        }
        if (GuiButton((Rectangle){ x+w-48, y, 48, rh }, "del"))
        { AudioPlayButton(); ZenUndoPush(); pendingGroupDel = gi; }
        y += rh + 2;

        if (expanded)
        {
            int colorProp = ZenGroupColorProp(e->kind, gi);
            for (int i = 0; i < nt; i++)
            {
                float t = times[i];
                bool sel = ZenKeyIsSelected(zen.selElem, gi, t);
                Rectangle kr = { x+20, y, w-20, 18 };
                bool pressed = GuiButton(kr, "");
                ZenLabelTip((Rectangle){ kr.x + 4, kr.y + 2, kr.width - 28, 14 },
                            ZenGroupKeyLabel(e, gi, t),
                            "Click: edit in the modal. Shift+click: multi-select.");
                if (sel) DrawRectangleRec(kr, (Color){ 60, 90, 140, 120 });
                if (colorProp >= 0)
                {
                    Color sc = AnimElemColorProp(e, colorProp, t); sc.a = 255;
                    ZenDrawSwatch((Rectangle){ kr.x + kr.width - 18, kr.y + 2, 14, 14 }, sc);
                }
                if (pressed)
                {
                    AudioPlayButton();
                    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
                    ZenSelKey(zen.selElem, gi, t, shift);
                    ZenTrackModalOpen(zen.selElem, gi);
                }
                y += 20;
            }
            y += 4;
        }
    }
    if (pendingGroupDel >= 0)
    {
        if (zen.selGroup == pendingGroupDel) ZenSelClear();
        ZenGroupDeleteTracks(e, pendingGroupDel);
        ZenSelValidate();
    }

    // add-track dropdown (header inline, list drawn topmost as an overlay).
    y += 4;
    int addCount = AnimGroupCountFor(e->kind);
    if (zen.addTrackSel >= addCount) zen.addTrackSel = 0;
    // never sit on a group that already has a track: slide to the first free
    // one so +track always names something it can actually create.
    if (ZenGroupHasTrack(e, zen.addTrackSel))
        for (int i = 0; i < addCount; i++)
            if (!ZenGroupHasTrack(e, i)) { zen.addTrackSel = i; break; }
    bool addTaken = ZenGroupHasTrack(e, zen.addTrackSel);   // every group tracked
    Rectangle addR = { x, y, w-56, rh };
    if (addTaken) GuiDisable();
    if (GuiButton((Rectangle){ x+w-52, y, 52, rh }, "+track"))
    {
        AudioPlayButton(); ZenUndoPush();
        ZenGroupWriteKey(e, zen.addTrackSel, zen.playhead);
        ZenSelKey(zen.selElem, zen.addTrackSel, zen.playhead, false);
        ZenTrackModalOpen(zen.selElem, zen.addTrackSel);
    }
    if (addTaken) GuiEnable();
    const AnimPropGroup *addG = AnimGroupAt(e->kind, zen.addTrackSel);
    if (GuiButton(addR, TextFormat("%s  v", addG ? addG->name : "?")))
    { AudioPlayButton(); zen.addTrackOpen = !zen.addTrackOpen; }
    zen.addTrackRect = addR;
    y += rh + 10;

    return (y + 12) - y0;
}

// The add-track group list, drawn topmost (flips above when out of room).
// Runs in the overlay pass AFTER GuiUnlock(): opening the list is what locks
// the base layer, so drawn from inside ZenPanelsGui() its rows never register
// a press (raygui gates presses on !guiLocked, but still draws them).
void ZenPanelsOverlaysGui(void)
{
    if (!zen.addTrackOpen) return;
    if (zen.selElem < 0 || zen.selElem >= zen.doc.elemCount)
    { zen.addTrackOpen = false; return; }

    AnimElem *e = &zen.doc.elems[zen.selElem];
    Vector2 screen = ScreenStateSize();
    Rectangle hdr = zen.addTrackRect;
    int n = AnimGroupCountFor(e->kind);
    float ih = 20.0f, listH = ih * n;
    float ly = (hdr.y + hdr.height + listH <= screen.y - 4.0f)
             ? hdr.y + hdr.height : hdr.y - listH;
    if (ly < 4.0f) ly = 4.0f;
    Rectangle bg = { hdr.x, ly, hdr.width, listH };
    DrawRectangleRec(bg, (Color){ 32, 34, 40, 255 });
    DrawRectangleLinesEx(bg, 1.0f, (Color){ 70, 74, 84, 255 });

    for (int i = 0; i < n; i++)
    {
        const AnimPropGroup *g = AnimGroupAt(e->kind, i);
        Rectangle rr = { bg.x, bg.y + i*ih, bg.width, ih };
        bool exists = ZenGroupHasTrack(e, i);

        // highlight goes under the button so it can't paint over the label.
        if (i == zen.addTrackSel && !exists)
            DrawRectangleRec(rr, (Color){ 90, 140, 220, 60 });

        if (exists)
        {
            // already tracked: dimmed, and clicking jumps to its modal rather
            // than offering to create it again. A disabled GuiButton never
            // reports a press, so the hit test is manual.
            GuiDisable();
            GuiButton(rr, TextFormat("%s  (exists)", g ? g->name : "?"));
            GuiEnable();
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                CheckCollisionPointRec(GetMousePosition(), rr))
            {
                AudioPlayButton();
                ZenSelTrack(zen.selElem, i);
                ZenTrackModalOpen(zen.selElem, i);
                zen.addTrackOpen = false;
            }
        }
        else if (GuiButton(rr, g ? g->name : "?"))
        { AudioPlayButton(); zen.addTrackSel = i; zen.addTrackOpen = false; }
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        !CheckCollisionPointRec(GetMousePosition(), bg) &&
        !CheckCollisionPointRec(GetMousePosition(), hdr))
        zen.addTrackOpen = false;
}

// ---------------------------------------------------------------------------
//  TIMELINE
// ---------------------------------------------------------------------------
static void DrawDiamond(float cx, float cy, float r, Color c)
{
    DrawTriangle((Vector2){cx,cy-r},(Vector2){cx-r,cy},(Vector2){cx+r,cy}, c);
    DrawTriangle((Vector2){cx-r,cy},(Vector2){cx,cy+r},(Vector2){cx+r,cy}, c);
}

static void DrawMarkerTriangle(float cx, float cy, float r, bool down, Color c)
{
    if (down) DrawTriangle((Vector2){cx-r,cy-r},(Vector2){cx,cy},(Vector2){cx+r,cy-r}, c);
    else      DrawTriangle((Vector2){cx,cy},(Vector2){cx-r,cy+r},(Vector2){cx+r,cy+r}, c);
}

static void DrawDottedV(float x, float y0, float y1, Color c)
{
    for (float yy = y0; yy < y1; yy += 8.0f)
    {
        float seg = (yy + 4.0f < y1) ? yy + 4.0f : y1;
        DrawLine((int)x, (int)yy, (int)x, (int)seg, c);
    }
}

// ---------------------------------------------------------------------------
//  Timeline right-click menu: pause markers, plus key cloning / deletion.
//
//  The viewport already has a context menu (ZenViewContextGui) but it lists the
//  selected ELEMENT's tracks - a different scope entirely - so the timeline gets
//  its own. Same shape and same rules: file-static, drawn in the UNLOCKED
//  overlay pass, click-away closes.
//
//  Two clone flavours, and the difference matters:
//    * on empty bar - restate the WHOLE pose here, by cloning every track's
//      last key to the left. This is the "hold what I have" move: it freezes
//      the element as it was authored at the previous keys, which is not the
//      same as sampling the element here (that would bake the mid-blend pose).
//    * on a key - copy THAT one group key to the playhead, for repeating a
//      single pose later without touching the other tracks.
// ---------------------------------------------------------------------------
static bool    s_tlCtxOpen = false;
static Vector2 s_tlCtxPos;
static float   s_tlCtxTime = 0.0f;  // doc time the right-click landed on
static int     s_tlCtxPause = -1;   // marker under the cursor, -1 = empty spot
static int     s_tlCtxKeyGroup = -1; // group of the key under the cursor, or -1
static float   s_tlCtxKeyTime  = 0.0f;
static int     s_tlCtxKeyElem  = -1; // element that key belonged to when opened

bool ZenTimelineCtxOpen(void)  { return s_tlCtxOpen; }
void ZenTimelineCtxClose(void) { s_tlCtxOpen = false; }

// Every visible track of e gets its last key left of t restated at t. Returns
// how many groups were cloned; 0 means there was nothing to the left anywhere.
static int CloneAllTracksAt(AnimElem *e, float t)
{
    int done = 0;
    for (int gi = 0, gn = AnimGroupCountFor(e->kind); gi < gn; gi++)
    {
        if (!ZenGroupHasTrack(e, gi)) continue;
        float src = ZenGroupKeyTimeLeftOf(e, gi, t);
        if (src < 0.0f) continue;                   // track starts after t
        if (ZenGroupCloneKeyTo(e, gi, src, t)) done++;
    }
    return done;
}

void ZenTimelineCtxGui(void)
{
    if (!s_tlCtxOpen) return;

    // the doc can change under an open menu (undo, file switch): a stale marker
    // or element index would act on the wrong thing.
    if (s_tlCtxPause >= zen.doc.pauseCount) { s_tlCtxOpen = false; return; }
    if (s_tlCtxKeyElem >= zen.doc.elemCount) { s_tlCtxOpen = false; return; }

    bool onMarker = (s_tlCtxPause >= 0);
    bool onKey    = (s_tlCtxKeyGroup >= 0 && s_tlCtxKeyElem >= 0);
    bool full     = (zen.doc.pauseCount >= ANIM_PAUSES_MAX);

    AnimElem *ke = onKey ? &zen.doc.elems[s_tlCtxKeyElem] : NULL;
    // The clone lands on the PLAYHEAD, not the click point: the playhead is the
    // position the user parked deliberately, and it is what every other "here"
    // in this editor means.
    float dstT   = zen.playhead;
    bool  keyDst = onKey && fabsf(s_tlCtxKeyTime - dstT) > ZEN_AUTOKEY_EPS;

    // rows, in the order they are drawn.
    enum { ROW_PAUSE, ROW_CLONE_ALL, ROW_KEY_CLONE, ROW_KEY_DELETE, ROW_MAX };
    const char *labels[ROW_MAX];
    const char *tips[ROW_MAX];
    bool        offs[ROW_MAX];
    int         rows[ROW_MAX], rowN = 0;

    labels[ROW_PAUSE] = onMarker ? "Delete pause marker" : "Insert pause marker";
    tips[ROW_PAUSE]   = onMarker
        ? "Remove this hold; playback runs straight through afterwards"
        : (full ? "This animation is at its pause-marker limit"
                : "Hold playback here until the viewer presses a key");
    offs[ROW_PAUSE]   = !onMarker && full;
    rows[rowN++] = ROW_PAUSE;

    bool selElem = zen.selElem >= 0 && zen.selElem < zen.doc.elemCount;
    if (!onKey)
    {
        labels[ROW_CLONE_ALL] = "Clone all tracks to playhead";
        tips[ROW_CLONE_ALL]   = selElem
            ? "Restate every track of the selected element at the playhead, "
              "copying each track's last key before it - the element holds the "
              "pose it was authored into instead of drifting on through the blend"
            : "Select an element first - this clones ITS tracks";
        offs[ROW_CLONE_ALL]   = !selElem;
        rows[rowN++] = ROW_CLONE_ALL;
    }
    else
    {
        labels[ROW_KEY_CLONE] = "Clone key to playhead";
        tips[ROW_KEY_CLONE]   = keyDst
            ? "Copy this key - values and easing - to the playhead, leaving the "
              "original where it is"
            : "The playhead is already on this key; move it first";
        offs[ROW_KEY_CLONE]   = !keyDst;
        rows[rowN++] = ROW_KEY_CLONE;

        labels[ROW_KEY_DELETE] = "Delete key";
        tips[ROW_KEY_DELETE]   = "Remove this key from every track in its group";
        offs[ROW_KEY_DELETE]   = false;
        rows[rowN++] = ROW_KEY_DELETE;
    }

    const float rowH = 24.0f;
    float w = 170.0f;
    for (int i = 0; i < rowN; i++)
    {
        float lw = ZenTextW(labels[rows[i]]) + 32.0f;
        if (lw > w) w = lw;
    }

    Rectangle bg = { s_tlCtxPos.x, s_tlCtxPos.y, w, rowN*rowH + 8.0f };
    ScreenState *ss = ScreenStateGet();
    if (bg.x + bg.width  > ss->width)  bg.x = ss->width  - bg.width;
    if (bg.y + bg.height > ss->height) bg.y = ss->height - bg.height;

    DrawRectangleRec(bg, (Color){ 32, 34, 40, 250 });
    DrawRectangleLinesEx(bg, 1.0f, (Color){ 70, 74, 84, 255 });

    int fired = -1;
    for (int i = 0; i < rowN; i++)
    {
        int id = rows[i];
        Rectangle r = { bg.x + 4, bg.y + 4 + i*rowH, w - 8, rowH };
        if (offs[id]) GuiDisable();
        if (GuiButton(r, labels[id])) fired = id;
        if (offs[id]) GuiEnable();
        ZenTip(r, tips[id]);
    }

    if (fired >= 0)
    {
        AudioPlayButton();
        ZenUndoPush();
        switch (fired)
        {
        case ROW_PAUSE:
            if (onMarker)
            {
                AnimDocRemovePause(&zen.doc, s_tlCtxPause);
                if (zen.selPause == s_tlCtxPause) zen.selPause = -1;
                else if (zen.selPause > s_tlCtxPause) zen.selPause--;
            }
            else if (AnimDocAddPause(&zen.doc, s_tlCtxTime, ZEN_PAUSE_EPS))
                zen.selPause = AnimDocPauseAt(&zen.doc, s_tlCtxTime, ZEN_PAUSE_EPS);
            break;

        case ROW_CLONE_ALL:
            CloneAllTracksAt(&zen.doc.elems[zen.selElem], dstT);
            ZenTrackModalSync();
            break;

        case ROW_KEY_CLONE:
            if (ZenGroupCloneKeyTo(ke, s_tlCtxKeyGroup, s_tlCtxKeyTime, dstT))
            {
                ZenSelKey(s_tlCtxKeyElem, s_tlCtxKeyGroup, dstT, false);
                ZenTrackModalSync();
            }
            break;

        case ROW_KEY_DELETE:
            ZenGroupDeleteKeyAt(ke, s_tlCtxKeyGroup, s_tlCtxKeyTime);
            zen.selKeyCount = 0;
            ZenSelValidate();       // the group may have lost its last key
            ZenTrackModalSync();
            break;
        }
        zen.docDirty = true;
        s_tlCtxOpen  = false;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        !CheckCollisionPointRec(GetMousePosition(), bg))
        s_tlCtxOpen = false;                        // click-away closes
}

// While a fired signal plays, the doc timeline is frozen and this strip takes
// over the lane area: the signal's own keys in u (0..1 of its length) with a
// marker riding the player's clock. Read-only - editing lives in the signal
// modal; this only answers "what is happening right now".
static void DrawSignalOverlay(float x, float y, float w, float h,
                              float trackLeft, float trackW)
{
    const AnimSignal *sg = zen.preview.sig;
    if (!sg) return;

    // dim what's frozen underneath.
    DrawRectangleRec((Rectangle){ x+1, y+1, w-2, h-2 }, (Color){ 12, 13, 16, 170 });

    float len = sg->length > 0.0f ? sg->length : 1.0f;
    float u = ZenClampF(zen.preview.clock / len, 0.0f, 1.0f);

    Rectangle strip = { x+1, y+1, w-2, h-2 };
    DrawRectangleLinesEx(strip, 1.0f, (Color){ 150, 220, 160, 200 });
    DrawText(TextFormat("SIGNAL  %s", sg->name), (int)x+6, (int)y+4, 10,
             (Color){ 150, 220, 160, 255 });
    DrawText(TextFormat("u %.2f  /  %.2fs", u, len),
             (int)(x + w - 108), (int)y+4, 10, (Color){ 150, 220, 160, 200 });

    // one lane per (element, group) the signal targets, keys at their u.
    float laneTop = y + 18, laneBot = y + h - 6;
    int lanes[16][2], ln = 0;                       // {elemIdx, groupIdx}
    for (int ei = 0; ei < zen.doc.elemCount && ln < 16; ei++)
    {
        AnimElem *e = &zen.doc.elems[ei];
        for (int gi = 0, gn = AnimGroupCountFor(e->kind); gi < gn && ln < 16; gi++)
            if (ZenSigGroupHasTarget((AnimSignal *)sg, ei, gi))
            { lanes[ln][0] = ei; lanes[ln][1] = gi; ln++; }
    }

    float rowH = ln > 0 ? (laneBot - laneTop) / (float)ln : 0.0f;
    for (int i = 0; i < ln; i++)
    {
        AnimElem *e = &zen.doc.elems[lanes[i][0]];
        const AnimPropGroup *g = AnimGroupAt(e->kind, lanes[i][1]);
        float ry = laneTop + i*rowH + rowH*0.5f;
        DrawText(TextFormat("%s.%s", e->name, g ? g->name : "?"),
                 (int)x+4, (int)ry-5, 10, (Color){ 120, 170, 130, 255 });
        DrawLine((int)trackLeft, (int)ry, (int)(x + w - 8), (int)ry,
                 (Color){ 60, 90, 68, 255 });

        float us[ZEN_GROUP_TIMES_MAX];
        int nu = ZenSigGroupKeyTimes((AnimSignal *)sg, lanes[i][0], lanes[i][1], us);
        for (int k = 0; k < nu; k++)
        {
            float kx = trackLeft + trackW * ZenClampF(us[k], 0.0f, 1.0f);
            bool passed = us[k] <= u;
            DrawDiamond(kx, ry, 7.0f, (Color){ 15, 16, 20, 255 });
            DrawDiamond(kx, ry, 5.0f, passed ? (Color){ 150, 220, 160, 255 }
                                             : (Color){ 90, 130, 100, 255 });
        }
    }
    if (ln == 0)
        DrawText("(this signal has no keyed targets)", (int)(x + w*0.5f - 90),
                 (int)(y + h*0.5f - 5), 10, (Color){ 110, 140, 118, 255 });

    // the signal's own playhead, riding its clock.
    float sx = trackLeft + trackW * u;
    DrawLine((int)sx, (int)(y+16), (int)sx, (int)(y+h-2), (Color){ 150, 240, 165, 255 });
    DrawRectangleRec((Rectangle){ sx-6, y+14, 12, 8 }, (Color){ 150, 240, 165, 255 });
}

static void DrawTimeline(float x, float y, float w, float h)
{
    bool thin = h < 60.0f;
    Rectangle bar = { x, y, w, h };
    DrawRectangleRec(bar, Fade((Color){ 24, 26, 30, 255 },
                               (zen.panelAlphaMode == 0 ? 255 : zen.panelAlphaMode == 1 ? 140 : 75) / 255.0f));
    DrawRectangleLinesEx(bar, 1.0f, (Color){ 70, 74, 84, 255 });

    float dur = zen.doc.duration > 0 ? zen.doc.duration : 1.0f;
    float gutter = 56.0f, padR = 8.0f;
    float trackLeft = x + gutter, trackW = w - gutter - padR;

    #define T2X(t) (trackLeft + (trackW) * ((t)/dur))
    #define X2T(px) (((px) - trackLeft)/trackW * dur)

    for (float s = 0; s <= dur + 0.001f; s += 0.5f)
    {
        float tx = T2X(s);
        DrawLine((int)tx, (int)y+2, (int)tx, (int)(y+h-16), (Color){ 50,54,62,255 });
        if (fmodf(s,1.0f) < 0.01f)
            DrawText(TextFormat("%.0f", s), (int)tx+2, (int)(y+h-14), 10, (Color){110,116,128,255});
    }

    Vector2 mouse = GetMousePosition();
    bool ctrl  = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT)   || IsKeyDown(KEY_RIGHT_SHIFT);
    bool press = !zen.guiLocked && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    bool keyHit = false;

    // intro / outro trim dead zones.
    float inEnd = AnimDocIntroEnd(&zen.doc), outStart = AnimDocOutroStart(&zen.doc);
    float inX = T2X(inEnd), outX = T2X(outStart);
    if (inEnd > 0.0f)
        DrawRectangleRec((Rectangle){ trackLeft, y+1, inX-trackLeft, h-2 },
                         (Color){ 90, 140, 200, 26 });
    if (outStart < dur)
        DrawRectangleRec((Rectangle){ outX, y+1, x+w-padR-outX, h-2 },
                         (Color){ 0, 0, 0, 120 });

    // lane layout, kept for the scrub handler below (which lane is the cursor
    // in, so scrubbing can follow the swimlane it passes over).
    int   laneVis[16], laneCount = 0;
    float laneTop = y + 4, laneRowH = 0.0f;

    // key under the cursor, for the right-click menu below (which needs to know
    // whether it opened ON a key or on empty bar).
    int   keyHotGroup = -1;
    float keyHotTime  = 0.0f;

    if (!thin && zen.selElem >= 0 && zen.selElem < zen.doc.elemCount)
    {
        AnimElem *e = &zen.doc.elems[zen.selElem];
        int vis[16], vn = 0, grpN = AnimGroupCountFor(e->kind);
        for (int gi = 0; gi < grpN && vn < 16; gi++)
            if (ZenGroupHasTrack(e, gi)) vis[vn++] = gi;
        if (vn == 0)
            DrawText("(no tracks - add one in the inspector to key frames)",
                     (int)(x + w*0.5f - 130), (int)(y + h*0.5f - 5), 10,
                     (Color){ 110, 116, 128, 255 });
        float rowH = (h - 24) / (float)(vn > 0 ? vn : 1);
        laneCount = vn; laneRowH = rowH;
        for (int i = 0; i < vn; i++) laneVis[i] = vis[i];
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
            // lane label is in the gutter and clips; tip shows the full name.
            DrawText(g ? g->name : "?", (int)x+2, (int)ry-5, 10, (Color){130,136,148,255});
            ZenTip((Rectangle){ x, ry-8, gutter, 16 },
                   g ? TextFormat("%s track of %s", g->name, e->name) : "?");

            int colorProp = ZenGroupColorProp(e->kind, gi);
            float times[ZEN_GROUP_TIMES_MAX];
            int   nt = ZenGroupKeyTimes(e, gi, times);
            for (int i = 0; i < nt; i++)
            {
                float t = times[i];
                float kx = T2X(t);
                Rectangle hit = { kx-10, ry-10, 20, 20 };
                bool hot = CheckCollisionPointRec(mouse, hit);
                if (hot && !zen.guiLocked) { keyHotGroup = gi; keyHotTime = t; }
                bool sel = ZenKeyIsSelected(zen.selElem, gi, t);
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
                    if (shift)
                    {
                        // Shift+click: toggle into the multi-selection, no drag.
                        ZenSelKey(zen.selElem, gi, t, true);
                        ZenTrackModalOpen(zen.selElem, gi);
                    }
                    else
                    {
                        ZenUndoPush();                 // once per drag gesture
                        zen.dragKeyGroup = gi; zen.dragKeyTime = t;
                        ZenSelKey(zen.selElem, gi, t, false);
                        ZenTrackModalOpen(zen.selElem, gi);
                        zen.scrollToSelKey = true;
                    }
                    keyHit = true;
                }
            }
        }
    }
    else if (!thin)
        DrawText("(select an element to see its keys)",
                 (int)(x + w*0.5f - 90), (int)(y + h*0.5f - 5), 10,
                 (Color){ 110, 116, 128, 255 });

    // playhead - muted with a pause glyph while something holds the doc clock:
    // either a signal playing over it, or a pause marker waiting for a key.
    bool sigLive = !AnimSignalPlayerDone(&zen.preview);
    bool clockHeld = sigLive || zen.pausedOnMarker;
    float phx = T2X(zen.playhead);
    Color phCol = clockHeld ? (Color){ 150, 90, 90, 255 } : (Color){ 255, 90, 90, 255 };
    DrawLine((int)phx, (int)y, (int)phx, (int)(y+h), phCol);
    DrawRectangleRec((Rectangle){ phx-6, y-2, 12, 10 }, phCol);
    if (clockHeld)
    {
        DrawRectangleRec((Rectangle){ phx-3, y, 2, 6 }, (Color){ 30, 30, 34, 255 });
        DrawRectangleRec((Rectangle){ phx+1, y, 2, 6 }, (Color){ 30, 30, 34, 255 });
    }

    if (sigLive) DrawSignalOverlay(x, y, w, h, trackLeft, trackW);

    // trim markers (hit-tested BEFORE the bar scrub).
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
        ZenUndoPush();
        if (introHot) zen.dragIntro = true; else zen.dragOutro = true;
        keyHit = true;
    }
    if ((zen.dragIntro || zen.dragOutro) && IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        float nt = ZenClampF(X2T(mouse.x), 0.0f, dur);
        if (zen.dragIntro) zen.doc.introEnd   = (nt > outStart) ? outStart : nt;
        else               zen.doc.outroStart = (nt < inEnd)    ? inEnd    : nt;
    }

    // PAUSE MARKERS: a green dashed vertical, shaped like the playhead on
    // purpose - it IS a second cursor, the one playback parks on. The grab tab
    // at the top is the drag target, kept clear of the key diamonds' hit rects.
    int pauseHot = -1;
    for (int i = 0; i < zen.doc.pauseCount; i++)
    {
        float px = T2X(zen.doc.pauses[i].t);
        Rectangle hit = { px-7, y, 14, 12 };
        bool hot = !zen.guiLocked && CheckCollisionPointRec(mouse, hit);
        if (hot) pauseHot = i;

        bool held = (zen.pausedOnMarker && zen.heldPause == i);
        bool lit  = hot || held || (zen.selPause == i);
        Color pc = lit ? (Color){ 150, 240, 165, 255 } : (Color){ 110, 210, 130, 255 };

        DrawDottedV(px, y+2, y+h-2, pc);
        if (lit) DrawDottedV(px+1, y+2, y+h-2, pc);   // thicken, don't recolour
        DrawRectangleRec((Rectangle){ px-6, y, 12, 8 }, pc);
        // the pause glyph itself: two bars, same idiom as the frozen playhead
        DrawRectangleRec((Rectangle){ px-3, y+2, 2, 4 }, (Color){ 24, 26, 30, 255 });
        DrawRectangleRec((Rectangle){ px+1, y+2, 2, 4 }, (Color){ 24, 26, 30, 255 });

        ZenTip(hit, zen.doc.pauses[i].once
            ? "Pause marker (first pass only) - drag to retime, right-click to delete"
            : "Pause marker - playback holds here until a key is pressed. Drag to "
              "retime, right-click to delete.");
    }

    if (press && pauseHot >= 0)
    {
        ZenUndoPush();                       // once per drag, not per frame
        zen.dragPause = pauseHot;
        zen.selPause  = pauseHot;
        keyHit = true;
    }
    if (zen.dragPause >= 0 && IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        if (zen.dragPause < zen.doc.pauseCount)
        {
            float nt = ZenClampF(X2T(mouse.x), 0.0f, dur);
            if (ctrl) nt = ZenClampF(roundf(nt / ZEN_TIMELINE_SNAP) * ZEN_TIMELINE_SNAP,
                                     0.0f, dur);
            // retiming re-sorts, so the marker's index can move under the drag
            zen.dragPause = AnimDocSetPauseTime(&zen.doc, zen.dragPause, nt);
            zen.selPause  = zen.dragPause;
            zen.docDirty  = true;
        }
        else zen.dragPause = -1;
    }

    // Right-click anywhere on the bar opens the context menu; what it offers
    // depends on what sits under the cursor - a pause marker, a key, or bar.
    if (!zen.guiLocked && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) &&
        CheckCollisionPointRec(mouse, (Rectangle){ x, y-4, w, h+4 }))
    {
        s_tlCtxPos   = mouse;
        s_tlCtxTime  = ZenClampF(X2T(mouse.x), 0.0f, dur);
        s_tlCtxPause = (pauseHot >= 0) ? pauseHot
                                       : AnimDocPauseAt(&zen.doc, s_tlCtxTime, ZEN_PAUSE_EPS);
        // A key's menu acts on THAT key, so it also remembers which element it
        // belongs to: the selection can move while the menu is open.
        s_tlCtxKeyGroup = keyHotGroup;
        s_tlCtxKeyTime  = keyHotTime;
        s_tlCtxKeyElem  = (keyHotGroup >= 0) ? zen.selElem : -1;
        s_tlCtxOpen  = true;
    }

    // capacity, where the markers live (they have no panel of their own)
    if (!thin)
        DrawText(TextFormat("pauses %d/%d", zen.doc.pauseCount, ANIM_PAUSES_MAX),
                 (int)(x + 4), (int)(y + h - 14), 10,
                 zen.doc.pauseCount ? (Color){ 110, 210, 130, 255 }
                                    : (Color){ 90, 94, 104, 255 });

    // bar scrub. Plain scrub re-points the track modal at whatever lane the
    // cursor is in; Shift+scrub sweeps keys into the selection as it crosses
    // them (the quick way to grab a run of keys without clicking each).
    if (press && !keyHit &&
        CheckCollisionPointRec(mouse, (Rectangle){ x, y-4, w, h+4 }))
    {
        zen.dragPlayhead = true;
        if (!shift) zen.selKeyCount = 0;            // plain scrub drops the set
    }

    if (zen.dragKeyGroup >= 0 && IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        AnimElem *de = &zen.doc.elems[zen.selElem];
        float nt = ZenClampF(X2T(mouse.x), 0.0f, dur);
        // Ctrl snaps the key to the tick grid while dragging.
        if (ctrl) nt = ZenClampF(roundf(nt / ZEN_TIMELINE_SNAP) * ZEN_TIMELINE_SNAP, 0.0f, dur);
        ZenGroupMoveKeyTo(de, zen.dragKeyGroup, zen.dragKeyTime, nt);
        zen.dragKeyTime = nt;
        if (zen.selKeyCount == 1) zen.selKeys[0] = nt;
        ZenTrackModalSync();
    }
    else if (zen.dragPlayhead && IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        float prevHead = zen.playhead;
        zen.playhead = ZenClampF(X2T(mouse.x), 0.0f, dur);
        zen.playing = false; zen.playPending = false;
        zen.preview.playing = false;

        // which swimlane is the cursor over?
        int lane = -1;
        if (laneCount > 0 && laneRowH > 0.0f)
        {
            int r0 = (int)((mouse.y - laneTop) / laneRowH);
            if (r0 >= 0 && r0 < laneCount) lane = laneVis[r0];
        }
        if (lane >= 0)
        {
            AnimElem *se = &zen.doc.elems[zen.selElem];
            float lt[ZEN_GROUP_TIMES_MAX];
            int ln = ZenGroupKeyTimes(se, lane, lt);

            // keys the playhead is sitting on, within the snap margin.
            float hits[ZEN_GROUP_TIMES_MAX];
            int hn = 0;
            for (int i = 0; i < ln; i++)
                if (fabsf(lt[i] - zen.playhead) <= ZEN_SCRUB_KEY_EPS)
                    hits[hn++] = lt[i];

            // Sweeping fast moves the playhead several keys in one frame, so a
            // shift-sweep also takes everything in the span it just travelled.
            float swLo = prevHead < zen.playhead ? prevHead : zen.playhead;
            float swHi = prevHead < zen.playhead ? zen.playhead : prevHead;
            float swept[ZEN_GROUP_TIMES_MAX];
            int sn = 0;
            for (int i = 0; i < ln; i++)
                if (lt[i] >= swLo - ZEN_SCRUB_KEY_EPS &&
                    lt[i] <= swHi + ZEN_SCRUB_KEY_EPS)
                    swept[sn++] = lt[i];

            // Open FIRST: ZenTrackModalOpen clears the key set when the group
            // changes, so the selection has to be written after it.
            bool laneChanged = (lane != zen.selGroup);
            ZenTrackModalOpen(zen.selElem, lane);

            if (shift)
            {
                // sweep-select: add every key crossed, never remove.
                if (laneChanged) zen.selKeyCount = 0;
                for (int i = 0; i < sn; i++)
                    if (!ZenKeyIsSelected(zen.selElem, lane, swept[i]) &&
                        zen.selKeyCount < ZEN_GROUP_TIMES_MAX)
                        zen.selKeys[zen.selKeyCount++] = swept[i];
            }
            else
            {
                // follow the lane: show its overlapped key(s), else the track.
                zen.selKeyCount = 0;
                for (int i = 0; i < hn && zen.selKeyCount < ZEN_GROUP_TIMES_MAX; i++)
                    zen.selKeys[zen.selKeyCount++] = hits[i];
            }
            ZenTrackModalSync();
        }
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
    { zen.dragPlayhead = false; zen.dragKeyGroup = -1;
      zen.dragIntro = false; zen.dragOutro = false; zen.dragPause = -1; }

    #undef T2X
    #undef X2T
}

// ---------------------------------------------------------------------------
//  Playback slide bookkeeping (called from Update).
// ---------------------------------------------------------------------------
void ZenPanelsUpdate(float dt)
{
    bool playbackUi = zen.playing || zen.playPending
                   || !AnimSignalPlayerDone(&zen.preview);
    if (playbackUi && !zen.prevPlaybackUi)
    {
        // hidden widgets must not keep capturing input.
        zen.edName = zen.edText = false; zen.edSigIdx = -1;
        ZenTextAreaClose();          // never leave it keyed to the old element
        zen.addTrackOpen = false; zen.easeDropOpen = false;
        ZenSigCloseDrops();
    }
    zen.prevPlaybackUi = playbackUi;
    float step = dt / 0.25f;
    if (playbackUi) { zen.panelAnim += step; if (zen.panelAnim > 1.0f) zen.panelAnim = 1.0f; }
    else            { zen.panelAnim -= step; if (zen.panelAnim < 0.0f) zen.panelAnim = 0.0f; }
}

// ---------------------------------------------------------------------------
//  Layout (below the menu bar), classic proportions.
// ---------------------------------------------------------------------------
void ZenPanelsGui(void)
{
    ScreenState *ss = ScreenStateGet();
    float W = (float)ss->width, H = (float)ss->height;

    float pad = 10.0f;
    float topY = ZEN_MENU_BAR_H + pad;
    float leftW = 220.0f, rightW = 380.0f;
    float bottomH = 180.0f, thinH = 26.0f;

    float k = AnimEaseApply(ANIM_EASE_SINE_INOUT, zen.panelAnim);
    bool uiHidden = k >= 0.999f;

    Rectangle leftPanel = { pad - (leftW + 2*pad)*k, topY,
                            leftW, H - topY - bottomH - 2*pad };
    Rectangle rightPanel = { W - rightW - pad + (rightW + 2*pad)*k, topY,
                             rightW, H - topY - bottomH - 2*pad };

    if (!uiHidden)
    {
        // left column: ELEMENTS above SIGNALS; either can be hidden from View.
        if (zen.showElems || zen.showSignals)
        {
            float leftGap = 6.0f;
            float elemH = !zen.showSignals ? leftPanel.height
                        : !zen.showElems   ? 0.0f
                        : (leftPanel.height - leftGap) * 0.60f;
            if (zen.showElems)
            {
                Rectangle elemPanel = { leftPanel.x, leftPanel.y, leftPanel.width, elemH };
                if (CheckCollisionPointRec(GetMousePosition(), elemPanel)) zen.uiHover = true;
                ZenDrawPanelBG(elemPanel, zen.panelAlphaMode);
                zen.elemView.contentH = PanelScroll(&zen.elemView, elemPanel, DrawElementList);
            }
            if (zen.showSignals)
            {
                float sy = leftPanel.y + (zen.showElems ? elemH + leftGap : 0);
                Rectangle sigPanel = { leftPanel.x, sy, leftPanel.width,
                                       leftPanel.y + leftPanel.height - sy };
                if (CheckCollisionPointRec(GetMousePosition(), sigPanel)) zen.uiHover = true;
                ZenDrawPanelBG(sigPanel, zen.panelAlphaMode);
                zen.sigView.contentH = PanelScroll(&zen.sigView, sigPanel, DrawSignalList);
            }
        }

        if (zen.showInspector)
        {
            if (CheckCollisionPointRec(GetMousePosition(), rightPanel)) zen.uiHover = true;
            ZenDrawPanelBG(rightPanel, zen.panelAlphaMode);
            zen.inspPanelRect = rightPanel;
            zen.inspView.contentH = PanelScroll(&zen.inspView, rightPanel, DrawInspector);
        }
    }

    if (zen.showTimeline)
    {
        // a live signal keeps the timeline open: the signal overlay is the
        // whole point, and a 26px strip can't show it.
        float tk = AnimSignalPlayerDone(&zen.preview) ? k : 0.0f;
        float by = H - bottomH - pad + tk * (bottomH - thinH);
        float tlH = H - pad - by;
        if (tlH < thinH) { tlH = thinH; by = H - pad - thinH; }
        if (CheckCollisionPointRec(GetMousePosition(),
            (Rectangle){ pad, by, W - 2*pad, tlH })) zen.uiHover = true;
        DrawTimeline(pad, by, W - 2*pad, tlH);
    }

}
