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
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// widget-local state: only this file's slider machinery needs it, so it
// stays file-scope rather than in ZenCtx.
static bool      sliderGestureOpen = false;
static Rectangle finePressRect = {0};
static bool      finePressActive = false;
static bool      fineSticky = false;
// Fine drags move the value by hand while raygui's slider keeps mapping the
// cursor straight onto the bar. fineBias records how far the two have drifted
// apart, so letting Shift go mid-drag resumes from the fine-tuned value
// instead of snapping the knob to the mouse.
static float     fineBias = 0.0f;
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
        case AP_T_CRUMBLE: return "Per-glyph crumble: 0 leaves the text intact, 1 scatters the letters apart. Which way they go, how far and how much they tumble are set once per element, in the inspector's crumble section";
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
        fineBias = 0.0f;
    }
}

// ---------------------------------------------------------------------------
//  Mouse gesture ownership
//
//  raygui fires GuiButton on RELEASE inside its bounds without checking where
//  the press began, and GuiSlider grabs the knob on IsMouseButtonDown (not
//  Pressed). So a drag that starts on the timeline and merely passes over a
//  modal would delete a key or move a slider. The fix is to latch which layer
//  owns the press and keep every OTHER layer GuiLock()ed until release -
//  raygui's own guiLocked gate then swallows both cases, so no call site of
//  GuiButton/GuiSlider has to change.
// ---------------------------------------------------------------------------

// Topmost layer under the cursor. The one place z-order is decided.
static ZenLayer LayerAtCursor(Vector2 m)
{
    // Full-screen modals dim everything and dropdowns close on an outside
    // click, so they own the cursor wherever it is.
    if (ZenMenuModalOpen() || ZenEasingModalOpen() || ZenCloneOpen() ||
        ZenStringPoolOpen() || zen.menuOpen >= 0 || zen.easeDropOpen ||
        zen.addTrackOpen || zen.strDropOpen || zen.shapeDropOpen ||
        zen.sigDropMode != 0 ||
        ZenViewCtxOpen() || ZenTimelineCtxOpen())
        return ZEN_LAYER_OVERLAY;

    // Export modal is drawn after both, so it wins any overlap with them.
    if (ZenExportOpen() && CheckCollisionPointRec(m, ZenExportRect()))
        return ZEN_LAYER_FLOAT_EXPORT;

    // Track modal is drawn after the signal one, so it wins any overlap.
    if (zen.trackModal.open && CheckCollisionPointRec(m, zen.trackModal.rect))
        return ZEN_LAYER_FLOAT_TRACK;
    if (zen.sigModalIdx >= 0 && CheckCollisionPointRec(m, zen.sigModalRect))
        return ZEN_LAYER_FLOAT_SIG;

    // uiHover is last frame's panel rects, ORed in by ZenPanelsGui as it draws.
    if (m.y <= ZEN_MENU_BAR_H || zen.uiHover) return ZEN_LAYER_BASE;
    return ZEN_LAYER_VIEWPORT;
}

void ZenMouseOwnerUpdate(void)
{
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    { zen.mousePress = GetMousePosition();
      zen.mouseOwner = LayerAtCursor(zen.mousePress); zen.mouseHeld = true; }
    // The release frame is the one GuiButton acts on, so hold the owner
    // through it and only drop it once the button is properly up.
    else if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT) &&
             !IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
    { zen.mouseOwner = ZEN_LAYER_NONE; zen.mouseHeld = false;
      zen.mouseReflow = false; }
}

void ZenMouseReflow(void) { if (zen.mouseHeld) zen.mouseReflow = true; }

bool ZenLayerActive(ZenLayer l)
{
    // No gesture in flight: every layer is live, so hover/focus still work.
    if (!zen.mouseHeld && !IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) return true;
    // A press that owned nothing leaves everything live (nothing to protect).
    return zen.mouseOwner == l || zen.mouseOwner == ZEN_LAYER_NONE;
}

// GuiButton that also demands the press landed on it. raygui fires on RELEASE
// inside the bounds no matter where the press began, so a button that reflows
// under a held cursor (the track modal re-lays out when a key is picked) would
// fire on a click the user aimed somewhere else entirely.
bool ZenButton(Rectangle r, const char *text)
{
    if (zen.mouseHeld &&
        (zen.mouseReflow || !CheckCollisionPointRec(zen.mousePress, r)))
    {
        bool wasLocked = GuiIsLocked();
        if (!wasLocked) GuiLock();
        GuiButton(r, text);
        if (!wasLocked) GuiUnlock();
        return false;
    }
    return GuiButton(r, text);
}

void ZenModalDrag(Rectangle title, Vector2 *pos, Vector2 origin,
                  bool *dragging, Vector2 *dragOff, ZenLayer own)
{
    Vector2 mouse = GetMousePosition();
    if (!*dragging && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        ZenLayerActive(own) && CheckCollisionPointRec(mouse, title))
    { *dragging = true;
      *dragOff = (Vector2){ mouse.x - origin.x, mouse.y - origin.y }; }
    if (*dragging)
    {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
            *pos = (Vector2){ mouse.x - dragOff->x, mouse.y - dragOff->y };
        else *dragging = false;
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

    // Does the CURRENT gesture belong to this slider? GuiSlider grabs on
    // IsMouseButtonDown, not Pressed, so a slider that merely slides under a
    // held cursor would latch and overwrite its value from mouse.x. That is
    // not hypothetical: picking a key in the track modal's tree switches the
    // modal to single-key mode, which inserts a row and shifts every slider
    // below it down under the button that is still down from the pick.
    // Requiring the press to have landed inside r makes the grab deliberate.
    // A gesture that reflowed the UI owns nothing: on the press frame the press
    // point IS the cursor, so a slider that just slid under it would pass the
    // positional test below and grab on the still-down button.
    bool ownPress = !zen.mouseHeld ||
                    (!zen.mouseReflow &&
                     (CheckCollisionPointRec(zen.mousePress, r) ||
                      CheckCollisionPointRec(zen.mousePress, lbl)));
    if (!ownPress)
    {
        // Lock rather than just discarding the value: GuiSlider's update path
        // also sets guiControlExclusiveMode, which would go on suppressing
        // every GuiButton until release.
        bool wasLocked = GuiIsLocked();
        if (!wasLocked) GuiLock();
        GuiSlider(r, label, TextFormat("%.2f", *v), &(float){ *v }, lo, hi);
        if (!wasLocked) GuiUnlock();
        return false;
    }

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
    // GuiIsLocked, not zen.guiLocked: sliders are drawn in every layer, and
    // this reports the lock of whichever one is drawing us right now.
    if (!GuiIsLocked() &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, lbl))
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

    // --- wheel steps the value: Ctrl by increment, Shift by a tenth of it ----
    bool ctrl  = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT)   || IsKeyDown(KEY_RIGHT_SHIFT);
    float wheel = GetMouseWheelMove();
    if ((ctrl || shift) && wheel != 0.0f && CheckCollisionPointRec(mouse, r))
    {
        float step = SliderStep(lo, hi);
        if (shift) step *= 0.1f;                  // minimal increment
        float nv = ZenClampF(*v + wheel * step, lo, hi);
        if (nv != *v) { ZenUndoPush(); *v = nv; return true; }
        return false;
    }

    // --- Shift = fine relative drag -----------------------------------------
    // Follows the LIVE Shift key, so a drag can move in and out of fine mode.
    // fineSticky only latches "undo already pushed for this gesture".
    if (!GuiIsLocked() &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, r))
    {
        finePressRect = r; finePressActive = true; fineSticky = false;
        fineBias = 0.0f;
    }
    if (finePressActive && SameRect(finePressRect, r) &&
        IsMouseButtonDown(MOUSE_BUTTON_LEFT) && shift)
    {
        if (!fineSticky) { ZenUndoPush(); fineSticky = true; }
        // let raygui keep exclusive-drag ownership, but throw its value away.
        float raw = *v;
        GuiSlider(r, label, TextFormat("%.2f", *v), &raw, lo, hi);
        float fstep = (hi - lo) * 0.0005f;
        float nv = ZenClampF(*v + GetMouseDelta().x * fstep, lo, hi);
        fineBias = nv - raw;                      // drift vs. the cursor mapping
        if (nv != *v) { *v = nv; return true; }
        return false;
    }

    // --- normal drag ---------------------------------------------------------
    float tmp = *v;
    GuiSlider(r, label, TextFormat("%.2f", tmp), &tmp, lo, hi);
    if (tmp != *v && fineBias != 0.0f)            // resume from the fine value
        tmp = ZenClampF(tmp + fineBias, lo, hi);
    if (ctrl && !shift && tmp != *v)              // Ctrl-drag snaps to increments
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

// How the letters come apart. These shape the effect but do not drive it: the
// 'crumble' track says how far along it is, these say what it looks like getting
// there. Shown even when the element has no crumble track, because they are what
// you set up BEFORE keying one. ZenEditSlider pushes its own undo snapshot per
// gesture, so these write straight to the field.
float ZenCrumbleRows(float x, float y, float w, float rh, float gap, AnimElem *e)
{
    if (!e || e->kind != AE_TEXT) return y;

    GuiLine((Rectangle){ x, y, w, 8 }, "crumble"); y += 12;

    Rectangle cdR = { x+44, y, w-44-50, rh };
    ZenEditSlider(cdR, "", &e->crumbleDir, 0.0f, 360.0f);
    ZenLabelTip((Rectangle){ x, y, 44, rh }, "dir",
                "Which way the letters travel, in degrees. 0 throws them right "
                "and the angle turns clockwise, so 90 is straight down (the "
                "default), 180 left, 270 up");
    y += rh + gap;

    Rectangle csR = { x+44, y, w-44-50, rh };
    ZenEditSlider(csR, "", &e->crumbleSpread, 0.0f, 360.0f);
    ZenLabelTip((Rectangle){ x, y, 44, rh }, "spread",
                "How wide the scatter fans out around that direction, in "
                "degrees. 0 sends every letter the same way; 360 throws them in "
                "all directions");
    y += rh + gap;

    Rectangle cnR = { x+44, y, w-44-50, rh };
    ZenEditSlider(cnR, "", &e->crumbleDist, 0.0f, 2.0f);
    ZenLabelTip((Rectangle){ x, y, 44, rh }, "dist",
                "How far the letters travel once the crumble reaches 1, as a "
                "fraction of the screen height. The motion accelerates, so most "
                "of it happens late");
    y += rh + gap;

    // "spin", not "rot": the rotation row elsewhere is this element's own
    // AP_T_ROT, and two rows labelled rot is a bug report waiting to happen.
    Rectangle crR = { x+44, y, w-44-50, rh };
    ZenEditSlider(crR, "", &e->crumbleRot, -720.0f, 720.0f);
    ZenLabelTip((Rectangle){ x, y, 44, rh }, "spin",
                "How much each letter tumbles by the time the crumble reaches "
                "1, in degrees. Each letter gets its own amount between plus and "
                "minus this, so 0 keeps them all upright");
    y += rh + gap;

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

// ============================================================================
//  Multi-line text area
//
//  raygui 4.5 cannot do this: GuiTextBox hard-disables multiline internally,
//  keeps one cursor index in a file-static shared by every box, tracks no
//  cursor line at all, and its paste stops at the first newline. So the whole
//  editor is hand-rolled here, following the same shape as the slider's
//  precise-entry box above: file-static state, one owner at a time, and the
//  caller toggles `active` on the returned true.
// ============================================================================

static Rectangle taRect = {0};      // which area owns the keyboard
static int  taCur = 0;              // cursor, a byte index into the buffer
static int  taSel = -1;             // selection anchor; <0 means no selection
static float taScroll = 0.0f;       // vertical scroll, pixels
static double taBlink = 0.0;

// Lines are measured with the same call raygui draws with, so the caret lands
// where the glyphs actually are.
#define TA_PAD   4.0f

static float TaLineH(void)
{
    return (float)GuiGetStyle(DEFAULT, TEXT_SIZE) + 4.0f;
}

// Start of the line containing byte `i`.
static int TaLineStart(const char *s, int i)
{
    while (i > 0 && s[i-1] != '\n') i--;
    return i;
}
static int TaLineEnd(const char *s, int i)
{
    while (s[i] && s[i] != '\n') i++;
    return i;
}

// Width of the first `n` bytes of `s`, as raygui draws them.
static float TaWidthN(const char *s, int n)
{
    char buf[ANIM_TEXT_LEN_MAX];
    if (n <= 0) return 0.0f;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    memcpy(buf, s, (size_t)n);
    buf[n] = '\0';
    return ZenTextW(buf);
}

static bool TaIsWordChar(char c)
{
    return !isspace((unsigned char)c);
}

// Next word boundary in `dir` (-1 back, +1 forward), the usual Ctrl+Arrow rule:
// skip the run you are in, then skip the whitespace after it.
static int TaWordJump(const char *s, int i, int dir)
{
    int len = (int)strlen(s);
    if (dir < 0)
    {
        while (i > 0 && !TaIsWordChar(s[i-1])) i--;
        while (i > 0 &&  TaIsWordChar(s[i-1])) i--;
    }
    else
    {
        while (i < len &&  TaIsWordChar(s[i])) i++;
        while (i < len && !TaIsWordChar(s[i])) i++;
    }
    return i;
}

static void TaDeleteRange(char *s, int a, int b)
{
    if (a > b) { int t = a; a = b; b = t; }
    memmove(s + a, s + b, strlen(s + b) + 1);
    taCur = a; taSel = -1;
}

// Drop the selection (if any) and insert `ins` at the cursor, bounded by cap.
static void TaInsert(char *s, int cap, const char *ins)
{
    if (taSel >= 0 && taSel != taCur) TaDeleteRange(s, taSel, taCur);
    int len = (int)strlen(s);
    int add = (int)strlen(ins);
    if (len + add > cap - 1) add = cap - 1 - len;
    if (add <= 0) return;
    memmove(s + taCur + add, s + taCur, (size_t)(len - taCur + 1));
    memcpy(s + taCur, ins, (size_t)add);
    taCur += add;
    taSel = -1;
}

bool ZenTextAreaTyping(void)      { return taRect.width > 0.0f; }
void ZenTextAreaClose(void)       { taRect = (Rectangle){0}; taSel = -1; }

// Rows the text needs, so callers can size the box to its content.
int ZenTextAreaRows(const char *s)
{
    int rows = 1;
    for (int i = 0; s[i]; i++) if (s[i] == '\n') rows++;
    return rows;
}
float ZenTextAreaHeight(const char *s, int maxRows)
{
    int rows = ZenTextAreaRows(s);
    if (rows > maxRows) rows = maxRows;
    return (float)rows * TaLineH() + 2.0f*TA_PAD;
}

// Returns true when the caller should flip its `active` flag (same contract as
// GuiTextBox): clicking in when inactive, or committing when active.
bool ZenEditTextArea(Rectangle r, char *text, int cap, bool active)
{
    Vector2 mouse = GetMousePosition();
    bool    inside = CheckCollisionPointRec(mouse, r);
    bool    owned = active && SameRect(taRect, r);
    float   lh = TaLineH();
    int     len = (int)strlen(text);
    bool    toggle = false;

    // --- frame / background -------------------------------------------------
    // GuiDrawRectangle is raygui-internal, so the frame is drawn with the
    // public calls using the same TEXTBOX style colours a GuiTextBox would.
    DrawRectangleRec(r, GetColor(GuiGetStyle(TEXTBOX, active ? BASE_COLOR_PRESSED
                                                             : BASE_COLOR_NORMAL)));
    DrawRectangleLinesEx(r, (float)GuiGetStyle(DEFAULT, BORDER_WIDTH),
                         GetColor(GuiGetStyle(TEXTBOX, active ? BORDER_COLOR_PRESSED
                                                              : BORDER_COLOR_NORMAL)));

    if (active && !owned) { taRect = r; taCur = len; taSel = -1; taScroll = 0.0f; }

    // --- input --------------------------------------------------------------
    if (owned)
    {
        bool ctrl  = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        bool shift = IsKeyDown(KEY_LEFT_SHIFT)   || IsKeyDown(KEY_RIGHT_SHIFT);
        int  before = taCur;
        bool moved = false;

        // remember where a selection started the moment Shift+motion begins.
        #define TA_ANCHOR() do { if (shift) { if (taSel < 0) taSel = before; } \
                                 else taSel = -1; } while (0)

        if (ctrl && IsKeyPressed(KEY_A)) { taSel = 0; taCur = len; }
        else if (ctrl && (IsKeyPressed(KEY_C) || IsKeyPressed(KEY_X)) &&
                 taSel >= 0 && taSel != taCur)
        {
            int a = taSel < taCur ? taSel : taCur;
            int b = taSel < taCur ? taCur : taSel;
            char buf[ANIM_TEXT_LEN_MAX];
            int n = b - a; if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
            memcpy(buf, text + a, (size_t)n); buf[n] = '\0';
            SetClipboardText(buf);
            if (IsKeyPressed(KEY_X)) { ZenUndoPush(); TaDeleteRange(text, a, b); }
        }
        else if (ctrl && IsKeyPressed(KEY_V))
        {
            const char *clip = GetClipboardText();
            if (clip && clip[0])
            {
                // keep newlines, normalise CRLF, drop the other control bytes -
                // raygui's own paste would have truncated at the first newline.
                char buf[ANIM_TEXT_LEN_MAX];
                int o = 0;
                for (int i = 0; clip[i] && o < (int)sizeof(buf) - 1; i++)
                {
                    char c = clip[i];
                    if (c == '\r') { if (clip[i+1] == '\n') continue; c = '\n'; }
                    if (c != '\n' && (unsigned char)c < 32) continue;
                    buf[o++] = c;
                }
                buf[o] = '\0';
                ZenUndoPush();
                TaInsert(text, cap, buf);
            }
        }
        else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))
        {
            // Shift+Enter is the newline; a bare Enter commits, matching every
            // other field in the editor.
            if (shift) { ZenUndoPush(); TaInsert(text, cap, "\n"); }
            else { toggle = true; ZenTextAreaClose(); }
        }
        else if (IsKeyPressed(KEY_BACKSPACE))
        {
            ZenUndoPush();
            if (taSel >= 0 && taSel != taCur) TaDeleteRange(text, taSel, taCur);
            else if (taCur > 0)
            {
                int a = ctrl ? TaWordJump(text, taCur, -1) : taCur - 1;
                TaDeleteRange(text, a, taCur);
            }
        }
        else if (IsKeyPressed(KEY_DELETE))
        {
            ZenUndoPush();
            if (taSel >= 0 && taSel != taCur) TaDeleteRange(text, taSel, taCur);
            else if (taCur < len)
            {
                int b = ctrl ? TaWordJump(text, taCur, +1) : taCur + 1;
                TaDeleteRange(text, taCur, b);
            }
        }
        else if (IsKeyPressed(KEY_LEFT))
        {
            TA_ANCHOR();
            taCur = ctrl ? TaWordJump(text, taCur, -1) : (taCur > 0 ? taCur-1 : 0);
            moved = true;
        }
        else if (IsKeyPressed(KEY_RIGHT))
        {
            TA_ANCHOR();
            taCur = ctrl ? TaWordJump(text, taCur, +1) : (taCur < len ? taCur+1 : len);
            moved = true;
        }
        else if (IsKeyPressed(KEY_HOME))
        { TA_ANCHOR(); taCur = TaLineStart(text, taCur); moved = true; }
        else if (IsKeyPressed(KEY_END))
        { TA_ANCHOR(); taCur = TaLineEnd(text, taCur); moved = true; }
        else if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_DOWN))
        {
            TA_ANCHOR();
            // hold the column by measured width, so the caret tracks the glyphs
            // rather than the byte count.
            int ls = TaLineStart(text, taCur);
            float goalX = TaWidthN(text + ls, taCur - ls);
            int target;
            if (IsKeyPressed(KEY_UP))
            {
                if (ls == 0) target = -1;
                else target = TaLineStart(text, ls - 1);
            }
            else
            {
                int le = TaLineEnd(text, taCur);
                target = text[le] ? le + 1 : -1;
            }
            if (target >= 0)
            {
                int te = TaLineEnd(text, target);
                int best = target;
                for (int i = target; i <= te; i++)
                    if (TaWidthN(text + target, i - target) <= goalX) best = i;
                taCur = best;
            }
            moved = true;
        }
        else
        {
            int cp;
            while ((cp = GetCharPressed()) != 0)
            {
                if (cp < 32 || cp > 126) continue;   // font is ASCII
                if (!moved) { ZenUndoPush(); moved = true; }
                char ins[2] = { (char)cp, 0 };
                TaInsert(text, cap, ins);
            }
            moved = false;
        }

        if (moved && !shift) taSel = -1;
        if (taCur != before) taBlink = GetTime();
        #undef TA_ANCHOR

        len = (int)strlen(text);
        if (taCur > len) taCur = len;
    }

    // --- click to place the caret / take focus ------------------------------
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        if (inside)
        {
            if (!active) toggle = true;
            taRect = r; taSel = -1; taBlink = GetTime();
            int line = (int)((mouse.y - r.y - TA_PAD - taScroll) / lh);
            if (line < 0) line = 0;
            int i = 0, ln = 0;
            while (ln < line && text[i]) { i = TaLineEnd(text, i); if (text[i]) i++; ln++; }
            int ls = i, le = TaLineEnd(text, i);
            float goalX = mouse.x - r.x - TA_PAD;
            int best = ls;
            for (int k = ls; k <= le; k++)
                if (TaWidthN(text + ls, k - ls) <= goalX) best = k;
            taCur = best;
        }
        else if (owned) { toggle = true; ZenTextAreaClose(); }
    }

    // --- scroll the caret into view -----------------------------------------
    float contentH = (float)ZenTextAreaRows(text) * lh;
    float viewH = r.height - 2.0f*TA_PAD;
    if (owned)
    {
        int caretLine = 0;
        for (int i = 0; i < taCur; i++) if (text[i] == '\n') caretLine++;
        float top = (float)caretLine * lh;
        if (top + taScroll < 0.0f)        taScroll = -top;
        if (top + lh + taScroll > viewH)  taScroll = viewH - top - lh;
    }
    if (contentH <= viewH) taScroll = 0.0f;
    else if (taScroll < viewH - contentH) taScroll = viewH - contentH;
    if (taScroll > 0.0f) taScroll = 0.0f;

    // --- draw ---------------------------------------------------------------
    BeginScissorMode((int)r.x + 1, (int)r.y + 1,
                     (int)r.width - 2, (int)r.height - 2);
    {
        Color fg = GetColor(GuiGetStyle(TEXTBOX, active ? TEXT_COLOR_PRESSED
                                                        : TEXT_COLOR_NORMAL));
        int selA = -1, selB = -1;
        if (owned && taSel >= 0 && taSel != taCur)
        { selA = taSel < taCur ? taSel : taCur; selB = taSel < taCur ? taCur : taSel; }

        int i = 0, line = 0;
        for (;;)
        {
            int le = TaLineEnd(text, i);
            float ly = r.y + TA_PAD + (float)line*lh + taScroll;

            if (ly + lh >= r.y && ly <= r.y + r.height)
            {
                // selection band for the part of this line inside the range
                if (selA >= 0 && selB > i - 1 && selA <= le)
                {
                    int a = selA > i ? selA : i;
                    int b = selB < le ? selB : le;
                    if (b > a)
                    {
                        float ax = TaWidthN(text + i, a - i);
                        float bx = TaWidthN(text + i, b - i);
                        DrawRectangleRec((Rectangle){ r.x + TA_PAD + ax, ly,
                                                      bx - ax, lh },
                                         Fade(GetColor(GuiGetStyle(TEXTBOX,
                                              BORDER_COLOR_PRESSED)), 0.4f));
                    }
                }

                char buf[ANIM_TEXT_LEN_MAX];
                int n = le - i;
                if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
                memcpy(buf, text + i, (size_t)n); buf[n] = '\0';
                DrawTextEx(GuiGetFont(), buf, (Vector2){ r.x + TA_PAD, ly },
                           (float)GuiGetStyle(DEFAULT, TEXT_SIZE),
                           (float)GuiGetStyle(DEFAULT, TEXT_SPACING), fg);

                // caret
                if (owned && taCur >= i && taCur <= le &&
                    fmod(GetTime() - taBlink, 1.0) < 0.5)
                {
                    float cx = r.x + TA_PAD + TaWidthN(text + i, taCur - i);
                    DrawRectangleRec((Rectangle){ cx, ly, 1.0f, lh }, fg);
                }
            }

            if (!text[le]) break;
            i = le + 1; line++;
        }
    }
    EndScissorMode();

    return toggle;
}
