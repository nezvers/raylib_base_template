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

// Width of `text` AS RAYGUI DRAWS IT. raygui adds DEFAULT/TEXT_SPACING once per
// glyph without scaling it, which MeasureTextEx's spacing argument does not
// reproduce - measuring any other way under-reports and the text overflows
// whatever box was sized from it (tooltips, ellipsized labels).
float ZenTextW(const char *text)
{
    if (!text || !text[0]) return 0.0f;
    float fs = (float)GuiGetStyle(DEFAULT, TEXT_SIZE);
    float sp = (float)GuiGetStyle(DEFAULT, TEXT_SPACING);
    return MeasureTextEx(GuiGetFont(), text, fs, 0.0f).x +
           sp * (float)TextLength(text);
}

// Plain-language description of an animatable property. The short .cfg names
// (AnimPropName: "rot", "w", "fade") are the file format and stay as they are;
// this is what the UI explains on hover.
const char *ZenPropDesc(int prop)
{
    switch (prop)
    {
        case AP_T_POS_X: return "Horizontal position of the text's center, as a fraction of the screen width (0 = left edge, 1 = right edge)";
        case AP_T_POS_Y: return "Vertical position of the text's center, as a fraction of the screen height (0 = top, 1 = bottom)";
        case AP_T_SIZE:  return "Font size, as a fraction of the screen height - so text keeps its proportions on any display";
        case AP_T_ALPHA: return "Opacity of the text: 0 is fully transparent, 1 is fully opaque";
        case AP_T_ROT:   return "Rotation of the whole text block, in degrees (positive turns clockwise)";
        case AP_T_CRUMBLE: return "Per-glyph crumble: 0 leaves the text intact, 1 scatters the letters apart";
        case AP_T_COLOR: return "Text colour. Keyed as RGB, so a key blends between colours over time";

        case AP_S_POS_X: return "Horizontal position of the shape's center, as a fraction of the screen width (0 = left edge, 1 = right edge)";
        case AP_S_POS_Y: return "Vertical position of the shape's center, as a fraction of the screen height (0 = top, 1 = bottom)";
        case AP_S_W:     return "Width of the shape, as a fraction of the screen width";
        case AP_S_H:     return "Height of the shape, as a fraction of the screen height";
        case AP_S_ALPHA: return "Opacity of the shape's fill: 0 is fully transparent, 1 is fully opaque";
        case AP_S_ROT:   return "Rotation of the shape, in degrees (positive turns clockwise)";
        case AP_S_COLOR: return "Fill colour of the shape. Keyed as RGB, so a key blends between colours over time";
        case AP_S_OUTLINE_COLOR: return "Colour of the shape's outline. Keyed as RGB";
        case AP_S_OUTLINE: return "Outline thickness, as a fraction of the screen height. 0 turns the outline off";
        case AP_S_OUTLINE_ALPHA: return "Opacity of the outline alone - the fill has its own alpha";
        case AP_S_SCALE: return "Uniform size multiplier on top of the width and height (1 = the authored size). The one-track way to grow or shrink a shape without adjusting w and h together";

        case AP_G_FADE:  return "Whole-screen fade toward the fade colour: 0 shows the scene, 1 covers it completely";
        case AP_G_COLOR: return "The colour the screen fades toward. Keyed as RGB";
        case AP_G_BG_ALPHA: return "Opacity of the scene's background fill behind every element";
        case AP_G_BG_COLOR: return "Colour of the scene's background fill. Keyed as RGB";
    }
    return NULL;
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

// Index of a doc element, for the ctx-level helpers that address by index.
static int ElemIndex(const AnimElem *e)
{
    for (int i = 0; i < zen.doc.elemCount; i++)
        if (&zen.doc.elems[i] == e) return i;
    return -1;
}

// Slider for an ANIMATABLE property: with auto-key ON the edit becomes a key at
// the playhead (starting the track if the property had none), so keying never
// needs a separate "add track" step. Auto-key OFF edits the untracked base
// pose and locks anything already tracked.
void ZenPropSlider(Rectangle r, AnimElem *e, int prop, float *baseField)
{
    float lo, hi; ZenPropRange(e, prop, &lo, &hi);
    AnimTrack *tr = AnimElemFindTrack(e, prop);
    if (!tr)
    {
        if (!zen.autoKey) { ZenEditSlider(r, "", baseField, lo, hi); return; }

        // auto-key on an untracked property: start the track on first edit.
        float bv = *baseField;
        if (ZenEditSlider(r, "", &bv, lo, hi))
        {
            ZenUndoPush();
            tr = AnimElemAddTrack(e, prop);
            if (tr)
            {
                // seed t=0 from the ORIGINAL pose before the base moves, or the
                // starting value would be the edit itself and nothing animates.
                if (zen.playhead > ZEN_AUTOKEY_EPS) ZenEnsureZeroKey(e, tr);
                *baseField = bv;
                AnimTrackWriteKeyAt(tr, zen.playhead, bv, ZEN_AUTOKEY_EPS);
                ZenAutoKeyFocus(ElemIndex(e), prop, zen.playhead);
            }
            else *baseField = bv;               // track cap reached: base only
        }
        return;
    }

    float v = AnimElemProp(e, prop, zen.playhead);
    if (!zen.autoKey) GuiSetState(STATE_DISABLED);
    if (ZenEditSlider(r, "", &v, lo, hi))
    {
        if (zen.playhead > ZEN_AUTOKEY_EPS) ZenEnsureZeroKey(e, tr);
        AnimTrackWriteKeyAt(tr, zen.playhead, v, ZEN_AUTOKEY_EPS);
        ZenAutoKeyFocus(ElemIndex(e), prop, zen.playhead);
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
                TextFormat(ctr ? "%s (rgb, keyed)" : "%s (rgb)", label),
                ZenPropDesc(prop));
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
        if (!ctr && zen.autoKey)
        {
            // auto-key on an untracked colour: start the track on first edit,
            // seeding t=0 from the ORIGINAL colour before the base changes.
            ZenUndoPush();
            ctr = AnimElemAddTrack(e, prop);
            if (ctr && zen.playhead > ZEN_AUTOKEY_EPS) ZenEnsureZeroColorKey(e, ctr);
        }
        if (!ctr) { base->r = nc.r; base->g = nc.g; base->b = nc.b; }
        else
        {
            if (zen.playhead > ZEN_AUTOKEY_EPS) ZenEnsureZeroColorKey(e, ctr);
            AnimTrackWriteColorKeyAt(ctr, zen.playhead, nc, ZEN_AUTOKEY_EPS);
            ZenAutoKeyFocus(ElemIndex(e), prop, zen.playhead);
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
    // bounded: TextCopy runs to the NUL, and tips are built with TextFormat.
    int n = (int)sizeof(zen.tipText) - 1;
    int i = 0;
    for (; i < n && text[i]; i++) zen.tipText[i] = text[i];
    zen.tipText[i] = '\0';
    zen.tipRect = r;
}

#define ZEN_TIP_LINES 6

// Greedy word wrap into `lines`, none wider than maxW. Returns the line count
// and the width of the widest line through `outW`.
static int TipWrap(const char *text, float maxW,
                   char lines[ZEN_TIP_LINES][160], float *outW)
{
    int n = 0;
    float widest = 0.0f;
    const char *p = text;
    lines[0][0] = '\0';                 // empty input still yields one blank line
    while (*p && n < ZEN_TIP_LINES)
    {
        char cur[160] = { 0 };
        int len = 0;
        while (*p == ' ') p++;
        const char *lineStart = p;
        while (*p)
        {
            // take the next word, keep it if the line still fits.
            const char *ws = p;
            while (*p && *p != ' ') p++;
            int wl = (int)(p - ws);
            int sep = len ? 1 : 0;
            if (len + sep + wl >= (int)sizeof(cur)) { p = ws; break; }

            char trial[160];
            for (int i = 0; i < len; i++) trial[i] = cur[i];
            if (sep) trial[len] = ' ';
            for (int i = 0; i < wl; i++) trial[len + sep + i] = ws[i];
            trial[len + sep + wl] = '\0';

            if (ZenTextW(trial) > maxW && len > 0) { p = ws; break; }
            for (int i = 0; i <= len + sep + wl; i++) cur[i] = trial[i];
            len += sep + wl;
            while (*p == ' ') p++;
        }
        if (len == 0)                       // one unbreakable word: hard-take it
        {
            p = lineStart;
            while (*p && *p != ' ' && len < (int)sizeof(cur) - 1) cur[len++] = *p++;
            cur[len] = '\0';
        }
        TextCopy(lines[n], cur);
        float lw = ZenTextW(lines[n]);
        if (lw > widest) widest = lw;
        n++;
    }
    *outW = widest;
    return n > 0 ? n : 1;
}

void ZenTipDraw(void)
{
    if (!zen.tipText[0]) return;
    Vector2 screen = ScreenStateSize();

    float pad = 6.0f;
    float lh = (float)GuiGetStyle(DEFAULT, TEXT_SIZE) + 4.0f;
    float maxW = screen.x * 0.4f; if (maxW > 360.0f) maxW = 360.0f;

    char lines[ZEN_TIP_LINES][160];
    float textW = 0.0f;
    int n = TipWrap(zen.tipText, maxW, lines, &textW);

    float w = textW + 2*pad;
    float h = n * lh + 2*pad;

    // flip rather than clip: a 3% margin keeps the box off the screen edges.
    float mx = screen.x * 0.03f, my = screen.y * 0.03f;
    Vector2 mp = GetMousePosition();
    float x = mp.x + 14, y = mp.y + 18;
    if (x + w > screen.x - mx) x = mp.x - 14 - w;
    if (y + h > screen.y - my) y = mp.y - 6 - h;
    if (x < 4) x = 4;
    if (y < 4) y = 4;

    DrawRectangleRec((Rectangle){ x, y, w, h }, (Color){ 24, 26, 31, 245 });
    DrawRectangleLinesEx((Rectangle){ x, y, w, h }, 1.0f, (Color){ 100, 104, 116, 255 });
    for (int i = 0; i < n; i++)
        GuiLabel((Rectangle){ x + pad, y + pad + i*lh, w - 2*pad, lh }, lines[i]);

    zen.tipText[0] = '\0';          // consumed; next frame re-records
}

// GuiLabel that ellipsizes to fit its rect; hovering shows the full text
// (plus an optional description) as a tooltip when anything was cut off.
void ZenLabelTip(Rectangle r, const char *text, const char *desc)
{
    bool cut = false;
    char buf[256];
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
