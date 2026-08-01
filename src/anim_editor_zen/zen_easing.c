// ============================================================================
//  zen_easing.c  -  easing browser + multi-segment bezier graph editor
//
//  Browser: every easing (builtins + customs) with a curve thumbnail, a hide
//  checkbox (dropdown filter, persisted immediately) and, for customs, delete.
//
//  The graph editor stages a knot list (AnimEasePt[]): drag knots and their
//  in/out handles, double-click the curve to insert a knot, right-click one to
//  remove it. Saving as a NEW name is always available; remixing a custom can
//  also overwrite it in place (which changes the look of every key already
//  using that name - deliberate, it is the same curve).
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
#include <math.h>

#define EASE_ROW_H   30.0f

static bool  s_browserOpen = false;
static float s_scroll = 0.0f;

static bool  s_editOpen = false;
static char  s_editName[ANIM_CUSTOM_NAME_MAX];
static bool  s_editNameEdit = false;
static AnimEasePt s_pts[ANIM_EASE_PTS_MAX]; // staged knots
static int   s_ptCount = 2;
static int   s_ghostId = -1;                // curve being remixed (any id), -1 = fresh
static int   s_dragPt = -1;                 // knot index being dragged, -1 none
static int   s_dragWhat = 0;                // 0 knot, 1 in-handle, 2 out-handle
static float s_prevT = 0.0f;                // preview dot clock
static double s_lastClick = 0.0;            // double-click detection (insert)
static Vector2 s_lastClickPos;

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

// eased y of the STAGED curve at progress x (same math the runtime uses).
static float StagedY(float x) { return AnimEasePtsEval(s_pts, s_ptCount, x); }

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
    s_editOpen = true; s_editNameEdit = false; s_prevT = 0.0f;
    s_dragPt = -1; s_dragWhat = 0;
    s_ghostId = baseId;
    s_editName[0] = '\0';
    const AnimCustomEase *c = AnimCustomEaseGet(baseId);
    if (c)
    {
        for (int i = 0; i < c->ptCount; i++) s_pts[i] = c->pts[i];
        s_ptCount = c->ptCount;
        TextCopy(s_editName, c->name);      // remix starts on the same name
    }
    else
    {
        AnimEasePtsFromCubic(s_pts, 0.25f, 0.25f, 0.75f, 0.75f);
        s_ptCount = 2;
    }
}

// knot insert / remove -------------------------------------------------------
static void InsertPtAt(float x)
{
    if (s_ptCount >= ANIM_EASE_PTS_MAX) return;
    if (x <= s_pts[0].x || x >= s_pts[s_ptCount-1].x) return;
    int at = 1;
    while (at < s_ptCount && s_pts[at].x < x) at++;
    for (int i = s_ptCount; i > at; i--) s_pts[i] = s_pts[i-1];
    // sit the new knot on the curve so the shape barely changes.
    float span = s_pts[at+1].x - s_pts[at-1].x;
    float h = span > 0 ? span * 0.15f : 0.05f;
    s_pts[at] = (AnimEasePt){ x, StagedY(x), -h, 0.0f, h, 0.0f };
    s_ptCount++;
}

static void RemovePtAt(int idx)
{
    if (s_ptCount <= 2 || idx <= 0 || idx >= s_ptCount - 1) return;
    for (int i = idx; i < s_ptCount - 1; i++) s_pts[i] = s_pts[i+1];
    s_ptCount--;
}

static void DrawGraphEditor(void)
{
    if (!s_editOpen) return;
    ScreenState *ss = ScreenStateGet();
    float W = (float)ss->width, H = (float)ss->height;
    const float yMin = -0.5f, yMax = 1.5f;

    float mw = 380, mh = 528;
    Rectangle m = { (W-mw)/2, (H-mh)/2, mw, mh };
    if (m.y < 4) m.y = 4;
    DrawRectangleRec(m, (Color){ 40, 42, 48, 255 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 90, 94, 104, 255 });
    const AnimCustomEase *base = AnimCustomEaseGet(s_ghostId);
    GuiLabel((Rectangle){ m.x+16, m.y+10, mw-32, 20 },
             base ? TextFormat("EASING  %s", base->name) : "NEW EASING");
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

    // staged curve, sampled the way the runtime evaluates it.
    Vector2 prev = CurveMap(g, 0, StagedY(0.0f), yMin, yMax);
    for (int i = 1; i <= 96; i++)
    {
        float x = (float)i / 96.0f;
        Vector2 p = CurveMap(g, x, StagedY(x), yMin, yMax);
        DrawLineV(prev, p, (Color){ 120, 190, 255, 255 });
        prev = p;
    }

    // knots + their handle arms.
    for (int i = 0; i < s_ptCount; i++)
    {
        Vector2 k = CurveMap(g, s_pts[i].x, s_pts[i].y, yMin, yMax);
        if (i > 0)
        {
            Vector2 hi = CurveMap(g, s_pts[i].x + s_pts[i].ix,
                                  s_pts[i].y + s_pts[i].iy, yMin, yMax);
            DrawLineV(k, hi, (Color){ 255, 210, 90, 140 });
            DrawCircleV(hi, 5.0f, (Color){ 255, 210, 90, 255 });
        }
        if (i < s_ptCount - 1)
        {
            Vector2 ho = CurveMap(g, s_pts[i].x + s_pts[i].ox,
                                  s_pts[i].y + s_pts[i].oy, yMin, yMax);
            DrawLineV(k, ho, (Color){ 255, 210, 90, 140 });
            DrawCircleV(ho, 5.0f, (Color){ 255, 210, 90, 255 });
        }
        bool ends = (i == 0 || i == s_ptCount - 1);
        DrawCircleV(k, 6.0f, ends ? (Color){ 150, 156, 170, 255 }
                                  : (Color){ 130, 220, 140, 255 });
    }

    // --- interaction ---------------------------------------------------------
    Vector2 mouse = GetMousePosition();
    bool inGraph = CheckCollisionPointRec(mouse, g);
    bool alt  = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && inGraph)
    {
        s_dragPt = -1;
        for (int i = 0; i < s_ptCount && s_dragPt < 0; i++)
        {
            Vector2 k = CurveMap(g, s_pts[i].x, s_pts[i].y, yMin, yMax);
            if (i > 0)
            {
                Vector2 hi = CurveMap(g, s_pts[i].x + s_pts[i].ix,
                                      s_pts[i].y + s_pts[i].iy, yMin, yMax);
                if (CheckCollisionPointCircle(mouse, hi, 11.0f))
                { s_dragPt = i; s_dragWhat = 1; break; }
            }
            if (i < s_ptCount - 1)
            {
                Vector2 ho = CurveMap(g, s_pts[i].x + s_pts[i].ox,
                                      s_pts[i].y + s_pts[i].oy, yMin, yMax);
                if (CheckCollisionPointCircle(mouse, ho, 11.0f))
                { s_dragPt = i; s_dragWhat = 2; break; }
            }
            if (CheckCollisionPointCircle(mouse, k, 11.0f))
            { s_dragPt = i; s_dragWhat = 0; }
        }

        // nothing grabbed + a quick second click on the curve inserts a knot.
        double now = GetTime();
        if (s_dragPt < 0 && now - s_lastClick < 0.35 &&
            CheckCollisionPointCircle(mouse, s_lastClickPos, 12.0f))
            InsertPtAt(ZenClampF((mouse.x - g.x) / g.width, 0.0f, 1.0f));
        s_lastClick = now; s_lastClickPos = mouse;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && inGraph)
    {
        for (int i = 1; i < s_ptCount - 1; i++)
        {
            Vector2 k = CurveMap(g, s_pts[i].x, s_pts[i].y, yMin, yMax);
            if (CheckCollisionPointCircle(mouse, k, 11.0f)) { RemovePtAt(i); break; }
        }
    }
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) s_dragPt = -1;

    if (s_dragPt >= 0)
    {
        float cx = ZenClampF((mouse.x - g.x) / g.width, 0.0f, 1.0f);
        float cy = ZenClampF(yMin + (g.y + g.height - mouse.y) / g.height * (yMax - yMin),
                             yMin, yMax);
        if (ctrl) { cx = roundf(cx / 0.05f) * 0.05f; cy = roundf(cy / 0.05f) * 0.05f; }
        AnimEasePt *p = &s_pts[s_dragPt];
        if (s_dragWhat == 0)
        {
            // endpoints keep their x; inner knots stay between their neighbours.
            if (s_dragPt > 0 && s_dragPt < s_ptCount - 1)
                p->x = ZenClampF(cx, s_pts[s_dragPt-1].x, s_pts[s_dragPt+1].x);
            p->y = cy;
        }
        else
        {
            float hx = cx - p->x, hy = cy - p->y;
            if (s_dragWhat == 1) { p->ix = hx; p->iy = hy; }
            else                 { p->ox = hx; p->oy = hy; }
            // handles mirror for a smooth knot unless Alt breaks the tangent.
            if (!alt && s_dragPt > 0 && s_dragPt < s_ptCount - 1)
            {
                if (s_dragWhat == 1) { p->ox = -hx; p->oy = -hy; }
                else                 { p->ix = -hx; p->iy = -hy; }
            }
        }
    }

    // preview dot: eased y at a looping clock (pause at the end of each lap).
    s_prevT += GetFrameTime() / 1.6f;
    if (s_prevT > 1.0f) s_prevT = -0.25f;       // brief hold at the start
    if (s_prevT > 0.0f)
    {
        float cy = StagedY(s_prevT);
        DrawCircleV(CurveMap(g, s_prevT, cy, yMin, yMax), 4.0f,
                    (Color){ 130, 220, 140, 255 });
        // and on the right edge: the value alone, like a moving element would.
        DrawCircleV((Vector2){ g.x + g.width + 12,
                               CurveMap(g, 0, cy, yMin, yMax).y }, 5.0f,
                    (Color){ 130, 220, 140, 200 });
    }

    float fy = g.y + g.height + 8;
    GuiLabel((Rectangle){ m.x+16, fy, mw-32, 18 },
             TextFormat("%d knots - drag to shape, double-click adds, right-click removes",
                        s_ptCount));
    ZenTip((Rectangle){ m.x+16, fy, mw-32, 18 },
           "Alt+drag a handle breaks the tangent, Ctrl snaps to a 0.05 grid. "
           "Two knots at the same x make a hold step.");
    fy += 22;

    GuiLabel((Rectangle){ m.x+16, fy, 50, 24 }, "name");
    Rectangle tb = { m.x + 66, fy, mw - 82, 24 };
    if (GuiTextBox(tb, s_editName, ANIM_CUSTOM_NAME_MAX, s_editNameEdit))
        s_editNameEdit = !s_editNameEdit;
    fy += 32;

    // "taken" only blocks Save as new; overwriting the remixed curve is the
    // other button, and its own name is expected to be taken.
    int nameId = s_editName[0] ? AnimEaseByName(s_editName) : ANIM_EASE_LINEAR;
    bool taken = s_editName[0] &&
                 !TextIsEqual(AnimEaseName(nameId), "linear");
    bool canNew = s_editName[0] && !taken;
    if (!canNew) GuiDisable();
    if (GuiButton((Rectangle){ m.x + 16, fy, 110, 26 }, "Save as new"))
    {
        AudioPlayButton();
        if (AnimCustomEaseAdd(s_editName, s_pts, s_ptCount) >= 0)
        {
            AnimCustomEasesSave(EasingsPath());
            s_editOpen = false;
        }
    }
    if (!canNew) GuiEnable();

    // overwrite is only offered when remixing an existing custom.
    bool canSave = base != NULL;
    if (!canSave) GuiDisable();
    if (GuiButton((Rectangle){ m.x + 134, fy, 100, 26 }, "Overwrite"))
    {
        AudioPlayButton();
        if (AnimCustomEaseUpdate(s_ghostId, s_pts, s_ptCount))
        {
            AnimCustomEasesSave(EasingsPath());
            s_editOpen = false;
        }
    }
    if (!canSave) GuiEnable();
    ZenTip((Rectangle){ m.x + 134, fy, 100, 26 },
           canSave ? "Replaces this easing everywhere it is already used"
                   : "Only a saved custom easing can be overwritten");
    if (taken)
        GuiLabel((Rectangle){ m.x + 16, fy + 28, mw - 32, 18 },
                 base && TextIsEqual(s_editName, base->name)
                 ? "same name: use Overwrite, or type a new one to branch"
                 : "name taken");

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
