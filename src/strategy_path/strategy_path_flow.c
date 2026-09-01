// ============================================================================
//  strategy_path_flow.c  -  flow fields: one answer for a whole crowd
//
//  A* answers "how do I get from HERE to there". A flow field answers "which
//  way to there, from ANYWHERE" - one build, then every unit reads its own tile
//  and walks. That inversion is the whole point: the cost of an order stops
//  scaling with the number of units it was given to.
//
//  THE BUILD IS A DIJKSTRA FROM THE GOAL, OUTWARD. Backwards from A*, and it has
//  to be: a search from each unit would be one search per unit, which is what
//  this replaces. Relaxing outward from the destination visits every cell once
//  and leaves each holding its true cost-to-goal.
//
//  THE QUEUE IS A BINARY HEAP, reversing the plan's bucketed Dijkstra. See the
//  comment above FlowBuild for why - the short version is that the ring's O(1)
//  pop rests on three simultaneous invariants that produced five successive
//  plausible-but-wrong fields, and a field build costs well under a millisecond
//  either way on any map that actually exists.
//
//  THE GRAPH IS DIRECTED. Entering a tile costs that tile's terrain, so the
//  cheapest way there is not the cheapest way back. This build runs outward
//  from the goal while units walk inward, which decides which tile's cost each
//  step is charged - again, see FlowBuild.
// ============================================================================

#include "strategy_path.h"

#include <string.h>

#define SP_STEP_ORTHO   10
#define SP_STEP_DIAG    14
#define SP_COST_MAX     3           // highest terrain cost: SP_COST_SKIRT

// Goal keys are coarsened to 2x2 tile blocks so near-identical clicks share a
// field. Two tiles of error vanishes under formation offsets.
#define GOAL_COARSEN    2

static SpFlowField   s_fields[SP_FLOW_FIELDS_MAX];
static const SpGrid *s_grid;
static uint32_t      s_navVersion;      // set by SpFlowSetVersion on every change
static int32_t       s_sweepMark[SP_FLOW_FIELDS_MAX];

// ----------------------------------------------------------------------------
//  Lifecycle
// ----------------------------------------------------------------------------
void SpFlowReset(const SpGrid *g)
{
    s_grid = g;
    s_navVersion = 0;
    for (int i = 0; i < SP_FLOW_FIELDS_MAX; i++)
    {
        s_fields[i].live     = false;
        s_fields[i].refCount = 0;
    }
}

int SpFlowLiveCount(void)
{
    int n = 0;
    for (int i = 0; i < SP_FLOW_FIELDS_MAX; i++) if (s_fields[i].live) n++;
    return n;
}

static SpCell GoalKey(int gx, int gz)
{
    // Snap to the block centre so every goal in the block produces the same key
    // AND the same actual destination - otherwise two units sharing a field
    // would be walking to subtly different places.
    int bx = (gx/GOAL_COARSEN)*GOAL_COARSEN;
    int bz = (gz/GOAL_COARSEN)*GOAL_COARSEN;
    if (!SpGridPassable(s_grid, bx, bz))
    {
        // The block centre landed in a wall; keep the caller's own tile, which
        // SpNearestOpen already guaranteed is open. Sharing is a nice-to-have,
        // correctness is not.
        bx = gx; bz = gz;
    }
    return (SpCell)(bz*s_grid->w + bx);
}

// A wall placed or demolished invalidates every field over the whole grid - a
// field is a global answer, so any local change can make part of it wrong. That
// is cheap to detect (one integer compare) and expensive to be wrong about, so
// it is deliberately all-or-nothing rather than an attempt at partial repair.
void SpFlowSetVersion(uint32_t version) { s_navVersion = version; }

bool SpFlowValid(SpFieldId id)
{
    if (id < 0 || id >= SP_FLOW_FIELDS_MAX) return false;
    const SpFlowField *f = &s_fields[id];
    return f->live && (f->navVersion == s_navVersion);
}

// ----------------------------------------------------------------------------
//  The build
//
//  A BINARY HEAP, NOT THE BUCKET RING THE PLAN CALLED FOR - and that is a
//  deliberate reversal, recorded here so it is not "optimised" back.
//
//  Bucketed Dijkstra really is O(cells) against the heap's O(cells log cells),
//  and on paper that is ~65k operations versus ~1M on the largest grid. But the
//  ring is only correct while three separate invariants hold at once: every
//  relaxation must land inside one lap, a bucket holds several costs at a time
//  so staleness cannot be judged from the bucket index, and a future entry must
//  survive a lap that walks past it. Five attempts here produced five different
//  wrong fields, every one of which looked completely plausible - the arrows all
//  pointed downhill and every unit arrived, the routes were simply not the
//  cheapest ones. Only the cross-check against a plain Dijkstra told them apart.
//
//  The measured cost decides it: a field build on the largest AUTHORED map
//  (96x96, 9216 cells - the 65k worst case is a grid size no map uses) is well
//  under a millisecond either way, and it happens on a group order, not per
//  frame. Paying log(cells) for a queue whose correctness is self-evident is
//  the right trade. If a profiler ever shows field builds mattering, the ring
//  can come back - with the Dijkstra cross-check already in place to keep it
//  honest.
// ----------------------------------------------------------------------------
static SpCell   s_heap[SP_CELLS_MAX];
static uint16_t s_heapKey[SP_CELLS_MAX];    // key of the entry at each heap slot
static int32_t  s_heapCount;

static void HeapPush(SpCell c, uint16_t key)
{
    int32_t i = s_heapCount++;
    while (i > 0)
    {
        int32_t parent = (i - 1)/2;
        if (s_heapKey[parent] <= key) break;
        s_heap[i]    = s_heap[parent];
        s_heapKey[i] = s_heapKey[parent];
        i = parent;
    }
    s_heap[i]    = c;
    s_heapKey[i] = key;
}

static SpCell HeapPop(uint16_t *outKey)
{
    SpCell top = s_heap[0];
    *outKey    = s_heapKey[0];

    s_heapCount--;
    if (s_heapCount > 0)
    {
        SpCell   mc = s_heap[s_heapCount];
        uint16_t mk = s_heapKey[s_heapCount];
        int32_t i = 0;
        for (;;)
        {
            int32_t l = 2*i + 1;
            if (l >= s_heapCount) break;
            int32_t r = l + 1;
            int32_t best = (r < s_heapCount && s_heapKey[r] < s_heapKey[l]) ? r : l;
            if (s_heapKey[best] >= mk) break;
            s_heap[i]    = s_heap[best];
            s_heapKey[i] = s_heapKey[best];
            i = best;
        }
        s_heap[i]    = mc;
        s_heapKey[i] = mk;
    }
    return top;
}

static void FlowBuild(SpFlowField *f, int gx, int gz)
{
    const SpGrid *g = s_grid;
    int cells = g->w*g->h;

    for (int i = 0; i < cells; i++) f->integration[i] = SP_FLOW_UNREACHED;
    s_heapCount = 0;

    SpCell goal = (SpCell)(gz*g->w + gx);
    f->integration[goal] = 0;
    HeapPush(goal, 0);

    // Byte-identical to the table in strategy_path_astar.c. Both cover the same
    // eight neighbours either way, so a mismatch is not a bug today - but two
    // tables that are "the same set in a different order" is precisely how a
    // real divergence gets introduced later without anyone noticing.
    static const int DX[8] = {  1, -1,  0,  0,  1,  1, -1, -1 };
    static const int DZ[8] = {  0,  0,  1, -1,  1, -1,  1, -1 };

    while (s_heapCount > 0)
    {
        uint16_t key;
        SpCell c = HeapPop(&key);

        // Lazy deletion: a cell relaxed cheaper after being pushed leaves its
        // dearer copy in the heap. When that surfaces, its key no longer matches
        // the cell's recorded cost, so it is skipped. Cheaper than a decrease-key
        // and impossible to get subtly wrong.
        if (key != f->integration[c]) continue;

        int cx = (int)(c % (SpCell)g->w);
        int cz = (int)(c / (SpCell)g->w);

        // THE GRAPH IS DIRECTED, AND THIS BUILD RUNS AGAINST THE TRAFFIC.
        //
        // A step costs the terrain of the tile being ENTERED - that is what A*
        // charges, and it is what makes crossing a ford expensive rather than
        // leaving one. So cost(A->B) is not cost(B->A) wherever the two tiles
        // differ: the graph is directed even though the walls are not.
        //
        // The relaxation below runs OUTWARD from the goal, but the unit will
        // walk the other way. The edge being priced is the neighbour stepping
        // INTO this cell, so the terrain charged is THIS cell's, not the
        // neighbour's.
        //
        // Backwards, this produces a field that is internally consistent and
        // entirely plausible - every arrow downhill, every unit arriving -
        // while pricing every route as though it were walked in reverse. On
        // uniform ground the two are identical, which is why it survives every
        // maze test; it diverges only once terrain costs vary.
        uint32_t hereCost = (uint32_t)SpGridCost(g, cx, cz);

        for (int i = 0; i < 8; i++)
        {
            int nx = cx + DX[i], nz = cz + DZ[i];
            if (SpGridCost(g, nx, nz) == SP_COST_BLOCKED) continue;

            // Same corner rule as A* and SpLosClear. A field that permits a
            // corner cut sends units diagonally between two walls, where they
            // wedge - and unlike a bad A* path, EVERY unit on the field does it.
            if (i >= 4 && (!SpGridPassable(g, cx + DX[i], cz) ||
                           !SpGridPassable(g, cx, cz + DZ[i]))) continue;

            uint32_t step = (uint32_t)((i >= 4) ? SP_STEP_DIAG : SP_STEP_ORTHO)*hereCost;
            uint32_t nd   = (uint32_t)f->integration[c] + step;
            if (nd >= SP_FLOW_UNREACHED) continue;      // saturate rather than wrap

            SpCell nc = (SpCell)(nz*g->w + nx);
            if (nd >= f->integration[nc]) continue;

            f->integration[nc] = (uint16_t)nd;
            HeapPush(nc, (uint16_t)nd);
        }
    }

    // -- Direction pass -------------------------------------------------------
    // Steepest descent on the integration field. Done as a separate pass rather
    // than during relaxation because a cell's cheapest neighbour is not final
    // until everything around it has settled.
    for (int z = 0; z < g->h; z++)
    {
        for (int x = 0; x < g->w; x++)
        {
            SpCell c = (SpCell)(z*g->w + x);
            f->dirX[c] = 0;
            f->dirZ[c] = 0;
            if (f->integration[c] == SP_FLOW_UNREACHED) continue;
            if (f->integration[c] == 0) continue;       // the goal itself

            uint16_t best = f->integration[c];
            int bx = 0, bz = 0;
            for (int i = 0; i < 8; i++)
            {
                int nx = x + DX[i], nz = z + DZ[i];
                if (!SpGridInBounds(g, nx, nz)) continue;
                if (!SpGridPassable(g, nx, nz)) continue;
                if (i >= 4 && (!SpGridPassable(g, x + DX[i], z) ||
                               !SpGridPassable(g, x, z + DZ[i]))) continue;
                uint16_t v = f->integration[nz*g->w + nx];
                if (v < best) { best = v; bx = DX[i]; bz = DZ[i]; }
            }
            f->dirX[c] = (int8_t)bx;
            f->dirZ[c] = (int8_t)bz;
        }
    }

    f->goalCell   = goal;
    f->navVersion = s_navVersion;
    f->live       = true;
    f->refCount   = 0;
}

// ----------------------------------------------------------------------------
//  Cache
// ----------------------------------------------------------------------------
SpFieldId SpFlowFind(int gx, int gz)
{
    if (s_grid == NULL) return SP_FIELD_NONE;
    SpCell key = GoalKey(gx, gz);
    for (int i = 0; i < SP_FLOW_FIELDS_MAX; i++)
    {
        if (!s_fields[i].live || s_fields[i].goalCell != key) continue;
        if (s_fields[i].navVersion != s_navVersion)
        {
            // Built against a grid that has since changed. Retire it here
            // rather than anywhere else: this is the one place every lookup
            // passes through, so a stale field cannot be handed out by
            // some path that forgot to check.
            s_fields[i].live = false;
            continue;
        }
        return (SpFieldId)i;
    }
    return SP_FIELD_NONE;
}

SpFieldId SpFlowAcquire(int gx, int gz)
{
    if (s_grid == NULL) return SP_FIELD_NONE;

    SpFieldId hit = SpFlowFind(gx, gz);
    if (hit != SP_FIELD_NONE) return hit;

    // A free slot, else the least recently used one that nobody is standing on.
    int slot = -1;
    for (int i = 0; i < SP_FLOW_FIELDS_MAX; i++)
        if (!s_fields[i].live) { slot = i; break; }

    if (slot < 0)
    {
        float oldest = 1e30f;
        for (int i = 0; i < SP_FLOW_FIELDS_MAX; i++)
        {
            if (s_fields[i].refCount > 0) continue;     // in use: never evict
            if (s_fields[i].lastUsed < oldest) { oldest = s_fields[i].lastUsed; slot = i; }
        }
    }
    // Every slot pinned. Returning NONE makes the caller fall back to individual
    // paths - slower, never wrong. Evicting a field out from under the units
    // walking it would strand them mid-crowd, which is much worse.
    if (slot < 0) return SP_FIELD_NONE;

    SpCell key = GoalKey(gx, gz);
    int kx = (int)(key % (SpCell)s_grid->w);
    int kz = (int)(key / (SpCell)s_grid->w);
    FlowBuild(&s_fields[slot], kx, kz);
    return (SpFieldId)slot;
}

// ----------------------------------------------------------------------------
//  Queries
// ----------------------------------------------------------------------------
bool SpFlowDir(SpFieldId id, int tx, int tz, float *outX, float *outZ)
{
    *outX = 0.0f; *outZ = 0.0f;
    if (id < 0 || id >= SP_FLOW_FIELDS_MAX) return false;
    const SpFlowField *f = &s_fields[id];
    if (!f->live || f->navVersion != s_navVersion) return false;
    if (!SpGridInBounds(s_grid, tx, tz)) return false;

    SpCell c = (SpCell)(tz*s_grid->w + tx);
    if (f->integration[c] == SP_FLOW_UNREACHED) return false;

    int dx = f->dirX[c], dz = f->dirZ[c];
    if (dx == 0 && dz == 0) return false;       // standing on the goal

    // Diagonals are normalized so a unit does not travel 1.41x faster on them.
    if (dx != 0 && dz != 0) { *outX = (float)dx*0.70710678f; *outZ = (float)dz*0.70710678f; }
    else                    { *outX = (float)dx;             *outZ = (float)dz; }
    return true;
}

uint16_t SpFlowCost(SpFieldId id, int tx, int tz)
{
    if (id < 0 || id >= SP_FLOW_FIELDS_MAX) return SP_FLOW_UNREACHED;
    const SpFlowField *f = &s_fields[id];
    if (!f->live || f->navVersion != s_navVersion) return SP_FLOW_UNREACHED;
    if (!SpGridInBounds(s_grid, tx, tz)) return SP_FLOW_UNREACHED;
    return f->integration[tz*s_grid->w + tx];
}

// ----------------------------------------------------------------------------
//  Refcount sweep
// ----------------------------------------------------------------------------
void SpFlowSweepBegin(void)
{
    for (int i = 0; i < SP_FLOW_FIELDS_MAX; i++) s_sweepMark[i] = 0;
}

void SpFlowSweepMark(SpFieldId id)
{
    if (id < 0 || id >= SP_FLOW_FIELDS_MAX) return;
    s_sweepMark[id]++;
}

void SpFlowSweepEnd(float now)
{
    for (int i = 0; i < SP_FLOW_FIELDS_MAX; i++)
    {
        s_fields[i].refCount = s_sweepMark[i];
        if (s_sweepMark[i] > 0) s_fields[i].lastUsed = now;
    }
}
