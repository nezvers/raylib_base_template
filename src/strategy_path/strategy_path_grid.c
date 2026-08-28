// ============================================================================
//  strategy_path_grid.c  -  the navigation grid: costs, stamping, skirt
//
//  WHY THIS LANDS BEFORE ANY PATHFINDING. The runtime has never called
//  SgmTilePassable - units walk through water, cliffs and buildings today. A*
//  is only as correct as the grid under it, and a grid bug looks exactly like
//  a search bug from the outside. So the grid ships on its own phase, with an
//  overlay that can be A/B'd against the map forge's already-trusted
//  passability view, and nothing depends on it yet.
//
//  TWO GRIDS, NOT ONE (the caller owns both; see NavRebuild in
//  strategy_world.c). A static grid holds terrain alone; the live grid is that
//  plus buildings and nodes. Demolition then restores from static by memcpy
//  over the footprint, which is the only way to get it RIGHT: once two
//  obstacles have overlapped a tile, "what was this tile before?" has no answer
//  you can compute from the live grid alone.
//
//  THE SKIRT IS A GLOBAL PASS, not a per-stamp one. Raising the neighbours of a
//  newly blocked tile is easy; LOWERING them again when the obstacle goes away
//  is not, because a tile may be skirting two obstacles at once. Recomputing
//  the whole skirt is O(cells) - about 65k byte compares on the largest map,
//  well under a millisecond - and it happens on placement and demolition, not
//  per frame. Cheap and unconditionally correct beats clever and subtly wrong.
// ============================================================================

#include "strategy_path.h"

#include <string.h>
#include <math.h>

// ----------------------------------------------------------------------------
//  Basics
// ----------------------------------------------------------------------------
void SpGridInit(SpGrid *g, int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > SP_GRID_MAX) w = SP_GRID_MAX;
    if (h > SP_GRID_MAX) h = SP_GRID_MAX;

    g->w = w;
    g->h = h;
    // Only the used extent is filled. Everything past it is unreachable through
    // the bounds check, so clearing 64 KB to touch 9 KB of it would be waste.
    memset(g->cost, SP_COST_NORMAL, (size_t)(w*h));
}

bool SpGridInBounds(const SpGrid *g, int x, int z)
{
    return (x >= 0) && (z >= 0) && (x < g->w) && (z < g->h);
}

uint8_t SpGridCost(const SpGrid *g, int x, int z)
{
    if (!SpGridInBounds(g, x, z)) return SP_COST_BLOCKED;
    return g->cost[z*g->w + x];
}

void SpGridSet(SpGrid *g, int x, int z, uint8_t cost)
{
    if (!SpGridInBounds(g, x, z)) return;
    g->cost[z*g->w + x] = cost;
}

// ----------------------------------------------------------------------------
//  Tile <-> world
//
//  These two must match SgmTileToWorld/SgmWorldToTile exactly. They are
//  duplicated rather than shared because this module cannot include
//  strategy_map.h and stay headless - and because the built-in layout has a nav
//  grid with no SgmMap behind it at all. path_tests cross-checks them against
//  the SGM originals so the duplication cannot silently drift.
// ----------------------------------------------------------------------------
Vector3 SpTileToWorld(const SpGrid *g, int x, int z)
{
    return (Vector3){
        (float)x - (float)g->w*0.5f + 0.5f,
        0.0f,
        (float)z - (float)g->h*0.5f + 0.5f,
    };
}

void SpWorldToTile(const SpGrid *g, float wx, float wz, int *outX, int *outZ)
{
    // floorf, not a cast: a cast truncates toward zero, so every tile left of
    // the origin would be off by one. Same note as strategy_map.c, same reason.
    if (outX != NULL) *outX = (int)floorf(wx + (float)g->w*0.5f);
    if (outZ != NULL) *outZ = (int)floorf(wz + (float)g->h*0.5f);
}

// ----------------------------------------------------------------------------
//  Stamping
// ----------------------------------------------------------------------------
void SpGridStampRect(SpGrid *g, float wx, float wz, int halfX, int halfZ,
                     uint8_t cost)
{
    int cx, cz;
    SpWorldToTile(g, wx, wz, &cx, &cz);

    for (int z = cz - halfZ; z <= cz + halfZ; z++)
    {
        for (int x = cx - halfX; x <= cx + halfX; x++)
        {
            SpGridSet(g, x, z, cost);   // out-of-bounds writes drop silently
        }
    }
}

void SpGridRestoreRect(SpGrid *dst, const SpGrid *src,
                       float wx, float wz, int halfX, int halfZ, int pad)
{
    if ((dst->w != src->w) || (dst->h != src->h)) return;   // not the same grid

    int cx, cz;
    SpWorldToTile(dst, wx, wz, &cx, &cz);

    // The pad matters: the removed obstacle's SKIRT extended one tile past its
    // footprint, and that skirt has to come back to terrain too. The caller
    // then rebuilds the skirt, which re-adds whatever a neighbouring obstacle
    // still justifies.
    int x0 = cx - halfX - pad, x1 = cx + halfX + pad;
    int z0 = cz - halfZ - pad, z1 = cz + halfZ + pad;
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if (x1 >= dst->w) x1 = dst->w - 1;
    if (z1 >= dst->h) z1 = dst->h - 1;

    for (int z = z0; z <= z1; z++)
    {
        // Row-at-a-time: the rectangle is contiguous along x, and a building
        // footprint is only a handful of tiles wide, so this is mostly about
        // saying "copy the row" rather than raw speed.
        size_t n = (size_t)(x1 - x0 + 1);
        memcpy(&dst->cost[z*dst->w + x0], &src->cost[z*src->w + x0], n);
    }
}

// ----------------------------------------------------------------------------
//  Obstacle skirt
// ----------------------------------------------------------------------------
void SpGridBuildSkirt(SpGrid *g)
{
    // Two passes over one array, because raising a tile in place would make it
    // a skirt source for its own neighbours and the elevated band would creep
    // outward by one tile per row scanned. The first pass reads, the second
    // writes; nothing read is anything written.
    static uint8_t s_mark[SP_CELLS_MAX];    // static: 64 KB has no business on
                                            //   a stack, and this is called
                                            //   from placement, not per frame
    int cells = g->w*g->h;
    memset(s_mark, 0, (size_t)cells);

    for (int z = 0; z < g->h; z++)
    {
        for (int x = 0; x < g->w; x++)
        {
            uint8_t c = g->cost[z*g->w + x];
            if (c == SP_COST_BLOCKED) continue;

            bool touches = false;
            for (int dz = -1; (dz <= 1) && !touches; dz++)
            {
                for (int dx = -1; dx <= 1; dx++)
                {
                    if ((dx == 0) && (dz == 0)) continue;
                    // Off-grid counts as blocked, so the map border grows a
                    // skirt too - which is right: a unit pinned against the
                    // edge by a crowd is in the same trouble as one pinned
                    // against a cliff.
                    if (SpGridCost(g, x + dx, z + dz) == SP_COST_BLOCKED)
                    {
                        touches = true;
                        break;
                    }
                }
            }
            if (touches) s_mark[z*g->w + x] = 1;
        }
    }

    for (int i = 0; i < cells; i++)
    {
        if (!s_mark[i]) continue;
        // Only ever raise, and only to exactly SP_COST_SKIRT. That is what
        // makes this idempotent: running it twice in a row cannot compound the
        // cost, and a shallow-water tile beside a cliff ends up skirted rather
        // than keeping its cheaper shallow cost.
        if (g->cost[i] < SP_COST_SKIRT) g->cost[i] = SP_COST_SKIRT;
    }
}

// ----------------------------------------------------------------------------
//  Nearest open tile
// ----------------------------------------------------------------------------
bool SpNearestOpen(const SpGrid *g, int tx, int tz, int maxRing,
                   int *outX, int *outZ)
{
    if (SpGridPassable(g, tx, tz))
    {
        if (outX != NULL) *outX = tx;
        if (outZ != NULL) *outZ = tz;
        return true;
    }

    // Square rings outward. Not a true nearest-by-distance search - a corner of
    // ring 2 is farther than an edge of ring 3 - but the error is under a tile
    // and a ring walk costs no queue, no visited set and no allocation. For
    // "the click landed in a lake, walk to the shore" that is the right trade.
    for (int r = 1; r <= maxRing; r++)
    {
        for (int d = -r; d <= r; d++)
        {
            const int cand[4][2] = {
                { tx + d, tz - r },     // top edge
                { tx + d, tz + r },     // bottom edge
                { tx - r, tz + d },     // left edge
                { tx + r, tz + d },     // right edge
            };
            for (int i = 0; i < 4; i++)
            {
                if (!SpGridPassable(g, cand[i][0], cand[i][1])) continue;
                if (outX != NULL) *outX = cand[i][0];
                if (outZ != NULL) *outZ = cand[i][1];
                return true;
            }
        }
    }
    return false;
}
