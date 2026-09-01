// ============================================================================
//  strategy_path_form.c  -  formation geometry, as pure math
//
//  THE MODULE LINE, AGAIN. Everything here answers a question about SHAPE:
//  where slot i of n sits in formation-local space, and how wide the open
//  ground is around a tile. Neither question needs a Unit, a world, or a
//  faction - which is exactly why they live here and not in strategy_move.c.
//
//  WHY THIS WAS EXTRACTED. The formation bug that hurt most (D3) was a constant
//  that could not be satisfied: FORMUP_TIGHT demanded units pack tighter than
//  their own slots were spaced. Nothing caught it because the layout math sat
//  in a file that cannot link into a test - strategy_move.c includes
//  strategy_types.h, which drags in the whole world. Here it is testable, and
//  path_tests asserts spacing, extent and uniqueness for every shape at every
//  size that matters.
//
//  THE SHAPE IS A PLAIN int, deliberately. FormationShape lives in
//  strategy_types.h and this module must never include it; strategy_move.c
//  guards the two enumerations' agreement with a _Static_assert, which fails at
//  compile time if anyone reorders one of them.
// ============================================================================

#include "strategy_path.h"

#include <math.h>
#include <stddef.h>   // NULL: prevSlot is optional

// Slot pitch. Derived from the unit radius by the caller's constant and passed
// in, because this module has no opinion about how big a unit is - but every
// shape below expresses itself in multiples of it, so the whole layout scales
// with one number.

// Golden angle, for FORM_FREEFORM's sunflower placement. Irrational multiples
// of a turn are what stop successive points from ever lining up into spokes.
#define SP_FORM_GOLDEN_ANGLE  2.39996323f

void SpFormSlotLocal(int shape, int i, int count, float spacing,
                     const SpFormCaps *caps, float *outR, float *outF)
{
    float r = 0.0f, f = 0.0f;
    if (count < 1) count = 1;
    if (i < 0) i = 0;

    switch (shape)
    {
        case SP_FORM_LINE:
        {
            // Wide and shallow: fill the width first, only adding depth once a
            // rank is full. This is the shape that answers the spearhead - the
            // whole point is that nobody stands behind anybody.
            //
            // WIDTH IS CAPPED, and it has to be. Two ranks of 100 units is 73
            // world units across, which is most of the long march - the outer
            // slots land off the map, the caller's nearest-open ring cannot
            // pull them back, and those units are stranded. Past the cap the
            // line simply gains ranks, which is what a real line does when it
            // runs out of frontage.
            int perRank = (int)(caps->lineMaxWidth/spacing);
            if (perRank < 1) perRank = 1;
            int wanted = (count + caps->lineRanks - 1)/caps->lineRanks;
            if (wanted < 1) wanted = 1;
            if (wanted < perRank) perRank = wanted;

            int rank = i/perRank, col = i % perRank;
            int fullRanks = count/perRank;
            int inThis = (rank == fullRanks) ? (count % perRank) : perRank;
            if (inThis <= 0) inThis = perRank;
            r = ((float)col - (float)(inThis - 1)*0.5f)*spacing;
            f = -(float)rank*spacing;
        } break;

        case SP_FORM_COLUMN:
        {
            // Narrow and deep. The shape you want for a gap or a bridge, and
            // the one that arrives strung out - which is the honest trade.
            // Widens past the depth cap rather than running off the map.
            int maxRows = (int)(caps->columnMaxDepth/spacing);
            if (maxRows < 1) maxRows = 1;
            int files = caps->columnFiles;
            if (files < 1) files = 1;
            while (files < count && (count + files - 1)/files > maxRows) files++;

            int file = i % files, row = i/files;
            r = ((float)file - (float)(files - 1)*0.5f)*spacing;
            f = -(float)row*spacing;
        } break;

        case SP_FORM_TWO_COLUMN:
        {
            // Two files with a lane between them. Alternating left/right keeps
            // the two sides the same length as units are added.
            //
            // Past the depth cap each side thickens into several sub-files
            // rather than stretching: the lane down the middle is the point of
            // the shape, so it is preserved while the files themselves widen.
            int maxRows = (int)(caps->columnMaxDepth/spacing);
            if (maxRows < 1) maxRows = 1;
            int perSide = (count + 1)/2;
            int sub = 1;
            while (sub < perSide && (perSide + sub - 1)/sub > maxRows) sub++;

            int side = i % 2, idx = i/2;
            int subFile = idx % sub, row = idx/sub;
            float lane = caps->twoColumnLane*0.5f + (float)subFile*spacing;
            r = (side == 0) ? -lane : lane;
            f = -(float)row*spacing;
        } break;

        case SP_FORM_WEDGE:
        {
            // A V with its point forward. Row n holds n+1 units, so the rows
            // grow as they fall back; solving for the row containing slot i is
            // the triangular-number inverse.
            int row = (int)((sqrtf(8.0f*(float)i + 1.0f) - 1.0f)*0.5f);
            int rowStart = row*(row + 1)/2;
            int col = i - rowStart;
            r = ((float)col - (float)row*0.5f)*spacing;
            f = -(float)row*spacing;
        } break;

        case SP_FORM_FREEFORM:
        {
            // Sunflower / golden-angle placement over a disc. NOT uniform
            // random, for two reasons that both matter: it is deterministic, so
            // the same order twice gives the same layout and the slot overlay
            // stays readable frame to frame; and it has no clumps, which
            // uniform random emphatically does at a few hundred samples.
            //
            // RADIUS GROWS AS sqrt(i), which is the whole trick. Area grows as
            // r^2, so spacing the radius by the square root keeps the density
            // constant as the count rises. Grow it linearly instead and a
            // thousand units become a thin ring with nothing in the middle.
            float k = sqrtf((float)i + 0.5f);
            float ang = (float)i*SP_FORM_GOLDEN_ANGLE;
            float rad = k*spacing*SP_FORM_FREEFORM_PITCH;
            r = rad*cosf(ang);
            f = rad*sinf(ang);
        } break;

        case SP_FORM_GRID:
        default:
        {
            int cols = (int)ceilf(sqrtf((float)count));
            if (cols < 1) cols = 1;
            int rows = (count + cols - 1)/cols;
            int row = i/cols, col = i % cols;
            r = ((float)col - (float)(cols - 1)*0.5f)*spacing;
            f = ((float)row - (float)(rows - 1)*0.5f)*spacing;
        } break;
    }

    *outR = r;
    *outF = f;
}

// Mean forward-offset of a shape's slots, which is how far the BLOCK'S MASS sits
// from the point the layout is built around.
//
// WHY THIS EXISTS. GRID centres itself on the destination, but LINE, COLUMN and
// WEDGE all grow backward from it - a 100-unit column's mean slot is eighteen
// world units BEHIND the click. Two things follow, and both were reported as
// bugs. The block never lands where the player pointed; and on a short move the
// rear ranks are ordered to ground behind where they already stand, so they walk
// BACKWARDS to take up a position in the final formation.
//
// Subtracting this from every slot's forward offset re-anchors the shape on its
// own centroid, so every shape lands centred on the click the way GRID always
// did. It does not change the shape - only where the shape is pinned.
float SpFormForwardBias(int shape, int count, float spacing,
                        const SpFormCaps *caps)
{
    if (count < 1) return 0.0f;

    float sum = 0.0f;
    for (int i = 0; i < count; i++)
    {
        float r, f;
        SpFormSlotLocal(shape, i, count, spacing, caps, &r, &f);
        sum += f;
    }
    return sum/(float)count;
}

// Half-extent of the whole block: the furthest any slot sits from the centre.
// Computed by walking the slots rather than by a per-shape closed form, because
// a closed form is a second description of the layout that silently goes stale
// the moment a shape changes - and this runs once per order, not per frame.
float SpFormHalfExtent(int shape, int count, float spacing,
                       const SpFormCaps *caps)
{
    float worst = 0.0f;
    for (int i = 0; i < count; i++)
    {
        float r, f;
        SpFormSlotLocal(shape, i, count, spacing, caps, &r, &f);
        float d = sqrtf(r*r + f*f);
        if (d > worst) worst = d;
    }
    return worst;
}

// ----------------------------------------------------------------------------
//  Corridor width
//
//  How much open ground is there ACROSS the direction of travel? That is the
//  question a formation has to answer before it decides whether the terrain can
//  hold it, and it is deliberately not the question the flow field answers -
//  the field's integration cost says "how far to the goal", so a long way round
//  and a narrow gap read identically. A block that funnels on the strength of
//  that would funnel on every detour.
//
//  Probed perpendicular to travel, counting passable tiles each way up to a
//  cap. The cap is what keeps this cheap: past a couple of dozen tiles the
//  ground is open by any standard a formation cares about, and counting further
//  buys nothing.
// ----------------------------------------------------------------------------
int SpFormCorridorWidth(const SpGrid *g, int tx, int tz,
                        float dirX, float dirZ, int maxTiles)
{
    if (maxTiles < 1) maxTiles = 1;

    // Perpendicular to travel, snapped to whichever axis dominates. A tile grid
    // has no diagonals worth walking here: the probe is a width estimate, and
    // stepping the true perpendicular through a staircase of cells would count
    // the same tile twice as often as it skipped one.
    int px, pz;
    if (fabsf(dirX) > fabsf(dirZ)) { px = 0; pz = 1; }
    else                           { px = 1; pz = 0; }

    int width = 1;
    if (!SpGridPassable(g, tx, tz)) return 0;

    for (int s = 1; s <= maxTiles; s++)
    {
        if (!SpGridPassable(g, tx + px*s, tz + pz*s)) break;
        width++;
    }
    for (int s = 1; s <= maxTiles; s++)
    {
        if (!SpGridPassable(g, tx - px*s, tz - pz*s)) break;
        width++;
    }
    return width;
}

// Insertion sort, exposed here so the assignment and the caller share one.
// n is capped at the formation maximum and this runs on a click, not per frame:
// qsort's function-pointer indirection would cost more than it saves, and
// insertion sort on nearly-sorted input is close to linear.
void SpFormSortByKey(SpFormSortEntry *a, int n)
{
    for (int i = 1; i < n; i++)
    {
        SpFormSortEntry v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j].key > v.key) { a[j+1] = a[j]; j--; }
        a[j+1] = v;
    }
}

// ============================================================================
//  Slot assignment
//
//  WHICH UNIT GOES TO WHICH SLOT. The first cut projected units and slots onto
//  one axis and zipped the sorted orders, which is spatially coherent - nobody
//  crosses the formation - but only along that ONE axis. Across it the pairing
//  is arbitrary, and the cost shows up exactly where the player notices it: a
//  group already standing on the destination, re-ordered, walks an average 30-47%
//  further than it needs to, and individual units cross the whole block to
//  reach a slot an arm's length from where they started.
//
//  THE SEED IS ANGULAR, NOT AXIAL. Sorting both sets by bearing from their own
//  centroid pairs a ring of units to a ring of slots in rotational order, which
//  is right for the case that actually looks bad - a block re-ordered onto
//  itself with a new facing. An axial seed has to undo its own ordering to get
//  there; the angular one starts most of the way home.
//
//  THEN PAIRWISE REPAIR. Repeatedly take two assigned units and swap their slots
//  if the swap shortens the pair. That is a 2-opt on the assignment, and it
//  cannot make things worse by construction - every accepted swap strictly
//  reduces total distance, so the pass is monotone and needs no tolerance.
//
//  FOUR PASSES, MEASURED. Most of the gain is in the first (worst-unit walk at
//  512 units: 29.4 -> 22.0); four is where it flattens (19.3) at 0.7 ms, and
//  sixteen buys 1.5% more for four times the cost. This runs on a click, not per
//  frame, which is the only reason a quadratic-flavoured pass is affordable at
//  all - and why the pass count is a constant rather than a budget.
// ============================================================================
#define SP_FORM_ASSIGN_PASSES 4

// Deterministic pair stride. The repair pass must visit different pairings each
// round or it re-tests the same pairs and stalls; a fixed odd stride walks the
// whole permutation without a PRNG, so the same order twice gives the same
// answer - which the slot overlay depends on to stay readable.
static int SpFormPairStride(int n, int pass)
{
    static const int strides[SP_FORM_ASSIGN_PASSES] = { 1, 3, 7, 13 };
    int s = strides[pass % SP_FORM_ASSIGN_PASSES];
    if (s >= n) s = 1;
    return s;
}

void SpFormAssign(const SpFormPoint *units, const SpFormPoint *slots, int n,
                  int *outSlotOf, SpFormSortEntry *scratchA, SpFormSortEntry *scratchB)
{
    if (n <= 0) return;
    if (n == 1) { outSlotOf[0] = 0; return; }

    // -- Centroids -----------------------------------------------------------
    float ucx = 0.0f, ucz = 0.0f, scx = 0.0f, scz = 0.0f;
    for (int i = 0; i < n; i++)
    {
        ucx += units[i].x; ucz += units[i].z;
        scx += slots[i].x; scz += slots[i].z;
    }
    ucx /= (float)n; ucz /= (float)n;
    scx /= (float)n; scz /= (float)n;

    // -- Angular seed --------------------------------------------------------
    for (int i = 0; i < n; i++)
    {
        scratchA[i].key   = atan2f(units[i].z - ucz, units[i].x - ucx);
        scratchA[i].index = i;
        scratchB[i].key   = atan2f(slots[i].z - scz, slots[i].x - scx);
        scratchB[i].index = i;
    }
    SpFormSortByKey(scratchA, n);
    SpFormSortByKey(scratchB, n);

    for (int k = 0; k < n; k++) outSlotOf[scratchA[k].index] = scratchB[k].index;

    // -- Pairwise repair -----------------------------------------------------
    for (int pass = 0; pass < SP_FORM_ASSIGN_PASSES; pass++)
    {
        int stride = SpFormPairStride(n, pass);
        int swaps  = 0;

        for (int i = 0; i < n; i++)
        {
            int j = i + stride;
            if (j >= n) break;

            int si = outSlotOf[i], sj = outSlotOf[j];

            float aix = units[i].x - slots[si].x, aiz = units[i].z - slots[si].z;
            float ajx = units[j].x - slots[sj].x, ajz = units[j].z - slots[sj].z;
            float bix = units[i].x - slots[sj].x, biz = units[i].z - slots[sj].z;
            float bjx = units[j].x - slots[si].x, bjz = units[j].z - slots[si].z;

            float now   = sqrtf(aix*aix + aiz*aiz) + sqrtf(ajx*ajx + ajz*ajz);
            float swapd = sqrtf(bix*bix + biz*biz) + sqrtf(bjx*bjx + bjz*bjz);

            // Strictly shorter, with a small epsilon so float noise cannot make
            // two equivalent pairings swap back and forth between passes.
            if (swapd < now - 0.0001f)
            {
                outSlotOf[i] = sj;
                outSlotOf[j] = si;
                swaps++;
            }
        }
        if (swaps == 0) break;      // converged: further passes cannot help
    }
}

// ============================================================================
//  Assignment WITH memory
//
//  The plain SpFormAssign above pairs two point sets with no history, which is
//  right the first time a group is ordered and wrong every time after. Slots are
//  rebuilt around each new destination and the facing is re-derived from
//  centroid-to-destination, so an ordinary click swings the facing a few degrees
//  and the from-scratch pairing reshuffles nearly the whole block - measured at
//  33 units in 36 for a 15-degree swing. A formation that reshuffles on every
//  order is not a formation; it is a crowd that happens to be the right shape.
//
//  SO IDENTITY IS THE DEFAULT AND RE-ASSIGNMENT IS THE EXCEPTION. When the shape
//  and the member count are unchanged, every unit keeps the slot INDEX it
//  already holds. The block then translates and rotates rigidly - which is what
//  "keep their position" means - and no unit crosses to the far side, because
//  nobody is re-paired at all.
//
//  WHEN CONTENTION EXISTS, IT IS RESOLVED LOCALLY. Units that were not in the
//  formation (reinforcements, a unit that broke off and came back) have no
//  remembered slot and must be placed. They take the nearest FREE slot - and
//  because everyone who kept their slot is already standing on it, the free
//  slots are exactly the gaps those units left behind. That is why the search is
//  a local one and never sends a unit across the block: the vacancy it is
//  looking for is, by construction, near the hole in the formation.
// ============================================================================

// Assign with remembered slots. `prevSlot[i]` is unit i's slot index from its
// last order, or -1 if it has none / the shape or count changed. Slots already
// claimed by a keeping unit are never handed out twice.
//
// Returns the number of units that had to be placed fresh, which the caller can
// use to tell "the block just turned" from "the block was rebuilt".
int SpFormAssignStable(const SpFormPoint *units, const SpFormPoint *slots, int n,
                       const int *prevSlot, int *outSlotOf,
                       unsigned char *slotTaken)
{
    if (n <= 0) return 0;

    for (int i = 0; i < n; i++) { outSlotOf[i] = -1; slotTaken[i] = 0; }

    // -- Pass 1: everyone who can keep their slot, keeps it -------------------
    // No distance test and no cost comparison. A unit standing in a formation
    // that is merely turning has not moved relative to the block, and asking
    // whether some other slot is now marginally closer is exactly the question
    // whose answer reshuffles the block.
    int kept = 0;
    for (int i = 0; i < n; i++)
    {
        int s = (prevSlot != NULL) ? prevSlot[i] : -1;
        if (s < 0 || s >= n) continue;
        if (slotTaken[s]) continue;         // two units claiming one slot: first wins,
                                            //   the loser is placed below
        slotTaken[s]  = 1;
        outSlotOf[i]  = s;
        kept++;
    }

    // -- Pass 2: place the rest in the nearest free slot ----------------------
    // Nearest-first over the units that still need one. Because the keepers are
    // already on their slots, the free slots ARE the gaps in the block, so this
    // is a local search in practice however it is written.
    int placed = 0;
    for (int i = 0; i < n; i++)
    {
        if (outSlotOf[i] >= 0) continue;

        int best = -1;
        float bestD = 0.0f;
        for (int s = 0; s < n; s++)
        {
            if (slotTaken[s]) continue;
            float dx = units[i].x - slots[s].x;
            float dz = units[i].z - slots[s].z;
            float d  = dx*dx + dz*dz;       // squared: ordering is all that matters
            if (best < 0 || d < bestD) { best = s; bestD = d; }
        }

        if (best < 0) break;                // no free slot left; caller pads
        slotTaken[best] = 1;
        outSlotOf[i]    = best;
        placed++;
    }

    // Anything still unassigned (only possible if slots ran out) falls to the
    // last slot, matching what the caller did before this function existed.
    for (int i = 0; i < n; i++)
        if (outSlotOf[i] < 0) outSlotOf[i] = n - 1;

    (void)kept;
    return placed;
}
