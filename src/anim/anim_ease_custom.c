// ============================================================================
//  anim_ease_custom.c  -  user-authored cubic-bezier easings + hide flags
//
//  Evaluation is the CSS timing-function scheme: the bezier maps a parameter
//  t to a point (x(t), y(t)) with fixed endpoints (0,0) and (1,1); easing a
//  progress p means solving x(t) = p for t (Newton with a bisection fallback)
//  and returning y(t). Control x's are clamped to [0,1] so x(t) stays
//  monotonic and the solve always converges; y's may overshoot freely.
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
    if (p <= 0.0f) return 0.0f;
    if (p >= 1.0f) return 1.0f;
    return Bez1(SolveX(p, ClampX(c->x1), ClampX(c->x2)), c->y1, c->y2);
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

int AnimCustomEaseAdd(const char *name, float x1, float y1, float x2, float y2)
{
    if (!NameOk(name)) return -1;
    for (int i = 0; i < ANIM_CUSTOM_EASE_MAX; i++)
    {
        if (s_custom[i].used) continue;
        s_custom[i] = (AnimCustomEase){ 0 };
        TextCopy(s_custom[i].name, name);
        s_custom[i].x1 = ClampX(x1); s_custom[i].y1 = y1;
        s_custom[i].x2 = ClampX(x2); s_custom[i].y2 = y2;
        s_custom[i].used = true;
        return ANIM_EASE_COUNT + i;
    }
    return -1;
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

    char line[128];
    while (fgets(line, sizeof(line), f))
    {
        char name[64];
        float x1, y1, x2, y2;
        if (sscanf(line, "ease %63s %f %f %f %f", name, &x1, &y1, &x2, &y2) == 5)
            AnimCustomEaseAdd(name, x1, y1, x2, y2);
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
    fprintf(f, "# ease <name> <x1> <y1> <x2> <y2>   cubic bezier (0,0)-(1,1)\n");
    fprintf(f, "# hide <name>                       dropdown filter only\n");
    for (int i = 0; i < ANIM_CUSTOM_EASE_MAX; i++)
        if (s_custom[i].used)
            fprintf(f, "ease %s %f %f %f %f\n", s_custom[i].name,
                    s_custom[i].x1, s_custom[i].y1, s_custom[i].x2, s_custom[i].y2);
    for (int i = 0; i < ANIM_EASE_COUNT; i++)
        if (s_builtinHidden[i]) fprintf(f, "hide %s\n", AnimEaseName(i));
    for (int i = 0; i < ANIM_CUSTOM_EASE_MAX; i++)
        if (s_custom[i].used && s_custom[i].hidden)
            fprintf(f, "hide %s\n", s_custom[i].name);
    fclose(f);
    return true;
}
