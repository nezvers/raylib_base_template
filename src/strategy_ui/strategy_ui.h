#ifndef STRATEGY_UI_H
#define STRATEGY_UI_H

// ============================================================================
//  strategy_ui.h  -  the shared widget set and palette for the strategy tools
//
//  The asset forge grew a good set of controls - tooltips that flip at the
//  screen edge, sliders with Ctrl-snap and Shift fine-drag, scissor-aware hit
//  testing - and every one of them was a file-static in strategy_forge.c. The
//  map forge needs the same controls, and the palette was ALREADY copy-pasted
//  into strategy_showcase.c, so a second copy would have made three.
//
//  EVERYTHING HERE IS SCREEN SPACE, drawn from a state's Gui(). main.c runs
//  Draw() inside the small letterboxed render target and Gui() after it against
//  the real window, so these widgets use raw GetMousePosition() with NO
//  Screen2Target - that call converts INTO the target space the tools do not
//  use.
//
//  PER-FRAME STATE. Tooltips, the modal gate and the scissor clip are all
//  cross-widget: one widget records a tip, another paints it last. File-scope
//  statics cannot span translation units, so that state lives in one plain POD
//  struct reached through UiCtx() - the same shape as ZenCtx in the zen editor,
//  and for the same reason. It holds no resources; there is nothing to free.
//
//  FRAME CONTRACT, per state, per frame:
//      UiFrameBegin(modalOpen)   once, before any widget
//      ... widgets ...
//      UiTipDraw(fontSize)       once, LAST, so the tip paints over everything
// ============================================================================

#include "raylib.h"
#include <stdbool.h>

// ---------------------------------------------------------------------------
//  Palette. The forge's values, now with one owner.
// ---------------------------------------------------------------------------
#define UI_COL_BG        (Color){  18,  20,  26, 255 }
#define UI_COL_PANEL     (Color){  26,  29,  37, 255 }
#define UI_COL_PANEL_HI  (Color){  33,  37,  47, 255 }
#define UI_COL_LINE      (Color){  52,  57,  70, 255 }
#define UI_COL_LINE_HI   (Color){ 110, 120, 145, 255 }
#define UI_COL_TEXT      (Color){ 232, 236, 245, 255 }
#define UI_COL_TEXT_DIM  (Color){ 138, 146, 166, 255 }
#define UI_COL_ACCENT    (Color){ 200, 150, 255, 255 }   // the CUSTOM violet
#define UI_COL_WARN      (Color){ 255, 170,  90, 255 }
#define UI_COL_TIP_BG    (Color){  14,  15,  20, 245 }

#define UI_TIP_MAX  256

// ---------------------------------------------------------------------------
//  Cross-widget per-frame state
// ---------------------------------------------------------------------------
typedef struct {
    // Tooltip recorded by whichever widget the cursor is over this frame, and
    // painted by UiTipDraw() after everything else - a tip drawn in place would
    // be overpainted by the panels that come after it.
    char      tip[UI_TIP_MAX];

    // A modal is up somewhere in the owning state. Widgets BEHIND it must not
    // answer the mouse.
    bool      modalOpen;

    // Set while a modal paints its OWN controls. Widgets consult UiModalBlocks()
    // rather than modalOpen: the guard exists to stop clicks reaching the editor
    // behind a modal, and a modal's own buttons are not behind it. Without the
    // exemption a save prompt's SAVE and CANCEL are both dead.
    bool      inModal;

    // Active clip rectangle for scrolled panels. BeginScissorMode clips PIXELS
    // but not hit testing, so a control scrolled out of view still answers the
    // mouse. Zero width means "no clip", which is the normal case.
    Rectangle clip;

    // Fine-drag bookkeeping. Only one slider can be dragged at a time, so one
    // set of fields covers every UiSlider on screen.
    Rectangle fineRect;
    bool      fineActive;
    float     fineBias;
} UiCtx;

UiCtx *UiCtxGet(void);

// Start a frame: clears the recorded tip and publishes whether a modal is up.
void UiFrameBegin(bool modalOpen);

// Modal gate. UiSetInModal(true) while a modal draws its own controls.
void UiSetInModal(bool value);
bool UiModalBlocks(void);

// Scissor clip for scrolled panels. Pass a zero-width rect to clear it.
void UiSetClip(Rectangle clip);
Rectangle UiGetClip(void);
bool UiClipAllows(Vector2 mouse);

// One scale factor against a 1280-wide design, clamped so a tool stays usable
// on a small window without its panels eating the viewport.
float UiScale(void);

// ---------------------------------------------------------------------------
//  Widgets
// ---------------------------------------------------------------------------
// `primary` paints the accent-filled variant used for the one action a panel is
// really about. Both honour the modal gate, the clip rect and the tooltip.
bool UiButtonEx(Rectangle r, const char *label, bool enabled,
                const char *tip, int fs, bool primary);
bool UiButton(Rectangle r, const char *label, bool enabled,
              const char *tip, int fs);

// Snap increment for a slider, keyed on its range. The buckets matter: a naive
// two-way split put the +-4 geometry sliders in the same bucket as rotation and
// gave them a step of 5, which snapped them to -5/0/5 and so did nothing at all
// inside the range anyone actually uses.
float UiSliderStep(float lo, float hi);

// A labelled float slider. Returns true on the frames it actually changed the
// value, so the caller can open an undo gesture rather than pushing per frame.
// CTRL snaps to increments, SHIFT drags at a twentieth of the normal rate.
bool UiSlider(Rectangle r, const char *label, float *v,
              float lo, float hi, int fs);

// A row of mutually exclusive chips. Returns the newly picked index, or -1.
int UiChips(Rectangle r, const char **labels, int count, int active,
            int fs, Color accent);

// Record the tooltip for `r` if the cursor is over it. Paint with UiTipDraw().
void UiTip(Rectangle r, const char *text);
void UiTipDraw(int fontSize);

// Dim the screen, draw a centred panel with a title and an optional message,
// and return the panel rect so the caller can lay its buttons out inside.
Rectangle UiModalFrame(const char *title, const char *msg, float s, int fs,
                       float mw, float mh);

#endif // STRATEGY_UI_H
