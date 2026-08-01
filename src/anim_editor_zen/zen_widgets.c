// ============================================================================
//  zen_widgets.c  -  shared gui building blocks of the Zen editor
//
//  The precise-input slider (double-click to type, Ctrl+wheel to step,
//  Shift for sticky fine drags), the property/colour row helpers that write
//  keys under auto-key, panel backgrounds, and the hand-rolled hover
//  tooltip: raygui tooltips only fire on FOCUSED widgets (buttons), so
//  read-only labels record their tip here and ZenTipDraw() paints the one
//  winner topmost at the end of Gui().
// ============================================================================

#include "raylib.h"
#include "raygui.h"
#include "zen_internal.h"
#include "../screen_state/screen_state.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// widget-local state: only this file's slider machinery needs it, so it
// stays file-scope rather than in ZenCtx.
static bool      sliderGestureOpen = false;
static Rectangle finePressRect = {0};
static bool      finePressActive = false;
static bool      fineSticky = false;
static bool      edSliderActive = false;
static Rectangle edSliderRect = {0};
static char      edSliderBuf[16];
static double    lastSliderClick = 0.0;
static Rectangle lastSliderClickRect = {0};

float ZenTextW(const char *text)
{
    float fs = (float)GuiGetStyle(DEFAULT, TEXT_SIZE);
    float sp = fs / (float)GuiGetFont().baseSize;
    return MeasureTextEx(GuiGetFont(), text, fs, sp).x;
}

float ZenClampF(float v, float lo, float hi)
{
    if (v < lo) return lo; if (v > hi) return hi; return v;
}

static bool SameRect(Rectangle a, Rectangle b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

// step size for Ctrl+wheel / Ctrl-drag snapping, keyed on the value range.
static float SliderStep(float lo, float hi) { return (hi - lo <= 2.0f) ? 0.01f : 0.1f; }

// A slider's precise-entry textbox is capturing the keyboard.
bool ZenSliderTyping(void)
{
    return edSliderActive;
}

// One undo snapshot per drag gesture; ends when the mouse comes up (called
// once per frame from Gui()).
void ZenWidgetsFrameEnd(void)
{
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
    {
        sliderGestureOpen = false;
        finePressActive = false; fineSticky = false; finePressRect = (Rectangle){0};
    }
}

bool ZenEditSlider(Rectangle r, const char *label, float *v, float lo, float hi)
{
    // a disabled slider (auto-key off) gets no precise input either.
    if (GuiGetState() == STATE_DISABLED)
    {
        float tmp = *v;
        GuiSlider(r, label, TextFormat("%.2f", tmp), &tmp, lo, hi);
        return false;
    }

    float pad = (float)GuiGetStyle(SLIDER, TEXT_PADDING);
    Rectangle lbl = { r.x + r.width + pad, r.y, 44, r.height };
    Vector2 mouse = GetMousePosition();

    // --- textbox mode: this slider owns the open value editor ---------------
    if (edSliderActive && SameRect(edSliderRect, r))
    {
        GuiSlider(r, label, "", &(float){ *v }, lo, hi);
        bool commit = false, cancel = false;
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
                float nv = ZenClampF((float)atof(edSliderBuf), lo, hi);
                if (nv != *v) { ZenUndoPush(); *v = nv; return true; }
            }
        }
        return false;
    }

    // --- open the textbox on a double-click of the value label --------------
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

    // --- Ctrl+wheel steps the value in fixed increments ---------------------
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    float wheel = GetMouseWheelMove();
    if (ctrl && wheel != 0.0f && CheckCollisionPointRec(mouse, r))
    {
        float step = SliderStep(lo, hi);
        float nv = ZenClampF(*v + wheel * step, lo, hi);
        if (nv != *v) { ZenUndoPush(); *v = nv; return true; }
        return false;
    }

    // --- Shift = fine relative drag (sticky until release) ------------------
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, r))
    {
        finePressRect = r; finePressActive = true; fineSticky = false;
    }
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (finePressActive && SameRect(finePressRect, r) &&
        IsMouseButtonDown(MOUSE_BUTTON_LEFT) && (shift || fineSticky))
    {
        if (!fineSticky) { ZenUndoPush(); fineSticky = true; }
        GuiSlider(r, label, TextFormat("%.2f", *v), &(float){ *v }, lo, hi);
        float fstep = (hi - lo) * 0.0015f;
        float nv = ZenClampF(*v + GetMouseDelta().x * fstep, lo, hi);
        if (nv != *v) { *v = nv; return true; }
        return false;
    }

    // --- normal drag ---------------------------------------------------------
    float tmp = *v;
    GuiSlider(r, label, TextFormat("%.2f", tmp), &tmp, lo, hi);
    if (ctrl && tmp != *v)                        // Ctrl-drag snaps to increments
    {
        float step = SliderStep(lo, hi);
        tmp = ZenClampF(roundf(tmp / step) * step, lo, hi);
    }
    if (tmp == *v) return false;
    if (!sliderGestureOpen) { ZenUndoPush(); sliderGestureOpen = true; }
    *v = tmp;
    return true;
}

// ---------------------------------------------------------------------------
//  Keyframe seeding + property helpers (verbatim semantics from the classic
//  editor: every track keeps a t=0 START key).
// ---------------------------------------------------------------------------
void ZenEnsureZeroKey(AnimElem *e, AnimTrack *tr)
{
    if (tr->keyCount == 0)
        AnimTrackAddKey(tr, 0.0f, AnimElemProp(e, tr->prop, 0.0f), ANIM_EASE_LINEAR);
}

void ZenEnsureZeroColorKey(AnimElem *e, AnimTrack *tr)
{
    if (tr->keyCount == 0)
        AnimTrackAddColorKey(tr, 0.0f, AnimElemColorProp(e, tr->prop, 0.0f),
                             ANIM_EASE_LINEAR);
}

int ZenColorPropFor(int kind)
{
    switch (kind)
    {
        case AE_TEXT:  return AP_T_COLOR;
        case AE_SHAPE: return AP_S_COLOR;
        default:       return AP_G_COLOR;
    }
}

// Canvas reference dimension for a SIZE property's axis; 0 for non-size props.
static float SizePropRef(int prop, Vector2 game)
{
    switch (prop)
    {
        case AP_S_W:                                    return game.x;
        case AP_T_SIZE: case AP_S_H: case AP_S_OUTLINE: return game.y;
        default:                                        return 0.0f;
    }
}

void ZenPropRange(const AnimElem *e, int prop, float *lo, float *hi)
{
    *lo = AnimPropMin(prop);
    *hi = AnimPropMax(prop);
    if (e->sizeAbsolute)
    {
        float ref = SizePropRef(prop, ScreenStateTargetSize());
        if (ref > 0.0f) { *lo *= ref; *hi *= ref; }
    }
}

void ZenConvertSizeUnits(AnimElem *e, bool toAbsolute)
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

// Slider for an ANIMATABLE property: no track -> edits the base field; with a
// track -> value at the playhead, auto-key ON writes a key there, OFF locks it.
void ZenPropSlider(Rectangle r, AnimElem *e, int prop, float *baseField)
{
    float lo, hi; ZenPropRange(e, prop, &lo, &hi);
    AnimTrack *tr = AnimElemFindTrack(e, prop);
    if (!tr) { ZenEditSlider(r, "", baseField, lo, hi); return; }

    float v = AnimElemProp(e, prop, zen.playhead);
    if (!zen.autoKey) GuiSetState(STATE_DISABLED);
    if (ZenEditSlider(r, "", &v, lo, hi))
    {
        if (zen.playhead > ZEN_AUTOKEY_EPS) ZenEnsureZeroKey(e, tr);
        AnimTrackWriteKeyAt(tr, zen.playhead, v, ZEN_AUTOKEY_EPS);
    }
    if (!zen.autoKey) GuiSetState(STATE_NORMAL);
}

void ZenDrawSwatch(Rectangle r, Color c)
{
    DrawRectangleRec(r, c);
    DrawRectangleLinesEx(r, 1.0f, (Color){ 70, 74, 84, 255 });
}

// RGB slider rows for ONE colour property; tracked -> auto-key at the playhead.
float ZenColorRGBRows(float x, float y, float w, AnimElem *e, int prop,
                      Color *base, const char *label)
{
    float rh = 24.0f;
    AnimTrack *ctr = AnimElemFindTrack(e, prop);
    Color cc = AnimElemColorProp(e, prop, zen.playhead);
    ZenLabelTip((Rectangle){ x, y, w-24, rh },
                TextFormat(ctr ? "%s (rgb, keyed)" : "%s (rgb)", label), NULL);
    ZenDrawSwatch((Rectangle){ x+w-20, y+3, 18, 18 }, cc);
    y += rh;
    float cr=cc.r, cg=cc.g, cb=cc.b;
    bool changed = false;
    if (ctr && !zen.autoKey) GuiSetState(STATE_DISABLED);
    if (ZenEditSlider((Rectangle){ x+16, y, w-16-50, 16 }, "R", &cr, 0,255)) changed=true; y+=18;
    if (ZenEditSlider((Rectangle){ x+16, y, w-16-50, 16 }, "G", &cg, 0,255)) changed=true; y+=18;
    if (ZenEditSlider((Rectangle){ x+16, y, w-16-50, 16 }, "B", &cb, 0,255)) changed=true; y+=18;
    if (ctr && !zen.autoKey) GuiSetState(STATE_NORMAL);
    if (changed)
    {
        Color nc = { (unsigned char)cr,(unsigned char)cg,(unsigned char)cb, 255 };
        if (!ctr) { base->r = nc.r; base->g = nc.g; base->b = nc.b; }
        else
        {
            if (zen.playhead > ZEN_AUTOKEY_EPS) ZenEnsureZeroColorKey(e, ctr);
            AnimTrackWriteColorKeyAt(ctr, zen.playhead, nc, ZEN_AUTOKEY_EPS);
        }
    }
    return y;
}

// ---------------------------------------------------------------------------
//  Panel chrome
// ---------------------------------------------------------------------------
static unsigned char PanelAlpha(int mode)
{
    return mode == 0 ? 255 : mode == 1 ? 140 : 75;
}

void ZenDrawPanelBG(Rectangle r, int mode)
{
    Color bg = GetColor(GuiGetStyle(DEFAULT, BACKGROUND_COLOR));
    Color ln = GetColor(GuiGetStyle(DEFAULT, LINE_COLOR));
    DrawRectangleRec(r, Fade(bg, PanelAlpha(mode) / 255.0f));
    DrawRectangleLinesEx(r, 1.0f, ln);
}

// ---------------------------------------------------------------------------
//  Hover tooltip: last writer this frame wins (widgets draw back-to-front,
//  so the topmost hovered widget naturally records last).
// ---------------------------------------------------------------------------
void ZenTip(Rectangle r, const char *text)
{
    if (!text || !text[0]) return;
    if (!CheckCollisionPointRec(GetMousePosition(), r)) return;
    TextCopy(zen.tipText, text);
    zen.tipRect = r;
}

void ZenTipDraw(void)
{
    if (!zen.tipText[0]) return;
    const char *text = zen.tipText;
    Vector2 screen = ScreenStateSize();

    float pad = 6.0f;
    float w = ZenTextW(text) + 2*pad;
    float h = (float)GuiGetStyle(DEFAULT, TEXT_SIZE) + 2*pad;
    Vector2 m = GetMousePosition();
    float x = m.x + 14, y = m.y + 18;
    if (x + w > screen.x - 4) x = screen.x - 4 - w;
    if (y + h > screen.y - 4) y = m.y - h - 6;

    DrawRectangleRec((Rectangle){ x, y, w, h }, (Color){ 24, 26, 31, 245 });
    DrawRectangleLinesEx((Rectangle){ x, y, w, h }, 1.0f, (Color){ 100, 104, 116, 255 });
    GuiLabel((Rectangle){ x + pad, y + pad, w - 2*pad, h - 2*pad }, text);

    zen.tipText[0] = '\0';          // consumed; next frame re-records
}

// GuiLabel that ellipsizes to fit its rect; hovering shows the full text
// (plus an optional description) as a tooltip when anything was cut off.
void ZenLabelTip(Rectangle r, const char *text, const char *desc)
{
    bool cut = false;
    char buf[128];
    TextCopy(buf, text);
    int len = (int)strlen(buf);
    while (len > 1 && ZenTextW(buf) > r.width - 4)
    {
        // trim one char and re-mark the ellipsis.
        buf[--len] = '\0';
        if (len >= 2) { buf[len-1] = '.'; buf[len-2] = '.'; }
        cut = true;
    }
    GuiLabel(r, buf);
    if (cut || desc)
        ZenTip(r, desc ? (cut ? TextFormat("%s - %s", text, desc) : desc)
                       : text);
}
