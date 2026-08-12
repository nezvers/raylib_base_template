// ============================================================================
//  anim.c  -  authoring helpers, evaluation and playback for anim.h
//
//  Like scene_anim.c, evaluation is pure data: AnimTrackEval walks keyframes
//  and eases between them. This file only knows HOW each element kind is drawn
//  (text via DrawTextPro, shapes via DrawRectangle/Circle, global as a screen
//  fade). See anim_io.* for load/save and anim_editor_zen/ for authoring.
// ============================================================================

#include "anim.h"
#include "anim_ease_custom.h"
#include "anim_shape_pool.h"    // SHAPE_CUSTOM pixel shapes + their texture cache
#include "../include/easing.h"
#include "../screen_state/screen_state.h"
#include <string.h>
#include <math.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
//  Easing id -> name/function table. Built once at runtime (no file-scope
//  function-pointer initializers - MSVC-clean, house convention). Indexed by
//  AnimEase, and the names are the stable .cfg representation.
// ---------------------------------------------------------------------------
typedef struct { const char *name; EaseFn fn; } EaseRow;

static EaseRow s_ease[ANIM_EASE_COUNT];
static bool    s_easeInit = false;

static void EaseTableInit(void)
{
    if (s_easeInit) return;
    s_ease[ANIM_EASE_LINEAR]      = (EaseRow){ "linear",     NULL };
    s_ease[ANIM_EASE_SINE_IN]     = (EaseRow){ "sineIn",     sineEaseInf };
    s_ease[ANIM_EASE_SINE_OUT]    = (EaseRow){ "sineOut",    sineEaseOutf };
    s_ease[ANIM_EASE_SINE_INOUT]  = (EaseRow){ "sineInOut",  sineEaseInOutf };
    s_ease[ANIM_EASE_QUAD_IN]     = (EaseRow){ "quadIn",     quadraticEaseInf };
    s_ease[ANIM_EASE_QUAD_OUT]    = (EaseRow){ "quadOut",    quadraticEaseOutf };
    s_ease[ANIM_EASE_QUAD_INOUT]  = (EaseRow){ "quadInOut",  quadraticEaseInOutf };
    s_ease[ANIM_EASE_CUBIC_IN]    = (EaseRow){ "cubicIn",    cubicEaseInf };
    s_ease[ANIM_EASE_CUBIC_OUT]   = (EaseRow){ "cubicOut",   cubicEaseOutf };
    s_ease[ANIM_EASE_CUBIC_INOUT] = (EaseRow){ "cubicInOut", cubicEaseInOutf };
    s_ease[ANIM_EASE_EXPO_IN]     = (EaseRow){ "expoIn",     exponentialEaseInf };
    s_ease[ANIM_EASE_EXPO_OUT]    = (EaseRow){ "expoOut",    exponentialEaseOutf };
    s_ease[ANIM_EASE_BACK_IN]     = (EaseRow){ "backIn",     backEaseInf };
    s_ease[ANIM_EASE_BACK_OUT]    = (EaseRow){ "backOut",    backEaseOutf };
    s_ease[ANIM_EASE_ELASTIC_OUT] = (EaseRow){ "elasticOut", elasticEaseOutf };
    s_ease[ANIM_EASE_BOUNCE_OUT]  = (EaseRow){ "bounceOut",  bounceEaseOutf };
    s_easeInit = true;
}

const char *AnimEaseName(int ease)
{
    EaseTableInit();
    if (ease >= ANIM_EASE_COUNT)
    {
        const char *nm = AnimCustomEaseName(ease);
        if (nm) return nm;
    }
    if (ease < 0 || ease >= ANIM_EASE_COUNT) return s_ease[ANIM_EASE_LINEAR].name;
    return s_ease[ease].name;
}

int AnimEaseByName(const char *name)
{
    EaseTableInit();
    for (int i = 0; i < ANIM_EASE_COUNT; i++)
        if (TextIsEqual(s_ease[i].name, name)) return i;
    int custom = AnimCustomEaseByName(name);
    if (custom >= 0) return custom;
    return ANIM_EASE_LINEAR;    // unknown names (incl. deleted customs) degrade
}

float AnimEaseApply(int ease, float p)
{
    EaseTableInit();
    if (ease >= ANIM_EASE_COUNT) return AnimCustomEaseEval(ease, p);
    if (ease <= ANIM_EASE_LINEAR) return p;
    return s_ease[ease].fn ? s_ease[ease].fn(p) : p;
}

int AnimEaseCount(void) { return ANIM_EASE_COUNT; }

// deterministic pseudo-random in [-1,1] from an int seed (defined below the
// draw helpers; forward-declared here for the crumble preview).
float Rand11i(int seed);

// ---------------------------------------------------------------------------
//  Authoring / defaults
// ---------------------------------------------------------------------------
void AnimDocInit(AnimDoc *doc)
{
    memset(doc, 0, sizeof(*doc));
    TextCopy(doc->name, "untitled");
    doc->duration   = 2.0f;
    doc->introEnd   = 0.0f;
    doc->outroStart = doc->duration;
    // Smooth looping is the default: a snapping wrap is the special case, and a
    // document that never loops is unaffected by these two either way.
    doc->loopSmooth = true;
    doc->loopBlend  = ANIM_LOOP_BLEND_DEFAULT;
    doc->elemCount  = 0;
    doc->signalCount = 0;
    doc->pauseCount = 0;
    doc->stringCount = 0;
}

void AnimElemInit(AnimElem *e, AnimElemKind kind)
{
    memset(e, 0, sizeof(*e));
    e->kind         = kind;
    e->color        = RAYWHITE;
    e->shapeKind    = SHAPE_RECT;
    e->outlineColor = RAYWHITE;
    e->outlineFrac  = 0.0f;              // outline off by default
    e->scaleFrac    = 1.0f;              // shapes start at their authored size
    e->rotBase      = 0.0f;              // upright by default
    e->sizeAbsolute = false;             // sizes are canvas fractions by default
    e->cornerMode   = false;             // center+size authoring by default
    e->outlineCrisp = false;             // faceted polygon outline by default
    // Crumble scatter shape. These four are the values the effect was hardcoded
    // to before it was parameterized, so a doc that never touches them - and
    // every file written before they existed, since a missing token leaves
    // these standing - crumbles exactly the way it always did.
    e->crumbleRot    = 90.0f;            // +/- 90 deg of tumble per glyph
    e->crumbleDir    = 90.0f;            // straight down
    e->crumbleSpread = 12.0f;            // the old sideways drift, as a cone
    e->crumbleDist   = 0.5f;             // half the canvas height
    e->trackCount   = 0;

    switch (kind)
    {
        case AE_TEXT:
            TextCopy(e->name, "text");
            TextCopy(e->text, "TEXT");
            e->posFrac  = (Vector2){ 0.5f, 0.4f };   // center
            e->sizeFrac = (Vector2){ 0.10f, 0.10f }; // x=font size (of height)
            break;
        case AE_SHAPE:
            TextCopy(e->name, "shape");
            e->posFrac  = (Vector2){ 0.5f, 0.5f };   // center
            e->sizeFrac = (Vector2){ 0.20f, 0.20f }; // w frac, h frac
            break;
        case AE_GLOBAL:
            TextCopy(e->name, "global");
            e->color    = BLACK;                     // fade-to colour
            e->bgColor  = (Color){ 0, 0, 0, 0 };     // background off (a = 0)
            e->posFrac  = (Vector2){ 0.0f, 0.0f };
            e->sizeFrac = (Vector2){ 0.0f, 0.0f };
            break;
    }
}

AnimElem *AnimDocAddElem(AnimDoc *doc, AnimElemKind kind)
{
    if (doc->elemCount >= ANIM_ELEMS_MAX) return NULL;
    AnimElem *e = &doc->elems[doc->elemCount++];
    AnimElemInit(e, kind);
    return e;
}

// Signal targets address elements BY INDEX, so any reshuffle of doc->elems has
// to be mirrored onto them or a signal silently starts driving the wrong
// element. Both helpers below are the only places elems is reordered.

// Drop every target pointing at `gone`, and shift the ones above it down.
static void SignalsDropElem(AnimDoc *doc, int gone)
{
    for (int s = 0; s < doc->signalCount; s++)
    {
        AnimSignal *sg = &doc->signals[s];
        for (int t = sg->targetCount - 1; t >= 0; t--)
        {
            if (sg->targets[t].elemIdx == gone)
            {
                for (int m = t; m < sg->targetCount - 1; m++)
                    sg->targets[m] = sg->targets[m + 1];
                sg->targetCount--;
            }
            else if (sg->targets[t].elemIdx > gone)
                sg->targets[t].elemIdx--;
        }
    }
}

// Follow a swap of elements a <-> b.
static void SignalsSwapElem(AnimDoc *doc, int a, int b)
{
    for (int s = 0; s < doc->signalCount; s++)
    {
        AnimSignal *sg = &doc->signals[s];
        for (int t = 0; t < sg->targetCount; t++)
        {
            if      (sg->targets[t].elemIdx == a) sg->targets[t].elemIdx = b;
            else if (sg->targets[t].elemIdx == b) sg->targets[t].elemIdx = a;
        }
    }
}

// Follow an insertion at `at` (everything from `at` up shifted one slot).
static void SignalsInsertElem(AnimDoc *doc, int at)
{
    for (int s = 0; s < doc->signalCount; s++)
    {
        AnimSignal *sg = &doc->signals[s];
        for (int t = 0; t < sg->targetCount; t++)
            if (sg->targets[t].elemIdx >= at) sg->targets[t].elemIdx++;
    }
}

void AnimDocRemoveElem(AnimDoc *doc, int idx)
{
    if (idx < 0 || idx >= doc->elemCount) return;
    for (int i = idx; i < doc->elemCount - 1; i++) doc->elems[i] = doc->elems[i + 1];
    doc->elemCount--;
    SignalsDropElem(doc, idx);
}

void AnimDocMoveElem(AnimDoc *doc, int idx, int delta)
{
    if (idx < 0 || idx >= doc->elemCount) return;
    int to = idx + delta;
    if (to < 0 || to >= doc->elemCount) return;         // already at an end
    AnimElem tmp   = doc->elems[idx];
    doc->elems[idx] = doc->elems[to];
    doc->elems[to]  = tmp;
    SignalsSwapElem(doc, idx, to);
}

// "title" -> "title_2", "title_3", ... until nothing ELSE in the doc collides.
// Leaves the name as-is if every suffix is taken (names are cosmetic).
void AnimDocUniquifyElemName(AnimDoc *doc, int idx)
{
    if (!doc || idx < 0 || idx >= doc->elemCount) return;
    char *name = doc->elems[idx].name;

    bool taken = false;
    for (int i = 0; i < doc->elemCount && !taken; i++)
        if (i != idx && TextIsEqual(doc->elems[i].name, name)) taken = true;
    if (!taken) return;

    char base[ANIM_NAME_MAX];
    TextCopy(base, name);
    for (int n = 2; n < 100; n++)
    {
        const char *cand = TextFormat("%s_%d", base, n);
        bool hit = false;
        for (int i = 0; i < doc->elemCount && !hit; i++)
            if (i != idx && TextIsEqual(doc->elems[i].name, cand)) hit = true;
        if (!hit) { TextCopy(name, cand); return; }
    }
}

AnimElem *AnimDocDuplicateElem(AnimDoc *doc, int idx)
{
    if (idx < 0 || idx >= doc->elemCount) return NULL;
    if (doc->elemCount >= ANIM_ELEMS_MAX) return NULL;

    // shift the tail up one slot, then drop the copy right after the source so
    // the duplicate lands next to what it came from (and keeps its z-order).
    for (int i = doc->elemCount; i > idx + 1; i--) doc->elems[i] = doc->elems[i - 1];
    doc->elemCount++;
    SignalsInsertElem(doc, idx + 1);        // targets above the copy shift up

    AnimElem *dup = &doc->elems[idx + 1];
    *dup = doc->elems[idx];                 // plain value: tracks/keys come along
    AnimDocUniquifyElemName(doc, idx + 1);
    return dup;
}

AnimTrack *AnimElemFindTrack(AnimElem *e, int prop)
{
    for (int i = 0; i < e->trackCount; i++)
        if (e->tracks[i].prop == prop) return &e->tracks[i];
    return NULL;
}

AnimTrack *AnimElemAddTrack(AnimElem *e, int prop)
{
    if (AnimElemFindTrack(e, prop)) return NULL;      // one track per property
    if (e->trackCount >= ANIM_TRACKS_MAX) return NULL;
    AnimTrack *tr = &e->tracks[e->trackCount++];
    tr->prop     = prop;
    tr->keyCount = 0;
    return tr;
}

void AnimElemRemoveTrack(AnimElem *e, int idx)
{
    if (idx < 0 || idx >= e->trackCount) return;
    for (int i = idx; i < e->trackCount - 1; i++) e->tracks[i] = e->tracks[i + 1];
    e->trackCount--;
}

// Insert a whole key at its sorted slot (keys stay ascending in t) so the
// exact slot is known - no re-find that could alias an equal key.
static AnimKey *TrackInsertKey(AnimTrack *tr, AnimKey k)
{
    if (tr->keyCount >= ANIM_KEYS_MAX) return NULL;
    int at = tr->keyCount;
    while (at > 0 && tr->keys[at - 1].t > k.t)
    {
        tr->keys[at] = tr->keys[at - 1];
        at--;
    }
    tr->keyCount++;
    tr->keys[at] = k;
    return &tr->keys[at];
}

AnimKey *AnimTrackAddKey(AnimTrack *tr, float t, float value, int ease)
{
    return TrackInsertKey(tr, (AnimKey){ t, value, (Color){0,0,0,0}, ease });
}

AnimKey *AnimTrackAddColorKey(AnimTrack *tr, float t, Color c, int ease)
{
    return TrackInsertKey(tr, (AnimKey){ t, 0.0f, c, ease });
}

void AnimTrackRemoveKey(AnimTrack *tr, int idx)
{
    if (idx < 0 || idx >= tr->keyCount) return;
    for (int i = idx; i < tr->keyCount - 1; i++) tr->keys[i] = tr->keys[i + 1];
    tr->keyCount--;
}

int AnimTrackSetKeyTime(AnimTrack *tr, int idx, float t)
{
    if (idx < 0 || idx >= tr->keyCount) return -1;
    AnimKey k = tr->keys[idx];
    k.t = t;
    AnimTrackRemoveKey(tr, idx);
    // Re-insert at the sorted slot; insert cannot fail (we just freed a slot).
    AnimKey *slot = TrackInsertKey(tr, k);
    return (int)(slot - tr->keys);
}

AnimKey *AnimTrackWriteKeyAt(AnimTrack *tr, float t, float value, float eps)
{
    for (int i = 0; i < tr->keyCount; i++)
        if (fabsf(tr->keys[i].t - t) <= eps)
        {
            tr->keys[i].value = value;      // ease kept
            return &tr->keys[i];
        }
    return AnimTrackAddKey(tr, t, value, ANIM_EASE_LINEAR);
}

AnimKey *AnimTrackWriteColorKeyAt(AnimTrack *tr, float t, Color c, float eps)
{
    for (int i = 0; i < tr->keyCount; i++)
        if (fabsf(tr->keys[i].t - t) <= eps)
        {
            tr->keys[i].cval = c;           // ease kept
            return &tr->keys[i];
        }
    return AnimTrackAddColorKey(tr, t, c, ANIM_EASE_LINEAR);
}

bool AnimPropIsStepped(int prop)
{
    // Both of these key a POOL INDEX, not a quantity. This one predicate is
    // what makes AnimTrackEval snap instead of interpolate, what makes
    // AnimElemProp skip signal override / loop blend / sequence offset / pos
    // anchor (all of which are additive floats that would land BETWEEN two pool
    // entries and resolve to the wrong one), and what makes the zen editor drop
    // the easing controls for the group.
    return prop == AP_T_STRING || prop == AP_S_SHAPE;
}

bool AnimPropIsColor(int prop)
{
    return prop == AP_T_COLOR || prop == AP_S_COLOR ||
           prop == AP_S_OUTLINE_COLOR || prop == AP_G_COLOR ||
           prop == AP_G_BG_COLOR;
}

void AnimTrackSortKeys(AnimTrack *tr)
{
    // Insertion sort by t (keyCount is tiny; keeps keys ascending for Eval).
    for (int i = 1; i < tr->keyCount; i++)
    {
        AnimKey key = tr->keys[i];
        int j = i - 1;
        while (j >= 0 && tr->keys[j].t > key.t)
        {
            tr->keys[j + 1] = tr->keys[j];
            j--;
        }
        tr->keys[j + 1] = key;
    }
}

// ---------------------------------------------------------------------------
//  Evaluation
// ---------------------------------------------------------------------------
bool AnimTrackSegment(const AnimTrack *tr, float t, int *i0, int *i1)
{
    if (!tr || tr->keyCount == 0) return false;
    if (t <= tr->keys[0].t)              { *i0 = *i1 = 0; return true; }
    if (t >= tr->keys[tr->keyCount-1].t) { *i0 = *i1 = tr->keyCount - 1; return true; }

    // Find the segment [a, b] containing t (keys are sorted ascending).
    for (int i = 1; i < tr->keyCount; i++)
        if (t <= tr->keys[i].t) { *i0 = i - 1; *i1 = i; return true; }

    *i0 = *i1 = tr->keyCount - 1;               // unreachable, keeps compiler happy
    return true;
}

// Eased fraction through the segment [a, b] at time t (right key's ease).
static float SegmentFraction(const AnimKey *a, const AnimKey *b, float t)
{
    float span = b->t - a->t;
    float p    = (span > 0.0f) ? (t - a->t) / span : 1.0f;
    return AnimEaseApply(b->ease, p);
}

float AnimTrackEval(const AnimTrack *tr, float t, float missing)
{
    int i0, i1;
    if (!AnimTrackSegment(tr, t, &i0, &i1)) return missing;
    const AnimKey *a = &tr->keys[i0], *b = &tr->keys[i1];
    if (i0 == i1) return a->value;
    // A stepped property never blends: the left key's value holds across the
    // segment and the right key SNAPS in AT its own time - landing exactly on a
    // key must show that key's value, not still the previous one. Without this
    // an AP_T_STRING track would sample index 1.5 and truncate to the wrong
    // entry.
    if (AnimPropIsStepped(tr->prop)) return (t >= b->t) ? b->value : a->value;
    float p = SegmentFraction(a, b, t);
    return a->value + (b->value - a->value) * p;
}

// One colour channel mixed by p, clamped so back/elastic overshoot can't wrap.
static unsigned char MixChannel(unsigned char a, unsigned char b, float p)
{
    float v = (float)a + ((float)b - (float)a) * p;
    if (v < 0.0f) v = 0.0f; if (v > 255.0f) v = 255.0f;
    return (unsigned char)(v + 0.5f);
}

Color AnimTrackEvalColor(const AnimTrack *tr, float t, Color missing)
{
    int i0, i1;
    if (!AnimTrackSegment(tr, t, &i0, &i1)) return missing;
    const AnimKey *a = &tr->keys[i0], *b = &tr->keys[i1];
    Color out;
    if (i0 == i1) out = a->cval;
    else
    {
        float p = SegmentFraction(a, b, t);
        out = (Color){ MixChannel(a->cval.r, b->cval.r, p),
                       MixChannel(a->cval.g, b->cval.g, p),
                       MixChannel(a->cval.b, b->cval.b, p), 255 };
    }
    out.a = missing.a;   // alpha is NOT part of colour tracks (alpha track/base)
    return out;
}

// ---------------------------------------------------------------------------
//  Signal override hook.
//
//  A firing signal transiently drives some (element, property) pairs, winning
//  over the doc's own timeline. The draw helpers below all funnel through
//  AnimElemProp / AnimElemColorProp, so the override is applied THERE rather
//  than threaded through every drawing signature.
//
//  The active override is file-scope state installed for the duration of one
//  AnimDocDrawEx call (same singleton style as the rest of the project). It
//  needs the element's INDEX, which the const AnimElem* alone can't give, so
//  the doc being drawn is recorded and the index recovered by pointer offset.
// ---------------------------------------------------------------------------
static const AnimSignalPlayer *s_ovr    = NULL;
static const AnimDoc          *s_ovrDoc = NULL;

// AnimSignalPlayerSeqOffset (public, see anim.h) is defined below with the rest
// of the signal-player evaluation; AnimElemProp above folds it into every read.

// Index of `e` within the doc currently being drawn, or -1.
static int OverrideElemIdx(const AnimElem *e)
{
    if (!s_ovrDoc) return -1;
    ptrdiff_t d = e - s_ovrDoc->elems;
    if (d < 0 || d >= s_ovrDoc->elemCount) return -1;
    return (int)d;
}

// ---------------------------------------------------------------------------
//  Smooth-loop hook (same file-scope-context trick as the override above).
//
//  A looping clock wraps from the loop END back to the loop START, and keyframe
//  evaluation clamps, so the last key's pose is held to the end of the cycle and
//  then jumps. Over the last `s_loopBlend` seconds every property is instead
//  eased into the pose it holds at `s_loopStart`, so by the time the clock wraps
//  the two poses are identical and the seam disappears.
//
//  Installed by AnimDocDrawLoop for the duration of one draw, and only when the
//  playback actually loops - a one-shot play must show its tail as authored.
// ---------------------------------------------------------------------------
static bool  s_loopOn    = false;
static float s_loopStart = 0.0f, s_loopEnd = 0.0f, s_loopBlend = 0.0f;

// Blend weight at time t: 0 outside the window, 1 exactly at the loop end.
// Smoothstep, so the blend eases in rather than kinking the motion when it
// starts. Returns 0 whenever the blend is inactive.
static float LoopBlendWeight(float t)
{
    if (!s_loopOn || s_loopBlend <= 0.0f) return 0.0f;
    float from = s_loopEnd - s_loopBlend;
    if (t <= from) return 0.0f;
    float p = (t - from) / s_loopBlend;
    if (p > 1.0f) p = 1.0f;
    return p * p * (3.0f - 2.0f * p);
}

// Colour of `prop` from its track if there is one, else the matching base
// colour. The plain timeline read, with no signal override and no loop blend.
static Color TrackOrBaseColor(const AnimElem *e, int prop, float t)
{
    // Exact-prop lookup: a shape can carry BOTH a fill and an outline colour
    // track, so "any colour track" would alias them.
    Color base = (prop == AP_S_OUTLINE_COLOR) ? e->outlineColor
               : (prop == AP_G_BG_COLOR)      ? e->bgColor
                                              : e->color;

    for (int i = 0; i < e->trackCount; i++)
        if (e->tracks[i].prop == prop)
            return AnimTrackEvalColor(&e->tracks[i], t, base);
    return base;
}

Color AnimElemColorProp(const AnimElem *e, int prop, float t)
{
    if (s_ovr)
    {
        Color oc;
        int ei = OverrideElemIdx(e);
        if (ei >= 0 && AnimSignalPlayerEval(s_ovr, ei, prop, NULL, &oc))
        {
            // A signal owns this property outright while it plays: it already
            // blends from the live pose, so the loop blend must not touch it.
            oc.a = TrackOrBaseColor(e, prop, t).a;   // colour props carry no alpha
            return oc;
        }
    }

    Color c = TrackOrBaseColor(e, prop, t);

    float bw = LoopBlendWeight(t);
    if (bw > 0.0f)
    {
        Color c0 = TrackOrBaseColor(e, prop, s_loopStart);
        c = (Color){ MixChannel(c.r, c0.r, bw), MixChannel(c.g, c0.g, bw),
                     MixChannel(c.b, c0.b, bw), c.a };
    }
    return c;
}

Color AnimElemColor(const AnimElem *e, float t)
{
    int prop = (e->kind == AE_SHAPE)  ? AP_S_COLOR
             : (e->kind == AE_GLOBAL) ? AP_G_COLOR
                                      : AP_T_COLOR;
    return AnimElemColorProp(e, prop, t);
}

// Element base value for a property (the rest pose when no track drives it).
static float ElemBaseProp(const AnimElem *e, int prop)
{
    switch (prop)
    {
        case AP_T_POS_X: case AP_S_POS_X: return e->posFrac.x;
        case AP_T_POS_Y: case AP_S_POS_Y: return e->posFrac.y;
        case AP_T_SIZE:  case AP_S_W:     return e->sizeFrac.x;
        case AP_S_H:                      return e->sizeFrac.y;
        // rest-pose opacity comes from the base colour's alpha channel, so an
        // untracked element set semi-transparent in the inspector stays so.
        case AP_T_ALPHA: case AP_S_ALPHA: return (float)e->color.a / 255.0f;
        case AP_S_OUTLINE_ALPHA:          return (float)e->outlineColor.a / 255.0f;
        case AP_S_OUTLINE:                return e->outlineFrac;
        // 0 is the "unset" case (docs saved before scale existed, zeroed
        // structs); it means "as authored", not "collapsed to nothing".
        case AP_S_SCALE:                  return e->scaleFrac > 0.0f
                                               ? e->scaleFrac : 1.0f;
        case AP_T_ROT:   case AP_S_ROT:   return e->rotBase;
        case AP_T_CRUMBLE:                return 0.0f;
        // No string track -> the element shows its own text, which
        // AnimElemTextAt expresses as the fallback. -1 = "no pool entry".
        case AP_T_STRING:                 return -1.0f;
        // No shape track -> the element shows e->shapeName, which DrawShapeElem
        // resolves as the fallback. -1 = "no pool slot".
        case AP_S_SHAPE:                  return -1.0f;
        case AP_G_FADE:                   return 0.0f;
        case AP_G_BG_ALPHA:               return (float)e->bgColor.a / 255.0f;
        default:                          return 0.0f;
    }
}

// Value of `prop` from its track if there is one, else the element's base value.
// The plain timeline read, with no signal override and no loop blend.
static float TrackOrBase(const AnimElem *e, int prop, float t)
{
    const AnimTrack *tr = NULL;
    for (int i = 0; i < e->trackCount; i++)
        if (e->tracks[i].prop == prop) { tr = &e->tracks[i]; break; }
    if (!tr) return ElemBaseProp(e, prop);
    return AnimTrackEval(tr, t, ElemBaseProp(e, prop));
}

// ---- Corner-mode geometry (see anim.h) ------------------------------------
// A corners-mode shape is STORED center+size but AUTHORED by two corners. The
// on-screen half-extent is size*scale/2, so a corner sits at center +/- that;
// the inverse divides the rendered span back down by scale to recover w/h.
void AnimCornersToGeom(Vector2 p0, Vector2 p1, float scaleFrac,
                       float *cx, float *cy, float *w, float *h)
{
    float inv = (scaleFrac > 1e-6f) ? 1.0f / scaleFrac : 1.0f;
    if (cx) *cx = (p0.x + p1.x) * 0.5f;
    if (cy) *cy = (p0.y + p1.y) * 0.5f;
    if (w)  *w  = fabsf(p1.x - p0.x) * inv;
    if (h)  *h  = fabsf(p1.y - p0.y) * inv;
}

Vector2 AnimGeomToCorner(const AnimElem *e, int slot)
{
    float sc = (e->scaleFrac > 0.0f) ? e->scaleFrac : 1.0f;
    float hx = e->sizeFrac.x * sc * 0.5f, hy = e->sizeFrac.y * sc * 0.5f;
    return (slot == 0) ? (Vector2){ e->posFrac.x - hx, e->posFrac.y - hy }
                       : (Vector2){ e->posFrac.x + hx, e->posFrac.y + hy };
}

float AnimElemProp(const AnimElem *e, int prop, float t)
{
    int   ei = s_ovr ? OverrideElemIdx(e) : -1;
    float v;

    // A signal owns this property outright while it plays - it already blends
    // from the live pose, so the loop blend must not touch it. Otherwise it is
    // the plain timeline read plus the smooth-loop blend.
    // A STEPPED property is an identity, not a quantity: blending it, offsetting
    // it or easing it toward the loop start would all land between two entries
    // and resolve to the wrong one. It is the plain timeline read, full stop.
    if (AnimPropIsStepped(prop)) return TrackOrBase(e, prop, t);

    if (ei >= 0 && AnimSignalPlayerEval(s_ovr, ei, prop, &v, NULL))
    { /* v set by the signal */ }
    else
    {
        v = TrackOrBase(e, prop, t);
        float bw = LoopBlendWeight(t);
        if (bw > 0.0f) v += (TrackOrBase(e, prop, s_loopStart) - v) * bw;
    }

    // The per-instance sequence offset stacks ON TOP of whatever set v, so one
    // authored beat can fan several instances apart (see AnimSignal.usesSeq).
    if (ei >= 0) v += AnimSignalPlayerSeqOffset(s_ovr, ei, prop);
    // Spawn-anchor likewise stacks additively: it translates the authored motion
    // so the element is born at the cursor (see AnimSignal.posAnchor).
    if (ei >= 0) v += AnimSignalPlayerPosAnchor(s_ovr, ei, prop);
    return v;
}

float AnimPropMin(int prop)
{
    switch (prop)
    {
        case AP_T_ROT:   case AP_S_ROT:   return -360.0f;
        // positions reach a full screen beyond each edge (0..1 = on screen)
        // so elements can be keyed off screen and slide in/out.
        case AP_T_POS_X: case AP_S_POS_X:
        case AP_T_POS_Y: case AP_S_POS_Y: return -1.0f;
        default:                          return 0.0f;
    }
}

float AnimPropMax(int prop)
{
    if (AnimPropIsColor(prop)) return 255.0f;   // channels, if ever slid as floats
    switch (prop)
    {
        case AP_T_ROT:   case AP_S_ROT:   return 360.0f;
        case AP_T_POS_X: case AP_S_POS_X:
        case AP_T_POS_Y: case AP_S_POS_Y: return 2.0f;
        case AP_S_OUTLINE:                return 0.05f;   // ~36 px at 720p
        case AP_T_SIZE:  case AP_S_W:
        case AP_S_H:                      return 3.0f;    // allow off-screen sizes
        case AP_S_SCALE:                  return 10.0f;   // multiplier, 1 = rest
        // a pool INDEX, not a quantity: the whole pool must be reachable
        case AP_T_STRING:                 return (float)(ANIM_STRINGS_MAX - 1);
        case AP_S_SHAPE:                  return (float)(ANIM_SHAPE_POOL_MAX - 1);
        default:                          return 1.0f;
    }
}

// ---------------------------------------------------------------------------
//  Clone: make one element look like another at a chosen time (see anim.h)
// ---------------------------------------------------------------------------
// The animatable properties of each element kind, in storage order. Kept here
// rather than borrowed from anim_io's table because anim.c is the LOWER layer -
// anim_io includes anim.h, not the other way round.
static const int k_textProps[] = {
    AP_T_POS_X, AP_T_POS_Y, AP_T_SIZE, AP_T_ALPHA, AP_T_ROT, AP_T_CRUMBLE,
    AP_T_COLOR, AP_T_STRING,
};
static const int k_shapeProps[] = {
    AP_S_POS_X, AP_S_POS_Y, AP_S_W, AP_S_H, AP_S_ALPHA, AP_S_ROT, AP_S_COLOR,
    AP_S_OUTLINE_COLOR, AP_S_OUTLINE, AP_S_OUTLINE_ALPHA, AP_S_SCALE,
    AP_S_SHAPE,
};
static const int k_globalProps[] = {
    AP_G_FADE, AP_G_COLOR, AP_G_BG_ALPHA, AP_G_BG_COLOR,
};

static const int *PropsOfKind(int kind, int *count)
{
    switch (kind)
    {
        case AE_TEXT:   *count = (int)(sizeof(k_textProps)/sizeof(k_textProps[0]));
                        return k_textProps;
        case AE_SHAPE:  *count = (int)(sizeof(k_shapeProps)/sizeof(k_shapeProps[0]));
                        return k_shapeProps;
        case AE_GLOBAL: *count = (int)(sizeof(k_globalProps)/sizeof(k_globalProps[0]));
                        return k_globalProps;
        default:        *count = 0; return NULL;
    }
}

int AnimDocCloneElemState(AnimDoc *doc, int dstIdx, float dstT,
                          int srcIdx, float srcT, float eps, bool holdBefore)
{
    if (!doc) return 0;
    if (dstIdx < 0 || dstIdx >= doc->elemCount) return 0;
    if (srcIdx < 0 || srcIdx >= doc->elemCount) return 0;

    // Cloning from SELF is legitimate and useful: it copies the element's own
    // pose at one time onto another time, which is how a look is brought back
    // later without hand-retyping it. The two times must differ, or there is
    // nothing to say.
    if (dstIdx == srcIdx && fabsf(dstT - srcT) <= eps) return 0;

    AnimElem       *dst = &doc->elems[dstIdx];
    const AnimElem *src = &doc->elems[srcIdx];

    // A hold key sits just BEFORE the clone so the destination keeps its
    // current pose right up to dstT and then changes. Without it an
    // interpolated property drifts across the whole span from the previous
    // key. Nudged back by eps*2 so the write cannot land inside the clone
    // key's own epsilon window and overwrite it.
    float holdT = dstT - eps*2.0f;
    if (holdT < 0.0f) holdT = 0.0f;
    if (holdBefore && holdT >= dstT) holdBefore = false;    // no room before 0

    // Only the destination's own kind is cloneable: the AP_* ranges are disjoint
    // per kind, so a shape property on a text element would be dead storage.
    // A cross-kind clone therefore copies only what the two kinds share - which
    // is nothing, so it is a no-op rather than a corruption.
    int propCount = 0;
    const int *props = PropsOfKind(dst->kind, &propCount);
    if (!props || dst->kind != src->kind) return 0;

    int written = 0;
    for (int i = 0; i < propCount; i++)
    {
        int prop = props[i];
        bool isColor = AnimPropIsColor(prop);

        // The pure authored reads: no signal override, no loop blend, so what
        // gets cloned is what was authored - not what a live preview happens to
        // be showing.
        bool differs = false;
        float sv = 0.0f;
        Color sc = { 0, 0, 0, 0 };
        if (prop == AP_T_STRING)
        {
            // Compare the WORDS, not the index. An element with no string track
            // still shows text (its base e->text), and that is exactly the case
            // the clone exists for - reusing an expired block's wording. Working
            // off the raw index would read -1 for both and conclude "identical".
            const char *ss = AnimElemTextAt(src, doc, srcT);
            const char *ds = AnimElemTextAt(dst, doc, dstT);
            if (TextIsEqual(ss, ds)) continue;

            // The source's words must exist in the pool to be keyed at all; a
            // second element wanting the same string shares the entry.
            int at = AnimDocAddString(doc, ss);
            if (at < 0) return -1;              // ANIM_STRINGS_MAX reached
            sv = (float)at;
            differs = true;
        }
        else if (isColor)
        {
            sc = TrackOrBaseColor(src, prop, srcT);
            Color dc = TrackOrBaseColor(dst, prop, dstT);
            differs = (sc.r != dc.r) || (sc.g != dc.g) || (sc.b != dc.b);
        }
        else
        {
            sv = TrackOrBase(src, prop, srcT);
            float dv = TrackOrBase(dst, prop, dstT);
            differs = (fabsf(sv - dv) > 1e-4f);
        }
        if (!differs) continue;             // delta keys only: nothing to say here

        // What the destination shows just before the clone lands. Read BEFORE
        // any track is created, because creating one changes what is sampled.
        float dvHold = 0.0f;
        Color dcHold = { 0, 0, 0, 255 };
        int   dsHold = -1;
        if (isColor)              dcHold = TrackOrBaseColor(dst, prop, holdT);
        else if (prop == AP_T_STRING) dsHold = AnimDocAddString(doc,
                                          AnimElemTextAt(dst, doc, holdT));
        else                      dvHold = TrackOrBase(dst, prop, holdT);

        AnimTrack *tr = AnimElemFindTrack(dst, prop);
        bool fresh = (tr == NULL);
        if (!tr) tr = AnimElemAddTrack(dst, prop);
        if (!tr) return -1;                 // ANIM_TRACKS_MAX reached

        // A brand-new track needs a key at 0 holding what the element already
        // showed, or the clone key becomes the ONLY key and its value applies
        // to the whole timeline - the "changes at T" promise broken. This is
        // not just a string problem: a lone numeric key overrides the base
        // value everywhere too, so the element's original property is lost.
        if (fresh && dstT > 0.0f)
        {
            if (isColor)
                AnimTrackAddColorKey(tr, 0.0f, TrackOrBaseColor(dst, prop, 0.0f),
                                     ANIM_EASE_LINEAR);
            else if (prop == AP_T_STRING)
            {
                int pat = AnimDocAddString(doc, AnimElemTextAt(dst, doc, 0.0f));
                if (pat >= 0) AnimTrackAddKey(tr, 0.0f, (float)pat, ANIM_EASE_LINEAR);
            }
            else
                AnimTrackAddKey(tr, 0.0f, TrackOrBase(dst, prop, 0.0f),
                                ANIM_EASE_LINEAR);
        }

        // The hold key pins the old pose right up against the clone, so the
        // change is a step rather than a slide from whatever came before.
        if (holdBefore)
        {
            if (isColor) AnimTrackWriteColorKeyAt(tr, holdT, dcHold, eps);
            else if (prop == AP_T_STRING)
            { if (dsHold >= 0) AnimTrackWriteKeyAt(tr, holdT, (float)dsHold, eps); }
            else AnimTrackWriteKeyAt(tr, holdT, dvHold, eps);
        }

        AnimKey *k = isColor ? AnimTrackWriteColorKeyAt(tr, dstT, sc, eps)
                             : AnimTrackWriteKeyAt(tr, dstT, sv, eps);
        if (!k) return -1;                  // ANIM_KEYS_MAX reached
        written++;
    }
    return written;
}

// ---------------------------------------------------------------------------
//  String pool (see AnimString in anim.h). Indices are STABLE: a removed entry
//  leaves a hole rather than shifting its neighbours, because AP_T_STRING keys
//  all over the document store those indices.
// ---------------------------------------------------------------------------
int AnimDocFindString(const AnimDoc *doc, const char *text)
{
    if (!doc || !text) return -1;
    for (int i = 0; i < doc->stringCount; i++)
        if (doc->strings[i].used && TextIsEqual(doc->strings[i].text, text))
            return i;
    return -1;
}

const char *AnimDocStringAt(const AnimDoc *doc, int idx)
{
    if (!doc || idx < 0 || idx >= doc->stringCount) return NULL;
    if (!doc->strings[idx].used) return NULL;       // a hole left by a delete
    return doc->strings[idx].text;
}

int AnimDocAddString(AnimDoc *doc, const char *text)
{
    if (!doc || !text) return -1;

    // An identical entry is REUSED: typing the same words twice must not eat two
    // slots, which is what makes the inspector's auto-add idempotent.
    int found = AnimDocFindString(doc, text);
    if (found >= 0) return found;

    // Prefer a hole over growing: slots are scarce and a delete leaves gaps.
    for (int i = 0; i < doc->stringCount; i++)
        if (!doc->strings[i].used)
        {
            TextCopy(doc->strings[i].text, text);
            doc->strings[i].used = true;
            return i;
        }

    if (doc->stringCount >= ANIM_STRINGS_MAX) return -1;
    int at = doc->stringCount++;
    TextCopy(doc->strings[at].text, text);
    doc->strings[at].used = true;
    return at;
}

int AnimDocStringUsers(const AnimDoc *doc, int idx)
{
    if (!doc || idx < 0) return 0;
    int n = 0;
    for (int i = 0; i < doc->elemCount; i++)
    {
        const AnimElem *e = &doc->elems[i];
        if (e->kind != AE_TEXT) continue;
        for (int j = 0; j < e->trackCount; j++)
        {
            if (e->tracks[j].prop != AP_T_STRING) continue;
            for (int k = 0; k < e->tracks[j].keyCount; k++)
                if ((int)(e->tracks[j].keys[k].value + 0.5f) == idx) n++;
        }
    }
    return n;
}

bool AnimDocRemoveString(AnimDoc *doc, int idx)
{
    if (!doc || idx < 0 || idx >= doc->stringCount) return false;
    if (!doc->strings[idx].used) return false;
    if (AnimDocStringUsers(doc, idx) > 0) return false;   // still referenced

    doc->strings[idx].used = false;
    doc->strings[idx].text[0] = '\0';

    // Shrink the high-water mark when the tail is now empty, so a pool that is
    // filled and cleared does not stay "full" forever.
    while (doc->stringCount > 0 && !doc->strings[doc->stringCount - 1].used)
        doc->stringCount--;
    return true;
}

int AnimDocGCStrings(AnimDoc *doc)
{
    if (!doc) return 0;
    int freed = 0;
    // An EMPTY entry nobody points at is noise in the pool list - reclaim it.
    // A non-empty entry is the user's text and is never touched, referenced or
    // not; a referenced entry is never touched even when empty.
    for (int i = 0; i < doc->stringCount; i++)
    {
        if (!doc->strings[i].used) continue;
        if (doc->strings[i].text[0] != '\0') continue;
        if (AnimDocStringUsers(doc, i) > 0) continue;
        doc->strings[i].used = false;
        freed++;
    }
    while (doc->stringCount > 0 && !doc->strings[doc->stringCount - 1].used)
        doc->stringCount--;
    return freed;
}

const char *AnimElemTextAt(const AnimElem *e, const AnimDoc *doc, float t)
{
    if (!e) return "";
    if (e->kind != AE_TEXT) return e->text;

    const AnimTrack *tr = NULL;
    for (int i = 0; i < e->trackCount; i++)
        if (e->tracks[i].prop == AP_T_STRING) { tr = &e->tracks[i]; break; }

    if (tr && tr->keyCount > 0 && doc)
    {
        // Stepped: AnimTrackEval already holds the left key's value across the
        // segment, so this is an exact index, never a blend of two.
        int idx = (int)(AnimTrackEval(tr, t, -1.0f) + 0.5f);
        const char *s = AnimDocStringAt(doc, idx);
        if (s) return s;
        // Stale reference (entry deleted, or an index from another document):
        // fall back rather than blanking the element.
    }
    return e->text;
}

int AnimDocStringIdxAt(AnimDoc *doc, AnimElem *e, float t)
{
    if (!doc || !e || e->kind != AE_TEXT) return -1;

    const AnimTrack *tr = NULL;
    for (int i = 0; i < e->trackCount; i++)
        if (e->tracks[i].prop == AP_T_STRING) { tr = &e->tracks[i]; break; }

    if (tr && tr->keyCount > 0)
    {
        int idx = (int)(AnimTrackEval(tr, t, -1.0f) + 0.5f);
        if (AnimDocStringAt(doc, idx)) return idx;
        // A stale index resolves to nothing, so fall through and pool the words
        // the element is actually showing - the same fallback AnimElemTextAt
        // makes, expressed as an index.
    }
    return AnimDocAddString(doc, e->text);
}

// ---------------------------------------------------------------------------
//  Pause markers (see AnimPause in anim.h). The array is kept ascending in t.
// ---------------------------------------------------------------------------
int AnimDocPauseAt(const AnimDoc *doc, float t, float eps)
{
    if (!doc) return -1;
    for (int i = 0; i < doc->pauseCount; i++)
        if (fabsf(doc->pauses[i].t - t) <= eps) return i;
    return -1;
}

AnimPause *AnimDocAddPause(AnimDoc *doc, float t, float eps)
{
    if (!doc) return NULL;
    if (doc->pauseCount >= ANIM_PAUSES_MAX) return NULL;
    if (t < 0.0f) t = 0.0f;
    // Two holds at the same instant would read as one and could not be deleted
    // apart, so an existing marker here wins.
    if (AnimDocPauseAt(doc, t, eps) >= 0) return NULL;

    int at = doc->pauseCount;
    while (at > 0 && doc->pauses[at - 1].t > t)
    {
        doc->pauses[at] = doc->pauses[at - 1];
        at--;
    }
    doc->pauseCount++;
    doc->pauses[at] = (AnimPause){ t, false };   // holds every cycle by default
    return &doc->pauses[at];
}

void AnimDocRemovePause(AnimDoc *doc, int idx)
{
    if (!doc || idx < 0 || idx >= doc->pauseCount) return;
    for (int i = idx; i < doc->pauseCount - 1; i++) doc->pauses[i] = doc->pauses[i + 1];
    doc->pauseCount--;
}

int AnimDocSetPauseTime(AnimDoc *doc, int idx, float t)
{
    if (!doc || idx < 0 || idx >= doc->pauseCount) return -1;
    if (t < 0.0f) t = 0.0f;

    AnimPause p = doc->pauses[idx];
    p.t = t;
    AnimDocRemovePause(doc, idx);

    // Re-insert at the sorted slot; cannot fail, a slot was just freed.
    int at = doc->pauseCount;
    while (at > 0 && doc->pauses[at - 1].t > t)
    {
        doc->pauses[at] = doc->pauses[at - 1];
        at--;
    }
    doc->pauseCount++;
    doc->pauses[at] = p;
    return at;
}

int AnimDocNextPause(const AnimDoc *doc, float from, float to)
{
    if (!doc || to <= from) return -1;
    // Half-open (from, to]: a marker exactly AT `from` was already served on the
    // frame that stopped there, so it must not re-fire the instant it resumes.
    for (int i = 0; i < doc->pauseCount; i++)
    {
        float t = doc->pauses[i].t;
        if (t > from && t <= to) return i;       // ascending: the first is the earliest
    }
    return -1;
}

float AnimDocMaxKeyTime(const AnimDoc *doc)
{
    float d = 0.0f;
    for (int i = 0; i < doc->elemCount; i++)
        for (int j = 0; j < doc->elems[i].trackCount; j++)
        {
            const AnimTrack *tr = &doc->elems[i].tracks[j];
            for (int k = 0; k < tr->keyCount; k++)
                if (tr->keys[k].t > d) d = tr->keys[k].t;
        }
    return d;
}

float AnimDocOutroStart(const AnimDoc *doc)
{
    if (!doc) return 0.0f;
    // <= 0 is the "unset" case: docs saved before the trim existed, and any
    // zeroed struct, both mean "play the whole clock".
    float o = (doc->outroStart > 0.0f) ? doc->outroStart : doc->duration;
    if (o > doc->duration) o = doc->duration;
    return o < 0.0f ? 0.0f : o;
}

float AnimDocIntroEnd(const AnimDoc *doc)
{
    if (!doc) return 0.0f;
    float o = AnimDocOutroStart(doc);
    float i = doc->introEnd;
    if (i < 0.0f) i = 0.0f;
    if (i > o)    i = o;
    return i;
}

float AnimDocPlayLen(const AnimDoc *doc)
{
    return AnimDocOutroStart(doc) - AnimDocIntroEnd(doc);
}

// ---------------------------------------------------------------------------
//  Drawing (game space). One helper per element kind.
// ---------------------------------------------------------------------------
static float TextSpacingFor(float sizePx)
{
    return sizePx / (float)GetFontDefault().baseSize;
}

// `str` is the string to draw, already resolved through the document's pool by
// the caller (AnimElemTextAt). Passed in rather than looked up here so the
// renderer stays ignorant of the pool and needs no document pointer.
static void DrawTextElem(const AnimElem *e, float t, Vector2 game, const char *str)
{
    float alpha = AnimElemProp(e, AP_T_ALPHA, t);
    if (alpha <= 0.0f) return;

    float cxF   = AnimElemProp(e, AP_T_POS_X, t);
    float cyF   = AnimElemProp(e, AP_T_POS_Y, t);
    float sizeF = AnimElemProp(e, AP_T_SIZE,  t);
    float rot   = AnimElemProp(e, AP_T_ROT,   t);
    float crumble = AnimElemProp(e, AP_T_CRUMBLE, t);

    // absolute -> sizeF is already pixels; otherwise a fraction of game height.
    float sizePx  = fmaxf(1.0f, e->sizeAbsolute ? sizeF : game.y * sizeF);
    float spacing = TextSpacingFor(sizePx);
    Font  font    = GetFontDefault();

    Vector2 box   = MeasureTextEx(font, str, sizePx, spacing);
    float left    = game.x * cxF - box.x * 0.5f;   // pos = text center
    float top     = game.y * cyF - box.y * 0.5f;
    Color col     = Fade(AnimElemColor(e, t), alpha * (1.0f - crumble));
    if (col.a == 0) return;

    if (crumble > 0.0f)
    {
        // Crumble: throw each glyph along its own angle inside a cone. Shaped
        // by the element's four crumble fields (see anim.h) rather than
        // hardcoded, so the same track can be a collapse, an explosion or a
        // gust sideways. Still an approximation with NO physics state - every
        // glyph's angle and spin hash its INDEX, never the clock, which is what
        // keeps a frame a pure function of (doc, t) for the exporter.
        float scale = sizePx / (float)font.baseSize;
        float penX  = left;
        float penY  = top;
        int   idx   = 0;
        // Quadratic in crumble: the letters accelerate away, so most of the
        // travel happens late and the text stays readable as it starts to go.
        float reach = crumble * crumble * game.y * e->crumbleDist;
        for (const char *c = str; *c; c++)
        {
            int cp = (unsigned char)*c;
            // this loop places glyphs by hand, so it owns line breaking too
            // (DrawTextEx handles '\n' on the non-crumble paths below).
            if (cp == '\n') { penX = left; penY += sizePx + spacing; continue; }
            GlyphInfo gi = GetGlyphInfo(font, cp);
            float advance = (gi.advanceX == 0 ? gi.image.width : gi.advanceX) * scale
                          + spacing;
            if (cp != ' ')
            {
                char buf[2]  = { (char)cp, 0 };
                // The angle hash is OFFSET from the spin hash so the two stay
                // independent: without it a glyph's tumble would predict which
                // way it flies, and the cone would visibly pair up at narrow
                // spreads.
                float   ang  = (e->crumbleDir + Rand11i(idx + 977)
                                              * e->crumbleSpread * 0.5f) * DEG2RAD;
                Vector2 gsz  = MeasureTextEx(font, buf, sizePx, spacing);
                Vector2 org  = { gsz.x * 0.5f, gsz.y * 0.5f };
                Vector2 ctr  = { penX + org.x + cosf(ang) * reach,
                                 penY + org.y + sinf(ang) * reach };
                float   grot = rot + crumble * (Rand11i(idx) * e->crumbleRot);
                DrawTextPro(font, buf, ctr, org, grot, sizePx, spacing, col);
                idx++;
            }
            penX += advance;
        }
        return;
    }

    if (rot != 0.0f)
    {
        Vector2 org = { box.x * 0.5f, box.y * 0.5f };
        Vector2 ctr = { left + org.x, top + org.y };
        DrawTextPro(font, str, ctr, org, rot, sizePx, spacing, col);
    }
    else
    {
        DrawTextEx(font, str, (Vector2){ left, top }, sizePx, spacing, col);
    }
}

#define ELLIPSE_SEGS       36
// Max rim points of any shape (the ellipse).
#define SHAPE_RIM_MAX      ELLIPSE_SEGS

// Rotate local point (x, y) about the origin by (cr, sr) and place it at c.
static Vector2 RimPoint(Vector2 c, float x, float y, float cr, float sr)
{
    return (Vector2){ c.x + x*cr - y*sr, c.y + x*sr + y*cr };
}

// Rim builders: closed polygon outlines in SCREEN-CCW order (y-down, so local
// angles run NEGATIVE - raylib's DrawTriangleFan culls clockwise fans).
// All take half-extents and the rotation's cos/sin; return the point count.
static int RimRect(Vector2 c, float hw, float hh, float cr, float sr, Vector2 *out)
{
    out[0] = RimPoint(c, -hw, -hh, cr, sr);
    out[1] = RimPoint(c, -hw,  hh, cr, sr);
    out[2] = RimPoint(c,  hw,  hh, cr, sr);
    out[3] = RimPoint(c,  hw, -hh, cr, sr);
    return 4;
}

static int RimRhombus(Vector2 c, float hw, float hh, float cr, float sr, Vector2 *out)
{
    out[0] = RimPoint(c, 0.0f, -hh, cr, sr);
    out[1] = RimPoint(c, -hw, 0.0f, cr, sr);
    out[2] = RimPoint(c, 0.0f,  hh, cr, sr);
    out[3] = RimPoint(c,  hw, 0.0f, cr, sr);
    return 4;
}

static int RimTriangle(Vector2 c, float hw, float hh, float cr, float sr, Vector2 *out)
{
    out[0] = RimPoint(c, 0.0f, -hh, cr, sr);   // apex up
    out[1] = RimPoint(c, -hw,   hh, cr, sr);
    out[2] = RimPoint(c,  hw,   hh, cr, sr);
    return 3;
}

static int RimEllipse(Vector2 c, float rx, float ry, float cr, float sr, Vector2 *out)
{
    for (int i = 0; i < ELLIPSE_SEGS; i++)
    {
        float a = -(float)i / ELLIPSE_SEGS * 2.0f * PI;   // negative = screen CCW
        out[i] = RimPoint(c, cosf(a) * rx, sinf(a) * ry, cr, sr);
    }
    return ELLIPSE_SEGS;
}

// Fill + outline of a closed polygon whose rim is already in screen space.
// Fill is a triangle fan about the center (rim must be screen-CCW); outline is
// a DrawLineEx loop with circle-capped corners (raylib has no rotatable
// thick-outline primitive).
static void DrawPolyShape(Vector2 center, const Vector2 *pts, int n,
                          Color fill, Color line, float thickPx)
{
    if (fill.a > 0)
    {
        Vector2 fan[SHAPE_RIM_MAX + 2];
        fan[0] = center;
        for (int i = 0; i < n; i++) fan[i + 1] = pts[i];
        fan[n + 1] = pts[0];                    // close the fan
        DrawTriangleFan(fan, n + 2, fill);
    }
    if (thickPx >= 0.5f && line.a > 0)
    {
        for (int i = 0; i < n; i++)
        {
            Vector2 a = pts[i], b = pts[(i + 1) % n];
            DrawLineEx(a, b, thickPx, line);
            DrawCircleV(a, thickPx * 0.5f, line);   // cap the corner
        }
    }
}

static void DrawShapeElem(const AnimElem *e, float t, Vector2 game)
{
    float fillA = AnimElemProp(e, AP_S_ALPHA,         t);
    float outA  = AnimElemProp(e, AP_S_OUTLINE_ALPHA, t);
    // uniform multiplier on every size: the box, and the outline with it, so a
    // shape grown by scale keeps its authored rim-to-body proportion.
    float sc = AnimElemProp(e, AP_S_SCALE, t);
    if (sc < 0.0f) sc = 0.0f;
    // absolute -> the size props are pixels, so the canvas multiplier drops to 1.
    float uW = e->sizeAbsolute ? 1.0f : game.x;   // width-axis reference
    float uH = e->sizeAbsolute ? 1.0f : game.y;   // height-axis reference
    float thickPx = uH * AnimElemProp(e, AP_S_OUTLINE, t) * sc;
    // A custom shape's outline is PIXEL DATA, not a stroke, so it has no
    // thickness to fall below half a pixel - it is visible whenever its alpha is.
    if (e->shapeKind == SHAPE_CUSTOM) { if (fillA <= 0.0f && outA <= 0.0f) return; }
    else if (fillA <= 0.0f && (outA <= 0.0f || thickPx < 0.5f)) return;

    float cxF = AnimElemProp(e, AP_S_POS_X, t);
    float cyF = AnimElemProp(e, AP_S_POS_Y, t);
    float wF  = AnimElemProp(e, AP_S_W,     t) * sc;
    float hF  = AnimElemProp(e, AP_S_H,     t) * sc;
    float rot = AnimElemProp(e, AP_S_ROT,   t);

    Color fill = Fade(AnimElemColorProp(e, AP_S_COLOR,         t), fillA);
    Color line = Fade(AnimElemColorProp(e, AP_S_OUTLINE_COLOR, t), outA);

    // position is ALWAYS a fraction; only the extents switch units.
    Vector2 c  = { game.x * cxF, game.y * cyF };
    float   hw = uW * wF * 0.5f;
    float   hh = uH * hF * 0.5f;
    float   cr = cosf(rot * DEG2RAD), sr = sinf(rot * DEG2RAD);

    if (e->shapeKind == SHAPE_LINE)
    {
        // Segment through the center: length = w, thickness = h, fill colour.
        // The half-length is projected per-axis (x uses the width reference, y the
        // height reference) so in % mode the endpoints are true canvas fractions -
        // e.g. (0,0)..(1,1) stays pinned corner-to-corner across resizes, matching
        // how every other shape's extents scale. In px mode uW=uH=1, so the offset
        // stays isotropic pixels.
        float hlx = fmaxf(uW * wF * 0.5f, 0.0f);
        float hly = fmaxf(uH * wF * 0.5f, 0.0f);
        float th = fmaxf(uH * hF, 1.0f);
        Vector2 a = { c.x - hlx * cr, c.y - hly * sr };
        Vector2 b = { c.x + hlx * cr, c.y + hly * sr };
        if (fill.a == 0) return;
        DrawLineEx(a, b, th, fill);
        DrawCircleV(a, th * 0.5f, fill);        // round caps
        DrawCircleV(b, th * 0.5f, fill);
        return;
    }

    if (e->shapeKind == SHAPE_CUSTOM)
    {
        // WHICH shape: the stepped AP_S_SHAPE track if it has keys, else the
        // element's rest-pose name. The track stores a pool slot as a float, so
        // round rather than truncate - a value that round-tripped through the
        // .cfg as 1.9999998 would otherwise land on slot 1.
        float sIdx = AnimElemProp(e, AP_S_SHAPE, t);
        int slot = sIdx < 0.0f ? AnimShapePoolFindByName(e->shapeName)
                               : (int)(sIdx + 0.5f);

        Texture2D fillTex, lineTex;
        if (!AnimShapePoolTextures(slot, &fillTex, &lineTex))
        {
            // Missing shape: a hatched box, drawn HERE rather than in the editor
            // so exports and the game show the problem too instead of a hole.
            Color warn = Fade(MAGENTA, fillA > 0.0f ? fillA : 1.0f);
            Vector2 box[SHAPE_RIM_MAX];
            int bn = RimRect(c, hw, hh, cr, sr, box);
            DrawPolyShape(c, box, bn, BLANK, warn, 2.0f);
            DrawLineEx(box[0], box[2], 2.0f, warn);     // one diagonal is enough
            return;
        }

        // Both stencils cover the same box, so one dest/origin serves both.
        // DrawTexturePro rotates about `origin`, which is the box centre - the
        // same center-anchored rotation every other shape uses, and pixel-exact
        // because POINT filtering keeps the texels square.
        Rectangle src  = { 0.0f, 0.0f, (float)fillTex.width, (float)fillTex.height };
        Rectangle dst  = { c.x, c.y, hw * 2.0f, hh * 2.0f };
        Vector2   orig = { hw, hh };
        if (fillA > 0.0f && fillTex.id) DrawTexturePro(fillTex, src, dst, orig, rot, fill);
        if (outA  > 0.0f && lineTex.id) DrawTexturePro(lineTex, src, dst, orig, rot, line);
        return;
    }

    Vector2 rim[SHAPE_RIM_MAX];
    int n = 0;
    switch (e->shapeKind)
    {
        case SHAPE_CIRCLE:   n = RimEllipse(c, hw, hh, cr, sr, rim);       break;
        case SHAPE_SQUARE:   n = RimRect(c, hh, hh, cr, sr, rim);          break;
        case SHAPE_RHOMBUS:  n = RimRhombus(c, hw, hh, cr, sr, rim);       break;
        case SHAPE_TRIANGLE: n = RimTriangle(c, hw, hh, cr, sr, rim);      break;
        case SHAPE_RECT:
        default:             n = RimRect(c, hw, hh, cr, sr, rim);          break;
    }

    // crisp circle: fill via the rim fan, but stroke as a smooth annulus so the
    // outline stays stable instead of shimmering along the faceted chords/caps.
    // (Ellipses hw!=hh approximate with the mean radius - crisp is a circle-only
    // authoring option, so this is acceptable.)
    if (e->shapeKind == SHAPE_CIRCLE && e->outlineCrisp)
    {
        DrawPolyShape(c, rim, n, fill, BLANK, 0.0f);   // fill only
        if (thickPx >= 0.5f && line.a > 0)
        {
            float radius = (hw + hh) * 0.5f;
            float inner  = radius - thickPx;
            if (inner < 0.0f) inner = 0.0f;
            DrawRing(c, inner, radius, 0.0f, 360.0f, ELLIPSE_SEGS, line);
        }
        return;
    }

    DrawPolyShape(c, rim, n, fill, line, thickPx);
}

// Scene background: a full-screen fill drawn BEFORE any element, so it never
// depends on where the global element sits in the list. Fully separate from
// the fade (which is drawn last, over everything, by DrawGlobalElem).
static void DrawGlobalBackground(const AnimElem *e, float t, Vector2 game)
{
    float a = AnimElemProp(e, AP_G_BG_ALPHA, t);
    if (a <= 0.0f) return;
    DrawRectangle(0, 0, (int)game.x, (int)game.y,
                  Fade(AnimElemColorProp(e, AP_G_BG_COLOR, t), a));
}

static void DrawGlobalElem(const AnimElem *e, float t, Vector2 game)
{
    float fade = AnimElemProp(e, AP_G_FADE, t);
    if (fade <= 0.0f) return;
    DrawRectangle(0, 0, (int)game.x, (int)game.y, Fade(AnimElemColor(e, t), fade));
}

void AnimLoopBlendBegin(const AnimDoc *doc, bool looping)
{
    if (!doc) { s_loopOn = false; return; }
    // The blend can never eat more than the cycle it belongs to.
    s_loopStart = AnimDocIntroEnd(doc);
    s_loopEnd   = AnimDocOutroStart(doc);
    float cycle = s_loopEnd - s_loopStart;
    s_loopBlend = (doc->loopBlend < cycle) ? doc->loopBlend : cycle;
    s_loopOn    = looping && doc->loopSmooth && s_loopBlend > 0.0f;
}

void AnimLoopBlendEnd(void)
{
    s_loopOn = false;
}

void AnimDocDrawLoop(const AnimDoc *doc, float t, const AnimSignalPlayer *ovr,
                     bool looping)
{
    Vector2 game = ScreenStateTargetSize();

    // install the override for this draw only (AnimElemProp / AnimElemColorProp
    // consult it), and always tear it down so it can't leak into a later draw.
    s_ovr    = (ovr && ovr->playing) ? ovr : NULL;
    s_ovrDoc = doc;

    AnimLoopBlendBegin(doc, looping);   // ...and the smooth-loop window

    // backgrounds first, in list order, so a global element's fill sits behind
    // every element regardless of its own index.
    for (int i = 0; i < doc->elemCount; i++)
        if (doc->elems[i].kind == AE_GLOBAL)
            DrawGlobalBackground(&doc->elems[i], t, game);

    for (int i = 0; i < doc->elemCount; i++)
    {
        const AnimElem *e = &doc->elems[i];
        switch (e->kind)
        {
            case AE_TEXT:   DrawTextElem(e, t, game, AnimElemTextAt(e, doc, t)); break;
            case AE_SHAPE:  DrawShapeElem(e, t, game);  break;
            case AE_GLOBAL: DrawGlobalElem(e, t, game); break;
        }
    }

    s_ovr = NULL; s_ovrDoc = NULL;
    AnimLoopBlendEnd();
}

void AnimDocDrawEx(const AnimDoc *doc, float t, const AnimSignalPlayer *ovr)
{
    AnimDocDrawLoop(doc, t, ovr, false);
}

void AnimDocDraw(const AnimDoc *doc, float t)
{
    AnimDocDrawLoop(doc, t, NULL, false);
}

// ---------------------------------------------------------------------------
//  Signal player
// ---------------------------------------------------------------------------
void AnimSignalPlayerStart(AnimSignalPlayer *p, const AnimSignal *sig,
                           const AnimDoc *doc, float docTime,
                           const SignalParams *params)
{
    if (!p) return;
    p->sig = sig; p->clock = 0.0f; p->playing = false;
    p->param = params ? *params : (SignalParams){0};
    if (!sig || !doc) return;
    // A signal that only drives posParams / seqTargets (no plain targets) still
    // plays - it has a live pose to ease from and a length to run.
    if (sig->targetCount <= 0 && sig->posParamCount <= 0 &&
        sig->seqTargetCount <= 0)
        return;

    // capture the live pose: this is the implicit key at u=0, so the signal
    // eases FROM whatever is on screen right now.
    for (int i = 0; i < sig->targetCount && i < ANIM_SIG_TARGETS_MAX; i++)
    {
        const AnimSigTarget *tg = &sig->targets[i];
        if (tg->elemIdx < 0 || tg->elemIdx >= doc->elemCount)
        {
            p->fromValue[i] = 0.0f;
            p->fromColor[i] = BLANK;
            continue;
        }
        const AnimElem *e = &doc->elems[tg->elemIdx];
        // read through the PLAIN path: a signal captures the timeline pose, it
        // must not sample a previous signal's override (that would compound).
        const AnimSignalPlayer *save = s_ovr; s_ovr = NULL;
        if (AnimPropIsColor(tg->prop))
            p->fromColor[i] = AnimElemColorProp(e, tg->prop, docTime);
        else
            p->fromValue[i] = AnimElemProp(e, tg->prop, docTime);
        s_ovr = save;
    }

    // capture the live geometry each Mouse-Position binding eases FROM. Read
    // through the plain path (s_ovr off) for the same reason as targets above.
    for (int i = 0; i < sig->posParamCount && i < ANIM_SIG_POS_MAX; i++)
    {
        const AnimSigPosParam *pp = &sig->posParams[i];
        p->fromPose[i].px = p->fromPose[i].py = 0.0f;
        p->fromPose[i].w  = p->fromPose[i].h  = 0.0f;
        p->fromPose[i].scale = 1.0f; p->fromPose[i].corner = false;
        if (pp->elemIdx < 0 || pp->elemIdx >= doc->elemCount) continue;
        const AnimElem *e = &doc->elems[pp->elemIdx];
        const AnimSignalPlayer *save = s_ovr; s_ovr = NULL;
        p->fromPose[i].px    = AnimElemProp(e, AP_S_POS_X, docTime);
        p->fromPose[i].py    = AnimElemProp(e, AP_S_POS_Y, docTime);
        p->fromPose[i].w     = AnimElemProp(e, AP_S_W,     docTime);
        p->fromPose[i].h     = AnimElemProp(e, AP_S_H,     docTime);
        p->fromPose[i].scale = AnimElemProp(e, AP_S_SCALE, docTime);
        s_ovr = save;
        p->fromPose[i].corner = (e->kind == AE_SHAPE && e->cornerMode);
    }
    p->playing = true;
}

void AnimSignalPlayerUpdate(AnimSignalPlayer *p, float dt)
{
    if (!p || !p->playing || !p->sig) return;
    p->clock += dt;
    if (p->clock >= p->sig->length) { p->clock = p->sig->length; p->playing = false; }
}

bool AnimSignalPlayerDone(const AnimSignalPlayer *p)
{
    return !p || !p->playing;
}

// Blend a scalar from an implicit u=0 anchor `v0` through keyed points at
// progress u: `t`/`val`/`ease` are parallel arrays of `n` keys, ascending in t.
// Before the first key it eases from v0; past the last it holds. This is the
// same shape as the target/color eval below, factored so the Mouse-Position
// bindings and the sequence envelope share it.
static float BlendKeyed(float v0, const float *t, const float *val,
                        const int *ease, int n, float u)
{
    if (n == 0) return v0;
    if (u >= t[n - 1]) return val[n - 1];        // past the end: hold
    float at = 0.0f, av = v0;
    int k = 0;
    for (; k < n; k++) if (u <= t[k]) break;
    float bt = t[k], bv = val[k]; int be = ease[k];
    if (k > 0) { at = t[k - 1]; av = val[k - 1]; }
    if (bt <= at) return bv;
    float f = AnimEaseApply(be, (u - at) / (bt - at));
    return av + (bv - av) * f;
}

// Eased point a Mouse-Position binding drives its corner/center to: each key's
// target is (mouse + offset), blended from the live point `live` over u.
static Vector2 PosBindingPoint(const AnimSignalPlayer *p, int i, Vector2 live,
                               float u)
{
    const AnimSigPosParam *pp = &p->sig->posParams[i];
    float t[ANIM_SIG_KEYS_MAX], vx[ANIM_SIG_KEYS_MAX], vy[ANIM_SIG_KEYS_MAX];
    int   e[ANIM_SIG_KEYS_MAX];
    int n = pp->keyCount; if (n > ANIM_SIG_KEYS_MAX) n = ANIM_SIG_KEYS_MAX;
    for (int k = 0; k < n; k++)
    {
        t[k] = pp->keys[k].t; e[k] = pp->keys[k].ease;
        vx[k] = p->param.pos.x + pp->keys[k].offX;
        vy[k] = p->param.pos.y + pp->keys[k].offY;
    }
    return (Vector2){ BlendKeyed(live.x, t, vx, e, n, u),
                      BlendKeyed(live.y, t, vy, e, n, u) };
}

// Mouse-Position bindings (the "--params--" section). Drives a geometry prop of
// `elemIdx` to the emitted mouse position + per-key offset, eased from the live
// pose captured at fire. A center binding translates POS_X/POS_Y; a corner
// binding (corners-mode shape) drives POS_X/POS_Y/W/H, moving that corner while
// the other holds - both corners recomputed to the stored center+size via
// AnimCornersToGeom. Returns false (prop unhandled) when the signal does not
// consume a position, no position was emitted, or nothing binds this prop.
//
// LIMITATION: uses the fire-time pose in canvas fractions (no live px-abs or
// animated-scale tracking) - good enough for the intended "corner to cursor";
// richer modes come later.
static bool PosParamEval(const AnimSignalPlayer *p, int elemIdx, int prop,
                         float *out)
{
    const AnimSignal *sig = p->sig;
    if (!sig->usesPos || !p->param.hasPos) return false;
    // Spawn-anchor mode does NOT replace the slot; it lets the authored value
    // through and adds a constant offset later (AnimSignalPlayerPosAnchor).
    if (sig->posAnchor) return false;
    bool isPos  = (prop == AP_S_POS_X || prop == AP_S_POS_Y ||
                   prop == AP_T_POS_X || prop == AP_T_POS_Y);
    bool isSize = (prop == AP_S_W || prop == AP_S_H);
    if (!isPos && !isSize) return false;

    float u = (sig->length > 0.0f) ? (p->clock / sig->length) : 1.0f;
    if (u < 0.0f) u = 0.0f; if (u > 1.0f) u = 1.0f;

    int iCenter = -1, i0 = -1, i1 = -1;
    for (int i = 0; i < sig->posParamCount && i < ANIM_SIG_POS_MAX; i++)
    {
        if (sig->posParams[i].elemIdx != elemIdx) continue;
        if (p->fromPose[i].corner)
        {
            if (sig->posParams[i].slot == 0) i0 = i; else i1 = i;
        }
        else iCenter = i;
    }

    if (iCenter >= 0 && isPos)                    // center: plain translate
    {
        bool x = (prop == AP_S_POS_X || prop == AP_T_POS_X);
        Vector2 live = { p->fromPose[iCenter].px, p->fromPose[iCenter].py };
        Vector2 pt = PosBindingPoint(p, iCenter, live, u);
        *out = x ? pt.x : pt.y;
        return true;
    }

    if (i0 >= 0 || i1 >= 0)                        // corner: stretch to cursor
    {
        int ref = (i0 >= 0) ? i0 : i1;             // both share the element pose
        float sc = p->fromPose[ref].scale;
        float hx = p->fromPose[ref].w * sc * 0.5f;
        float hy = p->fromPose[ref].h * sc * 0.5f;
        Vector2 P0 = { p->fromPose[ref].px - hx, p->fromPose[ref].py - hy };
        Vector2 P1 = { p->fromPose[ref].px + hx, p->fromPose[ref].py + hy };
        if (i0 >= 0) P0 = PosBindingPoint(p, i0, P0, u);
        if (i1 >= 0) P1 = PosBindingPoint(p, i1, P1, u);
        float cx, cy, w, h;
        AnimCornersToGeom(P0, P1, sc, &cx, &cy, &w, &h);
        switch (prop)
        {
            case AP_S_POS_X: *out = cx; return true;
            case AP_S_POS_Y: *out = cy; return true;
            case AP_S_W:     *out = w;  return true;
            case AP_S_H:     *out = h;  return true;
            default:         return false;
        }
    }
    return false;
}

bool AnimSignalPlayerEval(const AnimSignalPlayer *p, int elemIdx, int prop,
                          float *outValue, Color *outColor)
{
    if (!p || !p->playing || !p->sig) return false;

    // Mouse-Position bindings take precedence over plain targets for the
    // geometry props they drive (they replace the authored value outright).
    if (outValue && PosParamEval(p, elemIdx, prop, outValue)) return true;

    for (int i = 0; i < p->sig->targetCount && i < ANIM_SIG_TARGETS_MAX; i++)
    {
        const AnimSigTarget *tg = &p->sig->targets[i];
        if (tg->elemIdx != elemIdx || tg->prop != prop) continue;
        if (tg->keyCount == 0) return false;         // nothing to drive it with

        // normalized progress; length <= 0 means "instant" -> land on the end.
        float u = (p->sig->length > 0.0f) ? (p->clock / p->sig->length) : 1.0f;
        if (u < 0.0f) u = 0.0f; if (u > 1.0f) u = 1.0f;

        // Build the segment against the IMPLICIT u=0 key (the captured pose):
        // before the first stored key we interpolate from the capture, after
        // the last we hold it.
        AnimKey from;
        from.t = 0.0f; from.ease = ANIM_EASE_LINEAR;
        from.value = p->fromValue[i];
        from.cval  = p->fromColor[i];

        const AnimKey *a = &from, *b = &tg->keys[0];
        if (u >= tg->keys[tg->keyCount - 1].t)
            a = b = &tg->keys[tg->keyCount - 1];     // past the end: hold
        else
        {
            for (int k = 0; k < tg->keyCount; k++)
                if (u <= tg->keys[k].t)
                {
                    b = &tg->keys[k];
                    a = (k == 0) ? &from : &tg->keys[k - 1];
                    break;
                }
        }

        if (AnimPropIsColor(prop))
        {
            if (outColor)
            {
                if (a == b) *outColor = b->cval;
                else
                {
                    float f = SegmentFraction(a, b, u);
                    *outColor = (Color){ MixChannel(a->cval.r, b->cval.r, f),
                                         MixChannel(a->cval.g, b->cval.g, f),
                                         MixChannel(a->cval.b, b->cval.b, f), 255 };
                }
            }
        }
        else if (outValue)
        {
            // The implicit `from` key is the captured live pose, so the target
            // eases FROM whatever is on screen INTO its authored keys.
            if (a == b) *outValue = b->value;
            else *outValue = a->value + (b->value - a->value) *
                                        SegmentFraction(a, b, u);
        }
        return true;
    }
    return false;
}

// Sequence offset (the "--sequence--" section): adds seq * seqMult * env(u) to a
// scalar seqTarget, ON TOP of whatever else drives it, where env(u) is the 0..1
// envelope keyed by seqKeys (eased, implicit 0 at u=0, held after the last key).
// No keys -> a constant full offset. See AnimSignal.usesSeq.
float AnimSignalPlayerSeqOffset(const AnimSignalPlayer *p,
                                int elemIdx, int prop)
{
    if (!p || !p->playing || !p->sig) return 0.0f;
    const AnimSignal *sig = p->sig;
    if (!sig->usesSeq || p->seq == 0 || sig->seqMult == 0.0f) return 0.0f;

    bool match = false;
    for (int i = 0; i < sig->seqTargetCount && i < ANIM_SIG_SEQ_TARGETS; i++)
        if (sig->seqTargets[i].elemIdx == elemIdx &&
            sig->seqTargets[i].prop == prop) { match = true; break; }
    if (!match) return 0.0f;

    float u = (sig->length > 0.0f) ? (p->clock / sig->length) : 1.0f;
    if (u < 0.0f) u = 0.0f; if (u > 1.0f) u = 1.0f;

    float env = 1.0f;
    int n = sig->seqKeyCount; if (n > ANIM_SIG_SEQ_KEYS) n = ANIM_SIG_SEQ_KEYS;
    if (n > 0)
    {
        float t[ANIM_SIG_SEQ_KEYS], val[ANIM_SIG_SEQ_KEYS]; int e[ANIM_SIG_SEQ_KEYS];
        for (int k = 0; k < n; k++)
        { t[k] = sig->seqKeys[k].t; val[k] = sig->seqKeys[k].amt; e[k] = sig->seqKeys[k].ease; }
        env = BlendKeyed(0.0f, t, val, e, n, u);
    }
    return (float)p->seq * sig->seqMult * env;
}

// Spawn-anchor offset (AnimSignal.posAnchor): a constant translation that moves
// the element so its bound point at fire time lands on the emitted position, and
// zero for everything else. The reference point is captured in fromPose at fire
// (px/py = the element center; a corner binding shifts by half the fire-time
// size for its slot), so authored_point(0) never needs re-deriving here. Only
// the FIRST binding of the element is honoured - anchoring one element to two
// different cursor points is contradictory. Scalar POS props only; size is left
// to the authored animation (the element is translated whole, not stretched).
float AnimSignalPlayerPosAnchor(const AnimSignalPlayer *p, int elemIdx, int prop)
{
    if (!p || !p->playing || !p->sig) return 0.0f;
    const AnimSignal *sig = p->sig;
    if (!sig->usesPos || !sig->posAnchor || !p->param.hasPos) return 0.0f;

    bool isX = (prop == AP_S_POS_X || prop == AP_T_POS_X);
    bool isY = (prop == AP_S_POS_Y || prop == AP_T_POS_Y);
    if (!isX && !isY) return 0.0f;

    for (int i = 0; i < sig->posParamCount && i < ANIM_SIG_POS_MAX; i++)
    {
        if (sig->posParams[i].elemIdx != elemIdx) continue;
        // Bound point at fire: the center, or a corner shifted by half-size.
        float px = p->fromPose[i].px, py = p->fromPose[i].py;
        if (p->fromPose[i].corner)
        {
            float sc = p->fromPose[i].scale;
            float hx = p->fromPose[i].w * sc * 0.5f;
            float hy = p->fromPose[i].h * sc * 0.5f;
            float s  = (sig->posParams[i].slot == 0) ? -1.0f : 1.0f;
            px += s * hx; py += s * hy;
        }
        return isX ? (p->param.pos.x - px) : (p->param.pos.y - py);
    }
    return 0.0f;
}

// deterministic pseudo-random in [-1,1] from an int seed (repeatable scatter);
// same trick as scene_anim.c's Rand11, exposed here for the crumble preview.
float Rand11i(int seed)
{
    float s = sinf((float)seed * 12.9898f) * 43758.5455f;
    return 2.0f * (s - floorf(s)) - 1.0f;
}

// ---------------------------------------------------------------------------
//  Player
// ---------------------------------------------------------------------------
void AnimPlayerStart(AnimPlayer *p, const AnimDoc *doc, int dir,
                     float secStart, float secEnd)
{
    p->doc      = doc;
    p->dir      = dir;
    p->secStart = secStart;
    p->secEnd   = secEnd;
    p->clock    = 0.0f;
    p->playing  = true;
    p->introDone = false;
    // loop is left as the caller set it (default false via zeroed struct).
}

void AnimPlayerStartAll(AnimPlayer *p, const AnimDoc *doc, int dir)
{
    // The outro is trimmed: playing "the whole doc" stops at outroStart.
    AnimPlayerStart(p, doc, dir, 0.0f, doc ? AnimDocOutroStart(doc) : 0.0f);
}

static float SectionLen(const AnimPlayer *p)
{
    float len = p->secEnd - p->secStart;
    return len > 0.0f ? len : 0.0f;
}

void AnimPlayerUpdate(AnimPlayer *p, float dt)
{
    if (!p->doc || !p->playing) return;
    p->clock += dt;
    float len = SectionLen(p);
    if (p->clock >= len)
    {
        if (!p->loop) { p->clock = len; p->playing = false; return; }

        // Looping: the INTRO is a one-shot lead-in, so every cycle after the
        // first restarts at introEnd instead of at the section start. Reverse
        // playback has no intro concept - it just wraps the whole section.
        p->introDone = true;
        float loopStart = 0.0f;
        if (p->dir != ANIM_REV)
        {
            float ie = AnimDocIntroEnd(p->doc) - p->secStart;
            if (ie > 0.0f && ie < len) loopStart = ie;
        }
        float cycle = len - loopStart;
        p->clock = (cycle > 0.0f) ? loopStart + fmodf(p->clock - len, cycle)
                                  : loopStart;
    }
}

bool AnimPlayerDone(const AnimPlayer *p)
{
    return !p->playing;
}

void AnimPlayerSeek(AnimPlayer *p, float t)
{
    if (!p->doc) return;
    // Inverse of AnimPlayerSampleTime: reverse playback runs the doc backwards,
    // so the clock is measured from the far end of the section.
    float c = (p->dir == ANIM_REV) ? (p->secEnd - t) : (t - p->secStart);
    float len = SectionLen(p);
    if (c < 0.0f) c = 0.0f;
    if (c > len)  c = len;
    p->clock = c;
}

float AnimPlayerSampleTime(const AnimPlayer *p)
{
    // Forward: secStart -> secEnd. Reverse: secEnd -> secStart (doc runs back).
    return (p->dir == ANIM_REV) ? (p->secEnd - p->clock)
                                : (p->secStart + p->clock);
}

void AnimPlayerDraw(const AnimPlayer *p)
{
    if (!p->doc) return;
    AnimDocDraw(p->doc, AnimPlayerSampleTime(p));
}
