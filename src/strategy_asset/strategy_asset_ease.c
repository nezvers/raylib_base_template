// ============================================================================
//  strategy_asset_ease.c  -  see strategy_asset_ease.h
//
//  EVERY curve is baked as KNOTS, builtins included. That is a deliberate
//  choice and the reason deserves recording, because "resolve builtins by name
//  on load" was the obvious design and it is subtly wrong here.
//
//  strategy_asset.c does not link src/anim/ - on purpose, so the data model and
//  its IO stay headless-testable. Its evaluator therefore has no access to the
//  builtin easing table and can only evaluate knots. A slot that said "I am
//  sineOut, look me up" would play LINEAR everywhere except in code that
//  happens to link the anim module - which is the forge, and not the showcase.
//  A curve that previews correctly while authoring and then flattens in the
//  gallery is the worst possible failure: it is invisible until someone
//  notices the model moves wrong.
//
//  So a builtin is SAMPLED into knots at bake time. The name is kept purely as
//  a label for the picker, the knots are the truth, and every consumer plays
//  the same shape with no lookup. This also means an asset is genuinely
//  standalone in the strong sense: it does not depend on the builtin ENUM
//  either, so appending to AnimEase later cannot reshape an old asset.
//
//  Sampling cost: SGA_EASE_PTS_MAX knots with tangents fitted from the curve's
//  own slope. Eight cubic segments reproduce the smooth curves to well under a
//  pixel. The overshooting ones (backIn/backOut/elasticOut/bounceOut) are the
//  hard cases - bounceOut especially, whose four discontinuous parabolic arcs
//  cannot be matched exactly by eight smooth segments. It is close enough that
//  the motion reads identically, and it is the only honest option given that
//  the alternative plays them as a straight line.
// ============================================================================

#include "strategy_asset_ease.h"
#include "../anim/anim.h"
#include "../anim/anim_ease_custom.h"
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------------------
//  Sampling a builtin into knots
// ---------------------------------------------------------------------------
//  Knots sit at even x. Each knot's handles follow the curve's own slope there,
//  measured by finite difference, with the handle length at a third of the
//  segment - the standard Catmull-Rom-to-bezier conversion, which reproduces a
//  smooth curve through the sampled points rather than merely touching them.
static void BakeBuiltinPts(int easeId, SgaEasePt *out, int32_t *outCount)
{
    const int n = SGA_EASE_PTS_MAX;
    const float step = 1.0f/(float)(n - 1);

    for (int i = 0; i < n; i++)
    {
        float x = (float)i*step;
        if (i == n - 1) x = 1.0f;

        out[i].x = x;
        out[i].y = AnimEaseApply(easeId, x);
        out[i].ix = out[i].iy = 0.0f;
        out[i].ox = out[i].oy = 0.0f;
    }

    // Tangents. A central difference on the sampled curve, then handles at a
    // third of the neighbouring span in x with the matching rise in y.
    for (int i = 0; i < n; i++)
    {
        // Slope from the curve itself, not from the samples: sampling the
        // function twice around x is more faithful than differencing knots that
        // are already an approximation.
        float h = step*0.25f;
        float xa = out[i].x - h, xb = out[i].x + h;
        if (xa < 0.0f) xa = 0.0f;
        if (xb > 1.0f) xb = 1.0f;
        float dx = xb - xa;
        float slope = (dx > 1e-6f)
                    ? (AnimEaseApply(easeId, xb) - AnimEaseApply(easeId, xa))/dx
                    : 0.0f;

        if (i > 0)
        {
            float span = (out[i].x - out[i - 1].x)/3.0f;
            out[i].ix = -span;
            out[i].iy = -span*slope;
        }
        if (i < n - 1)
        {
            float span = (out[i + 1].x - out[i].x)/3.0f;
            out[i].ox = span;
            out[i].oy = span*slope;
        }
    }

    *outCount = n;
}

// ---------------------------------------------------------------------------
//  Baking
// ---------------------------------------------------------------------------
static bool SamePts(const SgaEasePt *a, const SgaEasePt *b, int n)
{
    for (int i = 0; i < n; i++)
    {
        if ((fabsf(a[i].x - b[i].x) > 1e-6f) || (fabsf(a[i].y - b[i].y) > 1e-6f) ||
            (fabsf(a[i].ox - b[i].ox) > 1e-6f) || (fabsf(a[i].oy - b[i].oy) > 1e-6f) ||
            (fabsf(a[i].ix - b[i].ix) > 1e-6f) || (fabsf(a[i].iy - b[i].iy) > 1e-6f))
            return false;
    }
    return true;
}

int StrategyAssetBakeEase(SgaAsset *a, int runtimeEaseId)
{
    if (a == NULL) return -1;
    if (runtimeEaseId <= ANIM_EASE_LINEAR) return -1;   // linear needs no slot
    if (!AnimEaseIdValid(runtimeEaseId)) return -1;

    // Build the slot we WANT, then look for it. Comparing by name alone would
    // miss the case where a custom curve was reshaped since it was baked - the
    // author edited the curve and expects the new shape, which is a different
    // slot, not the same one.
    SgaEase want;
    memset(&want, 0, sizeof(want));

    const char *nm = NULL;
    const AnimCustomEase *ce = AnimCustomEaseGet(runtimeEaseId);
    if (ce != NULL)
    {
        nm = ce->name;
        int n = ce->ptCount;
        if (n > SGA_EASE_PTS_MAX) n = SGA_EASE_PTS_MAX;
        if (n < 2) return -1;
        for (int i = 0; i < n; i++)
        {
            // AnimEasePt and SgaEasePt are the same six floats in the same
            // order, but copied field by field rather than memcpy'd: the two
            // structs live in different headers and nothing enforces that they
            // stay identical.
            want.pts[i].x  = ce->pts[i].x;   want.pts[i].y  = ce->pts[i].y;
            want.pts[i].ix = ce->pts[i].ix;  want.pts[i].iy = ce->pts[i].iy;
            want.pts[i].ox = ce->pts[i].ox;  want.pts[i].oy = ce->pts[i].oy;
        }
        want.ptCount = n;
    }
    else
    {
        nm = AnimEaseName(runtimeEaseId);
        BakeBuiltinPts(runtimeEaseId, want.pts, &want.ptCount);
    }

    if (nm != NULL)
    {
        strncpy(want.name, nm, SGA_EASE_NAME_MAX - 1);
        want.name[SGA_EASE_NAME_MAX - 1] = '\0';
    }

    for (int i = 0; i < a->easeCount; i++)
    {
        const SgaEase *e = &a->eases[i];
        if (e->ptCount != want.ptCount) continue;
        if (strncmp(e->name, want.name, SGA_EASE_NAME_MAX) != 0) continue;
        if (!SamePts(e->pts, want.pts, want.ptCount)) continue;
        return i;
    }

    if (a->easeCount >= SGA_EASES_MAX) return -1;
    a->eases[a->easeCount] = want;
    return a->easeCount++;
}

// ---------------------------------------------------------------------------
//  Reverse lookup, for the picker's "which curve is this key on?"
// ---------------------------------------------------------------------------
int StrategyAssetEaseRuntimeId(const SgaAsset *a, int index)
{
    if ((a == NULL) || (index < 0) || (index >= a->easeCount)) return -1;
    const char *nm = a->eases[index].name;
    if (nm[0] == '\0') return -1;

    int id = AnimCustomEaseByName(nm);
    if (id >= 0) return id;

    // AnimEaseByName answers LINEAR for anything unknown, so a curve baked
    // elsewhere would come back as "linear is selected" - wrong, and it would
    // let a stray click overwrite the baked shape. Confirm the name really is
    // the builtin's before believing it.
    id = AnimEaseByName(nm);
    if ((id > ANIM_EASE_LINEAR) && (strcmp(AnimEaseName(id), nm) == 0)) return id;
    return -1;
}

const char *StrategyAssetEaseName(const SgaAsset *a, int index)
{
    if ((a == NULL) || (index < 0) || (index >= a->easeCount)) return "linear";
    if (a->eases[index].name[0] == '\0') return "custom";
    return a->eases[index].name;
}

float StrategyAssetEaseApplyBaked(const SgaAsset *a, int index, float p)
{
    // The knots are the truth for builtins too (see the file header), so this
    // is just the asset's own evaluator. It exists as a named entry point so
    // the forge's curve preview cannot drift from what playback does.
    return StrategyAssetEase(a, index, p);
}

// ---------------------------------------------------------------------------
//  Compaction
// ---------------------------------------------------------------------------
void StrategyAssetCompactEases(SgaAsset *a)
{
    if (a == NULL) return;

    bool used[SGA_EASES_MAX];
    memset(used, 0, sizeof(used));

    for (int p = 0; p < a->partCount; p++)
        for (int st = 0; st < SGA_STATE_COUNT; st++)
        {
            const SgaPartAnim *an = &a->parts[p].anim[st];
            for (int k = 0; k < an->keyCount; k++)
            {
                int e = an->keys[k].ease;
                if ((e >= 0) && (e < a->easeCount)) used[e] = true;
            }
        }

    // remap[old] = new, or -1 for a slot nothing points at.
    int remap[SGA_EASES_MAX];
    int n = 0;
    for (int i = 0; i < a->easeCount; i++)
    {
        if (!used[i]) { remap[i] = -1; continue; }
        remap[i] = n;
        if (n != i) a->eases[n] = a->eases[i];
        n++;
    }
    for (int i = n; i < SGA_EASES_MAX; i++) memset(&a->eases[i], 0, sizeof(SgaEase));
    a->easeCount = n;

    for (int p = 0; p < a->partCount; p++)
        for (int st = 0; st < SGA_STATE_COUNT; st++)
        {
            SgaPartAnim *an = &a->parts[p].anim[st];
            for (int k = 0; k < an->keyCount; k++)
            {
                int e = an->keys[k].ease;
                an->keys[k].ease = ((e >= 0) && (e < SGA_EASES_MAX)) ? remap[e] : -1;
            }
        }
}
