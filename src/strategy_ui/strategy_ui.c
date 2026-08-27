// ============================================================================
//  strategy_ui.c  -  the shared widget set (see strategy_ui.h)
//
//  Lifted wholesale out of strategy_forge.c, behaviour unchanged. The comments
//  that explain WHY a control behaves the way it does came with it - they are
//  the record of what was already tried and rejected, which is exactly what a
//  later reader needs before "simplifying" one of these.
// ============================================================================

#include "strategy_ui.h"
#include "raygui.h"
#include "../screen_state/screen_state.h"
#include "../audio_state/audio_state.h"
#include <string.h>
#include <math.h>

// The module's whole state. POD, no resources, nothing to free.
static UiCtx ui;

UiCtx *UiCtxGet(void) { return &ui; }

void UiFrameBegin(bool modalOpen)
{
    ui.modalOpen = modalOpen;
    ui.inModal   = false;
    ui.tip[0]    = '\0';
}

void UiSetInModal(bool value) { ui.inModal = value; }

bool UiModalBlocks(void) { return ui.modalOpen && !ui.inModal; }

void UiSetClip(Rectangle clip) { ui.clip = clip; }

Rectangle UiGetClip(void) { return ui.clip; }

bool UiClipAllows(Vector2 mouse)
{
    if (ui.clip.width <= 0.0f) return true;
    return CheckCollisionPointRec(mouse, ui.clip);
}

float UiScale(void)
{
    Vector2 sc = ScreenStateSize();
    float s = sc.x / 1280.0f;
    if (s < 0.72f) s = 0.72f;
    if (s > 1.60f) s = 1.60f;
    return s;
}

// ---------------------------------------------------------------------------
//  Tooltips
// ---------------------------------------------------------------------------
void UiTip(Rectangle r, const char *text)
{
    if (!UiClipAllows(GetMousePosition())) return;
    if ((text == NULL) || (text[0] == '\0')) return;
    if (UiModalBlocks()) return;        // a tip from behind a modal is noise
    if (!CheckCollisionPointRec(GetMousePosition(), r)) return;
    TextCopy(ui.tip, text);
}

void UiTipDraw(int fontSize)
{
    if (!ui.tip[0]) return;

    Vector2 screen = ScreenStateSize();
    float pad = 8.0f;
    float w = (float)MeasureText(ui.tip, fontSize) + 2.0f*pad;
    float h = (float)fontSize + 2.0f*pad;

    // Flip rather than clip, so a tip near an edge stays readable.
    Vector2 mp = GetMousePosition();
    float x = mp.x + 16.0f, y = mp.y + 20.0f;
    if ((x + w) > (screen.x - 8.0f)) x = mp.x - 16.0f - w;
    if ((y + h) > (screen.y - 8.0f)) y = mp.y - 8.0f - h;
    if (x < 4.0f) x = 4.0f;
    if (y < 4.0f) y = 4.0f;

    DrawRectangleRec((Rectangle){ x, y, w, h }, UI_COL_TIP_BG);
    DrawRectangleLinesEx((Rectangle){ x, y, w, h }, 1.0f, UI_COL_LINE_HI);
    DrawText(ui.tip, (int)(x + pad), (int)(y + pad), fontSize, UI_COL_TEXT);

    ui.tip[0] = '\0';       // consumed; re-recorded next frame
}

// ---------------------------------------------------------------------------
//  Buttons
// ---------------------------------------------------------------------------
bool UiButtonEx(Rectangle r, const char *label, bool enabled,
                const char *tip, int fs, bool primary)
{
    Vector2 mp = GetMousePosition();
    bool hot = enabled && !UiModalBlocks() && UiClipAllows(mp) &&
               CheckCollisionPointRec(mp, r);

    Color fill, edge, tc;
    if (!enabled)
    {
        fill = Fade(UI_COL_PANEL, 0.45f);
        edge = Fade(UI_COL_LINE, 0.5f);
        tc   = Fade(UI_COL_TEXT_DIM, 0.35f);
    }
    else if (primary)
    {
        fill = hot ? Fade(UI_COL_ACCENT, 0.34f) : Fade(UI_COL_ACCENT, 0.18f);
        edge = UI_COL_ACCENT;
        tc   = UI_COL_TEXT;
    }
    else
    {
        fill = hot ? UI_COL_PANEL_HI : UI_COL_PANEL;
        edge = hot ? UI_COL_LINE_HI : UI_COL_LINE;
        tc   = UI_COL_TEXT;         // full strength: enabled must read as enabled
    }

    DrawRectangleRec(r, fill);
    DrawRectangleLinesEx(r, 1.0f, edge);
    DrawText(label, (int)(r.x + (r.width - (float)MeasureText(label, fs))*0.5f),
             (int)(r.y + (r.height - (float)fs)*0.5f), fs, tc);

    UiTip(r, tip);
    if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { AudioPlayButton(); return true; }
    return false;
}

bool UiButton(Rectangle r, const char *label, bool enabled,
              const char *tip, int fs)
{
    return UiButtonEx(r, label, enabled, tip, fs, false);
}

// ---------------------------------------------------------------------------
//  Sliders
// ---------------------------------------------------------------------------
static bool SameRectF(Rectangle a, Rectangle b)
{
    return (a.x == b.x) && (a.y == b.y) && (a.width == b.width) && (a.height == b.height);
}

// The buckets matter more than they look. A two-way split at "range <= 2" put
// the OFFSET and SIZE sliders (-4..4, so a range of 8) in the same bucket as
// rotation (-180..180) and gave them a step of 5 - which snapped them to -5, 0
// and 5 and so did nothing at all inside the range anyone actually uses. The
// geometry sliders are the ones most in need of a round number, so they get
// their own bucket at 0.05.
float UiSliderStep(float lo, float hi)
{
    float range = hi - lo;
    if (range <= 2.5f)   return 0.05f;   // blend, squareness, brightness
    if (range <= 12.0f)  return 0.05f;   // offsets, sizes, radii, heights (+-4)
    if (range <= 30.0f)  return 1.0f;    // sides: an integer count
    if (range <= 300.0f) return 5.0f;    // RGB 0..255
    return 5.0f;                         // rotation, +-180 degrees
}

bool UiSlider(Rectangle r, const char *label, float *v,
              float lo, float hi, int fs)
{
    float before = *v;
    Vector2 mouse = GetMousePosition();
    bool ctrl  = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT)   || IsKeyDown(KEY_RIGHT_SHIFT);

    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, ColorToInt(UI_COL_TEXT_DIM));
    DrawText(label, (int)r.x, (int)r.y, fs, UI_COL_TEXT_DIM);

    Rectangle bar = { r.x + 74.0f, r.y - 2.0f, r.width - 74.0f - 46.0f, r.height };

    // Ctrl/Shift + wheel steps the value without a drag at all, which is the
    // only way to nudge a slider by exactly one increment. A panel that scrolls
    // must refuse the wheel over its slider column, so the wheel reaches here
    // instead of moving the panel out from under the bar.
    float wheel = GetMouseWheelMove();
    if ((wheel != 0.0f) && UiClipAllows(mouse) && CheckCollisionPointRec(mouse, bar))
    {
        // A BARE wheel steps too, not just Ctrl/Shift. The panel behind refuses
        // the wheel anywhere in this column, so without this the wheel would
        // simply do nothing while the cursor sits on a bar - worse than either
        // behaviour on its own. Ctrl is the same step (it is the snap modifier
        // on drags, and a step already lands on the increment); Shift is the
        // fine one, a tenth of it.
        float step = UiSliderStep(lo, hi);
        if (shift) step *= 0.1f;
        float nv = *v + wheel*step;
        if (nv < lo) nv = lo;
        if (nv > hi) nv = hi;
        DrawText(TextFormat("%.2f", nv), (int)(bar.x + bar.width + 6.0f), (int)r.y,
                 fs, UI_COL_TEXT);
        if (nv != *v) { *v = nv; return true; }
        return false;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && UiClipAllows(mouse) &&
        CheckCollisionPointRec(mouse, bar))
    { ui.fineRect = bar; ui.fineActive = true; ui.fineBias = 0.0f; }

    bool fine = ui.fineActive && SameRectF(ui.fineRect, bar) &&
                IsMouseButtonDown(MOUSE_BUTTON_LEFT) && shift;

    if (fine)
    {
        // raygui keeps exclusive drag ownership of the bar, so let it run and
        // throw its value away - otherwise releasing Shift mid-drag would jump
        // the knob back to the cursor.
        float raw = *v;
        GuiSliderBar(bar, NULL, NULL, &raw, lo, hi);

        float fstep = (hi - lo)*0.0005f;
        float nv = *v + GetMouseDelta().x*fstep;
        if (nv < lo) nv = lo;
        if (nv > hi) nv = hi;
        ui.fineBias = nv - raw;     // drift between the fine value and the cursor
        *v = nv;
    }
    else
    {
        float tmp = *v;
        GuiSliderBar(bar, NULL, NULL, &tmp, lo, hi);

        // Resume from where fine mode left off rather than snapping to the
        // cursor's position on the bar.
        if ((tmp != *v) && (ui.fineBias != 0.0f))
        {
            tmp += ui.fineBias;
            if (tmp < lo) tmp = lo;
            if (tmp > hi) tmp = hi;
        }
        if (ctrl && (tmp != *v))
        {
            float step = UiSliderStep(lo, hi);
            tmp = roundf(tmp/step)*step;
            if (tmp < lo) tmp = lo;
            if (tmp > hi) tmp = hi;
        }
        *v = tmp;
    }

    DrawText(TextFormat("%.2f", *v), (int)(bar.x + bar.width + 6.0f), (int)r.y,
             fs, UI_COL_TEXT);

    return (*v != before);
}

// ---------------------------------------------------------------------------
//  Chips
// ---------------------------------------------------------------------------
int UiChips(Rectangle r, const char **labels, int count, int active,
            int fs, Color accent)
{
    if (count <= 0) return -1;
    Vector2 mp = GetMousePosition();
    float cw = (r.width - (float)(count - 1)*4.0f) / (float)count;
    int picked = -1;

    for (int i = 0; i < count; i++)
    {
        Rectangle c = { r.x + (float)i*(cw + 4.0f), r.y, cw, r.height };
        bool on = (i == active);
        bool hot = !UiModalBlocks() && UiClipAllows(mp) && CheckCollisionPointRec(mp, c);

        DrawRectangleRec(c, on ? Fade(accent, 0.22f) : (hot ? UI_COL_PANEL_HI : UI_COL_PANEL));
        DrawRectangleLinesEx(c, 1.0f, on ? accent : (hot ? UI_COL_LINE_HI : UI_COL_LINE));
        int tw = MeasureText(labels[i], fs);
        DrawText(labels[i], (int)(c.x + (cw - (float)tw)*0.5f),
                 (int)(c.y + (c.height - (float)fs)*0.5f), fs,
                 on ? UI_COL_TEXT : UI_COL_TEXT_DIM);

        if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        { AudioPlayButton(); picked = i; }
    }
    return picked;
}

// ---------------------------------------------------------------------------
//  Modal frame
// ---------------------------------------------------------------------------
Rectangle UiModalFrame(const char *title, const char *msg, float s, int fs,
                       float mw, float mh)
{
    Vector2 sc = ScreenStateSize();
    if (mw > sc.x - 40.0f) mw = sc.x - 40.0f;
    Rectangle m = { (sc.x - mw)*0.5f, (sc.y - mh)*0.5f, mw, mh };

    DrawRectangle(0, 0, (int)sc.x, (int)sc.y, (Color){ 0, 0, 0, 150 });
    DrawRectangleRec(m, UI_COL_PANEL);
    DrawRectangleLinesEx(m, 1.0f, UI_COL_LINE_HI);
    DrawText(title, (int)(m.x + 16.0f*s), (int)(m.y + 14.0f*s), fs, UI_COL_TEXT);
    if (msg && msg[0])
        DrawText(msg, (int)(m.x + 16.0f*s), (int)(m.y + 14.0f*s + (float)fs + 8.0f*s),
                 fs, UI_COL_TEXT_DIM);
    return m;
}
