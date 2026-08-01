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
    ZenLabelTip((Rectangle){ x+4, y, w-4, 20 }, "ELEMENTS",
                "Click selects - Ctrl+click multi-selects. All operations live in the Element menu.");
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
    ZenLabelTip((Rectangle){ x+4, y, w-4, 20 }, "SIGNALS",
                "Click opens the signal in its modal; the play glyph fires it live.");
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

// Keyframe-or-base write, same semantics as ZenPropSlider commits.
static void WritePropValue(AnimElem *e, int prop, float v, float *baseField)
{
    AnimTrack *tr = AnimElemFindTrack(e, prop);
    if (!tr) { if (baseField) *baseField = v; return; }
    if (!zen.autoKey) return;
    if (zen.playhead > ZEN_AUTOKEY_EPS) ZenEnsureZeroKey(e, tr);
    AnimTrackWriteKeyAt(tr, zen.playhead, v, ZEN_AUTOKEY_EPS);
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
        GuiLabel((Rectangle){ x, y, 44, rh }, "start");
        if (ZenEditSlider((Rectangle){ x+44, y, half, rh }, "x", &ax, loX, hiX)) ch = true;
        if (ZenEditSlider((Rectangle){ x2, y, half, rh }, "y", &ay, loY, hiY)) ch = true;
        y += rh + gap;
        GuiLabel((Rectangle){ x, y, 44, rh }, "end");
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
        GuiLabel((Rectangle){ x, y, 44, rh }, "P0");
        if (ZenEditSlider((Rectangle){ x+44, y, half, rh }, "x", &x0, loX, hiX)) ch = true;
        if (ZenEditSlider((Rectangle){ x2, y, half, rh }, "y", &y0, loY, hiY)) ch = true;
        y += rh + gap;
        GuiLabel((Rectangle){ x, y, 44, rh }, "P1");
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
        GuiLabel((Rectangle){ x, y, 40, rh }, "text");
        if (GuiTextBox((Rectangle){ x+44, y, w-44, rh }, e->text, ANIM_TEXT_LEN_MAX, zen.edText))
        { if (!zen.edText) ZenUndoPush(); zen.edText = !zen.edText; }
        y += rh + gap;
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
        GuiLabel((Rectangle){ x, y, 44, rh }, "units");
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
            GuiLabel((Rectangle){ x, y, 44, rh }, "anchor");
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
                GuiLabel((Rectangle){ x, y, 44, rh }, "scale"); y += rh + gap;
            }
        }
        else
        {
            ZenPropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, isText ? AP_T_POS_X : AP_S_POS_X, &e->posFrac.x);
            GuiLabel((Rectangle){ x, y, 44, rh }, "posX"); y += rh + gap;
            ZenPropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, isText ? AP_T_POS_Y : AP_S_POS_Y, &e->posFrac.y);
            GuiLabel((Rectangle){ x, y, 44, rh }, "posY"); y += rh + gap;
            ZenPropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, isText ? AP_T_SIZE : AP_S_W, &e->sizeFrac.x);
            GuiLabel((Rectangle){ x, y, 44, rh }, isText ? "size" : (e->shapeKind==SHAPE_LINE?"length":"w")); y += rh + gap;
            if (e->kind == AE_SHAPE)
            {
                ZenPropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, AP_S_H, &e->sizeFrac.y);
                GuiLabel((Rectangle){ x, y, 44, rh }, e->shapeKind==SHAPE_LINE?"thick":"h"); y += rh + gap;
                ZenPropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, AP_S_SCALE, &e->scaleFrac);
                GuiLabel((Rectangle){ x, y, 44, rh }, "scale"); y += rh + gap;
            }
        }

        if (e->kind == AE_SHAPE && e->cornerMode && e->shapeKind == SHAPE_LINE)
        {
            ZenPropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, AP_S_H, &e->sizeFrac.y);
            GuiLabel((Rectangle){ x, y, 44, rh }, "thick"); y += rh + gap;
        }

        if (!(e->kind == AE_SHAPE && e->cornerMode && e->shapeKind == SHAPE_LINE))
        {
            ZenPropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, isText ? AP_T_ROT : AP_S_ROT, &e->rotBase);
            GuiLabel((Rectangle){ x, y, 44, rh }, "rot"); y += rh + gap;
        }
    }

    y = ZenColorRGBRows(x, y, w, e, ZenColorPropFor(e->kind), &e->color, "color");
    float ca = e->color.a;
    if (ZenEditSlider((Rectangle){ x+16, y, w-16-50, 16 }, "A", &ca, 0,255))
        e->color.a = (unsigned char)ca;
    y += 22;

    if (e->kind == AE_GLOBAL)
    {
        GuiLine((Rectangle){ x, y, w, 8 }, "background"); y += 12;
        y = ZenColorRGBRows(x, y, w, e, AP_G_BG_COLOR, &e->bgColor, "background");
        float ba = e->bgColor.a;
        if (ZenEditSlider((Rectangle){ x+16, y, w-16-50, 16 }, "A", &ba, 0,255))
            e->bgColor.a = (unsigned char)ba;
        y += 22;
    }

    if (e->kind == AE_SHAPE)
    {
        GuiLine((Rectangle){ x, y, w, 8 }, "outline"); y += 12;
        ZenPropSlider((Rectangle){ x+44, y, w-44-50, rh }, e, AP_S_OUTLINE, &e->outlineFrac);
        GuiLabel((Rectangle){ x, y, 44, rh }, "thick"); y += rh + gap;
        y = ZenColorRGBRows(x, y, w, e, AP_S_OUTLINE_COLOR, &e->outlineColor, "outline");
        float oa = e->outlineColor.a;
        if (ZenEditSlider((Rectangle){ x+16, y, w-16-50, 16 }, "A", &oa, 0,255))
            e->outlineColor.a = (unsigned char)oa;
        y += 22;

        if (e->shapeKind == SHAPE_CIRCLE)
        {
            GuiLabel((Rectangle){ x, y, 44, rh }, "style");
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
        ZenLabelTip((Rectangle){ x + 22, y + 4, hdrR.width - 26, rh - 8 },
                    TextFormat("%s (%d)", g->name, nt),
                    "Click: select track (bulk edit in the modal). Arrow expands the key list.");
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
    Rectangle addR = { x, y, w-56, rh };
    if (GuiButton((Rectangle){ x+w-52, y, 52, rh }, "+track"))
    {
        AudioPlayButton(); ZenUndoPush();
        ZenGroupWriteKey(e, zen.addTrackSel, zen.playhead);
        ZenSelKey(zen.selElem, zen.addTrackSel, zen.playhead, false);
        ZenTrackModalOpen(zen.selElem, zen.addTrackSel);
    }
    const AnimPropGroup *addG = AnimGroupAt(e->kind, zen.addTrackSel);
    if (GuiButton(addR, TextFormat("%s  v", addG ? addG->name : "?")))
    { AudioPlayButton(); zen.addTrackOpen = !zen.addTrackOpen; }
    zen.addTrackRect = addR;
    y += rh + 10;

    return (y + 12) - y0;
}

// The add-track group list, drawn topmost (flips above when out of room).
static void DrawAddTrackOverlay(void)
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
        if (GuiButton(rr, g ? g->name : "?"))
        { AudioPlayButton(); zen.addTrackSel = i; zen.addTrackOpen = false; }
        if (i == zen.addTrackSel) DrawRectangleRec(rr, (Color){ 90, 140, 220, 60 });
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

    // playhead.
    float phx = T2X(zen.playhead);
    DrawLine((int)phx, (int)y, (int)phx, (int)(y+h), (Color){255,90,90,255});
    DrawRectangleRec((Rectangle){ phx-6, y-2, 12, 10 }, (Color){255,90,90,255});

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

    // bar scrub.
    if (press && !keyHit &&
        CheckCollisionPointRec(mouse, (Rectangle){ x, y-4, w, h+4 }))
    {
        zen.dragPlayhead = true;
        zen.selKeyCount = 0;                        // scrub drops key selection
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
        zen.playhead = ZenClampF(X2T(mouse.x), 0.0f, dur);
        zen.playing = false; zen.playPending = false;
        zen.preview.playing = false;
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
    { zen.dragPlayhead = false; zen.dragKeyGroup = -1;
      zen.dragIntro = false; zen.dragOutro = false; }

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
        float by = H - bottomH - pad + k * (bottomH - thinH);
        float tlH = H - pad - by;
        if (tlH < thinH) { tlH = thinH; by = H - pad - thinH; }
        if (CheckCollisionPointRec(GetMousePosition(),
            (Rectangle){ pad, by, W - 2*pad, tlH })) zen.uiHover = true;
        DrawTimeline(pad, by, W - 2*pad, tlH);
    }

    DrawAddTrackOverlay();
}
