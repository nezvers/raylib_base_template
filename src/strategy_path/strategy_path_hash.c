// ============================================================================
//  strategy_path_hash.c  -  uniform spatial hash over the XZ plane
//
//  THE PROBLEM IT SOLVES. Unit separation was O(n^2) over every unit in the
//  world: 4,560 pair tests at 96 units, 50 MILLION at 10,000. Its own comment
//  read "O(n^2) over 64 units is nothing", which was true when it was written.
//  Bucketing by position turns "compare against everyone" into "compare against
//  whoever shares your patch of ground", which is ~40,000 tests at 10,000 units
//  - about 1,250x fewer.
//
//  WHY REBUILD, NOT UPDATE. Every unit moves every frame, so an incremental
//  update is a remove plus an insert for essentially the whole population -
//  strictly more work than one linear pass that also leaves the arrays packed
//  and cache-friendly. Rebuilding is also stateless, which means a unit that
//  dies mid-frame cannot leave a stale entry behind.
//
//  ONLY head[] IS CLEARED per rebuild, not the SoA arrays: the chains are what
//  make an entry reachable, so orphaning them is enough. That is one memset
//  over the cell count instead of four over the item count.
// ============================================================================

#include "strategy_path.h"

#include <string.h>
#include <math.h>

// ----------------------------------------------------------------------------
//  Cell addressing
// ----------------------------------------------------------------------------
//  floorf, not a cast to int. A cast truncates TOWARD ZERO, so world x = -0.5
//  and x = +0.5 both land in cell 0 and the row either side of the origin is
//  twice as wide as every other row. strategy_map.c carries the same note for
//  the same reason; the bug it prevents is a seam of missed collisions running
//  straight through the middle of the map.
static inline int32_t CellAxis(float v, float origin, float invCellSize,
                               int32_t limit)
{
    int32_t c = (int32_t)floorf((v - origin)*invCellSize);
    if (c < 0) c = 0;
    if (c >= limit) c = limit - 1;
    return c;
}

void SpHashBegin(SpHash *h, float cellSize, float halfX, float halfZ)
{
    if (cellSize < 0.01f) cellSize = 0.01f;     // guard a divide, not a policy

    h->cellSize    = cellSize;
    h->invCellSize = 1.0f/cellSize;
    h->originX     = -halfX;
    h->originZ     = -halfZ;

    // +1 so the far edge lands inside the grid rather than one past it, and a
    // floor so a map larger than the hash can hold still works - it just packs
    // more units per cell. Degrading into a coarser hash is fine; overrunning
    // head[] is not.
    h->w = (int32_t)(2.0f*halfX*h->invCellSize) + 1;
    h->h = (int32_t)(2.0f*halfZ*h->invCellSize) + 1;
    if (h->w < 1) h->w = 1;
    if (h->h < 1) h->h = 1;

    while ((int64_t)h->w*(int64_t)h->h > SP_HASH_CELLS_MAX)
    {
        // Halve the resolution rather than clamping one axis, which would make
        // the hash lopsided and leave a strip of the map in a single cell.
        h->cellSize   *= 2.0f;
        h->invCellSize = 1.0f/h->cellSize;
        h->w = (int32_t)(2.0f*halfX*h->invCellSize) + 1;
        h->h = (int32_t)(2.0f*halfZ*h->invCellSize) + 1;
        if (h->w < 1) h->w = 1;
        if (h->h < 1) h->h = 1;
    }

    memset(h->head, -1, (size_t)(h->w*h->h)*sizeof(h->head[0]));
    h->count   = 0;
    h->dropped = 0;
}

bool SpHashInsert(SpHash *h, int32_t id, float x, float z)
{
    if (h->count >= SP_HASH_ITEMS_MAX)
    {
        h->dropped++;
        return false;
    }

    int32_t cx = CellAxis(x, h->originX, h->invCellSize, h->w);
    int32_t cz = CellAxis(z, h->originZ, h->invCellSize, h->h);
    int32_t c  = cz*h->w + cx;

    int32_t item = h->count++;
    h->px[item]  = x;
    h->pz[item]  = z;
    h->id[item]  = id;

    // Push onto the front of the chain: O(1) with no tail pointer. It reverses
    // insertion order within a cell, which is why the query is documented as
    // unordered.
    h->next[item] = h->head[c];
    h->head[c]    = item;
    return true;
}

int SpHashQuery(const SpHash *h, float x, float z, float radius,
                int32_t *out, int maxOut)
{
    if (maxOut <= 0) return 0;

    // Cell range covering the query disc. Computed from the disc's bounding
    // box, so a radius larger than one cell still works - it just walks more
    // cells. Sizing cellSize at ~2x the radius keeps this at a 2x2 block.
    int32_t x0 = CellAxis(x - radius, h->originX, h->invCellSize, h->w);
    int32_t x1 = CellAxis(x + radius, h->originX, h->invCellSize, h->w);
    int32_t z0 = CellAxis(z - radius, h->originZ, h->invCellSize, h->h);
    int32_t z1 = CellAxis(z + radius, h->originZ, h->invCellSize, h->h);

    float r2 = radius*radius;
    int   n  = 0;

    for (int32_t cz = z0; cz <= z1; cz++)
    {
        const int32_t row = cz*h->w;
        for (int32_t cx = x0; cx <= x1; cx++)
        {
            for (int32_t it = h->head[row + cx]; it >= 0; it = h->next[it])
            {
                // Squared distance: the radius test is the innermost operation
                // in the whole movement system, and sqrtf here would be one per
                // candidate rather than one per accepted neighbor.
                float dx = h->px[it] - x;
                float dz = h->pz[it] - z;
                if (dx*dx + dz*dz > r2) continue;

                out[n++] = h->id[it];
                if (n >= maxOut) return n;      // truncate, by design
            }
        }
    }
    return n;
}

// ----------------------------------------------------------------------------
//  Deterministic pair jitter
// ----------------------------------------------------------------------------
//  Two units at EXACTLY the same spot have no separation direction to compute -
//  the difference vector is zero. The old code picked one from (i % 2), which
//  depends on which unit the outer loop reached first; the pair therefore parts
//  differently depending on iteration order, and with a hash that order changes
//  every frame. The result is a pile that vibrates instead of resolving.
//
//  Mixing BOTH ids, min first, makes the direction a property of the pair
//  alone: order-independent, frame-stable, and assertable in a test.
void SpSeparationJitter(int32_t idA, int32_t idB, float *outX, float *outZ)
{
    uint32_t lo = (uint32_t)((idA < idB) ? idA : idB);
    uint32_t hi = (uint32_t)((idA < idB) ? idB : idA);

    // Integer avalanche so adjacent ids do not produce adjacent angles - unit
    // pairs are consecutive far more often than chance, and a smooth hash would
    // send a whole spawn batch the same way.
    uint32_t k = lo*2654435761u ^ (hi + 0x9E3779B9u + (lo << 6) + (lo >> 2));
    k ^= k >> 15;
    k *= 0x2C1B3C6Du;
    k ^= k >> 12;

    float angle = (float)(k & 0xFFFFu)*(6.2831853f/65536.0f);
    *outX = cosf(angle);
    *outZ = sinf(angle);
}
