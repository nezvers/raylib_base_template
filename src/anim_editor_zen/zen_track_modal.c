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
#include <math.h>
#include <stdlib.h>

#define TM_W       300.0f
#define TM_TITLE_H 24.0f
#define TM_RH      24.0f
#define TM_GAP     6.0f

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

// Reload the staged values from the selection (first selected key, or the
// track's first key) and clear every dirty flag. Called on selection change
// so the modal always mirrors what is picked - like the timeline does.
void ZenTrackModalSync(void)
{
    ZenTrackModal *tm = &zen.trackModal;
    if (tm->sig >= 0) return;               // signal mode edits live, no staging
    AnimElem *e = ModalElem();
    if (!e || zen.selGroup < 0) { tm->open = false; return; }

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
    Vector2 mouse = GetMousePosition();
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, titleR))
    { tm->dragging = true; tm->dragOff = (Vector2){ mouse.x - m.x, mouse.y - m.y }; }
    if (tm->dragging)
    {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
            tm->pos = (Vector2){ mouse.x - tm->dragOff.x, mouse.y - tm->dragOff.y };
        else tm->dragging = false;
    }

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

    float x = m.x + 10, w = m.width - 20 - 50;
    float y = m.y + TM_TITLE_H + 2;
    GuiLabel((Rectangle){ x, y, m.width - 20, 18 },
             "edits apply immediately to this signal key");
    y += 22;

    // u: moves the whole group key.
    GuiLabel((Rectangle){ x, y, 44, TM_RH }, "u");
    float u = zen.sigSelU;
    if (ZenEditSlider((Rectangle){ x + 44, y, w - 44, TM_RH }, "", &u, 0.0f, 1.0f))
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
            ZenLabelTip((Rectangle){ x, y, 44, TM_RH }, AnimPropName(prop),
                        AnimPropName(prop));
            if (k >= 0)
            {
                float v = tg->keys[k].value, lo, hi;
                ZenPropRange(el, prop, &lo, &hi);
                if (ZenEditSlider((Rectangle){ x + 44, y, w - 44, TM_RH }, "", &v, lo, hi))
                    tg->keys[k].value = v;
            }
            else GuiLabel((Rectangle){ x + 44, y, w - 44, TM_RH }, "(no key)");
            y += TM_RH + TM_GAP;
        }
    }

    // ease (header; the shared overlay applies it in signal mode).
    GuiLabel((Rectangle){ x, y, 44, TM_RH }, "ease");
    zen.easeDropRect = (Rectangle){ x + 44, y, w - 44, TM_RH };
    int ease = ZenSigGroupEase(sg, zen.sigSelElem, zen.sigSelGroup, zen.sigSelU);
    if (GuiButton(zen.easeDropRect, TextFormat("%s  v", AnimEaseName(ease))))
    { AudioPlayButton(); zen.easeDropOpen = !zen.easeDropOpen; }
    y += TM_RH + TM_GAP;

    if (GuiButton((Rectangle){ x, y, (m.width - 30) / 2.0f, TM_RH }, "delete key"))
    {
        AudioPlayButton(); ZenUndoPush();
        ZenSigGroupDeleteKeyAt(sg, zen.sigSelElem, zen.sigSelGroup, zen.sigSelU);
        ZenSigClearKeySel();
        tm->open = false;
    }
}

void ZenTrackModalGui(void)
{
    ZenTrackModal *tm = &zen.trackModal;
    if (!tm->open) { tm->rect = (Rectangle){0}; return; }
    if (tm->sig >= 0) { SigModeGui(tm); return; }
    AnimElem *e = ModalElem();
    if (!e || zen.selGroup < 0 || !ZenGroupHasTrack(e, zen.selGroup))
    { tm->open = false; tm->rect = (Rectangle){0}; return; }

    const AnimPropGroup *g = AnimGroupAt(e->kind, zen.selGroup);
    int cp = ZenGroupColorProp(e->kind, zen.selGroup);
    bool single = zen.selKeyCount == 1;
    bool track  = zen.selKeyCount == 0;

    // height: title + subtitle + time row (single) + member rows + ease + buttons
    int scalarRows = 0;
    for (int m = 0; g && m < g->propCount; m++)
        if (!AnimPropIsColor(g->props[m])) scalarRows++;
    float bodyH = TM_TITLE_H + 22 + (single ? TM_RH + TM_GAP : 0)
                + scalarRows * (TM_RH + TM_GAP)
                + (cp >= 0 ? TM_RH + 3*18 + TM_GAP : 0)
                + TM_RH + TM_GAP        // ease
                + TM_RH + 10;           // buttons
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
    Vector2 mouse = GetMousePosition();
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, title))
    { tm->dragging = true; tm->dragOff = (Vector2){ mouse.x - m.x, mouse.y - m.y }; }
    if (tm->dragging)
    {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
            tm->pos = (Vector2){ mouse.x - tm->dragOff.x, mouse.y - tm->dragOff.y };
        else tm->dragging = false;
    }

    // --- chrome --------------------------------------------------------------
    DrawRectangleRec(m, (Color){ 40, 42, 48, 252 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 110, 114, 126, 255 });
    DrawRectangleRec(title, (Color){ 52, 56, 66, 255 });
    ZenLabelTip((Rectangle){ m.x + 8, m.y + 3, title.width - 8, 18 },
                TextFormat("TRACK  %s . %s", e->name, g ? g->name : "?"),
                "Drag this bar to move the modal");
    if (GuiButton((Rectangle){ m.x + m.width - 22, m.y + 2, 20, 20 }, "x"))
    { AudioPlayButton(); tm->open = false; return; }

    float x = m.x + 10, w = m.width - 20 - 50;
    float y = m.y + TM_TITLE_H + 2;

    // subtitle: what the edits will hit.
    float allTimes[ZEN_GROUP_TIMES_MAX];
    int keyTotal = ZenGroupKeyTimes(e, zen.selGroup, allTimes);
    const char *scope = single ? TextFormat("key @ %.2fs", zen.selKeys[0])
                     : track  ? TextFormat("whole track (%d keys)", keyTotal)
                              : TextFormat("%d keys selected", zen.selKeyCount);
    GuiLabel((Rectangle){ x, y, m.width - 20, 18 }, scope);
    ZenTip((Rectangle){ x, y, m.width - 20, 18 },
           single ? "Edits apply immediately to this key"
                  : "Edits are staged; Apply writes only the changed fields");
    y += 22;

    // --- single-key: time row -----------------------------------------------
    if (single)
    {
        GuiLabel((Rectangle){ x, y, 40, TM_RH }, "time");
        if (GuiTextBox((Rectangle){ x + 44, y, w - 44, TM_RH }, tm->timeBuf,
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

        ZenLabelTip((Rectangle){ x, y, 44, TM_RH }, AnimPropName(prop),
                    AnimPropName(prop));
        if (single)
        {
            AnimTrack *tr = AnimElemFindTrack(e, prop);
            int k = -1;
            for (int j = 0; tr && j < tr->keyCount; j++)
                if (fabsf(tr->keys[j].t - refT) <= ZEN_AUTOKEY_EPS) { k = j; break; }
            if (k >= 0)
            {
                float v = tr->keys[k].value;
                if (ZenEditSlider((Rectangle){ x + 44, y, w - 44, TM_RH }, "", &v, lo, hi))
                    tr->keys[k].value = v;
            }
            else GuiLabel((Rectangle){ x + 44, y, w - 44, TM_RH }, "(no key)");
        }
        else
        {
            float v = tm->vals[mi];
            if (ZenEditSlider((Rectangle){ x + 44, y, w - 44, TM_RH }, "", &v, lo, hi))
            { tm->vals[mi] = v; tm->dVals[mi] = true; }
            if (tm->dVals[mi])                      // dirty marker
                DrawCircle((int)(x + w + 40), (int)(y + TM_RH/2), 3,
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
            DrawCircle((int)(x + w + 40), (int)(y + TM_RH/2), 3,
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
    GuiLabel((Rectangle){ x, y, 44, TM_RH }, "ease");
    zen.easeDropRect = (Rectangle){ x + 44, y, w - 44, TM_RH };
    if (GuiButton(zen.easeDropRect, TextFormat("%s  v", AnimEaseName(tm->ease))))
    { AudioPlayButton(); zen.easeDropOpen = !zen.easeDropOpen; }
    if (!single && tm->dEase)
        DrawCircle((int)(x + w + 40), (int)(y + TM_RH/2), 3,
                   (Color){ 255, 210, 90, 255 });
    y += TM_RH + TM_GAP;

    // --- footer buttons ------------------------------------------------------
    float bw = (m.width - 30) / 2.0f;
    if (single)
    {
        if (GuiButton((Rectangle){ x, y, bw, TM_RH }, "delete key"))
        {
            AudioPlayButton(); ZenUndoPush();
            ZenGroupDeleteKeyAt(e, zen.selGroup, zen.selKeys[0]);
            zen.selKeyCount = 0;
            ZenSelValidate();
        }
    }
    else if (track)
    {
        if (GuiButton((Rectangle){ x, y, bw, TM_RH }, "delete track"))
        {
            AudioPlayButton(); ZenUndoPush();
            ZenGroupDeleteTracks(e, zen.selGroup);
            ZenSelClear();
            return;
        }
        if (GuiButton((Rectangle){ x + bw + 10, y, bw, TM_RH }, "Apply to all"))
        { AudioPlayButton(); BulkApply(e); }
    }
    else
    {
        if (GuiButton((Rectangle){ x, y, bw, TM_RH }, "delete keys"))
        {
            AudioPlayButton(); ZenUndoPush();
            for (int i = 0; i < zen.selKeyCount; i++)
                ZenGroupDeleteKeyAt(e, zen.selGroup, zen.selKeys[i]);
            zen.selKeyCount = 0;
            ZenSelValidate();
        }
        if (GuiButton((Rectangle){ x + bw + 10, y, bw, TM_RH }, "Apply to selected"))
        { AudioPlayButton(); BulkApply(e); }
    }
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
