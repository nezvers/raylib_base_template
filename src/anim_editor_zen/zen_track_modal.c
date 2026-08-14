// ============================================================================
//  zen_track_modal.c  -  the ONE draggable track/key modal
//
//  Content follows the ctx track/key selection (zen.selElem, zen.selGroup,
//  zen.selKeys[]) and is opened from the inspector, the timeline or the
//  signal modal. The background stays interactive: the modal claims the
//  mouse only inside its own rect (ZenPanelsGui locks the panels while the
//  cursor is over it), never with a full-screen GuiLock.
//
//  Editing semantics:
//    exactly ONE key selected  -> live editing (time / member values / ease)
//    several keys or none      -> STAGED fields with per-field dirty flags;
//                                 Apply writes ONLY the touched fields to
//                                 every selected key (none = every key of
//                                 the track), one undo snapshot for the lot.
// ============================================================================

#include "raylib.h"
#include "raygui.h"
#include "zen_internal.h"
#include "../screen_state/screen_state.h"
#include "../audio_state/audio_state.h"
#include "../shape_editor/shape_editor.h"
#include <math.h>
#include <stdlib.h>

// Width is set by the WIDEST thing the modal has to say: a crumble member row
// is "crumble_spread" + a usable slider + the value readout, and at 300px the
// name column clipped to "crumbl..." while the slider had no room to aim with.
#define TM_W        420.0f
#define TM_LBL      104.0f      // property-name column, left of every slider
#define TM_VALW      50.0f      // ZenEditSlider's readout, right of every slider
#define TM_TITLE_H   24.0f
#define TM_RH        24.0f
#define TM_GAP        6.0f
#define TM_TREE_RH   18.0f      // one key row in the tree pane
#define TM_TREE_ROWS 5          // visible key rows before the pane scrolls

// The tree pane remembers the scope it was opened with, so focusing a single
// key from it (which shrinks the selection to one) can be undone by clicking
// the root row again.
static bool  s_treeCollapsed = false;
static float s_treeScroll    = 0.0f;
static float s_scopeTimes[ZEN_GROUP_TIMES_MAX];
static int   s_scopeCount    = 0;
static int   s_scopeElem     = -1;
static int   s_scopeGroup    = -1;

// The selected element/group, or NULL when the selection is stale.
static AnimElem *ModalElem(void)
{
    if (zen.selElem < 0 || zen.selElem >= zen.doc.elemCount) return NULL;
    return &zen.doc.elems[zen.selElem];
}

// Bulk targets: the selected key times, or every key time of the track when
// none are individually selected. Returns the count written to `out`.
static int BulkTimes(AnimElem *e, float *out)
{
    if (zen.selKeyCount > 0)
    {
        for (int i = 0; i < zen.selKeyCount; i++) out[i] = zen.selKeys[i];
        return zen.selKeyCount;
    }
    return ZenGroupKeyTimes(e, zen.selGroup, out);
}

// Remember the scope the tree was built from. Focusing one key out of a bulk
// selection must not overwrite it (that's what the root row restores), so a
// lone selected key never counts as a new scope.
static void ScopeCapture(void)
{
    if (zen.selElem == s_scopeElem && zen.selGroup == s_scopeGroup &&
        zen.selKeyCount == 1) return;
    s_scopeElem = zen.selElem;
    s_scopeGroup = zen.selGroup;
    s_scopeCount = zen.selKeyCount;
    for (int i = 0; i < zen.selKeyCount && i < ZEN_GROUP_TIMES_MAX; i++)
        s_scopeTimes[i] = zen.selKeys[i];
    s_treeScroll = 0.0f;
}

// Reload the staged values from the selection (first selected key, or the
// track's first key) and clear every dirty flag. Called on selection change
// so the modal always mirrors what is picked - like the timeline does.
void ZenTrackModalSync(void)
{
    ZenTrackModal *tm = &zen.trackModal;
    if (tm->sig >= 0) return;               // signal mode edits live, no staging
    AnimElem *e = ModalElem();
    if (!e) { tm->open = false; return; }
    // No group is the empty state, not a reason to close: there is simply
    // nothing to stage. Still rescope, or the tree keeps the old element's rows.
    if (zen.selGroup < 0) { ScopeCapture(); return; }

    float times[ZEN_GROUP_TIMES_MAX];
    int nt = BulkTimes(e, times);
    float t = nt > 0 ? times[0] : 0.0f;

    const AnimPropGroup *g = AnimGroupAt(e->kind, zen.selGroup);
    for (int m = 0; g && m < g->propCount && m < 8; m++)
    {
        tm->vals[m] = AnimElemProp(e, g->props[m], t);
        tm->dVals[m] = false;
    }
    Color c = {0};
    int cp = ZenGroupColorProp(e->kind, zen.selGroup);
    if (cp >= 0) c = AnimElemColorProp(e, cp, t);
    tm->cval = c; tm->dCval = false;
    tm->ease = ZenGroupEase(e, zen.selGroup, t); tm->dEase = false;
    TextCopy(tm->timeBuf, TextFormat("%.2f", t));
    tm->edTime = false;
    zen.easeDropOpen = false;
    ScopeCapture();
}

static void ModalShow(ZenTrackModal *tm)
{
    if (!tm->open)
    {
        tm->open = true;
        if (tm->pos.x == 0 && tm->pos.y == 0)       // first ever open: centerish
        {
            Vector2 s = ScreenStateSize();
            tm->pos = (Vector2){ s.x * 0.5f - TM_W * 0.5f, s.y * 0.25f };
        }
    }
}

void ZenTrackModalOpen(int elem, int gi)
{
    ZenTrackModal *tm = &zen.trackModal;
    tm->sig = -1;
    zen.selElem = elem;
    if (gi != zen.selGroup) { zen.selGroup = gi; zen.selKeyCount = 0; }
    ModalShow(tm);
    ZenTrackModalSync();
    ScopeCapture();
}

// Signal mode: the modal edits the signal group key picked in zen.sigSel*.
void ZenTrackModalOpenSig(int sigIdx)
{
    ZenTrackModal *tm = &zen.trackModal;
    tm->sig = sigIdx;
    ModalShow(tm);
    zen.easeDropOpen = false;
}

// Write the staged (dirty) fields to every bulk target time. One undo.
static void BulkApply(AnimElem *e)
{
    ZenTrackModal *tm = &zen.trackModal;
    const AnimPropGroup *g = AnimGroupAt(e->kind, zen.selGroup);
    if (!g) return;

    bool any = tm->dCval || tm->dEase;
    for (int m = 0; m < g->propCount && m < 8; m++) any = any || tm->dVals[m];
    if (!any) return;

    ZenUndoPush();
    float times[ZEN_GROUP_TIMES_MAX];
    int nt = BulkTimes(e, times);
    for (int i = 0; i < nt; i++)
    {
        float t = times[i];
        for (int m = 0; m < g->propCount && m < 8; m++)
        {
            int prop = g->props[m];
            AnimTrack *tr = AnimElemFindTrack(e, prop);
            if (!tr) continue;
            int k = -1;
            for (int j = 0; j < tr->keyCount; j++)
                if (fabsf(tr->keys[j].t - t) <= ZEN_AUTOKEY_EPS) { k = j; break; }
            if (k < 0) continue;                    // ragged member: no key here
            if (AnimPropIsColor(prop))
            {
                if (tm->dCval)
                    tr->keys[k].cval = (Color){ tm->cval.r, tm->cval.g, tm->cval.b, 255 };
            }
            else if (tm->dVals[m]) tr->keys[k].value = tm->vals[m];
        }
        if (tm->dEase) ZenGroupSetEaseAt(e, zen.selGroup, t, tm->ease);
    }
    ZenTrackModalSync();                            // re-baseline, clear dirty
}

// A filled scope badge - the modal's loudest signal of what an edit will hit.
static void DrawBadge(Rectangle r, const char *text, Color c)
{
    DrawRectangleRec(r, c);
    DrawRectangleLinesEx(r, 1.0f, Fade(WHITE, 0.25f));
    int fs = GuiGetStyle(DEFAULT, TEXT_SIZE);
    int tw = MeasureText(text, fs);
    DrawText(text, (int)(r.x + (r.width - tw)*0.5f),
             (int)(r.y + (r.height - fs)*0.5f), fs, (Color){ 20, 21, 25, 255 });
}

// One-line summary of a key: the first three scalar members at that time.
static const char *KeySummary(AnimElem *e, const AnimPropGroup *g, float t)
{
    const char *out = "";
    int shown = 0;
    for (int m = 0; g && m < g->propCount && shown < 3; m++)
    {
        if (AnimPropIsColor(g->props[m])) continue;
        out = TextFormat("%s%s%s %.0f", out, shown ? "  " : "",
                         AnimPropName(g->props[m]),
                         AnimElemProp(e, g->props[m], t));
        shown++;
    }
    return out;
}

// The tree pane: a root row naming the scope, then one row per key it covers.
// Clicking a key row focuses it (single-key live editing); clicking the root
// restores the scope the tree was built from. Returns the height it drew.
static float DrawKeyTree(Rectangle m, float y, AnimElem *e,
                         const AnimPropGroup *g, float *times, int nt)
{
    bool single = zen.selKeyCount == 1;
    bool track  = zen.selKeyCount == 0;
    float x = m.x + 10, w = m.width - 20;

    // ---- root row -----------------------------------------------------------
    Rectangle root = { x, y, w, TM_TREE_RH + 4 };
    bool rootHot = CheckCollisionPointRec(GetMousePosition(), root);
    DrawRectangleRec(root, rootHot ? (Color){ 70, 76, 90, 255 }
                                   : (Color){ 58, 62, 74, 255 });

    const char *scope = single ? TextFormat("key @ %.2fs", zen.selKeys[0])
                      : track  ? TextFormat("whole track (%d keys)", nt)
                               : TextFormat("%d keys selected", zen.selKeyCount);
    GuiLabel((Rectangle){ root.x + 16, root.y + 2, w - 90, TM_TREE_RH },
             TextFormat("%s . %s", g ? g->name : "?", scope));
    DrawText(s_treeCollapsed ? ">" : "v", (int)root.x + 5, (int)root.y + 5, 10,
             (Color){ 200, 205, 215, 255 });

    Rectangle badge = { root.x + w - 62, root.y + 3, 58, TM_TREE_RH - 2 };
    DrawBadge(badge, single ? "KEY" : track ? "TRACK" : "BULK",
              single ? (Color){ 120, 175, 255, 255 } : (Color){ 255, 200, 80, 255 });

    ZenTip(root, single
           ? "Editing one key - changes apply immediately. Click a row to "
             "switch keys, or this row to go back to the whole selection."
           : track
           ? "No key picked: edits are staged and Apply writes them to every "
             "key of this track."
           : "Several keys picked: edits are staged and Apply writes only the "
             "changed fields to all of them.");

    // This runs inside the floating modal, so guiLocked (the BASE layer's lock,
    // raised whenever the cursor is over a floater) is the wrong question: ask
    // whether this layer owns the gesture.
    if (rootHot && ZenLayerActive(ZEN_LAYER_FLOAT_TRACK) &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        ZenMouseReflow();       // collapsing or rescoping moves every row below
        AudioPlayButton();
        if (single && s_scopeElem == zen.selElem && s_scopeGroup == zen.selGroup)
        {
            // back to the scope this tree was opened with.
            zen.selKeyCount = s_scopeCount;
            for (int i = 0; i < s_scopeCount; i++) zen.selKeys[i] = s_scopeTimes[i];
            ZenTrackModalSync();
        }
        else s_treeCollapsed = !s_treeCollapsed;
    }
    y += TM_TREE_RH + 6;
    if (s_treeCollapsed) return TM_TREE_RH + 6;

    // ---- key rows -----------------------------------------------------------
    int rows = nt < TM_TREE_ROWS ? nt : TM_TREE_ROWS;
    Rectangle list = { x, y, w, rows * TM_TREE_RH };
    if (CheckCollisionPointRec(GetMousePosition(), list))
        s_treeScroll -= GetMouseWheelMove() * TM_TREE_RH;
    float maxScroll = nt * TM_TREE_RH - list.height;
    s_treeScroll = ZenClampF(s_treeScroll, 0.0f, maxScroll > 0 ? maxScroll : 0.0f);

    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    float pick = -1.0f; bool pickAdd = false;

    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    float ry = list.y - s_treeScroll;
    for (int i = 0; i < nt; i++)
    {
        Rectangle r = { list.x, ry, list.width, TM_TREE_RH };
        if (ry + TM_TREE_RH >= list.y && ry <= list.y + list.height)
        {
            bool cur = single && fabsf(zen.selKeys[0] - times[i]) <= ZEN_AUTOKEY_EPS;
            bool hot = CheckCollisionPointRec(GetMousePosition(), r) &&
                       CheckCollisionPointRec(GetMousePosition(), list);
            if (cur) DrawRectangleRec(r, (Color){ 90, 140, 220, 70 });
            else if (hot) DrawRectangleRec(r, (Color){ 255, 255, 255, 18 });
            DrawText(cur ? ">" : " ", (int)r.x + 8, (int)r.y + 4, 10,
                     (Color){ 200, 205, 215, 255 });
            GuiLabel((Rectangle){ r.x + 20, r.y, 96, TM_TREE_RH },
                     TextFormat("key @ %.2fs", times[i]));
            GuiLabel((Rectangle){ r.x + 118, r.y, r.width - 122, TM_TREE_RH },
                     KeySummary(e, g, times[i]));
            if (hot && ZenLayerActive(ZEN_LAYER_FLOAT_TRACK) &&
                IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            { pick = times[i]; pickAdd = ctrl; }
        }
        ry += TM_TREE_RH;
    }
    EndScissorMode();

    if (pick >= 0.0f)
    {
        // Selecting re-lays out the modal (single-key mode inserts a time row)
        // while the button is still down, sliding the sliders below the tree
        // under the cursor. Poison the gesture so none of them take it.
        ZenMouseReflow();
        AudioPlayButton();
        if (pickAdd) ZenSelKey(zen.selElem, zen.selGroup, pick, true);
        else
        {
            zen.selKeys[0] = pick;
            zen.selKeyCount = 1;
            ZenTrackModalSync();
        }
    }
    return TM_TREE_RH + 6 + rows * TM_TREE_RH + 4;
}

// Chrome + title-bar drag shared by both modes. Returns the modal rect.
static Rectangle ModalChrome(ZenTrackModal *tm, float bodyH, const char *title)
{
    Rectangle m = { tm->pos.x, tm->pos.y, TM_W, bodyH };
    Vector2 screen = ScreenStateSize();
    if (m.x < 0) m.x = 0; if (m.y < 0) m.y = 0;
    if (m.x + m.width  > screen.x) m.x = screen.x - m.width;
    if (m.y + m.height > screen.y) m.y = screen.y - m.height;
    tm->pos = (Vector2){ m.x, m.y };
    tm->rect = m;

    Rectangle titleR = { m.x, m.y, m.width - 24, TM_TITLE_H };
    ZenModalDrag(titleR, &tm->pos, (Vector2){ m.x, m.y },
                 &tm->dragging, &tm->dragOff, ZEN_LAYER_FLOAT_TRACK);

    DrawRectangleRec(m, (Color){ 40, 42, 48, 252 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 110, 114, 126, 255 });
    DrawRectangleRec(titleR, (Color){ 52, 56, 66, 255 });
    ZenLabelTip((Rectangle){ m.x + 8, m.y + 3, titleR.width - 8, 18 }, title,
                "Drag this bar to move the modal");
    if (GuiButton((Rectangle){ m.x + m.width - 22, m.y + 2, 20, 20 }, "x"))
    { AudioPlayButton(); tm->open = false; }
    return m;
}

// Signal mode: live editor for the group key (zen.sigSelElem, zen.sigSelGroup,
// zen.sigSelU) of signal tm->sig.
static void SigModeGui(ZenTrackModal *tm)
{
    if (tm->sig >= zen.doc.signalCount ||
        zen.sigSelElem < 0 || zen.sigSelElem >= zen.doc.elemCount ||
        zen.sigSelGroup < 0)
    { tm->open = false; tm->rect = (Rectangle){0}; return; }

    AnimSignal *sg = &zen.doc.signals[tm->sig];
    AnimElem *el = &zen.doc.elems[zen.sigSelElem];
    const AnimPropGroup *g = AnimGroupAt(el->kind, zen.sigSelGroup);
    if (!g || !ZenSigGroupHasTarget(sg, zen.sigSelElem, zen.sigSelGroup))
    { tm->open = false; tm->rect = (Rectangle){0}; return; }

    int scalarRows = 0; bool hasColor = false;
    for (int m = 0; m < g->propCount; m++)
    {
        if (AnimPropIsColor(g->props[m])) hasColor = true;
        else scalarRows++;
    }
    float bodyH = TM_TITLE_H + 22
                + TM_RH + TM_GAP                    // u
                + scalarRows * (TM_RH + TM_GAP)
                + (hasColor ? TM_RH + 3*18 + TM_GAP : 0)
                + TM_RH + TM_GAP                    // ease
                + TM_RH + 10;                       // delete
    Rectangle m = ModalChrome(tm, bodyH,
        TextFormat("SIGNAL KEY  %s . %s . %s", sg->name, el->name, g->name));
    if (!tm->open) return;

    float x = m.x + 10, w = m.width - 20 - TM_VALW;
    float y = m.y + TM_TITLE_H + 2;
    Rectangle banner = { x, y, m.width - 20, 18 };
    DrawRectangleRec(banner, (Color){ 58, 62, 74, 255 });
    GuiLabel((Rectangle){ banner.x + 6, banner.y, banner.width - 74, 18 },
             "live edit - no Apply needed");
    DrawBadge((Rectangle){ banner.x + banner.width - 62, banner.y + 1, 58, 16 },
              "SIGNAL", (Color){ 150, 220, 160, 255 });
    ZenTip(banner, "This is a signal's own key (position in u, not seconds); "
                   "every change lands on it immediately.");
    y += 22;

    // u: moves the whole group key.
    GuiLabel((Rectangle){ x, y, TM_LBL, TM_RH }, "u");
    float u = zen.sigSelU;
    if (ZenEditSlider((Rectangle){ x + TM_LBL, y, w - TM_LBL, TM_RH }, "", &u, 0.0f, 1.0f))
    {
        ZenSigGroupMoveKeyTo(sg, zen.sigSelElem, zen.sigSelGroup, zen.sigSelU, u);
        zen.sigSelU = u; zen.sigLastU = u;
    }
    y += TM_RH + TM_GAP;

    // member values.
    for (int mi = 0; mi < g->propCount; mi++)
    {
        int prop = g->props[mi];
        AnimSigTarget *tg = ZenSigFindTarget(sg, zen.sigSelElem, prop);
        int k = ZenSigTargetKeyNear(tg, zen.sigSelU);
        if (AnimPropIsColor(prop))
        {
            Color cc = k >= 0 ? tg->keys[k].cval : (Color){0,0,0,255};
            GuiLabel((Rectangle){ x, y, m.width - 44, TM_RH },
                     TextFormat("%s (rgb)", AnimPropName(prop)));
            ZenDrawSwatch((Rectangle){ m.x + m.width - 30, y + 3, 18, 18 },
                          (Color){ cc.r, cc.g, cc.b, 255 });
            y += TM_RH;
            float cr = cc.r, cg = cc.g, cb = cc.b;
            bool ch = false;
            if (ZenEditSlider((Rectangle){ x+16, y, w-16, 16 }, "R", &cr, 0,255)) ch=true; y+=18;
            if (ZenEditSlider((Rectangle){ x+16, y, w-16, 16 }, "G", &cg, 0,255)) ch=true; y+=18;
            if (ZenEditSlider((Rectangle){ x+16, y, w-16, 16 }, "B", &cb, 0,255)) ch=true; y+=18;
            if (ch && k >= 0)
                tg->keys[k].cval = (Color){ (unsigned char)cr, (unsigned char)cg,
                                            (unsigned char)cb, 255 };
            y += TM_GAP;
        }
        else
        {
            ZenLabelTip((Rectangle){ x, y, TM_LBL, TM_RH }, AnimPropName(prop),
                        ZenPropDesc(prop));
            if (k >= 0)
            {
                float v = tg->keys[k].value, lo, hi;
                ZenPropRange(el, prop, &lo, &hi);
                if (ZenEditSlider((Rectangle){ x + TM_LBL, y, w - TM_LBL, TM_RH }, "", &v, lo, hi))
                    tg->keys[k].value = v;
            }
            else GuiLabel((Rectangle){ x + TM_LBL, y, w - TM_LBL, TM_RH }, "(no key)");
            y += TM_RH + TM_GAP;
        }
    }

    // ease (header; the shared overlay applies it in signal mode).
    GuiLabel((Rectangle){ x, y, TM_LBL, TM_RH }, "ease");
    zen.easeDropRect = (Rectangle){ x + TM_LBL, y, w - TM_LBL, TM_RH };
    int ease = ZenSigGroupEase(sg, zen.sigSelElem, zen.sigSelGroup, zen.sigSelU);
    if (GuiButton(zen.easeDropRect, TextFormat("%s  v", AnimEaseName(ease))))
    { AudioPlayButton(); zen.easeDropOpen = !zen.easeDropOpen; }
    y += TM_RH + TM_GAP;

    if (ZenButton((Rectangle){ x, y, (m.width - 30) / 2.0f, TM_RH }, "delete key"))
    {
        AudioPlayButton(); ZenUndoPush();
        ZenSigGroupDeleteKeyAt(sg, zen.sigSelElem, zen.sigSelGroup, zen.sigSelU);
        ZenSigClearKeySel();
        tm->open = false;
    }
}

// Overwrite the group's key at `dst` with the stored values of the key at
// `src` (values, colour and ease), so +key duplicates a selected key instead
// of resampling the curve at the playhead. Both keys must already exist.
static void CopyKeyValues(AnimElem *e, int gi, float src, float dst)
{
    const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
    for (int m = 0; g && m < g->propCount; m++)
    {
        AnimTrack *tr = AnimElemFindTrack(e, g->props[m]);
        if (!tr) continue;
        int ks = -1, kd = -1;
        for (int j = 0; j < tr->keyCount; j++)
        {
            if (fabsf(tr->keys[j].t - src) <= ZEN_AUTOKEY_EPS) ks = j;
            if (fabsf(tr->keys[j].t - dst) <= ZEN_AUTOKEY_EPS) kd = j;
        }
        if (ks < 0 || kd < 0 || ks == kd) continue;
        tr->keys[kd].value = tr->keys[ks].value;
        tr->keys[kd].cval  = tr->keys[ks].cval;
        tr->keys[kd].ease  = tr->keys[ks].ease;
    }
}

// The element has nothing to edit. Rather than closing - which is what made the
// modal feel like it kept vanishing while clicking through elements - it stays
// put and offers the one thing that fixes the situation. Adding a SPECIFIC
// group stays the inspector's job: its +track dropdown is single-instance
// (zen.addTrackOpen / addTrackRect, drawn in the panels overlay pass), so a
// second copy driven from here would fight it. Group 0 covers the common case.
static void EmptyGui(ZenTrackModal *tm, AnimElem *e)
{
    float bodyH = TM_TITLE_H + TM_RH*2 + TM_GAP + 10;
    Rectangle m = { tm->pos.x, tm->pos.y, TM_W, bodyH };

    Vector2 screen = ScreenStateSize();
    if (m.x < 0) m.x = 0; if (m.y < 0) m.y = 0;
    if (m.x + m.width  > screen.x) m.x = screen.x - m.width;
    if (m.y + m.height > screen.y) m.y = screen.y - m.height;
    tm->pos = (Vector2){ m.x, m.y };
    tm->rect = m;

    Rectangle title = { m.x, m.y, m.width - 24, TM_TITLE_H };
    ZenModalDrag(title, &tm->pos, (Vector2){ m.x, m.y },
                 &tm->dragging, &tm->dragOff, ZEN_LAYER_FLOAT_TRACK);

    DrawRectangleRec(m, (Color){ 40, 42, 48, 252 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 110, 114, 126, 255 });
    DrawRectangleRec(title, (Color){ 52, 56, 66, 255 });
    ZenLabelTip((Rectangle){ m.x + 8, m.y + 3, title.width - 8, 18 },
                TextFormat("TRACK  %s . (none)", e->name),
                "Drag this bar to move the modal");
    if (GuiButton((Rectangle){ m.x + m.width - 22, m.y + 2, 20, 20 }, "x"))
    { AudioPlayButton(); tm->open = false; return; }

    float x = m.x + 10, w = m.width - 20;
    float y = m.y + TM_TITLE_H + 2;
    GuiLabel((Rectangle){ x, y, w, TM_RH }, "no tracks on this element");
    y += TM_RH + TM_GAP;

    const AnimPropGroup *g0 = AnimGroupAt(e->kind, 0);
    if (GuiButton((Rectangle){ x, y, w, TM_RH },
                  TextFormat("+ track  %s  @ %.2fs", g0 ? g0->name : "?", zen.playhead)))
    {
        AudioPlayButton(); ZenUndoPush();
        ZenGroupWriteKey(e, 0, zen.playhead);
        ZenSelKey(zen.selElem, 0, zen.playhead, false);
        ZenTrackModalOpen(zen.selElem, 0);
    }
}

// Open is not the same as on screen: playback is for watching, so the modal
// HIDES rather than closes and is back - same element, same key - the moment
// the clock stops. The ESC chain asks too, so a hidden modal does not eat the
// press. Matches the `playbackUi` predicate the panels slide on.
bool ZenTrackModalVisible(void)
{
    return zen.trackModal.open && !zen.playing && !zen.playPending
        && AnimSignalPlayerDone(&zen.preview);
}

void ZenTrackModalGui(void)
{
    ZenTrackModal *tm = &zen.trackModal;
    // Clearing the rect matters as much as skipping the draw: ZenLayerAt() hit
    // tests it, and a stale one would keep claiming clicks over the viewport.
    if (!ZenTrackModalVisible()) { tm->rect = (Rectangle){0}; return; }
    if (tm->sig >= 0) { SigModeGui(tm); return; }
    AnimElem *e = ModalElem();
    if (!e) { tm->open = false; tm->rect = (Rectangle){0}; return; }
    if (zen.selGroup < 0 || !ZenGroupHasTrack(e, zen.selGroup))
    { EmptyGui(tm, e); return; }

    const AnimPropGroup *g = AnimGroupAt(e->kind, zen.selGroup);
    int cp = ZenGroupColorProp(e->kind, zen.selGroup);
    bool single = zen.selKeyCount == 1;
    bool track  = zen.selKeyCount == 0;

    // the tree lists what the edits will hit, so its height comes first.
    float treeTimes[ZEN_GROUP_TIMES_MAX];
    int treeCount = BulkTimes(e, treeTimes);
    int treeRows = treeCount < TM_TREE_ROWS ? treeCount : TM_TREE_ROWS;
    float treeH = s_treeCollapsed ? TM_TREE_RH + 6
                                  : TM_TREE_RH + 6 + treeRows * TM_TREE_RH + 4;

    // height: title + tree + time row (single) + member rows + ease + buttons
    int scalarRows = 0;
    for (int m = 0; g && m < g->propCount; m++)
        if (!AnimPropIsColor(g->props[m])) scalarRows++;
    float bodyH = TM_TITLE_H + treeH + (single ? TM_RH + TM_GAP : 0)
                + scalarRows * (TM_RH + TM_GAP)
                + (cp >= 0 ? TM_RH + 3*18 + TM_GAP : 0)
                + TM_RH + TM_GAP        // ease
                + TM_RH + TM_GAP        // delete / Apply row
                + TM_RH + 10;           // +key / reset row
    Rectangle m = { tm->pos.x, tm->pos.y, TM_W, bodyH };

    // clamp on screen (also after window resizes).
    Vector2 screen = ScreenStateSize();
    if (m.x < 0) m.x = 0; if (m.y < 0) m.y = 0;
    if (m.x + m.width  > screen.x) m.x = screen.x - m.width;
    if (m.y + m.height > screen.y) m.y = screen.y - m.height;
    tm->pos = (Vector2){ m.x, m.y };
    tm->rect = m;

    // --- title bar drag ------------------------------------------------------
    Rectangle title = { m.x, m.y, m.width - 24, TM_TITLE_H };
    ZenModalDrag(title, &tm->pos, (Vector2){ m.x, m.y },
                 &tm->dragging, &tm->dragOff, ZEN_LAYER_FLOAT_TRACK);

    // --- chrome --------------------------------------------------------------
    DrawRectangleRec(m, (Color){ 40, 42, 48, 252 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 110, 114, 126, 255 });
    DrawRectangleRec(title, (Color){ 52, 56, 66, 255 });
    ZenLabelTip((Rectangle){ m.x + 8, m.y + 3, title.width - 8, 18 },
                TextFormat("TRACK  %s . %s", e->name, g ? g->name : "?"),
                "Drag this bar to move the modal");
    if (GuiButton((Rectangle){ m.x + m.width - 22, m.y + 2, 20, 20 }, "x"))
    { AudioPlayButton(); tm->open = false; return; }

    float x = m.x + 10, w = m.width - 20 - TM_VALW;
    float y = m.y + TM_TITLE_H + 2;

    // tree: the scope banner plus a row per key the edits will hit.
    float allTimes[ZEN_GROUP_TIMES_MAX];
    int keyTotal = ZenGroupKeyTimes(e, zen.selGroup, allTimes);
    y += DrawKeyTree(m, y, e, g, treeTimes, treeCount);

    // --- single-key: time row -----------------------------------------------
    if (single)
    {
        GuiLabel((Rectangle){ x, y, TM_LBL, TM_RH }, "time");
        if (GuiTextBox((Rectangle){ x + TM_LBL, y, w - TM_LBL, TM_RH }, tm->timeBuf,
                       sizeof(tm->timeBuf), tm->edTime))
        {
            if (!tm->edTime) tm->edTime = true;
            else
            {
                tm->edTime = false;
                float nt = ZenClampF((float)atof(tm->timeBuf), 0.0f, zen.doc.duration);
                if (fabsf(nt - zen.selKeys[0]) > ZEN_AUTOKEY_EPS)
                {
                    ZenUndoPush();
                    ZenGroupMoveKeyTo(e, zen.selGroup, zen.selKeys[0], nt);
                    zen.selKeys[0] = nt;
                    ZenTrackModalSync();
                }
            }
        }
        y += TM_RH + TM_GAP;
    }

    // --- member rows ---------------------------------------------------------
    float refT = single ? zen.selKeys[0] : (keyTotal ? allTimes[0] : 0.0f);
    for (int mi = 0; g && mi < g->propCount && mi < 8; mi++)
    {
        int prop = g->props[mi];
        if (AnimPropIsColor(prop)) continue;        // colour block below
        float lo, hi; ZenPropRange(e, prop, &lo, &hi);

        ZenLabelTip((Rectangle){ x, y, TM_LBL, TM_RH }, AnimPropName(prop),
                    ZenPropDesc(prop));

        // A string key holds a POOL INDEX, not a quantity: dragging a slider
        // through it would scrub across unrelated entries and land on holes.
        // Show the words instead, and hand editing to the pool modal that owns
        // them.
        if (prop == AP_T_STRING)
        {
            const char *words = "(no key here)";
            AnimTrack *tr = AnimElemFindTrack(e, prop);
            int k = -1;
            for (int j = 0; tr && j < tr->keyCount; j++)
                if (fabsf(tr->keys[j].t - refT) <= ZEN_AUTOKEY_EPS) { k = j; break; }
            if (k >= 0)
            {
                // +0.5f, not a bare cast: a value round-tripped through the file
                // as 1.9999998 would otherwise truncate to the WRONG entry.
                const char *s = AnimDocStringAt(&zen.doc,
                                    (int)(tr->keys[k].value + 0.5f));
                words = s ? s : "(missing string)";
            }

            Rectangle sr = { x + TM_LBL, y, w - TM_LBL, TM_RH };
            zen.strDropRect = sr;
            if (k < 0) GuiDisable();
            if (GuiButton(sr, TextFormat("%s  v", ZenTextPreview(words, 24))))
            { AudioPlayButton(); zen.strDropOpen = !zen.strDropOpen; }
            if (k < 0) GuiEnable();
            ZenTip(sr, k >= 0
                ? "The words this key switches to. Click to pick another string "
                  "from the pool - it changes THIS key only, at this time."
                : "No key at this time, so there is nothing to assign. Use +key "
                  "to make one here first.");
            y += TM_RH + TM_GAP;
            continue;
        }

        // Same story one pool over: a shape key holds a pool SLOT, so it gets a
        // picker rather than a slider.
        if (prop == AP_S_SHAPE)
        {
            const char *nm = "(no key here)";
            AnimTrack *tr = AnimElemFindTrack(e, prop);
            int k = -1;
            for (int j = 0; tr && j < tr->keyCount; j++)
                if (fabsf(tr->keys[j].t - refT) <= ZEN_AUTOKEY_EPS) { k = j; break; }
            if (k >= 0)
            {
                const AnimShapeDef *sd =
                    AnimShapePoolGet((int)(tr->keys[k].value + 0.5f));
                nm = sd ? sd->name : "(missing shape)";
            }

            Rectangle sr = { x + TM_LBL, y, w - TM_LBL, TM_RH };
            zen.shapeDropRect = sr;
            if (k < 0) GuiDisable();
            if (GuiButton(sr, TextFormat("%s  v", ZenTextPreview(nm, 24))))
            { AudioPlayButton(); zen.shapeDropOpen = !zen.shapeDropOpen; }
            if (k < 0) GuiEnable();
            ZenTip(sr, k >= 0
                ? "The pixel shape this key switches to. Click to pick another "
                  "from the pool - it changes THIS key only, at this time."
                : "No key at this time, so there is nothing to assign. Use +key "
                  "to make one here first.");
            y += TM_RH + TM_GAP;
            continue;
        }

        if (single)
        {
            AnimTrack *tr = AnimElemFindTrack(e, prop);
            int k = -1;
            for (int j = 0; tr && j < tr->keyCount; j++)
                if (fabsf(tr->keys[j].t - refT) <= ZEN_AUTOKEY_EPS) { k = j; break; }
            if (k >= 0)
            {
                float v = tr->keys[k].value;
                if (ZenEditSlider((Rectangle){ x + TM_LBL, y, w - TM_LBL, TM_RH }, "", &v, lo, hi))
                    tr->keys[k].value = v;
            }
            else GuiLabel((Rectangle){ x + TM_LBL, y, w - TM_LBL, TM_RH }, "(no key)");
        }
        else
        {
            float v = tm->vals[mi];
            if (ZenEditSlider((Rectangle){ x + TM_LBL, y, w - TM_LBL, TM_RH }, "", &v, lo, hi))
            { tm->vals[mi] = v; tm->dVals[mi] = true; }
            if (tm->dVals[mi])                      // dirty marker
                DrawCircle((int)(x + w + TM_VALW - 10), (int)(y + TM_RH/2), 3,
                           (Color){ 255, 210, 90, 255 });
        }
        y += TM_RH + TM_GAP;
    }

    // --- colour member -------------------------------------------------------
    if (cp >= 0)
    {
        Color cc;
        AnimTrack *ctr = AnimElemFindTrack(e, cp);
        int ck = -1;
        for (int j = 0; ctr && j < ctr->keyCount; j++)
            if (fabsf(ctr->keys[j].t - refT) <= ZEN_AUTOKEY_EPS) { ck = j; break; }
        cc = single ? (ck >= 0 ? ctr->keys[ck].cval : (Color){0,0,0,255}) : tm->cval;

        GuiLabel((Rectangle){ x, y, m.width - 44, TM_RH }, "color (rgb)");
        ZenDrawSwatch((Rectangle){ m.x + m.width - 30, y + 3, 18, 18 },
                      (Color){ cc.r, cc.g, cc.b, 255 });
        if (!single && tm->dCval)
            DrawCircle((int)(x + w + TM_VALW - 10), (int)(y + TM_RH/2), 3,
                       (Color){ 255, 210, 90, 255 });
        y += TM_RH;
        float cr = cc.r, cg = cc.g, cb = cc.b;
        bool ch = false;
        if (ZenEditSlider((Rectangle){ x+16, y, w-16, 16 }, "R", &cr, 0,255)) ch=true; y+=18;
        if (ZenEditSlider((Rectangle){ x+16, y, w-16, 16 }, "G", &cg, 0,255)) ch=true; y+=18;
        if (ZenEditSlider((Rectangle){ x+16, y, w-16, 16 }, "B", &cb, 0,255)) ch=true; y+=18;
        if (ch)
        {
            Color nc = { (unsigned char)cr, (unsigned char)cg, (unsigned char)cb, 255 };
            if (single) { if (ck >= 0) ctr->keys[ck].cval = nc; }
            else        { tm->cval = nc; tm->dCval = true; }
        }
        y += TM_GAP;
    }

    // --- ease (dropdown header; list drawn topmost by the overlay pass) -----
    // A stepped group has nothing to ease BETWEEN: AnimTrackEval returns the
    // left key's value outright, so an ease picked here would be stored, saved
    // and silently ignored. Say so instead of offering a control that lies.
    // (signal mode returned at the top of this function, so this is doc mode)
    bool stepped = zen.selGroup >= 0 && ZenGroupIsStepped(e->kind, zen.selGroup);
    if (stepped)
    {
        GuiLabel((Rectangle){ x, y, TM_LBL, TM_RH }, "ease");
        GuiLabel((Rectangle){ x + TM_LBL, y, w - TM_LBL, TM_RH }, "steps - no easing");
        ZenTip((Rectangle){ x, y, w, TM_RH }, e->kind == AE_SHAPE
               ? "The shape snaps at each key and holds until the next one. There "
                 "is no midpoint between two shapes, so nothing to ease."
               : "Text snaps at each key and holds those words until the next one. "
                 "There is no midpoint between two strings, so nothing to ease.");
        zen.easeDropOpen = false;
    }
    else
    {
    GuiLabel((Rectangle){ x, y, TM_LBL, TM_RH }, "ease");
    zen.easeDropRect = (Rectangle){ x + TM_LBL, y, w - TM_LBL, TM_RH };
    if (GuiButton(zen.easeDropRect, TextFormat("%s  v", AnimEaseName(tm->ease))))
    { AudioPlayButton(); zen.easeDropOpen = !zen.easeDropOpen; }
    if (!single && tm->dEase)
        DrawCircle((int)(x + w + TM_VALW - 10), (int)(y + TM_RH/2), 3,
                   (Color){ 255, 210, 90, 255 });
    }
    y += TM_RH + TM_GAP;

    // --- footer buttons ------------------------------------------------------
    float bw = (m.width - 30) / 2.0f;
    if (single)
    {
        if (ZenButton((Rectangle){ x, y, bw, TM_RH }, "delete key"))
        {
            AudioPlayButton(); ZenUndoPush();
            ZenGroupDeleteKeyAt(e, zen.selGroup, zen.selKeys[0]);
            zen.selKeyCount = 0;
            ZenSelValidate();
        }

        // Duplicate takes the slot Apply occupies in the other scopes: it only
        // means anything for ONE key, and its destination is the playhead - the
        // same target +key uses, so the two read as one idea. Blocked when a key
        // already sits there, since that key would be silently overwritten.
        bool dupHere = fabsf(zen.selKeys[0] - zen.playhead) <= ZEN_AUTOKEY_EPS;
        bool dupBlocked = false;
        for (int i = 0; i < keyTotal && !dupBlocked; i++)
            if (fabsf(allTimes[i] - zen.playhead) <= ZEN_AUTOKEY_EPS) dupBlocked = true;

        if (dupBlocked) GuiDisable();
        if (ZenButton((Rectangle){ x + bw + 10, y, bw, TM_RH }, "duplicate key"))
        {
            AudioPlayButton(); ZenUndoPush();
            ZenGroupWriteKey(e, zen.selGroup, zen.playhead);
            CopyKeyValues(e, zen.selGroup, zen.selKeys[0], zen.playhead);
            ZenSelKey(zen.selElem, zen.selGroup, zen.playhead, false);
            ZenTrackModalSync();
        }
        if (dupBlocked) GuiEnable();
        ZenTip((Rectangle){ x + bw + 10, y, bw, TM_RH },
               dupHere ? "The playhead is on this key - scrub elsewhere to copy it there"
               : dupBlocked ? "A key already exists at the playhead"
                            : "Copy this key's values to a new key at the playhead");
    }
    else if (track)
    {
        if (ZenButton((Rectangle){ x, y, bw, TM_RH }, "delete track"))
        {
            AudioPlayButton(); ZenUndoPush();
            ZenGroupDeleteTracks(e, zen.selGroup);
            ZenSelClear();
            return;
        }
        if (ZenButton((Rectangle){ x + bw + 10, y, bw, TM_RH }, "Apply to all"))
        { AudioPlayButton(); BulkApply(e); }
    }
    else
    {
        if (ZenButton((Rectangle){ x, y, bw, TM_RH }, "delete keys"))
        {
            AudioPlayButton(); ZenUndoPush();
            for (int i = 0; i < zen.selKeyCount; i++)
                ZenGroupDeleteKeyAt(e, zen.selGroup, zen.selKeys[i]);
            zen.selKeyCount = 0;
            ZenSelValidate();
        }
        if (ZenButton((Rectangle){ x + bw + 10, y, bw, TM_RH }, "Apply to selected"))
        { AudioPlayButton(); BulkApply(e); }
    }
    y += TM_RH + TM_GAP;

    // --- new key at the playhead + revert staged edits ------------------------
    // +key is offered in every mode: in bulk/track scope it is the counterpart
    // to Apply, which only rewrites keys that already exist.
    bool haveKey = false;
    for (int i = 0; i < keyTotal; i++)
        if (fabsf(allTimes[i] - zen.playhead) <= ZEN_AUTOKEY_EPS) { haveKey = true; break; }

    // this row is +key alone, so it spans both slots less the reset icon.
    float rw = 2.0f*bw + 10.0f - 34.0f;
    if (haveKey) GuiDisable();
    if (ZenButton((Rectangle){ x, y, rw, TM_RH },
                  TextFormat("+key @ %.2fs", zen.playhead)))
    {
        AudioPlayButton(); ZenUndoPush();
        // seed from the element's pose at the playhead, which holds the
        // previous key's value between keys.
        ZenGroupWriteKey(e, zen.selGroup, zen.playhead);
        // a single selected key is the better source: copy it verbatim so
        // +key duplicates it rather than resampling the curve.
        if (single) CopyKeyValues(e, zen.selGroup, zen.selKeys[0], zen.playhead);
        ZenSelKey(zen.selElem, zen.selGroup, zen.playhead, false);
        ZenTrackModalSync();
    }
    if (haveKey) GuiEnable();
    ZenTip((Rectangle){ x, y, rw, TM_RH },
           haveKey ? "A key already exists at the playhead - scrub elsewhere to add one"
                   : "Create a key at the playhead using the values shown above");

    bool dirty = tm->dCval || tm->dEase;
    for (int mi = 0; mi < 8 && !dirty; mi++) dirty = dirty || tm->dVals[mi];
    if (!dirty) GuiDisable();
    if (ZenButton((Rectangle){ x + rw + 6, y, 28, TM_RH }, "#211#"))
    { AudioPlayButton(); ZenTrackModalSync(); }   // drops every staged edit
    if (!dirty) GuiEnable();
}

// The ease list, drawn AFTER everything so it overlays the modal; flips above
// the header when there is no room below.
void ZenEaseDropOverlayGui(void)
{
    if (!zen.easeDropOpen || !zen.trackModal.open) return;
    bool sigMode = zen.trackModal.sig >= 0;
    AnimElem *e = ModalElem();
    if (!sigMode && (!e || zen.selGroup < 0)) { zen.easeDropOpen = false; return; }
    if (sigMode && (zen.trackModal.sig >= zen.doc.signalCount ||
                    zen.sigSelElem < 0 || zen.sigSelGroup < 0))
    { zen.easeDropOpen = false; return; }

    Vector2 screen = ScreenStateSize();
    Rectangle hdr = zen.easeDropRect;
    ZenTrackModal *tm = &zen.trackModal;

    // builtins + used customs, minus hidden ones (the current pick always
    // shows so a hidden ease already on the key stays reachable).
    int ids[ANIM_EASE_COUNT + ANIM_CUSTOM_EASE_MAX], n = 0;
    for (int i = 0; i < AnimEaseIdRange(); i++)
        if (AnimEaseIdValid(i) && (!AnimEaseIsHidden(i) || i == tm->ease))
            ids[n++] = i;
    float ih = 20.0f, listH = ih * n;
    float ly = (hdr.y + hdr.height + listH <= screen.y - 4.0f)
             ? hdr.y + hdr.height : hdr.y - listH;
    if (ly < 4.0f) ly = 4.0f;
    Rectangle bg = { hdr.x, ly, hdr.width, listH };
    DrawRectangleRec(bg, (Color){ 32, 34, 40, 255 });
    DrawRectangleLinesEx(bg, 1.0f, (Color){ 70, 74, 84, 255 });

    for (int row = 0; row < n; row++)
    {
        int i = ids[row];
        Rectangle rr = { bg.x, bg.y + row*ih, bg.width, ih };
        if (GuiButton(rr, AnimEaseName(i)))
        {
            AudioPlayButton();
            zen.easeDropOpen = false;
            if (sigMode)
            {
                ZenUndoPush();
                ZenSigGroupSetEaseAt(&zen.doc.signals[zen.trackModal.sig],
                                     zen.sigSelElem, zen.sigSelGroup, zen.sigSelU, i);
            }
            else if (zen.selKeyCount == 1)
            {
                ZenUndoPush();
                ZenGroupSetEaseAt(e, zen.selGroup, zen.selKeys[0], i);
                tm->ease = i;
            }
            else { tm->ease = i; tm->dEase = true; }
        }
        if (!sigMode && i == tm->ease) DrawRectangleRec(rr, (Color){ 90, 140, 220, 60 });
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        !CheckCollisionPointRec(GetMousePosition(), bg) &&
        !CheckCollisionPointRec(GetMousePosition(), hdr))
        zen.easeDropOpen = false;
}

// ---------------------------------------------------------------------------
//  String picker: which pool entry the SELECTED key switches to. This is the
//  only place a string is bound to a time - the pool modal owns the words
//  themselves, this owns which key shows which. Drawn topmost in the unlocked
//  overlay pass, like the ease list above.
// ---------------------------------------------------------------------------
void ZenStringDropOverlayGui(void)
{
    if (!zen.strDropOpen || !zen.trackModal.open) return;
    if (zen.trackModal.sig >= 0) { zen.strDropOpen = false; return; }

    AnimElem *e = ModalElem();
    if (!e || zen.selGroup < 0) { zen.strDropOpen = false; return; }

    AnimTrack *tr = AnimElemFindTrack(e, AP_T_STRING);
    if (!tr) { zen.strDropOpen = false; return; }

    // The key being edited is the one the row resolved: at the single selected
    // time, else the track's first - matching `refT` in ZenTrackModalGui.
    float times[ZEN_GROUP_TIMES_MAX];
    int   n = ZenGroupKeyTimes(e, zen.selGroup, times);
    float refT = (zen.selKeyCount == 1) ? zen.selKeys[0] : (n ? times[0] : 0.0f);
    int k = -1;
    for (int j = 0; j < tr->keyCount; j++)
        if (fabsf(tr->keys[j].t - refT) <= ZEN_AUTOKEY_EPS) { k = j; break; }
    if (k < 0) { zen.strDropOpen = false; return; }

    int cur = (int)(tr->keys[k].value + 0.5f);

    // Holes left by a delete are skipped: they are not choosable strings.
    int ids[ANIM_STRINGS_MAX], m = 0;
    for (int i = 0; i < zen.doc.stringCount; i++)
        if (zen.doc.strings[i].used) ids[m++] = i;

    Vector2 screen = ScreenStateSize();
    Rectangle hdr = zen.strDropRect;
    float ih = 20.0f, listH = ih * (m + 1);      // +1 for the "new string" row
    float ly = (hdr.y + hdr.height + listH <= screen.y - 4.0f)
             ? hdr.y + hdr.height : hdr.y - listH;
    if (ly < 4.0f) ly = 4.0f;
    Rectangle bg = { hdr.x, ly, hdr.width, listH };
    DrawRectangleRec(bg, (Color){ 32, 34, 40, 255 });
    DrawRectangleLinesEx(bg, 1.0f, (Color){ 70, 74, 84, 255 });

    for (int row = 0; row < m; row++)
    {
        int i = ids[row];
        Rectangle rr = { bg.x, bg.y + row*ih, bg.width, ih };
        if (GuiButton(rr, TextFormat("%02d  %s", i,
                                     ZenTextPreview(zen.doc.strings[i].text, 22))))
        {
            AudioPlayButton();
            zen.strDropOpen = false;
            ZenUndoPush();
            tr->keys[k].value = (float)i;
            // The element's own words are the fallback when the track is gone,
            // so mirror only the FIRST key: past that the text changes over
            // time and there is no single "the" text to stand for it.
            if (k == 0)
            {
                const char *s = AnimDocStringAt(&zen.doc, i);
                if (s) TextCopy(e->text, s);
            }
            zen.docDirty = true;
            ZenTrackModalSync();
        }
        if (i == cur) DrawRectangleRec(rr, (Color){ 90, 140, 220, 60 });
    }

    // The pool is where words are written; this list only chooses among them.
    Rectangle nr = { bg.x, bg.y + m*ih, bg.width, ih };
    if (GuiButton(nr, "+ new string..."))
    { AudioPlayButton(); zen.strDropOpen = false; ZenStringPoolShow(); }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        !CheckCollisionPointRec(GetMousePosition(), bg) &&
        !CheckCollisionPointRec(GetMousePosition(), hdr))
        zen.strDropOpen = false;
}

// ---------------------------------------------------------------------------
//  Shape-pool list for the AP_S_SHAPE row. The pool is GLOBAL, not part of the
//  document, so unlike the string list this one is not bounded by the doc - but
//  it is just as sparse (a deleted shape leaves its slot free) and is walked the
//  same way. The row it edits is opened from the inspector too, so this serves
//  BOTH: the inspector sets the element's rest-pose shapeName, the track modal
//  sets one key.
// ---------------------------------------------------------------------------
void ZenShapeDropOverlayGui(void)
{
    if (!zen.shapeDropOpen) return;

    // Which of the two callers opened it: a track key (modal open on a shape
    // group) or the inspector's rest-pose row.
    AnimTrack *tr = NULL;
    int k = -1;
    AnimElem *e = NULL;

    if (zen.trackModal.open && zen.trackModal.sig < 0 && zen.selGroup >= 0)
    {
        e = ModalElem();
        if (e) tr = AnimElemFindTrack(e, AP_S_SHAPE);
        if (tr)
        {
            float times[ZEN_GROUP_TIMES_MAX];
            int   n = ZenGroupKeyTimes(e, zen.selGroup, times);
            float refT = (zen.selKeyCount == 1) ? zen.selKeys[0]
                                                : (n ? times[0] : 0.0f);
            for (int j = 0; j < tr->keyCount; j++)
                if (fabsf(tr->keys[j].t - refT) <= ZEN_AUTOKEY_EPS) { k = j; break; }
            if (k < 0) tr = NULL;               // no key here: fall back to base
        }
    }
    if (!e) e = (zen.selElem >= 0 && zen.selElem < zen.doc.elemCount)
              ? &zen.doc.elems[zen.selElem] : NULL;
    if (!e || e->kind != AE_SHAPE) { zen.shapeDropOpen = false; return; }

    int cur = tr ? (int)(tr->keys[k].value + 0.5f)
                 : AnimShapePoolFindByName(e->shapeName);

    int ids[ANIM_SHAPE_POOL_MAX], m = 0;
    for (int i = 0; i < ANIM_SHAPE_POOL_MAX; i++)
        if (AnimShapeIdValid(i)) ids[m++] = i;

    Vector2 screen = ScreenStateSize();
    Rectangle hdr = zen.shapeDropRect;
    float ih = 20.0f, listH = ih * (m + 1);      // +1 for the "new shape" row
    float ly = (hdr.y + hdr.height + listH <= screen.y - 4.0f)
             ? hdr.y + hdr.height : hdr.y - listH;
    if (ly < 4.0f) ly = 4.0f;
    Rectangle bg = { hdr.x, ly, hdr.width, listH };
    DrawRectangleRec(bg, (Color){ 32, 34, 40, 255 });
    DrawRectangleLinesEx(bg, 1.0f, (Color){ 70, 74, 84, 255 });

    for (int row = 0; row < m; row++)
    {
        int i = ids[row];
        const AnimShapeDef *sd = AnimShapePoolGet(i);
        Rectangle rr = { bg.x, bg.y + row*ih, bg.width, ih };
        if (GuiButton(rr, TextFormat("%s  %dx%d", sd->name, sd->w, sd->h)))
        {
            AudioPlayButton();
            zen.shapeDropOpen = false;
            ZenUndoPush();
            if (tr)
            {
                tr->keys[k].value = (float)i;
                // The element's own shapeName is the fallback when the track is
                // gone, so mirror only the FIRST key - past that the shape
                // changes over time and no single one stands for it.
                if (k == 0) TextCopy(e->shapeName, sd->name);
            }
            else TextCopy(e->shapeName, sd->name);
            zen.docDirty = true;
            ZenTrackModalSync();
        }
        if (i == cur) DrawRectangleRec(rr, (Color){ 90, 140, 220, 60 });
    }

    // Shapes are drawn in their own editor; this list only chooses among them.
    Rectangle nr = { bg.x, bg.y + m*ih, bg.width, ih };
    if (GuiButton(nr, "+ new shape..."))
    {
        AudioPlayButton();
        zen.shapeDropOpen = false;
        ShapeEditorOpen("");
        AppStateTransition(&app_state_shape_editor);
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        !CheckCollisionPointRec(GetMousePosition(), bg) &&
        !CheckCollisionPointRec(GetMousePosition(), hdr))
        zen.shapeDropOpen = false;
}
