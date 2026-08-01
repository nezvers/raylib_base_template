// ============================================================================
//  zen_signal_modal.c  -  the Zen editor's draggable signal modal
//
//  Everything about ONE signal: name, length, flags, mouse-position bindings
//  ("params"), the per-instance sequence envelope, and the (element,
//  property-group) targets with their keys.
//
//  Zen changes vs the classic modal:
//    - DRAGGABLE by its title bar; no full-screen dim, the editor behind it
//      stays visible and clickable (input inside the modal rect is claimed
//      by locking the panels while the cursor is over it - see Gui()).
//    - Group KEYS are not edited inline: clicking a key (or +key) selects it
//      and opens the shared track modal in signal mode.
//    - During playback it steps aside; the playback fire panel takes over.
// ============================================================================

#include "raylib.h"
#include "raygui.h"
#include "zen_internal.h"
#include "../screen_state/screen_state.h"
#include "../audio_state/audio_state.h"
#include "../anim/anim_signal.h"
#include <math.h>

#define SM_W        560.0f
#define SM_H        440.0f
#define SM_TITLE_H  24.0f

enum { ZEN_SIGDROP_NONE = 0, ZEN_SIGDROP_ADD };

void ZenSigCloseDrops(void)
{
    zen.sigDropMode = ZEN_SIGDROP_NONE;
    zen.sigDropElem = -1;
}

void ZenSigClearKeySel(void)
{
    zen.sigSelElem = -1; zen.sigSelGroup = -1; zen.sigSelU = 0.0f;
    zen.sigPosSelElem = -1; zen.sigSeqSelU = -1.0f;
    if (zen.trackModal.sig >= 0) zen.trackModal.open = false;
}

// Bindings capture a signal's name/dir/section by value at register time, so
// any change to zen.doc.signals needs a re-register.
void ZenReRegisterSignals(void)
{
    AnimSignalUnregister(&zen.doc, &zen.preview);
    AnimSignalRegister(&zen.doc, &zen.preview, &zen.playhead);
}

// ---------------------------------------------------------------------------
//  Mouse-position bindings ("params") helpers - modal-local.
// ---------------------------------------------------------------------------
static int SigPosFind(AnimSignal *sg, int elemIdx, int slot)
{
    for (int i = 0; i < sg->posParamCount; i++)
        if (sg->posParams[i].elemIdx == elemIdx && sg->posParams[i].slot == slot)
            return i;
    return -1;
}

static void SigPosSortKeys(AnimSigPosParam *pp)
{
    // insertion sort by t; the arrays hold only a handful of keys.
    for (int i = 1; i < pp->keyCount; i++)
    {
        AnimPosKey k = pp->keys[i];
        int j = i - 1;
        while (j >= 0 && pp->keys[j].t > k.t) { pp->keys[j+1] = pp->keys[j]; j--; }
        pp->keys[j+1] = k;
    }
}

static AnimSigPosParam *SigPosAdd(AnimSignal *sg, int elemIdx, int slot)
{
    if (SigPosFind(sg, elemIdx, slot) >= 0) return NULL;
    if (sg->posParamCount >= ANIM_SIG_POS_MAX) return NULL;
    AnimSigPosParam *pp = &sg->posParams[sg->posParamCount++];
    *pp = (AnimSigPosParam){0};
    pp->elemIdx = elemIdx; pp->slot = slot;
    // seed one key so a fresh binding does something: ease onto the mouse at u=1
    pp->keys[0] = (AnimPosKey){ 1.0f, 0.0f, 0.0f, ANIM_EASE_SINE_OUT };
    pp->keyCount = 1;
    return pp;
}

static void SigPosRemoveAt(AnimSignal *sg, int idx)
{
    for (int i = idx; i < sg->posParamCount - 1; i++)
        sg->posParams[i] = sg->posParams[i+1];
    sg->posParamCount--;
}

static int SigPosKeyNear(const AnimSigPosParam *pp, float u)
{
    for (int i = 0; pp && i < pp->keyCount; i++)
        if (fabsf(pp->keys[i].t - u) <= ZEN_SIG_U_EPS) return i;
    return -1;
}

static void SigPosWriteKey(AnimSigPosParam *pp, float u)
{
    if (SigPosKeyNear(pp, u) >= 0 || pp->keyCount >= ANIM_SIG_KEYS_MAX) return;
    pp->keys[pp->keyCount++] = (AnimPosKey){ u, 0.0f, 0.0f, ANIM_EASE_SINE_OUT };
    SigPosSortKeys(pp);
}

static float SigPosFreeU(const AnimSigPosParam *pp, float pref)
{
    float times[ANIM_SIG_KEYS_MAX];
    for (int i = 0; i < pp->keyCount; i++) times[i] = pp->keys[i].t;
    return ZenSigFreeU(times, pp->keyCount, pref);
}

static void SigPosRemoveKeyAt(AnimSigPosParam *pp, int idx)
{
    for (int i = idx; i < pp->keyCount - 1; i++) pp->keys[i] = pp->keys[i+1];
    pp->keyCount--;
}

// ---------------------------------------------------------------------------
//  Sequence-envelope helpers - modal-local.
// ---------------------------------------------------------------------------
static int SigSeqTargetFind(AnimSignal *sg, int elemIdx, int prop)
{
    for (int i = 0; i < sg->seqTargetCount; i++)
        if (sg->seqTargets[i].elemIdx == elemIdx && sg->seqTargets[i].prop == prop)
            return i;
    return -1;
}

static void SigSeqTargetRemoveAt(AnimSignal *sg, int idx)
{
    for (int i = idx; i < sg->seqTargetCount - 1; i++)
        sg->seqTargets[i] = sg->seqTargets[i+1];
    sg->seqTargetCount--;
}

// A group is toggled as a unit so w/h fan together (colour never seq-targets).
static bool SigSeqGroupOn(AnimSignal *sg, int elemIdx, int kind, int gi)
{
    const AnimPropGroup *g = AnimGroupAt(kind, gi);
    int scal = 0, have = 0;
    for (int m = 0; g && m < g->propCount; m++)
    {
        if (AnimPropIsColor(g->props[m])) continue;
        scal++;
        if (SigSeqTargetFind(sg, elemIdx, g->props[m]) >= 0) have++;
    }
    return scal > 0 && have == scal;
}

static bool SigSeqGroupHasScalar(int kind, int gi)
{
    const AnimPropGroup *g = AnimGroupAt(kind, gi);
    for (int m = 0; g && m < g->propCount; m++)
        if (!AnimPropIsColor(g->props[m])) return true;
    return false;
}

static void SigSeqGroupSet(AnimSignal *sg, int elemIdx, int kind, int gi, bool on)
{
    const AnimPropGroup *g = AnimGroupAt(kind, gi);
    for (int m = 0; g && m < g->propCount; m++)
    {
        if (AnimPropIsColor(g->props[m])) continue;
        int idx = SigSeqTargetFind(sg, elemIdx, g->props[m]);
        if (on && idx < 0 && sg->seqTargetCount < ANIM_SIG_SEQ_TARGETS)
        {
            AnimSigSeqTarget *st = &sg->seqTargets[sg->seqTargetCount++];
            st->elemIdx = elemIdx; st->prop = g->props[m];
        }
        else if (!on && idx >= 0) SigSeqTargetRemoveAt(sg, idx);
    }
}

static int SigSeqKeyNear(const AnimSignal *sg, float u)
{
    for (int i = 0; i < sg->seqKeyCount; i++)
        if (fabsf(sg->seqKeys[i].t - u) <= ZEN_SIG_U_EPS) return i;
    return -1;
}

static void SigSeqSortKeys(AnimSignal *sg)
{
    for (int i = 1; i < sg->seqKeyCount; i++)
    {
        AnimSeqKey k = sg->seqKeys[i];
        int j = i - 1;
        while (j >= 0 && sg->seqKeys[j].t > k.t) { sg->seqKeys[j+1] = sg->seqKeys[j]; j--; }
        sg->seqKeys[j+1] = k;
    }
}

static void SigSeqWriteKey(AnimSignal *sg, float u)
{
    if (SigSeqKeyNear(sg, u) >= 0 || sg->seqKeyCount >= ANIM_SIG_SEQ_KEYS) return;
    sg->seqKeys[sg->seqKeyCount++] = (AnimSeqKey){ u, 1.0f, ANIM_EASE_SINE_OUT };
    SigSeqSortKeys(sg);
}

static float SigSeqFreeU(const AnimSignal *sg, float pref)
{
    float times[ANIM_SIG_SEQ_KEYS];
    for (int i = 0; i < sg->seqKeyCount; i++) times[i] = sg->seqKeys[i].t;
    return ZenSigFreeU(times, sg->seqKeyCount, pref);
}

static void SigSeqRemoveKeyAt(AnimSignal *sg, int idx)
{
    for (int i = idx; i < sg->seqKeyCount - 1; i++) sg->seqKeys[i] = sg->seqKeys[i+1];
    sg->seqKeyCount--;
}

// Position slots an element exposes: text/normal shape one (center), corners-
// mode shape two, global none.
static int SigElemSlots(const AnimElem *e, const char *names[2])
{
    if (e->kind == AE_GLOBAL) return 0;
    if (e->kind == AE_SHAPE && e->cornerMode)
    { names[0] = "P0"; names[1] = "P1"; return 2; }
    names[0] = "center"; return 1;
}

// ---------------------------------------------------------------------------
//  Sections (drawn inside the scrolled list).
// ---------------------------------------------------------------------------
static float DrawSigParamsSection(AnimSignal *sg, Rectangle list, float ly,
                                  float rh, float gap)
{
    DrawRectangleRec((Rectangle){ list.x, ly, list.width, rh },
                     (Color){ 44, 52, 60, 255 });
    ZenLabelTip((Rectangle){ list.x+6, ly+4, 220, rh-8 },
                sg->posAnchor ? "params  (spawn at cursor)"
                              : "params  (mouse -> slot + offset)",
                "Bind the emit position to element position slots");
    bool anchor = sg->posAnchor;
    GuiCheckBox((Rectangle){ list.x+list.width-210, ly+4, 16, 16 },
                "blend with animation", &anchor);
    if (anchor != sg->posAnchor)
    { AudioPlayButton(); ZenUndoPush(); sg->posAnchor = anchor; }
    ly += rh + 2;

    bool replay = sg->replay;
    GuiCheckBox((Rectangle){ list.x+list.width-210, ly+4, 16, 16 },
                "restart on fire", &replay);
    if (replay != sg->replay)
    { AudioPlayButton(); ZenUndoPush(); sg->replay = replay; }
    ly += rh + 2;

    if (sg->posAnchor)
    {
        GuiLabel((Rectangle){ list.x+14, ly, list.width-20, rh },
                 "bound elements spawn at the cursor; offset keys ignored");
        ly += rh;
    }

    int delBind = -1, delKeyBind = -1, delKeyIdx = -1;

    for (int e = 0; e < zen.doc.elemCount; e++)
    {
        const char *sn[2]; int ns = SigElemSlots(&zen.doc.elems[e], sn);
        for (int s = 0; s < ns; s++)
        {
            int bi = SigPosFind(sg, e, s);
            ZenLabelTip((Rectangle){ list.x+14, ly+4, list.width-160, rh-8 },
                        TextFormat("%s . %s", zen.doc.elems[e].name, sn[s]), NULL);
            if (bi < 0)
            {
                bool full = sg->posParamCount >= ANIM_SIG_POS_MAX;
                if (full) GuiSetState(STATE_DISABLED);
                if (GuiButton((Rectangle){ list.x+list.width-70, ly, 66, rh },
                              full ? "full" : "+ bind") && !full)
                { AudioPlayButton(); ZenUndoPush(); SigPosAdd(sg, e, s);
                  zen.sigPosSelElem = e; zen.sigPosSelSlot = s; zen.sigPosSelU = 1.0f; }
                if (full) GuiSetState(STATE_NORMAL);
                ly += rh + 2;
                continue;
            }
            AnimSigPosParam *pp = &sg->posParams[bi];
            if (GuiButton((Rectangle){ list.x+list.width-104, ly, 50, rh }, "+key"))
            { AudioPlayButton(); ZenUndoPush();
              float nu = SigPosFreeU(pp, zen.sigLastU); SigPosWriteKey(pp, nu);
              zen.sigPosSelElem = e; zen.sigPosSelSlot = s; zen.sigPosSelU = nu; }
            if (GuiButton((Rectangle){ list.x+list.width-50, ly, 50, rh }, "del"))
            { AudioPlayButton(); ZenUndoPush(); delBind = bi; }
            ly += rh + 2;

            for (int k = 0; k < pp->keyCount; k++)
            {
                float u = pp->keys[k].t;
                bool sel = (zen.sigPosSelElem == e && zen.sigPosSelSlot == s &&
                            fabsf(zen.sigPosSelU - u) <= ZEN_SIG_U_EPS);
                Rectangle kr = { list.x+26, ly, list.width-26, 20 };
                if (GuiButton(kr, TextFormat("%.2f   +(%.2f, %.2f)   %s", u,
                              pp->keys[k].offX, pp->keys[k].offY,
                              AnimEaseName(pp->keys[k].ease))))
                { AudioPlayButton(); zen.sigPosSelElem = e; zen.sigPosSelSlot = s;
                  zen.sigPosSelU = u; }
                if (sel) DrawRectangleRec(kr, (Color){ 60, 90, 140, 120 });
                ly += 22;
            }

            if (zen.sigPosSelElem == e && zen.sigPosSelSlot == s)
            {
                int k = SigPosKeyNear(pp, zen.sigPosSelU);
                if (k >= 0)
                {
                    AnimPosKey *kk = &pp->keys[k];
                    float u = kk->t, ox = kk->offX, oy = kk->offY;
                    GuiLabel((Rectangle){ list.x+14, ly, 40, rh }, "u");
                    if (ZenEditSlider((Rectangle){ list.x+58, ly, list.width-58-56, rh },
                                      "", &u, 0.0f, 1.0f))
                    { kk->t = u; SigPosSortKeys(pp); zen.sigPosSelU = u; zen.sigLastU = u; }
                    ly += rh + gap;
                    GuiLabel((Rectangle){ list.x+14, ly, 40, rh }, "off x");
                    if (ZenEditSlider((Rectangle){ list.x+58, ly, list.width-58-56, rh },
                                      "", &ox, -1.0f, 1.0f)) kk->offX = ox;
                    ly += rh + gap;
                    GuiLabel((Rectangle){ list.x+14, ly, 40, rh }, "off y");
                    if (ZenEditSlider((Rectangle){ list.x+58, ly, list.width-58-56, rh },
                                      "", &oy, -1.0f, 1.0f)) kk->offY = oy;
                    ly += rh + gap;
                    GuiLabel((Rectangle){ list.x+14, ly, 40, rh }, "ease");
                    if (GuiButton((Rectangle){ list.x+58, ly, 120, rh },
                                  AnimEaseName(kk->ease)))
                    { AudioPlayButton(); ZenUndoPush();
                      kk->ease = (kk->ease + 1) % AnimEaseCount(); }
                    if (GuiButton((Rectangle){ list.x+list.width-50, ly, 50, rh }, "del"))
                    { AudioPlayButton(); ZenUndoPush(); delKeyBind = bi; delKeyIdx = k; }
                    ly += rh + gap;
                }
            }
        }
    }
    ly += gap;

    if (delKeyBind >= 0)
        SigPosRemoveKeyAt(&sg->posParams[delKeyBind], delKeyIdx);
    else if (delBind >= 0)
    { SigPosRemoveAt(sg, delBind); zen.sigPosSelElem = -1; }
    return ly;
}

static float DrawSigSequenceSection(AnimSignal *sg, Rectangle list, float ly,
                                    float rh, float gap)
{
    DrawRectangleRec((Rectangle){ list.x, ly, list.width, rh },
                     (Color){ 44, 52, 60, 255 });
    ZenLabelTip((Rectangle){ list.x+6, ly+4, 200, rh-8 }, "sequence  (seq x mult)",
                "Per-instance offset: seq number x mult x envelope(u)");
    GuiLabel((Rectangle){ list.x+list.width-210, ly, 34, rh }, "mult");
    ZenEditSlider((Rectangle){ list.x+list.width-172, ly+2, 168, rh-4 }, "",
                  &sg->seqMult, -30.0f, 30.0f);
    ly += rh + 2;

    for (int e = 0; e < zen.doc.elemCount; e++)
    {
        int kind = zen.doc.elems[e].kind, grpN = AnimGroupCountFor(kind);
        for (int gi = 0; gi < grpN; gi++)
        {
            if (!SigSeqGroupHasScalar(kind, gi)) continue;
            const AnimPropGroup *g = AnimGroupAt(kind, gi);
            bool on = SigSeqGroupOn(sg, e, kind, gi), want = on;
            GuiCheckBox((Rectangle){ list.x+20, ly+2, 16, 16 },
                        TextFormat("%s . %s", zen.doc.elems[e].name, g->name), &want);
            if (want != on)
            { AudioPlayButton(); ZenUndoPush(); SigSeqGroupSet(sg, e, kind, gi, want); }
            ly += rh;
        }
    }

    if (GuiButton((Rectangle){ list.x+14, ly, 60, rh }, "+ key"))
    { AudioPlayButton(); ZenUndoPush();
      float nu = SigSeqFreeU(sg, zen.sigLastU); SigSeqWriteKey(sg, nu);
      zen.sigSeqSelU = nu; }
    ly += rh + 2;

    int delKey = -1;
    for (int k = 0; k < sg->seqKeyCount; k++)
    {
        float u = sg->seqKeys[k].t;
        bool sel = fabsf(zen.sigSeqSelU - u) <= ZEN_SIG_U_EPS;
        Rectangle kr = { list.x+26, ly, list.width-26, 20 };
        if (GuiButton(kr, TextFormat("%.2f   amt %.2f   %s", u, sg->seqKeys[k].amt,
                                     AnimEaseName(sg->seqKeys[k].ease))))
        { AudioPlayButton(); zen.sigSeqSelU = u; }
        if (sel) DrawRectangleRec(kr, (Color){ 60, 90, 140, 120 });
        ly += 22;
    }
    int sk = SigSeqKeyNear(sg, zen.sigSeqSelU);
    if (sk >= 0)
    {
        AnimSeqKey *kk = &sg->seqKeys[sk];
        float u = kk->t, amt = kk->amt;
        GuiLabel((Rectangle){ list.x+14, ly, 40, rh }, "u");
        if (ZenEditSlider((Rectangle){ list.x+58, ly, list.width-58-56, rh }, "",
                          &u, 0.0f, 1.0f))
        { kk->t = u; SigSeqSortKeys(sg); zen.sigSeqSelU = u; zen.sigLastU = u; }
        ly += rh + gap;
        GuiLabel((Rectangle){ list.x+14, ly, 40, rh }, "amt");
        if (ZenEditSlider((Rectangle){ list.x+58, ly, list.width-58-56, rh }, "",
                          &amt, 0.0f, 1.0f)) kk->amt = amt;
        ly += rh + gap;
        GuiLabel((Rectangle){ list.x+14, ly, 40, rh }, "ease");
        if (GuiButton((Rectangle){ list.x+58, ly, 120, rh }, AnimEaseName(kk->ease)))
        { AudioPlayButton(); ZenUndoPush(); kk->ease = (kk->ease + 1) % AnimEaseCount(); }
        if (GuiButton((Rectangle){ list.x+list.width-50, ly, 50, rh }, "del"))
        { AudioPlayButton(); ZenUndoPush(); delKey = sk; }
        ly += rh + gap;
    }
    ly += gap;
    if (delKey >= 0) { SigSeqRemoveKeyAt(sg, delKey); zen.sigSeqSelU = -1.0f; }
    return ly;
}

// ---------------------------------------------------------------------------
//  Playback fire panel: while anything plays, every signal gets a Fire button
//  (+ number-key shortcuts) so it can be triggered live.
// ---------------------------------------------------------------------------
static void DrawPlaybackSignals(void)
{
    bool playbackUi = zen.playing || !AnimSignalPlayerDone(&zen.preview);
    if (!playbackUi || zen.doc.signalCount <= 0) return;

    ScreenState *ss = ScreenStateGet();
    float W = (float)ss->width;
    float mw = 240, rh = 26.0f, gap = 4.0f, pad = 8.0f, headH = 18.0f;
    float mh = pad*2 + headH + 2 + zen.doc.signalCount*(rh + gap);
    Rectangle m = { W - mw - 12, ZEN_MENU_BAR_H + 26, mw, mh };
    DrawRectangleRec(m, (Color){ 40, 42, 48, 235 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 90, 94, 104, 255 });

    GuiLabel((Rectangle){ m.x+10, m.y+pad, mw-20, headH }, "SIGNALS  (fire live: keys 1-4)");
    float y = m.y + pad + headH + 2;
    for (int i = 0; i < zen.doc.signalCount; i++)
    {
        AnimSignal *sg = &zen.doc.signals[i];
        bool keyed = (i < 4);
        GuiLabel((Rectangle){ m.x+12, y, mw-70, rh },
                 TextFormat("%s%s%s", keyed ? TextFormat("[%d] ", i+1) : "",
                            sg->name, sg->usesPos ? "  (@mouse)" : ""));
        bool fire = GuiButton((Rectangle){ m.x+mw-58, y+2, 48, rh-4 }, "Fire");
        if (keyed && IsKeyPressed(KEY_ONE + i)) fire = true;
        if (fire) { AudioPlayButton(); ZenFireSignal(sg); }
        y += rh + gap;
    }
}

// ---------------------------------------------------------------------------
//  The modal itself.
// ---------------------------------------------------------------------------
void ZenSignalModalGui(void)
{
    DrawPlaybackSignals();

    if (zen.sigModalIdx < 0 || zen.sigModalIdx >= zen.doc.signalCount)
    { zen.sigModalIdx = -1; zen.sigModalRect = (Rectangle){0}; return; }

    bool playbackUi = zen.playing || !AnimSignalPlayerDone(&zen.preview);
    if (playbackUi) { zen.sigModalRect = (Rectangle){0}; return; }

    AnimSignal *sg = &zen.doc.signals[zen.sigModalIdx];
    ScreenState *ss = ScreenStateGet();
    float W = (float)ss->width, H = (float)ss->height;

    if (zen.sigModalPos.x == 0 && zen.sigModalPos.y == 0)
        zen.sigModalPos = (Vector2){ (W - SM_W) * 0.5f, (H - SM_H) * 0.5f };

    Rectangle m = { zen.sigModalPos.x, zen.sigModalPos.y, SM_W, SM_H };
    if (m.x < 0) m.x = 0; if (m.y < 0) m.y = 0;
    if (m.x + m.width  > W) m.x = W - m.width;
    if (m.y + m.height > H) m.y = H - m.height;
    zen.sigModalPos = (Vector2){ m.x, m.y };
    zen.sigModalRect = m;

    // title-bar drag.
    Rectangle title = { m.x, m.y, m.width - 24, SM_TITLE_H };
    Vector2 mouse = GetMousePosition();
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, title))
    { zen.sigDragging = true;
      zen.sigDragOff = (Vector2){ mouse.x - m.x, mouse.y - m.y }; }
    if (zen.sigDragging)
    {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
            zen.sigModalPos = (Vector2){ mouse.x - zen.sigDragOff.x,
                                         mouse.y - zen.sigDragOff.y };
        else zen.sigDragging = false;
    }

    DrawRectangleRec(m, (Color){ 40, 42, 48, 250 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 110, 114, 126, 255 });
    DrawRectangleRec(title, (Color){ 52, 56, 66, 255 });
    ZenLabelTip((Rectangle){ m.x + 8, m.y + 3, title.width - 8, 18 },
                TextFormat("SIGNAL  %s", sg->name),
                "Drag this bar to move; the editor behind stays usable");
    if (GuiButton((Rectangle){ m.x + m.width - 22, m.y + 2, 20, 20 }, "x"))
    {
        AudioPlayButton();
        zen.sigModalIdx = -1; zen.edSigIdx = -1;
        ZenSigCloseDrops(); ZenSigClearKeySel();
        return;
    }

    float rh = 24.0f, gap = 6.0f;
    float x = m.x + 14, w = m.width - 28, y = m.y + SM_TITLE_H + 8;

    // an expanded modal dropdown locks the rows underneath it.
    bool sigDropOpen = zen.sigDropMode != ZEN_SIGDROP_NONE;
    if (sigDropOpen) GuiLock();

    GuiLabel((Rectangle){ x, y, 44, rh }, "name");
    if (GuiTextBox((Rectangle){ x+44, y, 140, rh }, sg->name, ANIM_NAME_MAX,
                   zen.edSigIdx == zen.sigModalIdx))
    {
        if (zen.edSigIdx != zen.sigModalIdx) { ZenUndoPush(); zen.edSigIdx = zen.sigModalIdx; }
        else { zen.edSigIdx = -1; ZenReRegisterSignals(); }  // name change = rebind
    }
    GuiLabel((Rectangle){ x+196, y, 46, rh }, "length");
    ZenEditSlider((Rectangle){ x+244, y, w-244-52, rh }, "", &sg->length, 0.0f, 10.0f);
    y += rh + 4;
    ZenLabelTip((Rectangle){ x, y, w-220, 16 },
                sg->length <= 0.0f ? "0.00 = instant snap to the final keys"
                                   : "keys are fractions of the length (0..1)", NULL);
    y += 18;

    // flags in one row of checkboxes.
    bool terminal = sg->terminal;
    GuiCheckBox((Rectangle){ x, y, 16, 16 }, "terminal", &terminal);
    ZenTip((Rectangle){ x, y, 90, 18 }, "Tells the in-game player to stop the animation after this signal");
    if (terminal != sg->terminal)
    { AudioPlayButton(); ZenUndoPush(); sg->terminal = terminal; }

    bool usesPos = sg->usesPos;
    GuiCheckBox((Rectangle){ x + 110, y, 16, 16 }, "position param", &usesPos);
    ZenTip((Rectangle){ x + 110, y, 130, 18 }, "This signal consumes the emit location (fires at the mouse)");
    if (usesPos != sg->usesPos)
    { AudioPlayButton(); ZenUndoPush(); sg->usesPos = usesPos; }

    bool usesSeq = sg->usesSeq;
    GuiCheckBox((Rectangle){ x + 268, y, 16, 16 }, "sequence", &usesSeq);
    ZenTip((Rectangle){ x + 268, y, 110, 18 }, "This signal consumes the instance's sequence number");
    if (usesSeq != sg->usesSeq)
    { AudioPlayButton(); ZenUndoPush(); sg->usesSeq = usesSeq; }

    if (sg->usesSeq)
    {
        GuiLabel((Rectangle){ x + 390, y, 30, 18 }, "inst");
        if (GuiButton((Rectangle){ x + 422, y, 20, 18 }, "-"))
        { AudioPlayButton(); if (zen.previewSeq > 0) zen.previewSeq--;
          zen.preview.seq = zen.previewSeq; }
        if (GuiButton((Rectangle){ x + 444, y, 20, 18 }, "+"))
        { AudioPlayButton(); zen.previewSeq++; zen.preview.seq = zen.previewSeq; }
        GuiLabel((Rectangle){ x + 470, y, 40, 18 }, TextFormat("%d", zen.previewSeq));
    }
    y += 22;
    GuiLine((Rectangle){ x, y, w, 8 }, "targets"); y += 12;

    // --- scrolling list ------------------------------------------------------
    Rectangle list = { x, y, w, m.y + m.height - 48 - y };
    bool ctrlDown = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (!ctrlDown && CheckCollisionPointRec(mouse, list))
        zen.sigScroll += GetMouseWheelMove() * 24.0f;

    int  pendingDelElem = -1, pendingDelGroup = -1;

    bool inList = CheckCollisionPointRec(mouse, list);
    if (!inList && !sigDropOpen) GuiLock();

    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    float ly = list.y + zen.sigScroll;

    if (sg->usesPos) ly = DrawSigParamsSection(sg, list, ly, rh, gap);
    if (sg->usesSeq) ly = DrawSigSequenceSection(sg, list, ly, rh, gap);
    if (sg->usesPos || sg->usesSeq)
    { GuiLine((Rectangle){ list.x, ly, list.width, 8 }, "targets"); ly += 12; }

    // one block per DOCUMENT element; its authored groups nest under it.
    for (int e = 0; e < zen.doc.elemCount; e++)
    {
        int kind = zen.doc.elems[e].kind;

        DrawRectangleRec((Rectangle){ list.x, ly, list.width, rh },
                         (Color){ 52, 55, 62, 255 });
        ZenLabelTip((Rectangle){ list.x+6, ly+4, 180, rh-8 },
                    zen.doc.elems[e].name, NULL);

        bool sigFull = sg->targetCount >= ANIM_SIG_TARGETS_MAX;
        Rectangle addR = { list.x+list.width-86, ly, 82, rh };
        if (zen.sigDropMode == ZEN_SIGDROP_ADD && zen.sigDropElem == e)
            zen.sigDropHdr = addR;
        if (sigFull) GuiSetState(STATE_DISABLED);
        bool addHit = GuiButton(addR, sigFull ? "full" : "+ track");
        if (sigFull) GuiSetState(STATE_NORMAL);
        if (!sigFull && addHit)
        {
            AudioPlayButton();
            if (zen.sigDropMode == ZEN_SIGDROP_ADD && zen.sigDropElem == e)
                ZenSigCloseDrops();
            else { zen.sigDropMode = ZEN_SIGDROP_ADD; zen.sigDropElem = e;
                   zen.sigDropHdr = addR; }
        }
        ly += rh + 2;

        int grpN = AnimGroupCountFor(kind);
        for (int gi = 0; gi < grpN; gi++)
        {
            if (!ZenSigGroupHasTarget(sg, e, gi)) continue;
            const AnimPropGroup *g = AnimGroupAt(kind, gi);
            float times[ZEN_SIG_TIMES_MAX];
            int   nt = ZenSigGroupKeyTimes(sg, e, gi, times);

            ZenLabelTip((Rectangle){ list.x+14, ly+4, list.width-160, rh-8 },
                        TextFormat("%s (%d)", g->name, nt),
                        "Keys open in the track modal");

            if (GuiButton((Rectangle){ list.x+list.width-104, ly, 50, rh }, "+key"))
            {
                AudioPlayButton(); ZenUndoPush();
                float nu = ZenSigGroupFreeU(sg, e, gi, zen.sigLastU);
                ZenSigGroupWriteKey(sg, e, gi, nu);
                zen.sigSelElem = e; zen.sigSelGroup = gi; zen.sigSelU = nu;
                ZenTrackModalOpenSig(zen.sigModalIdx);
            }
            if (GuiButton((Rectangle){ list.x+list.width-50, ly, 50, rh }, "del"))
            { AudioPlayButton(); ZenUndoPush(); pendingDelElem = e; pendingDelGroup = gi; }
            ly += rh + 2;

            int colorProp = ZenGroupColorProp(kind, gi);
            for (int i = 0; i < nt; i++)
            {
                float u = times[i];
                bool sel = (zen.sigSelElem == e && zen.sigSelGroup == gi &&
                            fabsf(zen.sigSelU - u) <= ZEN_SIG_U_EPS);
                Rectangle kr = { list.x+26, ly, list.width-26, 20 };
                bool pressed = GuiButton(kr, ZenSigGroupKeyLabel(sg, e, gi, u));
                if (sel) DrawRectangleRec(kr, (Color){ 60, 90, 140, 120 });
                if (colorProp >= 0)
                {
                    AnimSigTarget *ct = ZenSigFindTarget(sg, e, colorProp);
                    int ck = ZenSigTargetKeyNear(ct, u);
                    if (ck >= 0)
                    {
                        Color sc = ct->keys[ck].cval; sc.a = 255;
                        ZenDrawSwatch((Rectangle){ kr.x+kr.width-20, kr.y+2, 16, 16 }, sc);
                    }
                }
                if (pressed)
                {
                    // click selects AND opens the shared track modal on it.
                    AudioPlayButton();
                    zen.sigSelElem = e; zen.sigSelGroup = gi; zen.sigSelU = u;
                    zen.sigLastU = u;
                    ZenTrackModalOpenSig(zen.sigModalIdx);
                }
                ly += 22;
            }
            ly += 4;
        }
        ly += gap;
    }

    EndScissorMode();
    if (!inList && !sigDropOpen) GuiUnlock();

    float contentH = (ly - (list.y + zen.sigScroll));
    float maxScroll = contentH - list.height;
    if (maxScroll < 0) maxScroll = 0;
    if (zen.sigScroll < -maxScroll) zen.sigScroll = -maxScroll;
    if (zen.sigScroll > 0) zen.sigScroll = 0;

    if (pendingDelGroup >= 0)
    {
        ZenSigGroupDeleteTargets(sg, pendingDelElem, pendingDelGroup);
        if (zen.sigSelElem == pendingDelElem && zen.sigSelGroup == pendingDelGroup)
            ZenSigClearKeySel();
        ZenSigCloseDrops();
    }

    // footer.
    float bh = 26, by = m.y + m.height - bh - 10;
    if (GuiButton((Rectangle){ x, by, 70, bh }, "Fire"))
    { AudioPlayButton(); ZenFireSignal(sg); }
    GuiLabel((Rectangle){ x+80, by, m.width-200, bh }, "fires from the CURRENT pose");

    if (sigDropOpen) GuiUnlock();
}

// The modal's group-adder dropdown, drawn topmost.
void ZenSignalOverlaysGui(void)
{
    if (zen.sigModalIdx < 0 || zen.sigModalIdx >= zen.doc.signalCount) return;
    if (zen.playing || !AnimSignalPlayerDone(&zen.preview)) return;
    if (zen.sigDropMode != ZEN_SIGDROP_ADD) return;
    if (zen.sigDropElem < 0 || zen.sigDropElem >= zen.doc.elemCount)
    { ZenSigCloseDrops(); return; }

    AnimSignal *sg = &zen.doc.signals[zen.sigModalIdx];
    ScreenState *ss = ScreenStateGet();
    float H = (float)ss->height;

    int kind  = zen.doc.elems[zen.sigDropElem].kind;
    int count = AnimGroupCountFor(kind);
    if (count <= 0) return;

    Rectangle hdr = zen.sigDropHdr;
    float ih = 20.0f, listH = ih * count;
    float ly = (hdr.y + hdr.height + listH <= H - 4.0f)
             ? hdr.y + hdr.height : hdr.y - listH;
    if (ly < 4.0f) ly = 4.0f;
    Rectangle bg = { hdr.x, ly, hdr.width, listH };
    DrawRectangleRec(bg, (Color){ 32, 34, 40, 255 });
    DrawRectangleLinesEx(bg, 1.0f, (Color){ 70, 74, 84, 255 });

    int picked = -1;
    for (int i = 0; i < count; i++)
    {
        Rectangle rr = { bg.x, bg.y + i*ih, bg.width, ih };
        const AnimPropGroup *g = AnimGroupAt(kind, i);
        bool fits = ZenSigGroupFits(sg, zen.sigDropElem, i);
        if (!fits) GuiSetState(STATE_DISABLED);
        if (GuiButton(rr, g ? g->name : "?") && fits) picked = i;
        if (!fits) GuiSetState(STATE_NORMAL);
    }

    if (picked >= 0)
    {
        AudioPlayButton(); ZenUndoPush();
        ZenSigGroupAddTargets(sg, zen.sigDropElem, picked);
        zen.sigSelElem = zen.sigDropElem; zen.sigSelGroup = picked;
        zen.sigSelU = zen.sigLastU;
        ZenSigCloseDrops();
    }
    else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
             !CheckCollisionPointRec(GetMousePosition(), bg) &&
             !CheckCollisionPointRec(GetMousePosition(), hdr))
        ZenSigCloseDrops();
}
