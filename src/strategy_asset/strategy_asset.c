// ============================================================================
//  strategy_asset.c  -  evaluation and drawing for authored assets
//
//  Kept free of the game's headers on purpose. strategy_world.c owns the
//  faction palette, but linking it in here would drag the whole simulation into
//  the headless test binary, so the palette arrives through a HOOK the app
//  installs once (StrategyAssetSetFactionTint). Unset, it falls back to a
//  neutral grey - which is exactly what a test wants and what a forge preview
//  can live with.
// ============================================================================

#include "strategy_asset.h"
#include "raymath.h"
#include "rlgl.h"
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
//  Faction palette hook
// ---------------------------------------------------------------------------
static Color (*s_tintFn)(int faction) = NULL;

void StrategyAssetSetFactionTint(Color (*fn)(int faction)) { s_tintFn = fn; }

static Color FactionTint(int faction)
{
    if (s_tintFn) return s_tintFn(faction);
    return (Color){ 160, 160, 160, 255 };
}

// ---------------------------------------------------------------------------
//  Name tables
// ---------------------------------------------------------------------------
static const char *k_catNames[SGA_CATEGORY_COUNT] = { "BUILDING", "UNIT", "RESOURCE" };
static const char *k_kindNames[SGA_KIND_COUNT] = {
    "CUBE", "CUBE WIRES", "SPHERE", "CYLINDER", "BAR", "LINE", "PATH"
};
static const char *k_tintNames[SGA_TINT_COUNT] = { "FIXED", "PARTIAL", "FACTION" };
static const char *k_stateNames[SGA_STATE_COUNT] = {
    "IDLE", "MOVING", "DAMAGED", "ATTACKING", "DIE", "HEALED"
};

const char *StrategyAssetCategoryName(int cat)
{
    if ((cat < 0) || (cat >= SGA_CATEGORY_COUNT)) return k_catNames[SGA_BUILDING];
    return k_catNames[cat];
}

const char *StrategyAssetKindName(int kind)
{
    if ((kind < 0) || (kind >= SGA_KIND_COUNT)) return k_kindNames[SGA_CUBE];
    return k_kindNames[kind];
}

const char *StrategyAssetTintName(int mode)
{
    if ((mode < 0) || (mode >= SGA_TINT_COUNT)) return k_tintNames[SGA_TINT_NONE];
    return k_tintNames[mode];
}

const char *StrategyAssetStateName(int state)
{
    if ((state < 0) || (state >= SGA_STATE_COUNT)) return k_stateNames[SGA_STATE_IDLE];
    return k_stateNames[state];
}

// ---------------------------------------------------------------------------
//  Motion paths
//
//  The ellipse is the base shape and `squareness` pulls it toward a rectangle.
//  Both profiles are sampled at the SAME angle, so the blend is a straight lerp
//  between two points that already correspond - travel stays smooth and nothing
//  jumps as squareness crosses any particular value.
//
//  The square profile is the ellipse's angle projected onto the unit square:
//  divide by whichever of |cos|/|sin| is larger and the point lands on the
//  border. Guarded so the degenerate all-zero angle cannot divide by zero.
// ---------------------------------------------------------------------------
Vector3 StrategyPathPoint(const SgaPath *path, float u)
{
    if (path == NULL) return (Vector3){ 0 };

    float ang = u*2.0f*PI;
    float c = cosf(ang);
    float s = sinf(ang);

    float sq = path->squareness;
    if (sq < 0.0f) sq = 0.0f;
    if (sq > 1.0f) sq = 1.0f;

    float px = c;
    float pz = s;
    if (sq > 0.0f)
    {
        float ac = fabsf(c);
        float as = fabsf(s);
        float m = (ac > as) ? ac : as;
        if (m > 1e-6f)
        {
            // Same angle, pushed out to the unit square's border.
            px = c + (c/m - c)*sq;
            pz = s + (s/m - s)*sq;
        }
    }

    Vector3 p = { px*path->radiusX, 0.0f, pz*path->radiusZ };

    // Euler X then Y then Z, matching how the forge's three sliders read.
    p = Vector3RotateByAxisAngle(p, (Vector3){ 1.0f, 0.0f, 0.0f },
                                 path->rotation.x*DEG2RAD);
    p = Vector3RotateByAxisAngle(p, (Vector3){ 0.0f, 1.0f, 0.0f },
                                 path->rotation.y*DEG2RAD);
    p = Vector3RotateByAxisAngle(p, (Vector3){ 0.0f, 0.0f, 1.0f },
                                 path->rotation.z*DEG2RAD);

    return Vector3Add(p, path->center);
}

// ---------------------------------------------------------------------------
//  Authoring
// ---------------------------------------------------------------------------
static void PartInit(SgaPart *p, int kind)
{
    memset(p, 0, sizeof(*p));
    p->kind = kind;
    p->visible = 1;
    p->sides = 8;
    p->size = (Vector3){ 0.6f, 0.6f, 0.6f };
    p->r0 = 0.3f;
    p->r1 = 0.3f;
    p->h = 0.8f;
    p->offset = (Vector3){ 0.0f, 0.3f, 0.0f };
    p->tintMode = SGA_TINT_FULL;
    p->tintAmount = 1.0f;
    p->color = RAYWHITE;

    p->path.radiusX = 0.5f;
    p->path.radiusZ = 0.5f;

    for (int i = 0; i < SGA_STATE_COUNT; i++) p->anim[i].pathPart = -1;

    TextCopy(p->name, StrategyAssetKindName(kind));
}

void StrategyAssetInit(SgaAsset *a, const char *name)
{
    if (a == NULL) return;
    memset(a, 0, sizeof(*a));

    TextCopy(a->name, (name && name[0]) ? name : "new asset");
    a->category = SGA_UNIT;
    a->subtype[0] = '\0';       // mandatory, and deliberately left empty:
                                // StrategyAssetValid names it as the thing to fill

    // One part, not zero. An empty viewport teaches nothing and gives the
    // camera nothing to frame.
    a->partCount = 1;
    PartInit(&a->parts[0], SGA_CYLINDER);

    StrategyAssetMeasure(a);
}

void StrategyAssetMeasure(SgaAsset *a)
{
    if (a == NULL) return;

    float top = 0.0f;
    float wide = 0.0f;

    for (int i = 0; i < a->partCount; i++)
    {
        const SgaPart *p = &a->parts[i];
        if (p->kind == SGA_PATH) continue;      // paths are not geometry

        float y = p->offset.y;
        float r = 0.0f;

        switch (p->kind)
        {
            case SGA_CUBE:
            case SGA_CUBE_WIRES:
                y += p->size.y*0.5f;
                r = fmaxf(fabsf(p->size.x), fabsf(p->size.z))*0.5f;
                break;

            case SGA_SPHERE:
                y += p->r0;
                r = p->r0;
                break;

            case SGA_CYLINDER:
                // DrawCylinder's position is its BASE, so the top is offset + h.
                y += p->h;
                r = fmaxf(p->r0, p->r1);
                break;

            case SGA_CYLINDER_EX:
            case SGA_LINE:
                y += p->size.y;
                r = fmaxf(fabsf(p->size.x), fabsf(p->size.z));
                if (p->kind == SGA_CYLINDER_EX) r += fmaxf(p->r0, p->r1);
                break;

            default: break;
        }

        // The part's own offset pushes its silhouette outward too.
        float off = fmaxf(fabsf(p->offset.x), fabsf(p->offset.z));
        if (y > top) top = y;
        if (off + r > wide) wide = off + r;
    }

    // Floors so a single flat part still frames sanely instead of putting the
    // preview camera inside the model.
    a->height = (top > 0.05f) ? top : 0.05f;
    a->radius = (wide > 0.05f) ? wide : 0.05f;
}

int StrategyAssetAddPart(SgaAsset *a, int kind)
{
    if ((a == NULL) || (a->partCount >= SGA_PARTS_MAX)) return -1;
    if ((kind < 0) || (kind >= SGA_KIND_COUNT)) kind = SGA_CUBE;

    int idx = a->partCount++;
    PartInit(&a->parts[idx], kind);
    StrategyAssetMeasure(a);
    return idx;
}

// Path references are held as part INDICES, so removing or reordering a part
// has to repoint every one of them or an animation silently starts following
// the wrong path. Both movers below do that fixup; nothing else may touch
// partCount.
static void RepointPaths(SgaAsset *a, int removed)
{
    for (int i = 0; i < a->partCount; i++)
        for (int s = 0; s < SGA_STATE_COUNT; s++)
        {
            int32_t *ref = &a->parts[i].anim[s].pathPart;
            if (*ref == removed)     *ref = -1;         // the path itself is gone
            else if (*ref > removed) (*ref)--;          // everything above shifted
        }
}

bool StrategyAssetRemovePart(SgaAsset *a, int index)
{
    if ((a == NULL) || (index < 0) || (index >= a->partCount)) return false;

    for (int i = index; i < a->partCount - 1; i++) a->parts[i] = a->parts[i + 1];
    a->partCount--;
    memset(&a->parts[a->partCount], 0, sizeof(a->parts[0]));

    RepointPaths(a, index);
    StrategyAssetMeasure(a);
    return true;
}

bool StrategyAssetMovePart(SgaAsset *a, int index, int delta)
{
    if ((a == NULL) || (index < 0) || (index >= a->partCount)) return false;

    int to = index + delta;
    if ((to < 0) || (to >= a->partCount)) return false;

    SgaPart tmp = a->parts[index];
    a->parts[index] = a->parts[to];
    a->parts[to] = tmp;

    // A swap is two renames at once: anything pointing at either slot has to
    // follow its path to the slot it moved to.
    for (int i = 0; i < a->partCount; i++)
        for (int s = 0; s < SGA_STATE_COUNT; s++)
        {
            int32_t *ref = &a->parts[i].anim[s].pathPart;
            if (*ref == index)   *ref = to;
            else if (*ref == to) *ref = index;
        }

    return true;
}

int StrategyAssetDuplicatePart(SgaAsset *a, int index)
{
    if ((a == NULL) || (index < 0) || (index >= a->partCount)) return -1;
    if (a->partCount >= SGA_PARTS_MAX) return -1;

    int idx = a->partCount++;
    a->parts[idx] = a->parts[index];

    // A copy that keeps the original's name is indistinguishable in the list.
    const char *base = a->parts[index].name;
    TextCopy(a->parts[idx].name, TextFormat("%s copy", base));

    StrategyAssetMeasure(a);
    return idx;
}

bool StrategyAssetValid(const SgaAsset *a, const char **why)
{
    if (a == NULL) { if (why) *why = "no asset"; return false; }

    if (a->name[0] == '\0')
    {
        if (why) *why = "Name is required.";
        return false;
    }
    if ((a->category < 0) || (a->category >= SGA_CATEGORY_COUNT))
    {
        if (why) *why = "Category is required.";
        return false;
    }
    if (a->subtype[0] == '\0')
    {
        if (why) *why = "Subtype is required - e.g. \"soldier\", \"quarry\", \"oak\".";
        return false;
    }
    if (a->partCount <= 0)
    {
        if (why) *why = "An asset needs at least one part.";
        return false;
    }

    // A model made only of invisible paths draws nothing at all - almost
    // certainly not what the author meant, and impossible to spot in a gallery.
    bool anyGeometry = false;
    for (int i = 0; i < a->partCount; i++)
        if ((a->parts[i].kind != SGA_PATH) && a->parts[i].visible) anyGeometry = true;

    if (!anyGeometry)
    {
        if (why) *why = "Every part is a path or hidden - nothing would be drawn.";
        return false;
    }

    if (why) *why = NULL;
    return true;
}

// The four ROLE_FACTION_* brightness deltas are the ones in strategy_models.c's
// PartColor. Kept here as literals rather than including that header, so a
// remix reproduces the built-in's colour exactly without this module depending
// on the game.
void StrategyAssetTintFromRole(int role, int32_t *mode, float *amount,
                               float *brightness)
{
    int32_t m = SGA_TINT_FULL;
    float amt = 1.0f;
    float br = 0.0f;

    switch (role)
    {
        case 0: m = SGA_TINT_NONE; amt = 0.0f; break;               // ROLE_FIXED
        case 1: m = SGA_TINT_FULL; amt = 1.0f; break;               // ROLE_FACTION
        case 2: m = SGA_TINT_PARTIAL; amt = 1.0f; br =  0.15f; break;   // LIGHT
        case 3: m = SGA_TINT_PARTIAL; amt = 1.0f; br = -0.25f; break;   // DARK
        case 4: m = SGA_TINT_PARTIAL; amt = 1.0f; br =  0.30f; break;   // BRIGHT
        case 5: m = SGA_TINT_PARTIAL; amt = 1.0f; br = -0.35f; break;   // BEAST
        default: break;
    }

    if (mode) *mode = m;
    if (amount) *amount = amt;
    if (brightness) *brightness = br;
}

// ---------------------------------------------------------------------------
//  Colour
// ---------------------------------------------------------------------------
Color StrategyAssetPartColor(const SgaPart *p, int faction, float alpha)
{
    if (p == NULL) return BLANK;

    Color base = p->color;

    if (p->tintMode == SGA_TINT_FULL)
    {
        base = FactionTint(faction);
    }
    else if (p->tintMode == SGA_TINT_PARTIAL)
    {
        Color f = FactionTint(faction);
        float k = p->tintAmount;
        if (k < 0.0f) k = 0.0f;
        if (k > 1.0f) k = 1.0f;

        // Blend the part's own colour toward the faction's, then apply the
        // brightness delta. Doing it in this order is what lets a part read as
        // "our army, but the dark one" instead of just "dark".
        base.r = (unsigned char)((float)p->color.r + ((float)f.r - (float)p->color.r)*k);
        base.g = (unsigned char)((float)p->color.g + ((float)f.g - (float)p->color.g)*k);
        base.b = (unsigned char)((float)p->color.b + ((float)f.b - (float)p->color.b)*k);
        base.a = p->color.a;
    }

    if (p->brightness != 0.0f) base = ColorBrightness(base, p->brightness);

    return Fade(base, alpha*(float)base.a/255.0f);
}

// ---------------------------------------------------------------------------
//  Easing
// ---------------------------------------------------------------------------
// Cubic bezier through one segment, solved for y at x = p. Same method as
// AnimEasePtsEval: control x's are clamped inside the span so x(t) stays
// monotonic and the solve always converges, while y is left unclamped because
// overshoot is the entire point of a back/elastic curve.
static float BezierSegY(float x0, float y0, float cx0, float cy0,
                        float cx1, float cy1, float x1, float y1, float p)
{
    float span = x1 - x0;
    if (span < 1e-5f) return y1;        // a hold step: snap to the right knot

    float u = (p - x0)/span;
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;

    float a = (cx0 - x0)/span;
    float b = (cx1 - x0)/span;
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    if (b < 0.0f) b = 0.0f;
    if (b > 1.0f) b = 1.0f;

    // Newton on x(t) = u, then bisect if it wanders (flat spots).
    float t = u;
    for (int i = 0; i < 6; i++)
    {
        float mt = 1.0f - t;
        float x = 3.0f*mt*mt*t*a + 3.0f*mt*t*t*b + t*t*t;
        float d = 3.0f*mt*mt*a + 6.0f*mt*t*(b - a) + 3.0f*t*t*(1.0f - b);
        if (fabsf(d) < 1e-6f) break;
        float nt = t - (x - u)/d;
        if (nt < 0.0f) nt = 0.0f;
        if (nt > 1.0f) nt = 1.0f;
        if (fabsf(nt - t) < 1e-6f) { t = nt; break; }
        t = nt;
    }

    float mt = 1.0f - t;
    return mt*mt*mt*y0 + 3.0f*mt*mt*t*cy0 + 3.0f*mt*t*t*cy1 + t*t*t*y1;
}

float StrategyAssetEase(const SgaAsset *a, int index, float p)
{
    if ((a == NULL) || (index < 0) || (index >= a->easeCount)) return p;

    const SgaEase *e = &a->eases[index];
    if (e->ptCount < 2) return p;       // builtin-by-name is resolved by the caller

    if (p <= 0.0f) return e->pts[0].y;
    if (p >= 1.0f) return e->pts[e->ptCount - 1].y;

    for (int i = 0; i < e->ptCount - 1; i++)
    {
        const SgaEasePt *l = &e->pts[i];
        const SgaEasePt *r = &e->pts[i + 1];
        if ((p < l->x) || (p > r->x)) continue;

        return BezierSegY(l->x, l->y, l->x + l->ox, l->y + l->oy,
                          r->x + r->ix, r->y + r->iy, r->x, r->y, p);
    }

    return e->pts[e->ptCount - 1].y;
}

// ---------------------------------------------------------------------------
//  Pose
// ---------------------------------------------------------------------------
void StrategyAssetPartPose(const SgaAsset *a, int partIndex, int state, float t,
                           Vector3 *outOffset, Vector3 *outRot, Vector3 *outScale)
{
    Vector3 off = { 0 };
    Vector3 rot = { 0 };
    Vector3 scl = { 1.0f, 1.0f, 1.0f };

    if ((a == NULL) || (partIndex < 0) || (partIndex >= a->partCount)) goto done;
    if ((state < 0) || (state >= SGA_STATE_COUNT)) state = SGA_STATE_IDLE;

    {
        const SgaPart *p = &a->parts[partIndex];
        const SgaPartAnim *an = &p->anim[state];
        if (an->keyCount <= 0) goto done;

        // Same rules as AnimTrackEval: hold the first value before the first
        // key, hold the last after the last, and let the RIGHT key's ease shape
        // the segment between them.
        const SgaKey *lo = &an->keys[0];
        const SgaKey *hi = &an->keys[an->keyCount - 1];
        float k = 0.0f;

        if (t <= lo->t)      { hi = lo; }
        else if (t >= hi->t) { lo = hi; }
        else
        {
            for (int i = 0; i < an->keyCount - 1; i++)
            {
                if ((t < an->keys[i].t) || (t > an->keys[i + 1].t)) continue;
                lo = &an->keys[i];
                hi = &an->keys[i + 1];
                float span = hi->t - lo->t;
                k = (span > 1e-6f) ? (t - lo->t)/span : 1.0f;
                break;
            }
        }

        float e = StrategyAssetEase(a, hi->ease, k);

        rot.x = lo->rot.x + (hi->rot.x - lo->rot.x)*e;
        rot.y = lo->rot.y + (hi->rot.y - lo->rot.y)*e;
        rot.z = lo->rot.z + (hi->rot.z - lo->rot.z)*e;

        scl.x = lo->scale.x + (hi->scale.x - lo->scale.x)*e;
        scl.y = lo->scale.y + (hi->scale.y - lo->scale.y)*e;
        scl.z = lo->scale.z + (hi->scale.z - lo->scale.z)*e;

        int pi = an->pathPart;
        if ((pi >= 0) && (pi < a->partCount) && (a->parts[pi].kind == SGA_PATH))
        {
            float u = lo->u + (hi->u - lo->u)*e;
            off = StrategyPathPoint(&a->parts[pi].path, u);
        }
    }

done:
    if (outOffset) *outOffset = off;
    if (outRot) *outRot = rot;
    if (outScale) *outScale = scl;
}

// ---------------------------------------------------------------------------
//  Drawing
// ---------------------------------------------------------------------------
void StrategyAssetDraw(const SgaAsset *a, int faction, Vector3 pos, float yawDeg,
                       float alpha, int state, float time)
{
    if (a == NULL) return;

    // The matrix is what lets any of this rotate at all: DrawCube and friends
    // are axis-aligned and take world coordinates, so the only way to spin a
    // model built from them is to turn the modelview underneath.
    rlPushMatrix();
    rlTranslatef(pos.x, pos.y, pos.z);
    rlRotatef(yawDeg, 0.0f, 1.0f, 0.0f);

    for (int i = 0; i < a->partCount; i++)
    {
        const SgaPart *p = &a->parts[i];
        if (!p->visible) continue;
        if (p->kind == SGA_PATH) continue;      // never geometry

        Vector3 aoff, arot, ascl;
        StrategyAssetPartPose(a, i, state, time, &aoff, &arot, &ascl);

        Color c = StrategyAssetPartColor(p, faction, alpha);

        // Per-part animation rides in its own matrix so the rest-pose offset
        // stays the thing the author typed - the animation moves the part, it
        // does not rewrite where the part lives.
        rlPushMatrix();
        rlTranslatef(aoff.x, aoff.y, aoff.z);
        if (arot.y != 0.0f) rlRotatef(arot.y, 0.0f, 1.0f, 0.0f);
        if (arot.x != 0.0f) rlRotatef(arot.x, 1.0f, 0.0f, 0.0f);
        if (arot.z != 0.0f) rlRotatef(arot.z, 0.0f, 0.0f, 1.0f);
        rlScalef(ascl.x, ascl.y, ascl.z);

        Vector3 o = p->offset;

        switch (p->kind)
        {
            case SGA_CUBE:
                DrawCube(o, p->size.x, p->size.y, p->size.z, c);
                break;

            case SGA_CUBE_WIRES:
                DrawCubeWires(o, p->size.x, p->size.y, p->size.z, c);
                break;

            case SGA_SPHERE:
                DrawSphere(o, p->r0, c);
                break;

            case SGA_CYLINDER:
                DrawCylinder(o, p->r0, p->r1, p->h, p->sides, c);
                break;

            case SGA_CYLINDER_EX:
            {
                Vector3 end = { o.x + p->size.x, o.y + p->size.y, o.z + p->size.z };
                DrawCylinderEx(o, end, p->r0, p->r1, p->sides, c);
            } break;

            case SGA_LINE:
            {
                Vector3 end = { o.x + p->size.x, o.y + p->size.y, o.z + p->size.z };
                DrawLine3D(o, end, c);
            } break;

            default: break;
        }

        rlPopMatrix();
    }

    rlPopMatrix();
}

void StrategyAssetDrawPath(const SgaPath *path, Vector3 pos, float yawDeg, Color c)
{
    if (path == NULL) return;

    rlPushMatrix();
    rlTranslatef(pos.x, pos.y, pos.z);
    rlRotatef(yawDeg, 0.0f, 1.0f, 0.0f);

    // 48 segments: smooth enough that a circle reads as a circle, cheap enough
    // to draw one per selected part every frame.
    const int steps = 48;
    Vector3 prev = StrategyPathPoint(path, 0.0f);
    for (int i = 1; i <= steps; i++)
    {
        Vector3 cur = StrategyPathPoint(path, (float)i/(float)steps);
        DrawLine3D(prev, cur, c);
        prev = cur;
    }

    rlPopMatrix();
}
