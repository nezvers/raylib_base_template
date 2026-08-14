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

// The group key time AT `t`, or -1 when the group has none there. Deliberately
// NOT the nearest-key rule the same-name carry uses: landing on an unrelated
// track because the old one had no counterpart should not fake a key selection
// out of whatever happens to be closest.
static bool GroupKeyTimeAt(AnimElem *e, int gi, float t, float *out)
{
    float times[ZEN_GROUP_TIMES_MAX];
    int nt = ZenGroupKeyTimes(e, gi, times);
    int k = KeyIndexNear(times, nt, sizeof(float), t, ZEN_AUTOKEY_EPS);
    if (k < 0) return false;
    *out = times[k];
    return true;
}

// Which pool slot a SHAPE_CUSTOM element shows at t: the track's stepped value
// if it has one, else the element's rest-pose name. ANIM_SHAPE_MISSING (-1) when
// neither resolves - the caller treats that as "nothing to key".
static int ShapeIdxAt(const AnimElem *e, float t)
{
    float v = AnimElemProp(e, AP_S_SHAPE, t);
    if (v >= 0.0f) return (int)(v + 0.5f);
    return AnimShapePoolFindByName(e->shapeName);
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
        else if (AnimPropIsStepped(prop))
        {
            // A stepped key holds a POOL INDEX, not a quantity. Seeding it the
            // way the branch below does would write the -1 "no pool entry" base
            // value and the key would resolve to nothing; carry what the element
            // actually shows instead. Both indices are read BEFORE any key is
            // added, since adding one changes what is sampled.
            int idx, zero;
            if (prop == AP_S_SHAPE)
            {
                // The shape pool is global, so unlike the string pool there is
                // nothing to intern - the element's name either resolves or the
                // element has no shape to key yet.
                idx  = ShapeIdxAt(e, t);
                zero = (t > ZEN_AUTOKEY_EPS && tr->keyCount == 0)
                     ? ShapeIdxAt(e, 0.0f) : -1;
            }
            else
            {
                idx  = AnimDocStringIdxAt(&zen.doc, e, t);
                zero = (t > ZEN_AUTOKEY_EPS && tr->keyCount == 0)
                     ? AnimDocStringIdxAt(&zen.doc, e, 0.0f) : -1;
            }
            if (idx < 0) continue;              // nothing to key - leave the track be
            // ZenEnsureZeroKey cannot serve here: it seeds through AnimElemProp,
            // which is exactly the sentinel this branch exists to avoid.
            if (zero >= 0) AnimTrackAddKey(tr, 0.0f, (float)zero, ANIM_EASE_LINEAR);
            AnimTrackWriteKeyAt(tr, t, (float)idx, ZEN_AUTOKEY_EPS);
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

// Copy the member key at srcT onto dstT, verbatim - value, colour AND ease.
// Unlike ZenGroupWriteKey this does NOT sample the element at the destination:
// the point of a clone is to restate an earlier pose exactly, so an eased
// segment running through dstT must not bleed into the copy. Members with no
// key at srcT are left alone; a group key is the union of its members, so a
// partial group clones partially - the same shape it had at the source.
// Returns true when at least one member key was written.
bool ZenGroupCloneKeyTo(AnimElem *e, int gi, float srcT, float dstT)
{
    const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
    if (!g || fabsf(srcT - dstT) <= ZEN_AUTOKEY_EPS) return false;

    bool any = false;
    for (int m = 0; m < g->propCount; m++)
    {
        AnimTrack *tr = AnimElemFindTrack(e, g->props[m]);
        int k = tr ? KeyIndexNear(tr->keys, tr->keyCount, sizeof(AnimKey),
                                  srcT, ZEN_AUTOKEY_EPS) : -1;
        if (k < 0) continue;
        AnimKey src = tr->keys[k];              // by value: the write below
                                                // inserts and shifts the array.
        AnimKey *dst = AnimPropIsColor(g->props[m])
                     ? AnimTrackWriteColorKeyAt(tr, dstT, src.cval, ZEN_AUTOKEY_EPS)
                     : AnimTrackWriteKeyAt(tr, dstT, src.value, ZEN_AUTOKEY_EPS);
        if (!dst) continue;                     // track full
        dst->value = src.value;                 // colour keys carry both
        dst->cval  = src.cval;
        dst->ease  = src.ease;
        any = true;
    }
    return any;
}

// The group key at or before t, excluding one sitting on t itself. -1 when the
// group has nothing to the left (its first key is later than t).
float ZenGroupKeyTimeLeftOf(AnimElem *e, int gi, float t)
{
    float times[ZEN_GROUP_TIMES_MAX];
    int n = ZenGroupKeyTimes(e, gi, times);
    float best = -1.0f;
    for (int i = 0; i < n; i++)
        if (times[i] < t - ZEN_AUTOKEY_EPS && times[i] > best) best = times[i];
    return best;
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

// True when EVERY member of the group snaps rather than blends. Such a group
// has no easing to speak of, so the UI must not offer one - AnimTrackEval
// ignores what it would store.
bool ZenGroupIsStepped(int kind, int gi)
{
    const AnimPropGroup *g = AnimGroupAt(kind, gi);
    if (!g || g->propCount == 0) return false;
    for (int m = 0; m < g->propCount; m++)
        if (!AnimPropIsStepped(g->props[m])) return false;
    return true;
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
        else if (prop == AP_S_SHAPE)
        {
            const AnimShapeDef *sd = AnimShapePoolGet(ShapeIdxAt(e, t));
            one = ZenTextPreview(sd ? sd->name : "(none)", 16);
        }
        else if (AnimPropIsStepped(prop))
        {
            // A pool index says nothing to read; show the words it resolves to.
            const char *s = AnimDocStringAt(&zen.doc,
                                (int)(AnimElemProp(e, prop, t) + 0.5f));
            one = ZenTextPreview(s ? s : "(none)", 16);
        }
        else one = TextFormat("%.2f", AnimElemProp(e, prop, t));
        if (m) strncat(vals, ",", sizeof(vals)-strlen(vals)-1);
        strncat(vals, one, sizeof(vals)-strlen(vals)-1);
    }
    // A stepped group's ease is never applied, so naming one here would be a
    // lie the user could not act on.
    if (ZenGroupIsStepped(e->kind, gi))
        return TextFormat("%.2f   %s", t, vals);
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

// Switch the selected element WITHOUT throwing away what is being edited.
// Closing the track modal on every element click made comparing two similar
// elements a reopen-and-re-find chore, so an open doc-track modal FOLLOWS the
// selection. Three landings, in order of how much of the edit survives:
//
//   1. the same group BY NAME (indices differ across kinds), if the new element
//      actually tracks it - keeps the closest thing to what was in hand, so it
//      lands on the key nearest the one being edited;
//   2. otherwise the element's first tracked group, since carrying a `crumble`
//      to a shape (or any track the target simply hasn't been given yet) used
//      to close the modal outright. Nothing carried over means nothing to be
//      near, so a key is only selected when one sits ON the playhead - the
//      whole track otherwise, which is the honest scope;
//   3. no tracks at all - selGroup goes to -1 and the modal draws its empty
//      state. Deliberately NOT ZenSelClear(), which would close it.
void ZenSelSwitchElem(int elem)
{
    if (elem < 0 || elem >= zen.doc.elemCount || elem == zen.selElem) return;

    AnimElem *old = (zen.selElem >= 0 && zen.selElem < zen.doc.elemCount)
                  ? &zen.doc.elems[zen.selElem] : NULL;
    const AnimPropGroup *og = (old && zen.selGroup >= 0)
                            ? AnimGroupAt(old->kind, zen.selGroup) : NULL;
    // An open modal follows even from the empty state (og == NULL): the element
    // it was parked on having no tracks is no reason to drop the next one.
    bool carry = zen.trackModal.open && zen.trackModal.sig < 0;
    float refT = (carry && zen.selKeyCount == 1) ? zen.selKeys[0] : zen.playhead;

    zen.selElem = elem;
    if (!carry) { ZenSelClear(); return; }

    AnimElem *e = &zen.doc.elems[elem];
    int gi = -1;
    for (int i = 0, n = AnimGroupCountFor(e->kind); i < n && gi < 0; i++)
    {
        const AnimPropGroup *g = AnimGroupAt(e->kind, i);
        if (og && g && TextIsEqual(g->name, og->name) && ZenGroupHasTrack(e, i)) gi = i;
    }
    bool sameName = gi >= 0;
    for (int i = 0, n = AnimGroupCountFor(e->kind); i < n && gi < 0; i++)
        if (ZenGroupHasTrack(e, i)) gi = i;

    zen.selGroup = gi;
    zen.selKeyCount = 0;
    if (gi < 0) { ZenMouseReflow(); ZenTrackModalSync(); return; }   // empty state

    if (sameName)
    {
        // Nearest key rather than the first: scrubbing to a moment and flipping
        // through elements should keep showing THAT moment on each of them.
        float times[ZEN_GROUP_TIMES_MAX];
        int nt = ZenGroupKeyTimes(e, gi, times);
        if (nt > 0)
        {
            int best = 0;
            for (int i = 1; i < nt; i++)
                if (fabsf(times[i] - refT) < fabsf(times[best] - refT)) best = i;
            zen.selKeys[0] = times[best];
            zen.selKeyCount = 1;
        }
    }
    else if (GroupKeyTimeAt(e, gi, zen.playhead, &zen.selKeys[0])) zen.selKeyCount = 1;

    // The modal relays out around the new key (row counts differ per group),
    // sliding its sliders under the still-down button that picked the element.
    ZenMouseReflow();
    ZenTrackModalSync();        // keeps the modal open, re-baselines its fields
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

// Auto-key just wrote a key: put it on screen. Selecting it and opening the
// track modal is what makes the write visible instead of silent. Re-entrant by
// design (the viewport calls this every drag frame), so a no-op when the modal
// already shows that exact key keeps it from fighting the user's edits.
void ZenAutoKeyFocus(int elem, int prop, float t)
{
    if (elem < 0 || elem >= zen.doc.elemCount) return;
    AnimElem *e = &zen.doc.elems[elem];

    int gi = -1;
    for (int i = 0, n = AnimGroupCountFor(e->kind); i < n && gi < 0; i++)
    {
        const AnimPropGroup *g = AnimGroupAt(e->kind, i);
        for (int m = 0; g && m < g->propCount; m++)
            if (g->props[m] == prop) { gi = i; break; }
    }
    if (gi < 0) return;

    if (zen.trackModal.open && zen.trackModal.sig < 0 &&
        zen.selElem == elem && zen.selGroup == gi &&
        zen.selKeyCount == 1 && fabsf(zen.selKeys[0] - t) <= ZEN_AUTOKEY_EPS)
        return;                                     // already showing it

    ZenSelKey(elem, gi, t, false);
    ZenTrackModalOpen(elem, gi);
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
