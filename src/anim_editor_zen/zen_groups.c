// ============================================================================
//  zen_groups.c  -  coordinated GROUP editing over the per-prop tracks
//
//  The panels and modals present a few logical tracks (Position, Color,
//  Outline, ...) instead of raw per-prop tracks; these helpers keep a
//  group's member tracks in lockstep at shared key times. Storage stays one
//  track per property - this is purely coordinated editing.
//
//  Also owns the ctx-level track/key SELECTION: the zen editor selects a
//  (element, group) track plus a SET of group-key times, which is what the
//  track modal bulk-edits.
// ============================================================================

#include "raylib.h"
#include "zen_internal.h"
#include <math.h>
#include <string.h>

// t is the first float of AnimKey: address keys generically by (base, stride).
#define KEY_T_AT(base, stride, i) (*(const float *)((const char *)(base) + (size_t)(i)*(stride)))

static int KeyIndexNear(const void *keys, int count, size_t stride, float t, float eps)
{
    for (int i = 0; i < count; i++)
        if (fabsf(KEY_T_AT(keys, stride, i) - t) <= eps) return i;
    return -1;
}

static void KeySortByT(void *keys, int count, size_t stride)
{
    unsigned char tmp[sizeof(AnimKey)];
    char *a = (char *)keys;
    for (int i = 1; i < count; i++)
    {
        memcpy(tmp, a + (size_t)i*stride, stride);
        float vt; memcpy(&vt, tmp, sizeof vt);
        int j = i - 1;
        while (j >= 0 && KEY_T_AT(keys, stride, j) > vt)
        { memcpy(a + (size_t)(j+1)*stride, a + (size_t)j*stride, stride); j--; }
        memcpy(a + (size_t)(j+1)*stride, tmp, stride);
    }
}

static void KeyRemoveAt(void *keys, int *count, size_t stride, int idx)
{
    char *a = (char *)keys;
    for (int i = idx; i < *count - 1; i++)
        memcpy(a + (size_t)i*stride, a + (size_t)(i+1)*stride, stride);
    (*count)--;
}

// ---------------------------------------------------------------------------
//  Group ops (element-track side).
// ---------------------------------------------------------------------------
int ZenGroupKeyTimes(AnimElem *e, int gi, float *out)
{
    const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
    int n = 0;
    for (int m = 0; g && m < g->propCount; m++)
    {
        AnimTrack *tr = AnimElemFindTrack(e, g->props[m]);
        if (!tr) continue;
        for (int k = 0; k < tr->keyCount; k++)
        {
            float t = tr->keys[k].t;
            bool dup = false;
            for (int i = 0; i < n; i++)
                if (fabsf(out[i] - t) <= ZEN_AUTOKEY_EPS) { dup = true; break; }
            if (!dup && n < ZEN_GROUP_TIMES_MAX) out[n++] = t;
        }
    }
    KeySortByT(out, n, sizeof(float));
    return n;
}

bool ZenGroupHasTrack(AnimElem *e, int gi)
{
    const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
    if (!g) return false;
    for (int m = 0; m < g->propCount; m++)
        if (AnimElemFindTrack(e, g->props[m])) return true;
    return false;
}

// Write a group key at t: every member gets one, seeded from the element's
// current value there (creating member tracks + a zero key as needed).
void ZenGroupWriteKey(AnimElem *e, int gi, float t)
{
    const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
    if (!g) return;
    for (int m = 0; m < g->propCount; m++)
    {
        int prop = g->props[m];
        AnimTrack *tr = AnimElemFindTrack(e, prop);
        if (!tr) tr = AnimElemAddTrack(e, prop);
        if (!tr) continue;
        if (AnimPropIsColor(prop))
        {
            Color c = AnimElemColorProp(e, prop, t);
            if (t > ZEN_AUTOKEY_EPS) ZenEnsureZeroColorKey(e, tr);
            AnimTrackWriteColorKeyAt(tr, t, c, ZEN_AUTOKEY_EPS);
        }
        else
        {
            float v = AnimElemProp(e, prop, t);
            if (t > ZEN_AUTOKEY_EPS) ZenEnsureZeroKey(e, tr);
            AnimTrackWriteKeyAt(tr, t, v, ZEN_AUTOKEY_EPS);
        }
    }
}

void ZenGroupDeleteTracks(AnimElem *e, int gi)
{
    const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
    if (!g) return;
    for (int m = 0; m < g->propCount; m++)
        for (int i = 0; i < e->trackCount; i++)
            if (e->tracks[i].prop == g->props[m]) { AnimElemRemoveTrack(e, i); break; }
}

void ZenGroupDeleteKeyAt(AnimElem *e, int gi, float t)
{
    const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
    for (int m = 0; g && m < g->propCount; m++)
    {
        AnimTrack *tr = AnimElemFindTrack(e, g->props[m]);
        if (!tr) continue;
        for (int k = tr->keyCount - 1; k >= 0; k--)
            if (fabsf(tr->keys[k].t - t) <= ZEN_AUTOKEY_EPS)
                KeyRemoveAt(tr->keys, &tr->keyCount, sizeof(AnimKey), k);
    }
}

void ZenGroupMoveKeyTo(AnimElem *e, int gi, float oldT, float newT)
{
    const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
    for (int m = 0; g && m < g->propCount; m++)
    {
        AnimTrack *tr = AnimElemFindTrack(e, g->props[m]);
        int k = tr ? KeyIndexNear(tr->keys, tr->keyCount, sizeof(AnimKey),
                                  oldT, ZEN_AUTOKEY_EPS) : -1;
        if (k < 0) continue;
        tr->keys[k].t = newT;
        KeySortByT(tr->keys, tr->keyCount, sizeof(AnimKey));
    }
}

void ZenGroupSetEaseAt(AnimElem *e, int gi, float t, int ease)
{
    const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
    for (int m = 0; g && m < g->propCount; m++)
    {
        AnimTrack *tr = AnimElemFindTrack(e, g->props[m]);
        int k = tr ? KeyIndexNear(tr->keys, tr->keyCount, sizeof(AnimKey),
                                  t, ZEN_AUTOKEY_EPS) : -1;
        if (k >= 0) tr->keys[k].ease = ease;
    }
}

// Representative ease of the group key at t: the first member key found.
int ZenGroupEase(AnimElem *e, int gi, float t)
{
    const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
    for (int m = 0; g && m < g->propCount; m++)
    {
        AnimTrack *tr = AnimElemFindTrack(e, g->props[m]);
        int k = tr ? KeyIndexNear(tr->keys, tr->keyCount, sizeof(AnimKey),
                                  t, ZEN_AUTOKEY_EPS) : -1;
        if (k >= 0) return tr->keys[k].ease;
    }
    return ANIM_EASE_LINEAR;
}

int ZenGroupColorProp(int kind, int gi)
{
    const AnimPropGroup *g = AnimGroupAt(kind, gi);
    for (int m = 0; g && m < g->propCount; m++)
        if (AnimPropIsColor(g->props[m])) return g->props[m];
    return -1;
}

// Compact one-line summary of a group key: "t   v0,v1,..   ease".
const char *ZenGroupKeyLabel(AnimElem *e, int gi, float t)
{
    const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
    char vals[80]; vals[0] = 0;
    for (int m = 0; g && m < g->propCount; m++)
    {
        int prop = g->props[m];
        const char *one;
        if (AnimPropIsColor(prop))
        {
            Color c = AnimElemColorProp(e, prop, t);
            one = TextFormat("#%02X%02X%02X", c.r, c.g, c.b);
        }
        else one = TextFormat("%.2f", AnimElemProp(e, prop, t));
        if (m) strncat(vals, ",", sizeof(vals)-strlen(vals)-1);
        strncat(vals, one, sizeof(vals)-strlen(vals)-1);
    }
    return TextFormat("%.2f   %s   %s", t, vals,
                      AnimEaseName(ZenGroupEase(e, gi, t)));
}

// ---------------------------------------------------------------------------
//  Signal-target groups: the same coordinated editing over a signal's
//  (element, property) targets, keyed in NORMALIZED u (0..1) - hence their
//  own epsilon (ZEN_SIG_U_EPS; ZEN_AUTOKEY_EPS is seconds).
// ---------------------------------------------------------------------------
AnimSigTarget *ZenSigFindTarget(AnimSignal *sg, int elemIdx, int prop)
{
    for (int i = 0; i < sg->targetCount; i++)
        if (sg->targets[i].elemIdx == elemIdx && sg->targets[i].prop == prop)
            return &sg->targets[i];
    return NULL;
}

int ZenSigTargetKeyNear(const AnimSigTarget *tg, float u)
{
    return tg ? KeyIndexNear(tg->keys, tg->keyCount, sizeof(AnimKey), u, ZEN_SIG_U_EPS) : -1;
}

void ZenSigTargetSortKeys(AnimSigTarget *tg)
{
    KeySortByT(tg->keys, tg->keyCount, sizeof(AnimKey));
}

bool ZenSigGroupHasTarget(AnimSignal *sg, int elemIdx, int gi)
{
    const AnimPropGroup *g = AnimGroupAt(zen.doc.elems[elemIdx].kind, gi);
    if (!g) return false;
    for (int m = 0; m < g->propCount; m++)
        if (ZenSigFindTarget(sg, elemIdx, g->props[m])) return true;
    return false;
}

// Free slots vs the members this group still needs: a 3-prop group can be
// refused while a 1-prop one still fits.
bool ZenSigGroupFits(AnimSignal *sg, int elemIdx, int gi)
{
    const AnimPropGroup *g = AnimGroupAt(zen.doc.elems[elemIdx].kind, gi);
    if (!g) return false;
    int need = 0;
    for (int m = 0; m < g->propCount; m++)
        if (!ZenSigFindTarget(sg, elemIdx, g->props[m])) need++;
    return sg->targetCount + need <= ANIM_SIG_TARGETS_MAX;
}

int ZenSigGroupKeyTimes(AnimSignal *sg, int elemIdx, int gi, float *out)
{
    const AnimPropGroup *g = AnimGroupAt(zen.doc.elems[elemIdx].kind, gi);
    int n = 0;
    for (int m = 0; g && m < g->propCount; m++)
    {
        AnimSigTarget *tg = ZenSigFindTarget(sg, elemIdx, g->props[m]);
        if (!tg) continue;
        for (int k = 0; k < tg->keyCount; k++)
        {
            float u = tg->keys[k].t;
            bool dup = false;
            for (int i = 0; i < n; i++)
                if (fabsf(out[i] - u) <= ZEN_SIG_U_EPS) { dup = true; break; }
            if (!dup && n < ZEN_SIG_TIMES_MAX) out[n++] = u;
        }
    }
    KeySortByT(out, n, sizeof(float));
    return n;
}

// Pick a u for a NEW key: prefer `pref` (the last u the user set anywhere);
// taken -> the midpoint of the largest free gap in [0,1].
float ZenSigFreeU(const float *times, int nt, float pref)
{
    if (nt == 0) return pref;

    bool taken = false;
    for (int i = 0; i < nt; i++)
        if (fabsf(times[i] - pref) <= ZEN_SIG_U_EPS) { taken = true; break; }
    if (!taken) return pref;

    float lo = 0.0f, hi = times[0], gap = times[0];
    for (int i = 0; i + 1 < nt; i++)
        if (times[i+1] - times[i] > gap)
        { gap = times[i+1] - times[i]; lo = times[i]; hi = times[i+1]; }
    if (1.0f - times[nt-1] > gap) { lo = times[nt-1]; hi = 1.0f; }
    return (lo + hi) * 0.5f;
}

float ZenSigGroupFreeU(AnimSignal *sg, int elemIdx, int gi, float pref)
{
    float times[ZEN_SIG_TIMES_MAX];
    int nt = ZenSigGroupKeyTimes(sg, elemIdx, gi, times);
    return ZenSigFreeU(times, nt, pref);
}

// Write a group key at u, seeded from the element's CURRENT pose (a signal
// key is an absolute destination; the pose under the playhead is what the
// user is looking at while authoring).
void ZenSigGroupWriteKey(AnimSignal *sg, int elemIdx, int gi, float u)
{
    const AnimElem *el = &zen.doc.elems[elemIdx];
    const AnimPropGroup *g = AnimGroupAt(el->kind, gi);
    if (!g) return;
    for (int m = 0; m < g->propCount; m++)
    {
        int prop = g->props[m];
        AnimSigTarget *tg = ZenSigFindTarget(sg, elemIdx, prop);
        if (!tg)
        {
            if (sg->targetCount >= ANIM_SIG_TARGETS_MAX) continue;
            tg = &sg->targets[sg->targetCount++];
            *tg = (AnimSigTarget){0};
            tg->elemIdx = elemIdx; tg->prop = prop;
        }
        int k = ZenSigTargetKeyNear(tg, u);
        if (k < 0)
        {
            if (tg->keyCount >= ANIM_SIG_KEYS_MAX) continue;
            k = tg->keyCount++;
            tg->keys[k].ease = ANIM_EASE_SINE_OUT;
        }
        tg->keys[k].t     = u;
        tg->keys[k].value = AnimElemProp(el, prop, zen.playhead);
        tg->keys[k].cval  = AnimElemColorProp(el, prop, zen.playhead);
        ZenSigTargetSortKeys(tg);
    }
}

// Create the group's missing member targets without keying anything.
void ZenSigGroupAddTargets(AnimSignal *sg, int elemIdx, int gi)
{
    const AnimPropGroup *g = AnimGroupAt(zen.doc.elems[elemIdx].kind, gi);
    if (!g) return;
    for (int m = 0; m < g->propCount; m++)
    {
        if (ZenSigFindTarget(sg, elemIdx, g->props[m])) continue;
        if (sg->targetCount >= ANIM_SIG_TARGETS_MAX) return;
        AnimSigTarget *tg = &sg->targets[sg->targetCount++];
        *tg = (AnimSigTarget){0};
        tg->elemIdx = elemIdx; tg->prop = g->props[m];
    }
}

void ZenSigGroupDeleteTargets(AnimSignal *sg, int elemIdx, int gi)
{
    const AnimPropGroup *g = AnimGroupAt(zen.doc.elems[elemIdx].kind, gi);
    if (!g) return;
    for (int i = sg->targetCount - 1; i >= 0; i--)
    {
        if (sg->targets[i].elemIdx != elemIdx) continue;
        bool member = false;
        for (int m = 0; m < g->propCount; m++)
            if (sg->targets[i].prop == g->props[m]) { member = true; break; }
        if (!member) continue;
        for (int j = i; j < sg->targetCount - 1; j++) sg->targets[j] = sg->targets[j+1];
        sg->targetCount--;
    }
}

void ZenSigGroupDeleteKeyAt(AnimSignal *sg, int elemIdx, int gi, float u)
{
    const AnimPropGroup *g = AnimGroupAt(zen.doc.elems[elemIdx].kind, gi);
    for (int m = 0; g && m < g->propCount; m++)
    {
        AnimSigTarget *tg = ZenSigFindTarget(sg, elemIdx, g->props[m]);
        if (!tg) continue;
        for (int k = tg->keyCount - 1; k >= 0; k--)
            if (fabsf(tg->keys[k].t - u) <= ZEN_SIG_U_EPS)
                KeyRemoveAt(tg->keys, &tg->keyCount, sizeof(AnimKey), k);
    }
}

void ZenSigGroupMoveKeyTo(AnimSignal *sg, int elemIdx, int gi, float oldU, float newU)
{
    const AnimPropGroup *g = AnimGroupAt(zen.doc.elems[elemIdx].kind, gi);
    for (int m = 0; g && m < g->propCount; m++)
    {
        AnimSigTarget *tg = ZenSigFindTarget(sg, elemIdx, g->props[m]);
        int k = ZenSigTargetKeyNear(tg, oldU);
        if (k < 0) continue;
        tg->keys[k].t = newU;
        ZenSigTargetSortKeys(tg);
    }
}

void ZenSigGroupSetEaseAt(AnimSignal *sg, int elemIdx, int gi, float u, int ease)
{
    const AnimPropGroup *g = AnimGroupAt(zen.doc.elems[elemIdx].kind, gi);
    for (int m = 0; g && m < g->propCount; m++)
    {
        AnimSigTarget *tg = ZenSigFindTarget(sg, elemIdx, g->props[m]);
        int k = ZenSigTargetKeyNear(tg, u);
        if (k >= 0) tg->keys[k].ease = ease;
    }
}

int ZenSigGroupEase(AnimSignal *sg, int elemIdx, int gi, float u)
{
    const AnimPropGroup *g = AnimGroupAt(zen.doc.elems[elemIdx].kind, gi);
    for (int m = 0; g && m < g->propCount; m++)
    {
        AnimSigTarget *tg = ZenSigFindTarget(sg, elemIdx, g->props[m]);
        int k = ZenSigTargetKeyNear(tg, u);
        if (k >= 0) return tg->keys[k].ease;
    }
    return ANIM_EASE_LINEAR;
}

// "u   v0,v1,..   ease" - reads the TARGET keys (absolute destinations).
const char *ZenSigGroupKeyLabel(AnimSignal *sg, int elemIdx, int gi, float u)
{
    const AnimPropGroup *g = AnimGroupAt(zen.doc.elems[elemIdx].kind, gi);
    char vals[80]; vals[0] = 0;
    for (int m = 0; g && m < g->propCount; m++)
    {
        AnimSigTarget *tg = ZenSigFindTarget(sg, elemIdx, g->props[m]);
        int k = ZenSigTargetKeyNear(tg, u);
        if (k < 0) continue;
        const char *one = AnimPropIsColor(g->props[m])
            ? TextFormat("#%02X%02X%02X", tg->keys[k].cval.r, tg->keys[k].cval.g,
                                          tg->keys[k].cval.b)
            : TextFormat("%.2f", tg->keys[k].value);
        if (vals[0]) strncat(vals, ",", sizeof(vals)-strlen(vals)-1);
        strncat(vals, one, sizeof(vals)-strlen(vals)-1);
    }
    return TextFormat("%.2f   %s   %s", u, vals,
                      AnimEaseName(ZenSigGroupEase(sg, elemIdx, gi, u)));
}

// ---------------------------------------------------------------------------
//  Track / key selection over the ctx.
// ---------------------------------------------------------------------------
void ZenSelClear(void)
{
    zen.selGroup = -1;
    zen.selKeyCount = 0;
    zen.trackModal.open = false;
}

// Select a whole track (no individual keys): the modal bulk-edits every key.
void ZenSelTrack(int elem, int gi)
{
    zen.selElem = elem;
    zen.selGroup = gi;
    zen.selKeyCount = 0;
    ZenTrackModalSync();
}

bool ZenKeyIsSelected(int elem, int gi, float t)
{
    if (elem != zen.selElem || gi != zen.selGroup) return false;
    for (int i = 0; i < zen.selKeyCount; i++)
        if (fabsf(zen.selKeys[i] - t) <= ZEN_AUTOKEY_EPS) return true;
    return false;
}

// Click / Shift+click a key. Additive toggles membership; plain click makes
// the key the only selection. Switching group/element drops the old set.
void ZenSelKey(int elem, int gi, float t, bool additive)
{
    if (elem != zen.selElem || gi != zen.selGroup || !additive)
    {
        zen.selElem = elem;
        zen.selGroup = gi;
        zen.selKeyCount = 0;
    }
    if (additive)
    {
        for (int i = 0; i < zen.selKeyCount; i++)
            if (fabsf(zen.selKeys[i] - t) <= ZEN_AUTOKEY_EPS)
            {   // toggle off
                zen.selKeys[i] = zen.selKeys[--zen.selKeyCount];
                ZenTrackModalSync();
                return;
            }
    }
    if (zen.selKeyCount < ZEN_GROUP_TIMES_MAX)
        zen.selKeys[zen.selKeyCount++] = t;
    ZenTrackModalSync();
}

// Selection must survive undo/redo/loads: drop what no longer exists.
void ZenSelValidate(void)
{
    if (zen.selElem < 0 || zen.selElem >= zen.doc.elemCount) { ZenSelClear(); return; }
    AnimElem *e = &zen.doc.elems[zen.selElem];
    if (zen.selGroup < 0) return;
    if (zen.selGroup >= AnimGroupCountFor(e->kind) ||
        !ZenGroupHasTrack(e, zen.selGroup)) { ZenSelClear(); return; }

    float times[ZEN_GROUP_TIMES_MAX];
    int nt = ZenGroupKeyTimes(e, zen.selGroup, times);
    for (int i = zen.selKeyCount - 1; i >= 0; i--)
    {
        bool found = false;
        for (int k = 0; k < nt; k++)
            if (fabsf(times[k] - zen.selKeys[i]) <= ZEN_AUTOKEY_EPS) { found = true; break; }
        if (!found) zen.selKeys[i] = zen.selKeys[--zen.selKeyCount];
    }
    ZenTrackModalSync();
}
