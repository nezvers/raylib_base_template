// ============================================================================
//  anim_ease_custom.c  -  user-authored multi-segment bezier easings + hides
//
//  Evaluation is the CSS timing-function scheme, applied per segment: find the
//  segment whose knots straddle the progress p, solve x(t) = p for t on that
//  segment's cubic (Newton with a bisection fallback) and return y(t). Control
//  x's are clamped inside the segment's x span so x(t) stays monotonic and the
//  solve always converges; y's may overshoot freely. A segment with no x span
//  is a hold: it snaps straight to the right knot's y.
// ============================================================================

#include "anim_ease_custom.h"
#include "raylib.h"     // TextIsEqual, TextCopy
#include <stdio.h>
#include <string.h>

static AnimCustomEase s_custom[ANIM_CUSTOM_EASE_MAX];
static bool           s_builtinHidden[ANIM_EASE_COUNT];

// ---------------------------------------------------------------------------
//  Bezier math
// ---------------------------------------------------------------------------
static float Bez1(float t, float c1, float c2)
{
    // cubic bezier component with endpoints 0 and 1:
    //   B(t) = 3(1-t)^2 t c1 + 3(1-t) t^2 c2 + t^3
    float u = 1.0f - t;
    return 3.0f*u*u*t*c1 + 3.0f*u*t*t*c2 + t*t*t;
}

static float Bez1d(float t, float c1, float c2)
{
    float u = 1.0f - t;
    return 3.0f*u*u*c1 + 6.0f*u*t*(c2 - c1) + 3.0f*t*t*(1.0f - c2);
}

static float SolveX(float p, float x1, float x2)
{
    // Newton first - converges in a few steps for sane curves.
    float t = p;
    for (int i = 0; i < 6; i++)
    {
        float d = Bez1d(t, x1, x2);
        if (d < 1e-5f) break;
        t -= (Bez1(t, x1, x2) - p) / d;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
    }
    if (Bez1(t, x1, x2) - p < 0.001f && Bez1(t, x1, x2) - p > -0.001f) return t;

    // Bisection fallback (x(t) is monotonic with clamped control x's).
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 24; i++)
    {
        t = (lo + hi) * 0.5f;
        if (Bez1(t, x1, x2) < p) lo = t; else hi = t;
    }
    return t;
}

static float ClampX(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// ---------------------------------------------------------------------------
//  Multi-segment eval: normalise the straddling segment to a unit cubic and
//  reuse the single-cubic solver on it.
// ---------------------------------------------------------------------------
float AnimEasePtsEval(const AnimEasePt *pts, int ptCount, float p)
{
    if (!pts || ptCount < 2) return p;
    if (p <= 0.0f) return pts[0].y;
    if (p >= 1.0f) return pts[ptCount-1].y;

    int s = 0;
    for (int i = 0; i < ptCount - 1; i++)
        if (p >= pts[i].x) s = i; else break;

    const AnimEasePt *a = &pts[s], *b = &pts[s+1];
    float span = b->x - a->x;
    if (span < 1e-5f) return b->y;              // hold step

    // controls as fractions of the segment's x span / y delta.
    float c1x = ClampX((a->ox) / span);
    float c2x = ClampX(1.0f + (b->ix) / span);
    float t   = SolveX((p - a->x) / span, c1x, c2x);

    // y is not normalised (the delta may be zero or negative), so evaluate the
    // cubic on absolute control y's.
    float u = 1.0f - t;
    float p0 = a->y, p1 = a->y + a->oy, p2 = b->y + b->iy, p3 = b->y;
    return u*u*u*p0 + 3.0f*u*u*t*p1 + 3.0f*u*t*t*p2 + t*t*t*p3;
}

void AnimEasePtsFromCubic(AnimEasePt *out, float x1, float y1, float x2, float y2)
{
    out[0] = (AnimEasePt){ 0.0f, 0.0f, 0.0f, 0.0f, x1, y1 };
    out[1] = (AnimEasePt){ 1.0f, 1.0f, x2 - 1.0f, y2 - 1.0f, 0.0f, 0.0f };
}

// Sanitises a caller's point list into a slot: endpoints pinned to x 0 and 1,
// knots kept in ascending x, count clamped.
static void StorePts(AnimCustomEase *c, const AnimEasePt *pts, int ptCount)
{
    if (ptCount < 2) ptCount = 2;
    if (ptCount > ANIM_EASE_PTS_MAX) ptCount = ANIM_EASE_PTS_MAX;
    for (int i = 0; i < ptCount; i++) c->pts[i] = pts[i];
    c->pts[0].x = 0.0f;
    c->pts[ptCount-1].x = 1.0f;
    for (int i = 1; i < ptCount - 1; i++)
    {
        c->pts[i].x = ClampX(c->pts[i].x);
        if (c->pts[i].x < c->pts[i-1].x) c->pts[i].x = c->pts[i-1].x;
    }
    c->ptCount = ptCount;
}

// ---------------------------------------------------------------------------
//  Slot access
// ---------------------------------------------------------------------------
static AnimCustomEase *Slot(int easeId)
{
    int s = easeId - ANIM_EASE_COUNT;
    if (s < 0 || s >= ANIM_CUSTOM_EASE_MAX || !s_custom[s].used) return NULL;
    return &s_custom[s];
}

const AnimCustomEase *AnimCustomEaseGet(int easeId) { return Slot(easeId); }

const char *AnimCustomEaseName(int easeId)
{
    AnimCustomEase *c = Slot(easeId);
    return c ? c->name : NULL;
}

int AnimCustomEaseByName(const char *name)
{
    for (int i = 0; i < ANIM_CUSTOM_EASE_MAX; i++)
        if (s_custom[i].used && TextIsEqual(s_custom[i].name, name))
            return ANIM_EASE_COUNT + i;
    return -1;
}

float AnimCustomEaseEval(int easeId, float p)
{
    AnimCustomEase *c = Slot(easeId);
    if (!c) return p;
    return AnimEasePtsEval(c->pts, c->ptCount, p);
}

// A name is storable when it fits the slot, has no whitespace (one token per
// line in the .cfg) and doesn't collide with any existing easing.
static bool NameOk(const char *name)
{
    if (!name || !name[0]) return false;
    int n = 0;
    for (const char *c = name; *c; c++, n++)
        if (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r' || *c == '#')
            return false;
    if (n >= ANIM_CUSTOM_NAME_MAX) return false;
    if (AnimCustomEaseByName(name) >= 0) return false;
    for (int i = 0; i < ANIM_EASE_COUNT; i++)
        if (TextIsEqual(AnimEaseName(i), name)) return false;
    return true;
}

int AnimCustomEaseAdd(const char *name, const AnimEasePt *pts, int ptCount)
{
    if (!NameOk(name) || !pts) return -1;
    for (int i = 0; i < ANIM_CUSTOM_EASE_MAX; i++)
    {
        if (s_custom[i].used) continue;
        s_custom[i] = (AnimCustomEase){ 0 };
        TextCopy(s_custom[i].name, name);
        StorePts(&s_custom[i], pts, ptCount);
        s_custom[i].used = true;
        return ANIM_EASE_COUNT + i;
    }
    return -1;
}

bool AnimCustomEaseUpdate(int easeId, const AnimEasePt *pts, int ptCount)
{
    AnimCustomEase *c = Slot(easeId);
    if (!c || !pts) return false;
    StorePts(c, pts, ptCount);
    return true;
}

bool AnimCustomEaseRemove(int easeId)
{
    AnimCustomEase *c = Slot(easeId);
    if (!c) return false;
    c->used = false;
    return true;
}

// ---------------------------------------------------------------------------
//  Hide flags (dropdown filter only; evaluation never checks them)
// ---------------------------------------------------------------------------
void AnimEaseSetHidden(int easeId, bool hidden)
{
    if (easeId > ANIM_EASE_LINEAR && easeId < ANIM_EASE_COUNT)
        s_builtinHidden[easeId] = hidden;       // linear is never hideable
    AnimCustomEase *c = Slot(easeId);
    if (c) c->hidden = hidden;
}

bool AnimEaseIsHidden(int easeId)
{
    if (easeId >= 0 && easeId < ANIM_EASE_COUNT) return s_builtinHidden[easeId];
    AnimCustomEase *c = Slot(easeId);
    return c ? c->hidden : false;
}

int AnimEaseIdRange(void) { return ANIM_EASE_COUNT + ANIM_CUSTOM_EASE_MAX; }

bool AnimEaseIdValid(int easeId)
{
    if (easeId >= 0 && easeId < ANIM_EASE_COUNT) return true;
    return Slot(easeId) != NULL;
}

// ---------------------------------------------------------------------------
//  Persistence  (anims/_easings.cfg)
// ---------------------------------------------------------------------------
bool AnimCustomEasesLoad(const char *path)
{
    // The file is the whole truth: wipe the current set first.
    memset(s_custom, 0, sizeof(s_custom));
    memset(s_builtinHidden, 0, sizeof(s_builtinHidden));

    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        char name[64];
        int  used = 0;
        if (sscanf(line, "ease %63s %n", name, &used) == 1 && used > 0)
        {
            // Read every float after the name: 4 of them is the legacy single
            // cubic, otherwise it's <n> followed by n knots of 6 floats.
            float v[1 + ANIM_EASE_PTS_MAX*6];
            int n = 0, adv = 0;
            const char *p = line + used;
            while (n < (int)(sizeof(v)/sizeof(v[0])) &&
                   sscanf(p, "%f %n", &v[n], &adv) == 1) { n++; p += adv; }

            AnimEasePt pts[ANIM_EASE_PTS_MAX];
            int ptCount = 0;
            if (n == 4)
            {
                AnimEasePtsFromCubic(pts, v[0], v[1], v[2], v[3]);
                ptCount = 2;
            }
            else if (n >= 7)
            {
                ptCount = (int)v[0];
                if (ptCount < 2) ptCount = 2;
                if (ptCount > ANIM_EASE_PTS_MAX) ptCount = ANIM_EASE_PTS_MAX;
                if (n < 1 + ptCount*6) ptCount = (n - 1) / 6;
                for (int i = 0; i < ptCount; i++)
                {
                    const float *q = &v[1 + i*6];
                    pts[i] = (AnimEasePt){ q[0], q[1], q[2], q[3], q[4], q[5] };
                }
            }
            if (ptCount >= 2) AnimCustomEaseAdd(name, pts, ptCount);
        }
        else if (sscanf(line, "hide %63s", name) == 1)
        {
            int id = AnimEaseByName(name);      // resolves customs too
            if (id > ANIM_EASE_LINEAR || AnimCustomEaseByName(name) >= 0)
                AnimEaseSetHidden(AnimCustomEaseByName(name) >= 0
                                  ? AnimCustomEaseByName(name) : id, true);
        }
    }
    fclose(f);
    return true;
}

bool AnimCustomEasesSave(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "# shared easing set - see anim_ease_custom.h for the grammar\n");
    fprintf(f, "# ease <name> <n> then n * (x y ix iy ox oy)   knots (0,0)-(1,1)\n");
    fprintf(f, "# hide <name>                                  dropdown filter only\n");
    for (int i = 0; i < ANIM_CUSTOM_EASE_MAX; i++)
    {
        if (!s_custom[i].used) continue;
        fprintf(f, "ease %s %d", s_custom[i].name, s_custom[i].ptCount);
        for (int k = 0; k < s_custom[i].ptCount; k++)
        {
            const AnimEasePt *p = &s_custom[i].pts[k];
            fprintf(f, "  %g %g %g %g %g %g",
                    p->x, p->y, p->ix, p->iy, p->ox, p->oy);
        }
        fprintf(f, "\n");
    }
    for (int i = 0; i < ANIM_EASE_COUNT; i++)
        if (s_builtinHidden[i]) fprintf(f, "hide %s\n", AnimEaseName(i));
    for (int i = 0; i < ANIM_CUSTOM_EASE_MAX; i++)
        if (s_custom[i].used && s_custom[i].hidden)
            fprintf(f, "hide %s\n", s_custom[i].name);
    fclose(f);
    return true;
}
