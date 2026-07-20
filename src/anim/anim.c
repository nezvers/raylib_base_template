// ============================================================================
//  anim.c  -  authoring helpers, evaluation and playback for anim.h
//
//  Like scene_anim.c, evaluation is pure data: AnimTrackEval walks keyframes
//  and eases between them. This file only knows HOW each element kind is drawn
//  (text via DrawTextPro, shapes via DrawRectangle/Circle, global as a screen
//  fade). See anim_io.* for load/save and anim_editor for authoring.
// ============================================================================

#include "anim.h"
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
    if (ease < 0 || ease >= ANIM_EASE_COUNT) return s_ease[ANIM_EASE_LINEAR].name;
    return s_ease[ease].name;
}

int AnimEaseByName(const char *name)
{
    EaseTableInit();
    for (int i = 0; i < ANIM_EASE_COUNT; i++)
        if (TextIsEqual(s_ease[i].name, name)) return i;
    return ANIM_EASE_LINEAR;
}

float AnimEaseApply(int ease, float p)
{
    EaseTableInit();
    if (ease <= ANIM_EASE_LINEAR || ease >= ANIM_EASE_COUNT) return p;
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
    doc->elemCount  = 0;
    doc->signalCount = 0;
}

void AnimElemInit(AnimElem *e, AnimElemKind kind)
{
    memset(e, 0, sizeof(*e));
    e->kind         = kind;
    e->color        = RAYWHITE;
    e->shapeKind    = SHAPE_RECT;
    e->outlineColor = RAYWHITE;
    e->outlineFrac  = 0.0f;              // outline off by default
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

bool AnimPropIsColor(int prop)
{
    return prop == AP_T_COLOR || prop == AP_S_COLOR ||
           prop == AP_S_OUTLINE_COLOR || prop == AP_G_COLOR;
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

// Index of `e` within the doc currently being drawn, or -1.
static int OverrideElemIdx(const AnimElem *e)
{
    if (!s_ovrDoc) return -1;
    ptrdiff_t d = e - s_ovrDoc->elems;
    if (d < 0 || d >= s_ovrDoc->elemCount) return -1;
    return (int)d;
}

Color AnimElemColorProp(const AnimElem *e, int prop, float t)
{
    // Exact-prop lookup: a shape can carry BOTH a fill and an outline colour
    // track, so "any colour track" would alias them.
    Color base = (prop == AP_S_OUTLINE_COLOR) ? e->outlineColor : e->color;

    if (s_ovr)
    {
        Color oc;
        int ei = OverrideElemIdx(e);
        if (ei >= 0 && AnimSignalPlayerEval(s_ovr, ei, prop, NULL, &oc))
        {
            oc.a = base.a;      // colour props never carry alpha (see below)
            return oc;
        }
    }

    for (int i = 0; i < e->trackCount; i++)
        if (e->tracks[i].prop == prop)
            return AnimTrackEvalColor(&e->tracks[i], t, base);
    return base;
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
        case AP_T_ROT:   case AP_S_ROT:   return 0.0f;
        case AP_T_CRUMBLE:                return 0.0f;
        case AP_G_FADE:                   return 0.0f;
        default:                          return 0.0f;
    }
}

float AnimElemProp(const AnimElem *e, int prop, float t)
{
    if (s_ovr)
    {
        float ov;
        int ei = OverrideElemIdx(e);
        if (ei >= 0 && AnimSignalPlayerEval(s_ovr, ei, prop, &ov, NULL)) return ov;
    }

    const AnimTrack *tr = NULL;
    for (int i = 0; i < e->trackCount; i++)
        if (e->tracks[i].prop == prop) { tr = &e->tracks[i]; break; }
    if (!tr) return ElemBaseProp(e, prop);
    return AnimTrackEval(tr, t, ElemBaseProp(e, prop));
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
        default:                          return 1.0f;
    }
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

static void DrawTextElem(const AnimElem *e, float t, Vector2 game)
{
    float alpha = AnimElemProp(e, AP_T_ALPHA, t);
    if (alpha <= 0.0f) return;

    float cxF   = AnimElemProp(e, AP_T_POS_X, t);
    float cyF   = AnimElemProp(e, AP_T_POS_Y, t);
    float sizeF = AnimElemProp(e, AP_T_SIZE,  t);
    float rot   = AnimElemProp(e, AP_T_ROT,   t);
    float crumble = AnimElemProp(e, AP_T_CRUMBLE, t);

    float sizePx  = fmaxf(1.0f, game.y * sizeF);
    float spacing = TextSpacingFor(sizePx);
    Font  font    = GetFontDefault();

    Vector2 box   = MeasureTextEx(font, e->text, sizePx, spacing);
    float left    = game.x * cxF - box.x * 0.5f;   // pos = text center
    float top     = game.y * cyF - box.y * 0.5f;
    Color col     = Fade(AnimElemColor(e, t), alpha * (1.0f - crumble));
    if (col.a == 0) return;

    if (crumble > 0.0f)
    {
        // Simple crumble preview: scatter glyphs downward by `crumble`. This is
        // an editor-friendly approximation (deterministic, no physics state) -
        // enough to see the effect on the timeline; runtime players can add the
        // full particle sim later if wanted.
        float scale = sizePx / (float)font.baseSize;
        float penX  = left;
        int   idx   = 0;
        for (const char *c = e->text; *c; c++)
        {
            int cp = (unsigned char)*c;
            GlyphInfo gi = GetGlyphInfo(font, cp);
            float advance = (gi.advanceX == 0 ? gi.image.width : gi.advanceX) * scale
                          + spacing;
            if (cp != ' ')
            {
                char buf[2]  = { (char)cp, 0 };
                float fall   = crumble * crumble * game.y * 0.5f;
                float drift  = sinf((float)idx * 12.9898f) * crumble * game.x * 0.05f;
                Vector2 gsz  = MeasureTextEx(font, buf, sizePx, spacing);
                Vector2 org  = { gsz.x * 0.5f, gsz.y * 0.5f };
                Vector2 ctr  = { penX + org.x + drift, top + org.y + fall };
                float   grot = rot + crumble * (Rand11i(idx) * 90.0f);
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
        DrawTextPro(font, e->text, ctr, org, rot, sizePx, spacing, col);
    }
    else
    {
        DrawTextEx(font, e->text, (Vector2){ left, top }, sizePx, spacing, col);
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
    float thickPx = game.y * AnimElemProp(e, AP_S_OUTLINE, t);
    if (fillA <= 0.0f && (outA <= 0.0f || thickPx < 0.5f)) return;

    float cxF = AnimElemProp(e, AP_S_POS_X, t);
    float cyF = AnimElemProp(e, AP_S_POS_Y, t);
    float wF  = AnimElemProp(e, AP_S_W,     t);
    float hF  = AnimElemProp(e, AP_S_H,     t);
    float rot = AnimElemProp(e, AP_S_ROT,   t);

    Color fill = Fade(AnimElemColorProp(e, AP_S_COLOR,         t), fillA);
    Color line = Fade(AnimElemColorProp(e, AP_S_OUTLINE_COLOR, t), outA);

    Vector2 c  = { game.x * cxF, game.y * cyF };
    float   hw = game.x * wF * 0.5f;
    float   hh = game.y * hF * 0.5f;
    float   cr = cosf(rot * DEG2RAD), sr = sinf(rot * DEG2RAD);

    if (e->shapeKind == SHAPE_LINE)
    {
        // Segment through the center: length = w, thickness = h, fill colour.
        float len = fmaxf(hw, 0.0f), th = fmaxf(game.y * hF, 1.0f);
        Vector2 a = { c.x - len * cr, c.y - len * sr };
        Vector2 b = { c.x + len * cr, c.y + len * sr };
        if (fill.a == 0) return;
        DrawLineEx(a, b, th, fill);
        DrawCircleV(a, th * 0.5f, fill);        // round caps
        DrawCircleV(b, th * 0.5f, fill);
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
    DrawPolyShape(c, rim, n, fill, line, thickPx);
}

static void DrawGlobalElem(const AnimElem *e, float t, Vector2 game)
{
    float fade = AnimElemProp(e, AP_G_FADE, t);
    if (fade <= 0.0f) return;
    DrawRectangle(0, 0, (int)game.x, (int)game.y, Fade(AnimElemColor(e, t), fade));
}

void AnimDocDrawEx(const AnimDoc *doc, float t, const AnimSignalPlayer *ovr)
{
    Vector2 game = ScreenStateTargetSize();

    // install the override for this draw only (AnimElemProp / AnimElemColorProp
    // consult it), and always tear it down so it can't leak into a later draw.
    s_ovr    = (ovr && ovr->playing) ? ovr : NULL;
    s_ovrDoc = doc;

    for (int i = 0; i < doc->elemCount; i++)
    {
        const AnimElem *e = &doc->elems[i];
        switch (e->kind)
        {
            case AE_TEXT:   DrawTextElem(e, t, game);   break;
            case AE_SHAPE:  DrawShapeElem(e, t, game);  break;
            case AE_GLOBAL: DrawGlobalElem(e, t, game); break;
        }
    }

    s_ovr = NULL; s_ovrDoc = NULL;
}

void AnimDocDraw(const AnimDoc *doc, float t)
{
    AnimDocDrawEx(doc, t, NULL);
}

// ---------------------------------------------------------------------------
//  Signal player
// ---------------------------------------------------------------------------
void AnimSignalPlayerStart(AnimSignalPlayer *p, const AnimSignal *sig,
                           const AnimDoc *doc, float docTime)
{
    if (!p) return;
    p->sig = sig; p->clock = 0.0f; p->playing = false;
    if (!sig || sig->targetCount <= 0 || !doc) return;

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

bool AnimSignalPlayerEval(const AnimSignalPlayer *p, int elemIdx, int prop,
                          float *outValue, Color *outColor)
{
    if (!p || !p->playing || !p->sig) return false;

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
            if (a == b) *outValue = b->value;
            else
            {
                float f = SegmentFraction(a, b, u);
                *outValue = a->value + (b->value - a->value) * f;
            }
        }
        return true;
    }
    return false;
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
