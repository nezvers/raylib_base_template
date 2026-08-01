// ============================================================================
//  zen_easing.c  -  easing browser + cubic-bezier graph editor (Phase 5)
//
//  Browser: every easing (builtins + customs) with a curve thumbnail, a hide
//  checkbox (dropdown filter, persisted immediately) and, for customs, delete.
//  Editing NEVER mutates an existing easing in place - the graph editor always
//  saves as a NEW name (existing curves prefill it and draw as a ghost), so
//  keys referencing the old name keep their shape.
//
//  Deleting is the one hazard: keys keep the name, and an unknown name loads
//  as linear. The confirm scans anims/*.cfg for the name and recommends Hide.
// ============================================================================

#include "raylib.h"
#include "raygui.h"
#include "zen_internal.h"
#include "../screen_state/screen_state.h"
#include "../audio_state/audio_state.h"
#include <string.h>

#define EASE_ROW_H   30.0f

static bool  s_browserOpen = false;
static float s_scroll = 0.0f;

static bool  s_editOpen = false;
static char  s_editName[ANIM_CUSTOM_NAME_MAX];
static bool  s_editNameEdit = false;
static float s_hx1, s_hy1, s_hx2, s_hy2;    // staged handles
static int   s_ghostId = -1;                // curve being remixed (any id), -1 = fresh
static int   s_dragHandle = 0;              // 0 none, 1 P1, 2 P2
static float s_prevT = 0.0f;                // preview dot clock

static int   s_delId = -1;                  // pending delete confirm
static int   s_delUses = 0;

static const char *EasingsPath(void) { return ZenAnimPath("_easings"); }

// ---------------------------------------------------------------------------
//  Accessors for the Gui()/ESC plumbing in anim_editor_zen.c
// ---------------------------------------------------------------------------
void ZenEasingBrowserOpen(void)
{
    s_browserOpen = true;
    s_editOpen = false; s_delId = -1; s_scroll = 0.0f;
}

bool ZenEasingModalOpen(void) { return s_browserOpen || s_editOpen; }
bool ZenEasingTyping(void)    { return s_editOpen && s_editNameEdit; }

bool ZenEasingEscClose(void)
{
    if (s_delId >= 0)   { s_delId = -1;        return true; }
    if (s_editOpen)     { s_editOpen = false;  return true; }
    if (s_browserOpen)  { s_browserOpen = false; return true; }
    return false;
}

// ---------------------------------------------------------------------------
//  Curve drawing (sampled straight from AnimEaseApply / staged handles)
// ---------------------------------------------------------------------------

// y for the STAGED bezier at parameter t (drawing needs no x-solve: we plot
// the parametric point (x(t), y(t)) directly).
static Vector2 StagedPoint(float t)
{
    float u = 1.0f - t;
    float bx = 3*u*u*t*s_hx1 + 3*u*t*t*s_hx2 + t*t*t;
    float by = 3*u*u*t*s_hy1 + 3*u*t*t*s_hy2 + t*t*t;
    return (Vector2){ bx, by };
}

// map curve space (x 0..1, y overshoot range) into a screen rect (y up).
static Vector2 CurveMap(Rectangle r, float x, float y, float yMin, float yMax)
{
    return (Vector2){ r.x + x * r.width,
                      r.y + r.height - (y - yMin) / (yMax - yMin) * r.height };
}

static void DrawEaseThumb(Rectangle r, int easeId)
{
    DrawRectangleRec(r, (Color){ 26, 28, 33, 255 });
    DrawRectangleLinesEx(r, 1.0f, (Color){ 60, 64, 74, 255 });
    Vector2 prev = CurveMap(r, 0, 0, -0.25f, 1.25f);
    for (int i = 1; i <= 24; i++)
    {
        float x = (float)i / 24.0f;
        Vector2 p = CurveMap(r, x, AnimEaseApply(easeId, x), -0.25f, 1.25f);
        DrawLineV(prev, p, (Color){ 120, 190, 255, 255 });
        prev = p;
    }
}

// ---------------------------------------------------------------------------
//  Delete hazard: how many saved anims reference this easing by name?
// ---------------------------------------------------------------------------
static bool WordIn(const char *hay, const char *word)
{
    int wl = (int)strlen(word);
    for (const char *p = strstr(hay, word); p; p = strstr(p + 1, word))
    {
        char before = p == hay ? ' ' : p[-1];
        char after = p[wl];
        if ((before == ' ' || before == '\t') &&
            (after == '\0' || after == ' ' || after == '\n' || after == '\r'))
            return true;
    }
    return false;
}

static int CountEaseUses(const char *name)
{
    int uses = 0;
    for (int i = 0; i < zen.animCount; i++)
    {
        char *txt = LoadFileText(ZenAnimPath(zen.animList[i]));
        if (!txt) continue;
        if (WordIn(txt, name)) uses++;
        UnloadFileText(txt);
    }
    return uses;
}

// ---------------------------------------------------------------------------
//  Graph editor
// ---------------------------------------------------------------------------
static void EditOpen(int baseId)
{
    s_editOpen = true; s_editNameEdit = false; s_dragHandle = 0; s_prevT = 0.0f;
    s_ghostId = baseId;
    s_editName[0] = '\0';
    const AnimCustomEase *c = AnimCustomEaseGet(baseId);
    if (c) { s_hx1 = c->x1; s_hy1 = c->y1; s_hx2 = c->x2; s_hy2 = c->y2; }
    else   { s_hx1 = 0.25f; s_hy1 = 0.25f; s_hx2 = 0.75f; s_hy2 = 0.75f; }
}

static void DrawGraphEditor(void)
{
    if (!s_editOpen) return;
    ScreenState *ss = ScreenStateGet();
    float W = (float)ss->width, H = (float)ss->height;
    const float yMin = -0.5f, yMax = 1.5f;

    float mw = 340, mh = 452;
    Rectangle m = { (W-mw)/2, (H-mh)/2, mw, mh };
    DrawRectangleRec(m, (Color){ 40, 42, 48, 255 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 90, 94, 104, 255 });
    GuiLabel((Rectangle){ m.x+16, m.y+10, mw-32, 20 }, "NEW EASING");
    if (GuiButton((Rectangle){ m.x + mw - 30, m.y + 6, 24, 24 }, "x"))
    { AudioPlayButton(); s_editOpen = false; return; }

    Rectangle g = { m.x + 30, m.y + 40, mw - 60, mw - 60 };
    DrawRectangleRec(g, (Color){ 26, 28, 33, 255 });
    // the unit band: eased value 0 and 1; overshoot lives outside it.
    float y0 = CurveMap(g, 0, 0, yMin, yMax).y;
    float y1 = CurveMap(g, 0, 1, yMin, yMax).y;
    DrawLineV((Vector2){ g.x, y0 }, (Vector2){ g.x+g.width, y0 }, (Color){ 70, 74, 84, 255 });
    DrawLineV((Vector2){ g.x, y1 }, (Vector2){ g.x+g.width, y1 }, (Color){ 70, 74, 84, 255 });
    DrawRectangleLinesEx(g, 1.0f, (Color){ 60, 64, 74, 255 });

    // ghost of the curve this one remixes (builtin or custom).
    if (s_ghostId >= 0)
    {
        Vector2 prev = CurveMap(g, 0, 0, yMin, yMax);
        for (int i = 1; i <= 48; i++)
        {
            float x = (float)i / 48.0f;
            Vector2 p = CurveMap(g, x, AnimEaseApply(s_ghostId, x), yMin, yMax);
            DrawLineV(prev, p, (Color){ 120, 126, 140, 90 });
            prev = p;
        }
    }

    // staged curve + handle arms.
    Vector2 p0 = CurveMap(g, 0, 0, yMin, yMax);
    Vector2 p3 = CurveMap(g, 1, 1, yMin, yMax);
    Vector2 h1 = CurveMap(g, s_hx1, s_hy1, yMin, yMax);
    Vector2 h2 = CurveMap(g, s_hx2, s_hy2, yMin, yMax);
    Vector2 prev = p0;
    for (int i = 1; i <= 48; i++)
    {
        Vector2 c = StagedPoint((float)i / 48.0f);
        Vector2 p = CurveMap(g, c.x, c.y, yMin, yMax);
        DrawLineV(prev, p, (Color){ 120, 190, 255, 255 });
        prev = p;
    }
    DrawLineV(p0, h1, (Color){ 255, 210, 90, 140 });
    DrawLineV(p3, h2, (Color){ 255, 210, 90, 140 });
    DrawCircleV(h1, 6.0f, (Color){ 255, 210, 90, 255 });
    DrawCircleV(h2, 6.0f, (Color){ 255, 210, 90, 255 });

    // handle dragging.
    Vector2 mouse = GetMousePosition();
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        if (CheckCollisionPointCircle(mouse, h1, 12.0f))      s_dragHandle = 1;
        else if (CheckCollisionPointCircle(mouse, h2, 12.0f)) s_dragHandle = 2;
    }
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) s_dragHandle = 0;
    if (s_dragHandle)
    {
        float cx = (mouse.x - g.x) / g.width;
        float cy = yMin + (g.y + g.height - mouse.y) / g.height * (yMax - yMin);
        cx = ZenClampF(cx, 0.0f, 1.0f);
        cy = ZenClampF(cy, yMin, yMax);
        if (s_dragHandle == 1) { s_hx1 = cx; s_hy1 = cy; }
        else                   { s_hx2 = cx; s_hy2 = cy; }
    }

    // preview dot: eased y at a looping clock (pause at the end of each lap).
    s_prevT += GetFrameTime() / 1.6f;
    if (s_prevT > 1.0f) s_prevT = -0.25f;       // brief hold at the start
    if (s_prevT > 0.0f)
    {
        Vector2 c = StagedPoint(s_prevT);
        DrawCircleV(CurveMap(g, c.x, c.y, yMin, yMax), 4.0f, (Color){ 130, 220, 140, 255 });
        // and on the right edge: the value alone, like a moving element would.
        DrawCircleV((Vector2){ g.x + g.width + 12,
                               CurveMap(g, 0, c.y, yMin, yMax).y }, 5.0f,
                    (Color){ 130, 220, 140, 200 });
    }

    // name + save (always saves as a NEW easing; taken names refuse).
    float fy = g.y + g.height + 12;
    GuiLabel((Rectangle){ m.x+16, fy, 50, 24 }, "name");
    Rectangle tb = { m.x + 66, fy, mw - 82, 24 };
    if (GuiTextBox(tb, s_editName, ANIM_CUSTOM_NAME_MAX, s_editNameEdit))
        s_editNameEdit = !s_editNameEdit;
    fy += 32;

    bool taken = s_editName[0] &&
                 !TextIsEqual(AnimEaseName(AnimEaseByName(s_editName)), "linear");
    if (!s_editName[0] || taken) GuiDisable();
    if (GuiButton((Rectangle){ m.x + 16, fy, 120, 26 }, "Save as new"))
    {
        AudioPlayButton();
        if (AnimCustomEaseAdd(s_editName, s_hx1, s_hy1, s_hx2, s_hy2) >= 0)
        {
            AnimCustomEasesSave(EasingsPath());
            s_editOpen = false;
        }
    }
    if (!s_editName[0] || taken) GuiEnable();
    if (taken) GuiLabel((Rectangle){ m.x + 146, fy + 3, mw - 160, 20 }, "name taken");
    if (GuiButton((Rectangle){ m.x + mw - 96, fy, 80, 26 }, "Cancel"))
    { AudioPlayButton(); s_editOpen = false; }
}

// ---------------------------------------------------------------------------
//  Delete confirm (with the usage warning)
// ---------------------------------------------------------------------------
static void DrawDeleteConfirm(void)
{
    if (s_delId < 0) return;
    const AnimCustomEase *c = AnimCustomEaseGet(s_delId);
    if (!c) { s_delId = -1; return; }

    ScreenState *ss = ScreenStateGet();
    float W = (float)ss->width, H = (float)ss->height;
    float mw = 380, mh = 150;
    Rectangle m = { (W-mw)/2, (H-mh)/2, mw, mh };
    DrawRectangleRec(m, (Color){ 40, 42, 48, 255 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 200, 120, 90, 255 });
    GuiLabel((Rectangle){ m.x+16, m.y+10, mw-32, 20 },
             TextFormat("DELETE EASING '%s'?", c->name));
    GuiLabel((Rectangle){ m.x+16, m.y+38, mw-32, 40 },
             s_delUses > 0
             ? TextFormat("Used by %d anim(s) - their keys fall back\nto linear. Hide keeps them working.", s_delUses)
             : "Not referenced by any saved anim.");

    float by = m.y + mh - 38;
    if (GuiButton((Rectangle){ m.x + 16, by, 100, 26 }, "Hide instead"))
    {
        AudioPlayButton();
        AnimEaseSetHidden(s_delId, true);
        AnimCustomEasesSave(EasingsPath());
        s_delId = -1;
    }
    if (GuiButton((Rectangle){ m.x + 126, by, 100, 26 }, "Delete"))
    {
        AudioPlayButton();
        AnimCustomEaseRemove(s_delId);
        AnimCustomEasesSave(EasingsPath());
        s_delId = -1;
    }
    if (GuiButton((Rectangle){ m.x + mw - 96, by, 80, 26 }, "Cancel"))
    { AudioPlayButton(); s_delId = -1; }
}

// ---------------------------------------------------------------------------
//  Browser
// ---------------------------------------------------------------------------
static void DrawBrowser(void)
{
    if (!s_browserOpen) return;
    ScreenState *ss = ScreenStateGet();
    float W = (float)ss->width, H = (float)ss->height;

    float mw = 430, mh = H - 120;
    Rectangle m = { (W-mw)/2, (H-mh)/2, mw, mh };
    DrawRectangle(0, 0, (int)W, (int)H, (Color){ 0, 0, 0, 120 });
    DrawRectangleRec(m, (Color){ 40, 42, 48, 255 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 90, 94, 104, 255 });
    GuiLabel((Rectangle){ m.x+16, m.y+10, mw-32, 20 }, "EASINGS");
    if (GuiButton((Rectangle){ m.x + mw - 30, m.y + 6, 24, 24 }, "x"))
    { AudioPlayButton(); s_browserOpen = false; return; }

    Rectangle list = { m.x + 8, m.y + 40, mw - 16, mh - 86 };
    // count rows for the scroll clamp.
    int rows = 0;
    for (int i = 0; i < AnimEaseIdRange(); i++) if (AnimEaseIdValid(i)) rows++;
    float contentH = rows * EASE_ROW_H;
    if (CheckCollisionPointRec(GetMousePosition(), list))
        s_scroll -= GetMouseWheelMove() * EASE_ROW_H * 2.0f;
    float maxScroll = contentH - list.height;
    s_scroll = ZenClampF(s_scroll, 0.0f, maxScroll > 0 ? maxScroll : 0.0f);

    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    float y = list.y - s_scroll;
    for (int i = 0; i < AnimEaseIdRange(); i++)
    {
        if (!AnimEaseIdValid(i)) continue;
        if (y + EASE_ROW_H >= list.y && y <= list.y + list.height)
        {
            bool custom = i >= ANIM_EASE_COUNT;
            DrawEaseThumb((Rectangle){ list.x + 4, y + 3, 42, EASE_ROW_H - 6 }, i);
            ZenLabelTip((Rectangle){ list.x + 54, y, 130, EASE_ROW_H },
                        AnimEaseName(i), custom ? "custom easing" : "built-in easing");

            bool hid = AnimEaseIsHidden(i);
            if (i != ANIM_EASE_LINEAR &&
                GuiCheckBox((Rectangle){ list.x + 196, y + 7, 16, 16 }, "hide", &hid))
            {
                AnimEaseSetHidden(i, hid);
                AnimCustomEasesSave(EasingsPath());
            }
            if (GuiButton((Rectangle){ list.x + 268, y + 3, 60, EASE_ROW_H - 6 },
                          custom ? "Remix" : "Edit"))
            { AudioPlayButton(); EditOpen(i); }
            if (custom &&
                GuiButton((Rectangle){ list.x + 334, y + 3, 50, EASE_ROW_H - 6 }, "Del"))
            {
                AudioPlayButton();
                s_delId = i;
                s_delUses = CountEaseUses(AnimEaseName(i));
            }
        }
        y += EASE_ROW_H;
    }
    EndScissorMode();

    if (GuiButton((Rectangle){ m.x + 12, m.y + mh - 36, 100, 26 }, "New..."))
    { AudioPlayButton(); EditOpen(-1); }
    if (GuiButton((Rectangle){ m.x + mw - 92, m.y + mh - 36, 80, 26 }, "Close"))
    { AudioPlayButton(); s_browserOpen = false; }
}

void ZenEasingGui(void)
{
    // the editor + confirm float over the browser; lock it while they're up.
    bool over = s_editOpen || s_delId >= 0;
    if (over) GuiLock();
    DrawBrowser();
    if (over) GuiUnlock();
    DrawGraphEditor();
    DrawDeleteConfirm();
}
