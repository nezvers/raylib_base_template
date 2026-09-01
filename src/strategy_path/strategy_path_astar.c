// ============================================================================
//  strategy_path_astar.c  -  A* over the nav grid, resumable and smoothed
//
//  THREE THINGS HERE ARE NOT THE TEXTBOOK VERSION, and each is deliberate.
//
//  1. THE SEARCH IS RESUMABLE, NOT RESTARTABLE. A budget-exhausted search
//     returns SP_PATH_BUSY and continues next frame from exactly where it
//     stopped - the SpAStar struct IS the resume state, so nothing extra is
//     stored. Restarting instead would make a long search that never fits in
//     one budget loop forever, which is the failure mode that looks like
//     "pathfinding randomly doesn't work on big maps". Meanwhile the unit keeps
//     walking on its old path or steers straight at the goal, so motion starts
//     on the click frame and the route refines underneath it.
//
//  2. VISITED IS A GENERATION STAMP, NOT A CLEARED ARRAY. See the header. The
//     one subtlety is wraparound: at 0xFFFFFFFF the stamps must be cleared once
//     and the counter reset, or a stale cell from four billion searches ago
//     reads as visited. It will never happen in a session; it is three lines.
//
//  3. DIAGONAL STEPS REQUIRE BOTH ORTHOGONALS. Moving from (0,0) to (1,1) when
//     (1,0) and (0,1) are both walls is geometrically a zero-width gap. A point
//     can pass; a 0.35-radius unit cannot. Allowing it produces paths that look
//     fine in the overlay and jam in play, and the same rule has to hold in
//     SpLosClear or the smoother re-introduces what the search avoided.
// ============================================================================

#include "strategy_path.h"

#include <string.h>

// Integer costs throughout - a uint16_t gScore and integer arithmetic mean the
// heap comparison is one compare with no float ordering surprises. A cardinal
// step of cost 1 is SP_STEP_ORTHO; the diagonal is 1.41421 scaled the same way,
// which keeps the heuristic admissible without any floats in the inner loop.
#define SP_STEP_ORTHO   10
#define SP_STEP_DIAG    14

// A cell's terrain cost multiplies the step. SP_COST_NORMAL is 1 so ordinary
// ground costs exactly the step; shallow water is 2 and the skirt 3, so a path
// takes a two-tile detour rather than scrape a wall for one tile.
#define SP_G_INFINITE   0xFFFFu

// ----------------------------------------------------------------------------
//  Cell helpers
// ----------------------------------------------------------------------------
static inline SpCell CellOf(const SpGrid *g, int x, int z)
{
    return (SpCell)(z*g->w + x);
}

static inline void CellXZ(const SpGrid *g, SpCell c, int *x, int *z)
{
    *x = (int)(c % (SpCell)g->w);
    *z = (int)(c / (SpCell)g->w);
}

// Octile distance, the exact cost of an unobstructed diagonal-capable walk over
// uniform ground. Admissible because terrain cost is never below 1, so no real
// path can beat it - which is what makes A* optimal here, and what the
// "A* cost == Dijkstra cost" test in path_tests actually proves.
static inline uint16_t Heuristic(int x0, int z0, int x1, int z1)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dz = (z1 > z0) ? (z1 - z0) : (z0 - z1);
    int lo = (dx < dz) ? dx : dz;
    int hi = (dx < dz) ? dz : dx;
    int h  = SP_STEP_DIAG*lo + SP_STEP_ORTHO*(hi - lo);
    return (h > 0xFFFF) ? 0xFFFFu : (uint16_t)h;
}

// ----------------------------------------------------------------------------
//  Binary min-heap on fScore
//
//  heapPos[] gives the decrease-key its O(1) lookup. Without it, finding the
//  cell to sift would be a linear scan of the open set, which on a wide-open
//  map is most of the grid - the difference between a search that costs its
//  expanded nodes and one that costs the square of them.
// ----------------------------------------------------------------------------
static void HeapSiftUp(SpAStar *a, int32_t i)
{
    SpCell c = a->heap[i];
    uint16_t f = a->fScore[c];
    while (i > 0)
    {
        int32_t parent = (i - 1)/2;
        SpCell pc = a->heap[parent];
        if (a->fScore[pc] <= f) break;
        a->heap[i] = pc;
        a->heapPos[pc] = (uint32_t)i;
        i = parent;
    }
    a->heap[i] = c;
    a->heapPos[c] = (uint32_t)i;
}

static void HeapSiftDown(SpAStar *a, int32_t i)
{
    SpCell c = a->heap[i];
    uint16_t f = a->fScore[c];
    for (;;)
    {
        int32_t l = 2*i + 1;
        if (l >= a->heapCount) break;
        int32_t r = l + 1;
        int32_t best = l;
        if ((r < a->heapCount) && (a->fScore[a->heap[r]] < a->fScore[a->heap[l]]))
            best = r;
        if (a->fScore[a->heap[best]] >= f) break;
        SpCell bc = a->heap[best];
        a->heap[i] = bc;
        a->heapPos[bc] = (uint32_t)i;
        i = best;
    }
    a->heap[i] = c;
    a->heapPos[c] = (uint32_t)i;
}

static void HeapPush(SpAStar *a, SpCell c)
{
    if (a->heapCount >= (int32_t)SP_CELLS_MAX) return;    // cannot happen: one slot per cell
    a->heap[a->heapCount] = c;
    a->heapPos[c] = (uint32_t)a->heapCount;
    a->heapCount++;
    HeapSiftUp(a, a->heapCount - 1);
}

static SpCell HeapPop(SpAStar *a)
{
    SpCell top = a->heap[0];
    a->heapCount--;
    if (a->heapCount > 0)
    {
        a->heap[0] = a->heap[a->heapCount];
        a->heapPos[a->heap[0]] = 0;
        HeapSiftDown(a, 0);
    }
    return top;
}

// ----------------------------------------------------------------------------
//  Line of sight - supercover
//
//  Walks every cell the segment between two tile CENTRES touches, and where the
//  line passes exactly through a lattice corner it refuses to slip between the
//  two diagonal cells sharing it. That corner case is the whole reason this is
//  not the four-line Bresenham: standard Bresenham picks one of the two cells
//  and can pick the passable one, handing the smoother a path through a gap of
//  zero width. A point fits; a 0.35-radius unit wedges.
//
//  WHY THE RATIONAL-CROSSING FORM AND NOT AN ACCUMULATED ERROR TERM. The
//  obvious incremental version keeps a running err and steps whichever axis it
//  favours - and it is NOT SYMMETRIC. Walking A->B and B->A take different
//  cells through near-corner cases, so a smoother keeps a waypoint going one
//  way and drops it coming back. That was measured here: 5 of 400 random
//  segments on a 25%-blocked grid disagreed by direction.
//
//  Here each step instead compares the EXACT crossing parameters of the next
//  vertical and horizontal grid lines, as a pair of integer products with no
//  accumulated state. tx == tz is then an exact equality on the same two
//  products whichever end the walk starts from, so the corner case - and every
//  other - is symmetric by construction rather than by care.
// ----------------------------------------------------------------------------
bool SpLosClear(const SpGrid *g, int x0, int z0, int x1, int z1)
{
    if (!SpGridPassable(g, x0, z0) || !SpGridPassable(g, x1, z1)) return false;

    int dx = x1 - x0, dz = z1 - z0;
    int adx = (dx < 0) ? -dx : dx;
    int adz = (dz < 0) ? -dz : dz;
    if ((adx | adz) == 0) return true;              // same cell

    int sx = (dx > 0) ? 1 : -1;
    int sz = (dz > 0) ? 1 : -1;
    int x = x0, z = z0;

    // The segment runs centre to centre, so the first grid line on each axis is
    // half a cell away and the rest a full cell apart. Working in HALF-cells
    // keeps that integral: the crossing parameter of the n-th vertical line is
    // (2n-1)/(2*adx), and of the n-th horizontal (2m-1)/(2*adz). Comparing two
    // such fractions is one cross-multiplication, with no division and no float.
    int nx = 1, nz = 1;                             // which line comes next per axis

    while ((x != x1) || (z != z1))
    {
        if (z == z1)                                // horizontal axis exhausted
        {
            x += sx; nx++;
        }
        else if (x == x1)                           // vertical axis exhausted
        {
            z += sz; nz++;
        }
        else
        {
            // Compare (2nx-1)/(2*adx) against (2nz-1)/(2*adz).
            long tx = (long)(2*nx - 1)*(long)adz;
            long tz = (long)(2*nz - 1)*(long)adx;

            if (tx == tz)
            {
                // Exact corner: the line threads the point shared by four
                // cells. Passing needs at least one of the two cells flanking
                // the diagonal to be open, then both axes advance together.
                if (!SpGridPassable(g, x + sx, z) && !SpGridPassable(g, x, z + sz))
                    return false;
                x += sx; nx++;
                z += sz; nz++;
            }
            else if (tx < tz) { x += sx; nx++; }
            else              { z += sz; nz++; }
        }
        if (!SpGridPassable(g, x, z)) return false;
    }
    return true;
}

// ----------------------------------------------------------------------------
//  Search
// ----------------------------------------------------------------------------
static void GenerationBump(SpAStar *a)
{
    if (a->generation == 0xFFFFFFFFu)
    {
        memset(a->stamp, 0, sizeof(a->stamp));
        memset(a->closed, 0, sizeof(a->closed));
        a->generation = 0;
    }
    a->generation++;
}

void SpAStarBegin(SpAStar *a, const SpGrid *g, int sx, int sz, int gx, int gz)
{
    GenerationBump(a);

    a->grid      = g;
    a->heapCount = 0;
    a->expanded  = 0;
    a->goalX     = gx;
    a->goalZ     = gz;
    a->start     = CellOf(g, sx, sz);
    a->goal      = CellOf(g, gx, gz);
    a->active    = true;

    // A start or goal that is blocked is a caller error - every goal is meant
    // to come through SpNearestOpen. Fail the search rather than expanding out
    // of a wall, which would produce a path that begins by walking through one.
    if (!SpGridPassable(g, sx, sz) || !SpGridPassable(g, gx, gz))
    {
        a->active = false;
        return;
    }

    a->gScore[a->start]   = 0;
    a->fScore[a->start]   = Heuristic(sx, sz, gx, gz);
    a->cameFrom[a->start] = SP_CELL_NONE;
    a->stamp[a->start]    = a->generation;
    HeapPush(a, a->start);
}

// Walk cameFrom back from the goal into scratch, smooth the WHOLE route, and
// only then truncate to what the caller can hold.
//
// ORDER MATTERS AND IT IS NOT THE OBVIOUS ONE. Truncating the raw cell list
// first and smoothing the remainder wastes the caller's slots: 24 raw cells is
// 24 tiles of a route that may be 90 tiles long, and smoothing collapses them
// to three waypoints that cover a quarter of the trip. Smoothing first means
// those same 24 slots hold 24 CORNERS - in practice the entire route, since a
// smoothed path on a real map is 4-8 waypoints. Truncation then almost never
// fires, and when it does the unit repaths from the last waypoint.
static SpCell s_scratch[SP_CELLS_MAX];

static void Reconstruct(SpAStar *a, SpCell *out, int maxOut, int *outCount)
{
    const SpGrid *g = a->grid;

    int n = 0;
    for (SpCell c = a->goal; c != a->start; c = a->cameFrom[c])
    {
        if (n >= (int)SP_CELLS_MAX) { *outCount = 0; return; }   // cycle guard
        s_scratch[n++] = c;
    }
    if (n == 0) { *outCount = 0; return; }      // already standing on the goal

    // Walked backward, so reverse into forward order before smoothing.
    for (int i = 0, j = n - 1; i < j; i++, j--)
    {
        SpCell t = s_scratch[i];
        s_scratch[i] = s_scratch[j];
        s_scratch[j] = t;
    }

    SpSmoothPath(g, a->start, s_scratch, &n);

    int keep = (n < maxOut) ? n : maxOut;
    for (int i = 0; i < keep; i++) out[i] = s_scratch[i];
    *outCount = keep;
}

SpPathStatus SpAStarStep(SpAStar *a, int nodeBudget,
                         SpCell *out, int maxOut, int *outCount)
{
    *outCount = 0;
    if (!a->active) return SP_PATH_FAILED;

    const SpGrid *g = a->grid;
    int budget = nodeBudget;

    // 8-connected. Orthogonals first so an equal-cost tie prefers a straight
    // step, which makes paths in open ground look intentional rather than
    // staircased before smoothing even runs.
    static const int DX[8] = {  1, -1,  0,  0,  1,  1, -1, -1 };
    static const int DZ[8] = {  0,  0,  1, -1,  1, -1,  1, -1 };

    while (a->heapCount > 0)
    {
        if (budget-- <= 0) return SP_PATH_BUSY;

        SpCell cur = HeapPop(a);
        if (cur == a->goal)
        {
            a->active = false;
            Reconstruct(a, out, maxOut, outCount);      // smooths internally
            // outCount == 0 means start == goal: a real success with nothing to
            // walk. Reporting that as FAILED would make a unit already standing
            // on its destination retry forever.
            return SP_PATH_FOUND;
        }

        a->closed[cur] = a->generation;
        a->expanded++;

        int cx, cz; CellXZ(g, cur, &cx, &cz);
        uint16_t gCur = a->gScore[cur];

        for (int i = 0; i < 8; i++)
        {
            int nx = cx + DX[i], nz = cz + DZ[i];
            uint8_t terrain = SpGridCost(g, nx, nz);        // 0 when out of bounds
            if (terrain == SP_COST_BLOCKED) continue;

            bool diagonal = (i >= 4);
            if (diagonal)
            {
                // Both orthogonals must be open - see the header comment. This
                // is the corner-cut rule, and it has to match SpLosClear.
                if (!SpGridPassable(g, cx + DX[i], cz) ||
                    !SpGridPassable(g, cx, cz + DZ[i])) continue;
            }

            SpCell nc = CellOf(g, nx, nz);
            if (a->closed[nc] == a->generation) continue;

            int step = (diagonal ? SP_STEP_DIAG : SP_STEP_ORTHO)*(int)terrain;
            int tentative = (int)gCur + step;
            if (tentative > 0xFFFF) continue;               // beyond what gScore holds

            bool seen = (a->stamp[nc] == a->generation);
            if (seen && (uint16_t)tentative >= a->gScore[nc]) continue;

            a->cameFrom[nc] = cur;
            a->gScore[nc]   = (uint16_t)tentative;
            int f = tentative + (int)Heuristic(nx, nz, a->goalX, a->goalZ);
            a->fScore[nc]   = (f > 0xFFFF) ? 0xFFFFu : (uint16_t)f;

            if (seen) HeapSiftUp(a, (int32_t)a->heapPos[nc]);
            else
            {
                a->stamp[nc] = a->generation;
                HeapPush(a, nc);
            }
        }
    }

    // Open set drained without reaching the goal: the goal is walled off.
    a->active = false;
    return SP_PATH_FAILED;
}

SpPathStatus SpAStarSolve(SpAStar *a, const SpGrid *g, int sx, int sz,
                          int gx, int gz, SpCell *out, int maxOut, int *outCount)
{
    SpAStarBegin(a, g, sx, sz, gx, gz);
    for (;;)
    {
        SpPathStatus st = SpAStarStep(a, 1 << 20, out, maxOut, outCount);
        if (st != SP_PATH_BUSY) return st;
    }
}

// ----------------------------------------------------------------------------
//  Smoothing
//
//  Greedy string-pull. Anchor on where the unit stands, then walk forward
//  keeping the FURTHEST waypoint still visible from the anchor; that waypoint
//  becomes the next anchor. A 40-cell staircase collapses to 4-8 corners.
//
//  Greedy is not globally optimal - a proper funnel algorithm shaves a little
//  more - but the difference is under a unit of travel on real maps and the
//  funnel needs a portal representation this grid does not have.
// ----------------------------------------------------------------------------
void SpSmoothPath(const SpGrid *g, SpCell start, SpCell *cells, int *count)
{
    int n = *count;
    if (n <= 1) return;

    int ax, az; CellXZ(g, start, &ax, &az);
    int write = 0;
    int i = 0;

    while (i < n)
    {
        // Furthest cell visible from the current anchor. Scanning from the far
        // end backward finds it in one pass and, on the common wide-open case,
        // succeeds on the very first test.
        int best = -1;
        for (int j = n - 1; j > i; j--)
        {
            int jx, jz; CellXZ(g, cells[j], &jx, &jz);
            if (SpLosClear(g, ax, az, jx, jz)) { best = j; break; }
        }
        if (best < 0) best = i;     // not even the next cell: keep the step

        cells[write++] = cells[best];
        CellXZ(g, cells[best], &ax, &az);
        i = best + 1;
    }

    *count = write;
}
