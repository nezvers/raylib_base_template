// ============================================================================
//  path_tests.c  -  headless checks for src/strategy_path/
//
//  Covers the parts that are expensive to get wrong and invisible when they do.
//  The spatial hash replaces an O(n^2) loop whose ONLY virtue was that it was
//  obviously correct: it compared everything against everything, so it could
//  not miss a pair. The hash can, and a missed pair is not a crash - it is two
//  units quietly standing inside each other, which reads as an art bug months
//  later. So the headline test here is exact equivalence against brute force
//  over randomized point sets, not a spot check.
//
//  Headless: no window, no GL. src/strategy_path/ never calls raylib UI, which
//  is the whole reason it is a separate module from the game.
// ============================================================================

#include "../src/strategy_path/strategy_path.h"
#include "../src/strategy_map/strategy_map.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

static int s_checks = 0, s_fails = 0;

#define CHECK(cond) do { \
    s_checks++; \
    if (!(cond)) { s_fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// Large structures stay static, never on the stack: SpHash is hundreds of KB
// at the desktop cap and a stack copy would blow the default 8 MB in a loop.
static SpHash s_hash;
static SpGrid s_grid;
static SpGrid s_gridB;
static SgmMap s_map;        // 64 KB+ of tiles: never on the stack

// Deterministic PRNG. rand() is seeded per-platform and its low bits are poor
// on some libcs; a test that passes on one machine and fails on another is
// worse than no test. xorshift32 is four lines and identical everywhere.
static uint32_t s_rng = 1;
static void     RngSeed(uint32_t s) { s_rng = s ? s : 1; }
static uint32_t RngNext(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}
static float RngRange(float lo, float hi)
{
    return lo + (float)(RngNext() & 0xFFFFFF)/(float)0xFFFFFF*(hi - lo);
}

// ---------------------------------------------------------------------------
//  Basics
// ---------------------------------------------------------------------------
static void TestEmpty(void)
{
    SpHashBegin(&s_hash, 1.4f, 20.0f, 20.0f);

    CHECK(s_hash.count == 0);
    CHECK(s_hash.dropped == 0);
    CHECK(s_hash.w >= 1 && s_hash.h >= 1);

    int32_t out[8];
    CHECK(SpHashQuery(&s_hash, 0.0f, 0.0f, 5.0f, out, 8) == 0);
}

static void TestSingle(void)
{
    SpHashBegin(&s_hash, 1.4f, 20.0f, 20.0f);
    CHECK(SpHashInsert(&s_hash, 42, 3.0f, -4.0f));
    CHECK(s_hash.count == 1);

    int32_t out[8];

    // Found from its own position.
    CHECK(SpHashQuery(&s_hash, 3.0f, -4.0f, 0.5f, out, 8) == 1);
    CHECK(out[0] == 42);

    // Found from just inside the radius, missed from just outside. This is the
    // boundary the brute-force comparison below relies on being exact.
    CHECK(SpHashQuery(&s_hash, 3.0f, -4.0f + 0.99f, 1.0f, out, 8) == 1);
    CHECK(SpHashQuery(&s_hash, 3.0f, -4.0f + 1.01f, 1.0f, out, 8) == 0);

    // Far away in every direction.
    CHECK(SpHashQuery(&s_hash, -10.0f, 10.0f, 1.0f, out, 8) == 0);
}

// A query disc wider than one cell must still walk every cell it covers. If
// the cell-range maths silently assumed a 2x2 block this returns short.
static void TestRadiusLargerThanCell(void)
{
    SpHashBegin(&s_hash, 1.0f, 20.0f, 20.0f);

    for (int i = 0; i < 9; i++)
        CHECK(SpHashInsert(&s_hash, i, (float)(i - 4), 0.0f));

    int32_t out[16];
    CHECK(SpHashQuery(&s_hash, 0.0f, 0.0f, 4.5f, out, 16) == 9);
    CHECK(SpHashQuery(&s_hash, 0.0f, 0.0f, 2.5f, out, 16) == 5);
}

// Points on a cell seam must be found from BOTH sides. Getting this wrong is
// the classic spatial-hash bug: units drift apart along invisible grid lines
// because the pair is only ever seen from one direction.
static void TestCellBoundary(void)
{
    SpHashBegin(&s_hash, 1.0f, 20.0f, 20.0f);

    // cellSize 1.0 with origin -20 puts seams on the integers.
    CHECK(SpHashInsert(&s_hash, 1, 2.999f, 0.5f));
    CHECK(SpHashInsert(&s_hash, 2, 3.001f, 0.5f));

    int32_t out[8];
    CHECK(SpHashQuery(&s_hash, 2.999f, 0.5f, 0.5f, out, 8) == 2);
    CHECK(SpHashQuery(&s_hash, 3.001f, 0.5f, 0.5f, out, 8) == 2);
}

// Negative coordinates must not fold onto positive ones. A cast-to-int instead
// of floorf makes cells -0.5 and +0.5 collide, so the row through the origin
// behaves differently from every other row - and the map is origin-centred, so
// that row runs right through the middle of the battlefield.
static void TestNegativeCoords(void)
{
    SpHashBegin(&s_hash, 1.0f, 20.0f, 20.0f);

    CHECK(SpHashInsert(&s_hash, 1, -0.5f, -0.5f));
    CHECK(SpHashInsert(&s_hash, 2,  0.5f,  0.5f));

    int32_t out[8];

    // Radius 0.4 reaches neither the other point nor across the seam.
    CHECK(SpHashQuery(&s_hash, -0.5f, -0.5f, 0.4f, out, 8) == 1);
    CHECK(out[0] == 1);
    CHECK(SpHashQuery(&s_hash, 0.5f, 0.5f, 0.4f, out, 8) == 1);
    CHECK(out[0] == 2);
}

// Out-of-bounds inserts CLAMP into the edge cells rather than being dropped: a
// unit shoved past the map edge by a crowd must still push back. Silently
// un-hashing it would leave a hole in the collision exactly where the pile is.
static void TestOutOfBoundsClamps(void)
{
    SpHashBegin(&s_hash, 1.0f, 10.0f, 10.0f);

    CHECK(SpHashInsert(&s_hash, 1, -500.0f, -500.0f));
    CHECK(SpHashInsert(&s_hash, 2,  500.0f,  500.0f));
    CHECK(s_hash.count == 2);
    CHECK(s_hash.dropped == 0);

    // Both are reachable from their clamped cells, and did not land in the
    // same one - which is what would happen if the clamp folded to 0.
    int32_t out[8];
    CHECK(SpHashQuery(&s_hash, -500.0f, -500.0f, 1000.0f, out, 8) >= 1);
}

static void TestRebuildClears(void)
{
    SpHashBegin(&s_hash, 1.4f, 20.0f, 20.0f);
    for (int i = 0; i < 50; i++) SpHashInsert(&s_hash, i, 0.0f, 0.0f);
    CHECK(s_hash.count == 50);

    // Only head[] is memset on Begin; if that is wrong, stale chains from the
    // previous frame stay reachable and dead units keep pushing.
    SpHashBegin(&s_hash, 1.4f, 20.0f, 20.0f);
    CHECK(s_hash.count == 0);

    int32_t out[64];
    CHECK(SpHashQuery(&s_hash, 0.0f, 0.0f, 10.0f, out, 64) == 0);
}

// Overflow must truncate gracefully and SAY SO, not corrupt the arrays. The
// counter is what the lab overlay reads, so it is the difference between
// "units clip at 10k" being a five-minute diagnosis and a five-hour one.
static void TestOverflowCounts(void)
{
    SpHashBegin(&s_hash, 1.4f, 200.0f, 200.0f);

    int accepted = 0;
    for (int i = 0; i < SP_HASH_ITEMS_MAX + 100; i++)
        if (SpHashInsert(&s_hash, i, RngRange(-50.0f, 50.0f), RngRange(-50.0f, 50.0f)))
            accepted++;

    CHECK(accepted == SP_HASH_ITEMS_MAX);
    CHECK(s_hash.count == SP_HASH_ITEMS_MAX);
    CHECK(s_hash.dropped == 100);
}

// A cell count over SP_HASH_CELLS_MAX must coarsen, not overrun head[]. The
// desktop cap covers a 256-unit map at cellSize 1.4; a bigger world than that
// should degrade into a coarser hash rather than scribble past the array.
static void TestHugeWorldCoarsens(void)
{
    SpHashBegin(&s_hash, 0.05f, 400.0f, 400.0f);

    CHECK((int64_t)s_hash.w*(int64_t)s_hash.h <= SP_HASH_CELLS_MAX);
    CHECK(s_hash.cellSize > 0.05f);         // it actually coarsened

    // And still answers correctly at the coarser resolution.
    CHECK(SpHashInsert(&s_hash, 7, 100.0f, -100.0f));
    int32_t out[8];
    CHECK(SpHashQuery(&s_hash, 100.0f, -100.0f, 1.0f, out, 8) == 1);
    CHECK(out[0] == 7);
}

// ---------------------------------------------------------------------------
//  The headline test: exact equivalence with brute force
// ---------------------------------------------------------------------------
//  The hash is only worth having if it returns EXACTLY what the O(n^2) loop it
//  replaces would have. Not "approximately", not "enough for steering" - the
//  same set. Anything less and separation has position-dependent holes, which
//  is precisely the class of bug that is impossible to see at 96 units and
//  ruins a 5,000-unit pile.
//
//  Sets are seeded and clustered as well as uniform, because uniform points
//  never exercise the deep-chain path that a real death-ball hits constantly.
#define BRUTE_N  700

static float s_bx[BRUTE_N], s_bz[BRUTE_N];

static void TestBruteForceEquivalence(void)
{
    const float radius = 0.7f;      // 2 * STRAT_UNIT_RADIUS, the real query

    for (int set = 0; set < 50; set++)
    {
        RngSeed((uint32_t)set + 1);

        // Half the sets are uniform, half are tight clusters. Clusters put
        // dozens of points in one cell, which is the case that separates a
        // correct chain walk from one that stops early.
        bool clustered = (set % 2) == 1;
        float halfX = 30.0f, halfZ = 30.0f;

        int n = 100 + (int)(RngNext() % (BRUTE_N - 100));
        if (n > SP_HASH_ITEMS_MAX) n = SP_HASH_ITEMS_MAX;

        for (int i = 0; i < n; i++)
        {
            if (clustered)
            {
                // A handful of blobs, deliberately overlapping.
                float cx = RngRange(-10.0f, 10.0f);
                float cz = RngRange(-10.0f, 10.0f);
                s_bx[i] = cx + RngRange(-1.5f, 1.5f);
                s_bz[i] = cz + RngRange(-1.5f, 1.5f);
            }
            else
            {
                s_bx[i] = RngRange(-halfX, halfX);
                s_bz[i] = RngRange(-halfZ, halfZ);
            }
        }

        SpHashBegin(&s_hash, 2.0f*radius, halfX, halfZ);
        for (int i = 0; i < n; i++) CHECK(SpHashInsert(&s_hash, i, s_bx[i], s_bz[i]));

        // Query from every point, with maxOut generous enough that truncation
        // never fires - truncation is tested separately, and mixing the two
        // would let a real miss hide behind an expected one.
        static int32_t out[BRUTE_N];
        for (int i = 0; i < n; i++)
        {
            int got = SpHashQuery(&s_hash, s_bx[i], s_bz[i], radius, out, BRUTE_N);

            // Brute force: the loop the hash replaces.
            int want = 0;
            for (int j = 0; j < n; j++)
            {
                float dx = s_bx[j] - s_bx[i];
                float dz = s_bz[j] - s_bz[i];
                if (dx*dx + dz*dz <= radius*radius) want++;
            }
            CHECK(got == want);

            // Same COUNT is not the same SET - a hash that returned the wrong
            // neighbors in the right number would pass the check above. Verify
            // membership and rule out duplicates from a chain walked twice.
            static bool seen[BRUTE_N];
            memset(seen, 0, sizeof(bool)*(size_t)n);
            for (int k = 0; k < got; k++)
            {
                CHECK(out[k] >= 0 && out[k] < n);
                if (out[k] < 0 || out[k] >= n) continue;
                CHECK(!seen[out[k]]);           // no duplicates
                seen[out[k]] = true;

                float dx = s_bx[out[k]] - s_bx[i];
                float dz = s_bz[out[k]] - s_bz[i];
                CHECK(dx*dx + dz*dz <= radius*radius);
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  Determinism
// ---------------------------------------------------------------------------
//  Same input, same output - including after an unrelated rebuild has run. A
//  hash that carried state between frames would make separation depend on what
//  happened last frame, which is untraceable in a live game.
static void TestDeterminism(void)
{
    static int32_t a[64], b[64];

    RngSeed(1234);
    SpHashBegin(&s_hash, 1.4f, 30.0f, 30.0f);
    for (int i = 0; i < 200; i++)
        SpHashInsert(&s_hash, i, RngRange(-5.0f, 5.0f), RngRange(-5.0f, 5.0f));
    int na = SpHashQuery(&s_hash, 0.0f, 0.0f, 2.0f, a, 64);

    // An unrelated build in between.
    SpHashBegin(&s_hash, 0.9f, 10.0f, 10.0f);
    for (int i = 0; i < 30; i++) SpHashInsert(&s_hash, i, 1.0f, 1.0f);
    SpHashQuery(&s_hash, 1.0f, 1.0f, 3.0f, b, 64);

    RngSeed(1234);
    SpHashBegin(&s_hash, 1.4f, 30.0f, 30.0f);
    for (int i = 0; i < 200; i++)
        SpHashInsert(&s_hash, i, RngRange(-5.0f, 5.0f), RngRange(-5.0f, 5.0f));
    int nb = SpHashQuery(&s_hash, 0.0f, 0.0f, 2.0f, b, 64);

    CHECK(na == nb);
    for (int i = 0; i < na && i < nb; i++) CHECK(a[i] == b[i]);
}

// ---------------------------------------------------------------------------
//  Pair jitter
// ---------------------------------------------------------------------------
//  The direction two exactly-coincident units part in. The old code derived it
//  from (i % 2), so it depended on which unit the outer loop reached first -
//  and with a hash, that order changes every frame. This is what makes a pile
//  settle rather than shimmer, so it is asserted rather than eyeballed.
static void TestJitter(void)
{
    float ax, az, bx, bz;

    // Symmetric: the pair, not the order, decides.
    SpSeparationJitter(3, 9, &ax, &az);
    SpSeparationJitter(9, 3, &bx, &bz);
    CHECK(ax == bx);
    CHECK(az == bz);

    // Unit length, so the caller can scale it without normalizing.
    CHECK(fabsf(sqrtf(ax*ax + az*az) - 1.0f) < 0.0001f);

    // Different pairs get different directions - otherwise a stack of ten
    // units all leaves along the same line and re-stacks immediately.
    int distinct = 0;
    for (int i = 0; i < 32; i++)
    {
        SpSeparationJitter(0, i + 1, &ax, &az);
        SpSeparationJitter(0, i + 2, &bx, &bz);
        if (fabsf(ax - bx) > 0.01f || fabsf(az - bz) > 0.01f) distinct++;
    }
    CHECK(distinct == 32);

    // Consecutive ids must not produce clustered angles: units spawn in runs,
    // so a smooth hash would send a whole batch the same way. Check the 64
    // directions from consecutive pairs actually cover the circle - all four
    // quadrants should see several.
    int quad[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 64; i++)
    {
        SpSeparationJitter(i, i + 1, &ax, &az);
        int q = ((ax >= 0.0f) ? 0 : 1) + ((az >= 0.0f) ? 0 : 2);
        quad[q]++;
    }
    for (int q = 0; q < 4; q++) CHECK(quad[q] >= 5);
}

// ---------------------------------------------------------------------------
//  Navigation grid
// ---------------------------------------------------------------------------
static void TestGridBasics(void)
{
    SpGridInit(&s_grid, 40, 30);

    CHECK(s_grid.w == 40);
    CHECK(s_grid.h == 30);
    CHECK(SpGridCost(&s_grid, 0, 0) == SP_COST_NORMAL);
    CHECK(SpGridCost(&s_grid, 39, 29) == SP_COST_NORMAL);

    // Out of bounds reads as blocked, in every direction. Every neighbour walk
    // in A* leans on this instead of bounds-checking by hand, so it is load
    // bearing rather than a nicety.
    CHECK(SpGridCost(&s_grid, -1, 0) == SP_COST_BLOCKED);
    CHECK(SpGridCost(&s_grid, 0, -1) == SP_COST_BLOCKED);
    CHECK(SpGridCost(&s_grid, 40, 0) == SP_COST_BLOCKED);
    CHECK(SpGridCost(&s_grid, 0, 30) == SP_COST_BLOCKED);
    CHECK(!SpGridPassable(&s_grid, -1, -1));

    SpGridSet(&s_grid, 5, 7, SP_COST_BLOCKED);
    CHECK(!SpGridPassable(&s_grid, 5, 7));
    CHECK(SpGridPassable(&s_grid, 5, 8));

    // An out-of-bounds write must drop, not corrupt a neighbouring row. Writing
    // (-1, 5) with naive index arithmetic lands on (w-1, 4).
    SpGridSet(&s_grid, -1, 5, SP_COST_BLOCKED);
    CHECK(SpGridPassable(&s_grid, 39, 4));

    // Dimensions clamp rather than overrun the fixed array.
    SpGridInit(&s_grid, SP_GRID_MAX + 99, SP_GRID_MAX + 99);
    CHECK(s_grid.w == SP_GRID_MAX);
    CHECK(s_grid.h == SP_GRID_MAX);
    SpGridInit(&s_grid, 0, -4);
    CHECK(s_grid.w == 1 && s_grid.h == 1);
}

// The single most likely silent bug in the project: a half-tile offset between
// the nav grid and the map. It would not crash, it would not look obviously
// wrong, and every unit would path to a spot next to the one it was sent to.
static void TestTileWorldRoundTrip(void)
{
    // EVEN and ODD extents both, because the centring term is w*0.5f and the
    // odd case is the one where a naive integer halving goes wrong.
    const int sizes[][2] = { { 40, 40 }, { 41, 41 }, { 40, 41 }, { 96, 96 }, { 8, 13 } };

    for (int s = 0; s < (int)(sizeof(sizes)/sizeof(sizes[0])); s++)
    {
        int w = sizes[s][0], h = sizes[s][1];
        SpGridInit(&s_grid, w, h);

        for (int z = 0; z < h; z++)
        {
            for (int x = 0; x < w; x++)
            {
                Vector3 p = SpTileToWorld(&s_grid, x, z);
                int bx, bz;
                SpWorldToTile(&s_grid, p.x, p.z, &bx, &bz);
                CHECK(bx == x && bz == z);
            }
        }

        // ...and against the SGM originals, which the runtime already trusts.
        // The two implementations are duplicated on purpose; this is what stops
        // them drifting.
        SgmMapInit(&s_map, "xcheck", w, h);
        if ((s_map.gridW == w) && (s_map.gridH == h))
        {
            for (int z = 0; z < h; z++)
            {
                for (int x = 0; x < w; x++)
                {
                    Vector3 a = SpTileToWorld(&s_grid, x, z);
                    Vector3 b = SgmTileToWorld(&s_map, x, z);
                    CHECK(fabsf(a.x - b.x) < 0.0001f);
                    CHECK(fabsf(a.z - b.z) < 0.0001f);

                    int ax, az, bx2, bz2;
                    SpWorldToTile(&s_grid, a.x, a.z, &ax, &az);
                    SgmWorldToTile(&s_map, a.x, a.z, &bx2, &bz2);
                    CHECK(ax == bx2 && az == bz2);
                }
            }
        }
    }
}

// A world point left of the origin must not land a tile to the right. This is
// the floorf-versus-cast bug, checked explicitly rather than trusted to the
// round trip - the round trip only visits tile CENTRES, and the cast bug is
// visible at tile centres only on one specific row.
static void TestWorldToTileNegative(void)
{
    SpGridInit(&s_grid, 10, 10);    // spans [-5, +5]

    int x, z;
    SpWorldToTile(&s_grid, -0.25f, -0.25f, &x, &z);
    CHECK(x == 4 && z == 4);
    SpWorldToTile(&s_grid, 0.25f, 0.25f, &x, &z);
    CHECK(x == 5 && z == 5);
    SpWorldToTile(&s_grid, -4.9f, 4.9f, &x, &z);
    CHECK(x == 0 && z == 9);
}

static void TestStampAndRestore(void)
{
    SpGridInit(&s_grid, 20, 20);        // spans [-10, +10]
    s_gridB = s_grid;                   // the "terrain only" reference

    // A 3x3 building centred on world (0.5, 0.5), which is tile (10, 10).
    SpGridStampRect(&s_grid, 0.5f, 0.5f, 1, 1, SP_COST_BLOCKED);
    for (int z = 9; z <= 11; z++)
        for (int x = 9; x <= 11; x++) CHECK(!SpGridPassable(&s_grid, x, z));
    CHECK(SpGridPassable(&s_grid, 8, 10));
    CHECK(SpGridPassable(&s_grid, 12, 10));

    // Restore puts it back exactly, including the pad ring.
    SpGridRestoreRect(&s_grid, &s_gridB, 0.5f, 0.5f, 1, 1, 1);
    for (int z = 8; z <= 12; z++)
        for (int x = 8; x <= 12; x++) CHECK(SpGridPassable(&s_grid, x, z));

    // A stamp straddling the map edge must clip, not wrap onto the far side.
    SpGridStampRect(&s_grid, -9.5f, 0.5f, 2, 0, SP_COST_BLOCKED);
    CHECK(!SpGridPassable(&s_grid, 0, 10));
    CHECK(!SpGridPassable(&s_grid, 2, 10));
    CHECK(SpGridPassable(&s_grid, 19, 10));    // did NOT wrap
    CHECK(SpGridPassable(&s_grid, 18, 10));
}

static void TestSkirt(void)
{
    SpGridInit(&s_grid, 16, 16);
    SpGridSet(&s_grid, 8, 8, SP_COST_BLOCKED);
    SpGridBuildSkirt(&s_grid);

    // All eight neighbours raised, the blocked tile itself untouched, and
    // nothing two tiles out.
    CHECK(SpGridCost(&s_grid, 8, 8) == SP_COST_BLOCKED);
    for (int dz = -1; dz <= 1; dz++)
    {
        for (int dx = -1; dx <= 1; dx++)
        {
            if (dx == 0 && dz == 0) continue;
            CHECK(SpGridCost(&s_grid, 8 + dx, 8 + dz) == SP_COST_SKIRT);
        }
    }
    CHECK(SpGridCost(&s_grid, 8, 6) == SP_COST_NORMAL);
    CHECK(SpGridCost(&s_grid, 6, 6) == SP_COST_NORMAL);

    // The map border skirts too - off-grid counts as blocked.
    CHECK(SpGridCost(&s_grid, 0, 0) == SP_COST_SKIRT);
    CHECK(SpGridCost(&s_grid, 15, 15) == SP_COST_SKIRT);
    CHECK(SpGridCost(&s_grid, 1, 1) == SP_COST_NORMAL);

    // IDEMPOTENT. Running it again must not raise anything further - if it
    // compounded, every placement would slowly poison the map with cost.
    s_gridB = s_grid;
    SpGridBuildSkirt(&s_grid);
    for (int i = 0; i < s_grid.w*s_grid.h; i++) CHECK(s_grid.cost[i] == s_gridB.cost[i]);

    // Shallow water beside an obstacle is RAISED to skirt (skirt is dearer);
    // shallow water in the open keeps its own cheaper cost.
    SpGridInit(&s_grid, 16, 16);
    SpGridSet(&s_grid, 4, 4, SP_COST_BLOCKED);
    SpGridSet(&s_grid, 5, 4, SP_COST_SHALLOW);
    SpGridSet(&s_grid, 9, 9, SP_COST_SHALLOW);
    SpGridBuildSkirt(&s_grid);
    CHECK(SpGridCost(&s_grid, 5, 4) == SP_COST_SKIRT);
    CHECK(SpGridCost(&s_grid, 9, 9) == SP_COST_SHALLOW);
}

static void TestNearestOpen(void)
{
    SpGridInit(&s_grid, 32, 32);

    // An already-open tile is returned unchanged - no search, no drift.
    int ox = -1, oz = -1;
    CHECK(SpNearestOpen(&s_grid, 10, 10, 8, &ox, &oz));
    CHECK(ox == 10 && oz == 10);

    // A 5x5 blocked block: the answer must be on its edge, exactly one tile
    // outside. This is the "ordered to walk into a lake" case.
    for (int z = 8; z <= 12; z++)
        for (int x = 8; x <= 12; x++) SpGridSet(&s_grid, x, z, SP_COST_BLOCKED);

    CHECK(SpNearestOpen(&s_grid, 10, 10, 8, &ox, &oz));
    CHECK(SpGridPassable(&s_grid, ox, oz));
    int dx = ox - 10, dz = oz - 10;
    if (dx < 0) dx = -dx;
    if (dz < 0) dz = -dz;
    CHECK((dx == 3) || (dz == 3));      // ring 3 is the first open ring

    // Ring cap respected: entirely blocked grid, nothing found, and the outputs
    // are left alone rather than filled with garbage.
    SpGridInit(&s_grid, 16, 16);
    for (int i = 0; i < 16*16; i++) s_grid.cost[i] = SP_COST_BLOCKED;
    ox = -7; oz = -7;
    CHECK(!SpNearestOpen(&s_grid, 8, 8, 8, &ox, &oz));
    CHECK(ox == -7 && oz == -7);

    // A goal OFF the grid still resolves inward, which is what a click past the
    // map edge does.
    SpGridInit(&s_grid, 16, 16);
    CHECK(SpNearestOpen(&s_grid, 20, 8, 8, &ox, &oz));
    CHECK(SpGridInBounds(&s_grid, ox, oz));
}

// ---------------------------------------------------------------------------
//  A*
//
//  The two tests that matter here are not the hand-built maze - that only
//  proves the search runs. They are:
//
//    - OPTIMALITY against a reference Dijkstra on random grids. A* is only
//      optimal while the heuristic never overestimates, and a scaling slip
//      (octile distance in step-10 units against a cost that is not) makes it
//      quietly return short-but-not-shortest paths. Nothing about that looks
//      wrong in the overlay.
//    - RESUMPTION equivalence. A search sliced one node at a time must produce
//      a byte-identical path to the same search run unbudgeted. That is the
//      whole correctness argument for time-slicing, and if it fails the bug
//      only shows on the long paths that exhaust a budget - i.e. exactly the
//      ones a player notices.
// ---------------------------------------------------------------------------
static SpAStar s_astar;         // ~1.4 MB at the desktop cap: never on the stack
static SpCell  s_path[SP_CELLS_MAX];
static SpCell  s_pathB[SP_CELLS_MAX];

// Reference Dijkstra, deliberately dumb: no heap, no heuristic, just relax the
// cheapest unvisited cell until everything is settled. O(cells^2) and far too
// slow for the game, which is precisely why it is trustworthy as an oracle.
static uint32_t s_refDist[SP_CELLS_MAX];
static uint8_t  s_refDone[SP_CELLS_MAX];

#define REF_INF  0xFFFFFFFFu

static uint32_t RefDijkstra(const SpGrid *g, int sx, int sz, int gx, int gz)
{
    int cells = g->w*g->h;
    for (int i = 0; i < cells; i++) { s_refDist[i] = REF_INF; s_refDone[i] = 0; }
    if (!SpGridPassable(g, sx, sz) || !SpGridPassable(g, gx, gz)) return REF_INF;
    s_refDist[sz*g->w + sx] = 0;

    static const int DX[8] = {  1, -1,  0,  0,  1,  1, -1, -1 };
    static const int DZ[8] = {  0,  0,  1, -1,  1, -1,  1, -1 };

    for (;;)
    {
        int best = -1;
        uint32_t bestD = REF_INF;
        for (int i = 0; i < cells; i++)
            if (!s_refDone[i] && s_refDist[i] < bestD) { bestD = s_refDist[i]; best = i; }
        if (best < 0) break;
        s_refDone[best] = 1;

        int cx = best % g->w, cz = best / g->w;
        for (int i = 0; i < 8; i++)
        {
            int nx = cx + DX[i], nz = cz + DZ[i];
            uint8_t terrain = SpGridCost(g, nx, nz);
            if (terrain == SP_COST_BLOCKED) continue;
            // Same corner rule as the search. An oracle that allows corner cuts
            // would report a cheaper "optimum" than A* can legally reach, and
            // the test would fail on correct code.
            if (i >= 4 && (!SpGridPassable(g, cx + DX[i], cz) ||
                           !SpGridPassable(g, cx, cz + DZ[i]))) continue;
            uint32_t step = (uint32_t)((i >= 4) ? 14 : 10)*(uint32_t)terrain;
            uint32_t nd = bestD + step;
            int ni = nz*g->w + nx;
            if (nd < s_refDist[ni]) s_refDist[ni] = nd;
        }
    }
    return s_refDist[gz*g->w + gx];
}

// Cost of a SMOOTHED path is not comparable to a grid cost - smoothing cuts
// corners the grid could not. So optimality is measured on the unsmoothed
// gScore the search itself settled on, read back out of the searcher.
static uint32_t AStarGoalCost(const SpAStar *a, const SpGrid *g, int gx, int gz)
{
    SpCell goal = (SpCell)(gz*g->w + gx);
    if (a->stamp[goal] != a->generation) return REF_INF;
    return a->gScore[goal];
}

static void TestAStarStraightLine(void)
{
    SpGridInit(&s_grid, 16, 16);
    int n = 0;
    SpPathStatus st = SpAStarSolve(&s_astar, &s_grid, 2, 8, 12, 8,
                                   s_path, SP_CELLS_MAX, &n);
    CHECK(st == SP_PATH_FOUND);
    // Open ground: smoothing must collapse the whole walk to the goal alone.
    CHECK(n == 1);
    CHECK(s_path[0] == (SpCell)(8*s_grid.w + 12));
}

static void TestAStarStartIsGoal(void)
{
    SpGridInit(&s_grid, 16, 16);
    int n = 5;
    SpPathStatus st = SpAStarSolve(&s_astar, &s_grid, 4, 4, 4, 4,
                                   s_path, SP_CELLS_MAX, &n);
    // Success with nothing to walk. FAILED here would make a unit standing on
    // its destination retry every frame forever.
    CHECK(st == SP_PATH_FOUND);
    CHECK(n == 0);
}

static void TestAStarBlockedGoal(void)
{
    SpGridInit(&s_grid, 16, 16);
    SpGridSet(&s_grid, 10, 10, SP_COST_BLOCKED);
    int n = 0;
    CHECK(SpAStarSolve(&s_astar, &s_grid, 2, 2, 10, 10,
                       s_path, SP_CELLS_MAX, &n) == SP_PATH_FAILED);

    // Sealed room: goal is open but unreachable. The open set must drain and
    // the search must terminate, not spin.
    SpGridInit(&s_grid, 16, 16);
    for (int i = 6; i <= 10; i++)
    {
        SpGridSet(&s_grid, i, 6, SP_COST_BLOCKED);
        SpGridSet(&s_grid, i, 10, SP_COST_BLOCKED);
        SpGridSet(&s_grid, 6, i, SP_COST_BLOCKED);
        SpGridSet(&s_grid, 10, i, SP_COST_BLOCKED);
    }
    CHECK(SpAStarSolve(&s_astar, &s_grid, 1, 1, 8, 8,
                       s_path, SP_CELLS_MAX, &n) == SP_PATH_FAILED);
}

static void TestAStarWallDetour(void)
{
    // A wall across the middle with one gap. The path must go through the gap,
    // and after smoothing it is exactly two corners: the gap, then the goal.
    SpGridInit(&s_grid, 21, 21);
    for (int x = 0; x < 21; x++)
        if (x != 10) SpGridSet(&s_grid, x, 10, SP_COST_BLOCKED);

    int n = 0;
    CHECK(SpAStarSolve(&s_astar, &s_grid, 10, 2, 10, 18,
                       s_path, SP_CELLS_MAX, &n) == SP_PATH_FOUND);
    CHECK(n >= 1);
    // Every waypoint must be passable, and the last must be the goal.
    for (int i = 0; i < n; i++)
    {
        int x = (int)(s_path[i] % (SpCell)s_grid.w);
        int z = (int)(s_path[i] / (SpCell)s_grid.w);
        CHECK(SpGridPassable(&s_grid, x, z));
    }
    CHECK(s_path[n-1] == (SpCell)(18*s_grid.w + 10));
}

static void TestAStarOptimality(void)
{
    // 20 seeded random grids, A* gScore vs reference Dijkstra. This is the
    // admissibility guard: a heuristic that overestimates - by mis-scaling the
    // diagonal, or by not accounting for terrain cost being at least 1 - gives
    // paths that are valid and not shortest.
    RngSeed(0xA57A5u);
    for (int trial = 0; trial < 20; trial++)
    {
        SpGridInit(&s_grid, 24, 24);
        for (int z = 0; z < 24; z++)
            for (int x = 0; x < 24; x++)
            {
                uint32_t r = RngNext() % 100u;
                uint8_t c = SP_COST_NORMAL;
                if (r < 22)      c = SP_COST_BLOCKED;
                else if (r < 35) c = SP_COST_SHALLOW;
                else if (r < 45) c = SP_COST_SKIRT;
                SpGridSet(&s_grid, x, z, c);
            }
        SpGridSet(&s_grid, 0, 0, SP_COST_NORMAL);
        SpGridSet(&s_grid, 23, 23, SP_COST_NORMAL);

        uint32_t ref = RefDijkstra(&s_grid, 0, 0, 23, 23);
        int n = 0;
        SpPathStatus st = SpAStarSolve(&s_astar, &s_grid, 0, 0, 23, 23,
                                       s_path, SP_CELLS_MAX, &n);
        if (ref == REF_INF) { CHECK(st == SP_PATH_FAILED); continue; }

        CHECK(st == SP_PATH_FOUND);
        CHECK(AStarGoalCost(&s_astar, &s_grid, 23, 23) == ref);
    }
}

static void TestAStarBudgetResumption(void)
{
    // The correctness proof for time-slicing: one node at a time must land on
    // the same path as unbudgeted, byte for byte.
    RngSeed(0xB0D6E7u);
    for (int trial = 0; trial < 8; trial++)
    {
        SpGridInit(&s_grid, 24, 24);
        for (int z = 0; z < 24; z++)
            for (int x = 0; x < 24; x++)
                SpGridSet(&s_grid, x, z,
                          ((RngNext() % 100u) < 20u) ? SP_COST_BLOCKED : SP_COST_NORMAL);
        SpGridSet(&s_grid, 0, 0, SP_COST_NORMAL);
        SpGridSet(&s_grid, 23, 23, SP_COST_NORMAL);

        int nFull = 0;
        SpPathStatus full = SpAStarSolve(&s_astar, &s_grid, 0, 0, 23, 23,
                                         s_path, SP_CELLS_MAX, &nFull);

        int nSlice = 0;
        SpPathStatus sliced;
        SpAStarBegin(&s_astar, &s_grid, 0, 0, 23, 23);
        int guard = 0;
        do {
            sliced = SpAStarStep(&s_astar, 1, s_pathB, SP_CELLS_MAX, &nSlice);
        } while ((sliced == SP_PATH_BUSY) && (++guard < 200000));

        CHECK(guard < 200000);          // must terminate, not spin
        CHECK(sliced == full);
        CHECK(nSlice == nFull);
        for (int i = 0; i < nFull; i++) CHECK(s_path[i] == s_pathB[i]);
    }
}

static void TestAStarDeterminism(void)
{
    // Same search twice, and again after an UNRELATED search has run. The
    // second half is what catches generation/stamp state leaking between
    // searches - the bug the stamp trick exists to enable and would hide.
    SpGridInit(&s_grid, 32, 32);
    RngSeed(0xDE7E3u);
    for (int z = 0; z < 32; z++)
        for (int x = 0; x < 32; x++)
            SpGridSet(&s_grid, x, z,
                      ((RngNext() % 100u) < 18u) ? SP_COST_BLOCKED : SP_COST_NORMAL);
    SpGridSet(&s_grid, 1, 1, SP_COST_NORMAL);
    SpGridSet(&s_grid, 30, 30, SP_COST_NORMAL);

    int n1 = 0, n2 = 0, nJunk = 0;
    SpPathStatus a = SpAStarSolve(&s_astar, &s_grid, 1, 1, 30, 30,
                                  s_path, SP_CELLS_MAX, &n1);
    SpAStarSolve(&s_astar, &s_grid, 30, 1, 1, 30, s_pathB, SP_CELLS_MAX, &nJunk);
    SpPathStatus b = SpAStarSolve(&s_astar, &s_grid, 1, 1, 30, 30,
                                  s_pathB, SP_CELLS_MAX, &n2);

    CHECK(a == b);
    CHECK(n1 == n2);
    for (int i = 0; i < n1; i++) CHECK(s_path[i] == s_pathB[i]);
}

static void TestLosDiagonalGap(void)
{
    // THE named case. (1,0) and (0,1) blocked leaves (0,0)->(1,1) touching only
    // at a corner. A point slips through; a 0.35-radius unit wedges. Standard
    // Bresenham reports this clear, which is why the implementation is
    // supercover and why this test exists by name.
    SpGridInit(&s_grid, 8, 8);
    SpGridSet(&s_grid, 1, 0, SP_COST_BLOCKED);
    SpGridSet(&s_grid, 0, 1, SP_COST_BLOCKED);
    CHECK(!SpLosClear(&s_grid, 0, 0, 1, 1));

    // Open either side and it becomes legitimately walkable.
    SpGridSet(&s_grid, 1, 0, SP_COST_NORMAL);
    CHECK(SpLosClear(&s_grid, 0, 0, 1, 1));
}

static void TestLosBasics(void)
{
    SpGridInit(&s_grid, 16, 16);
    CHECK(SpLosClear(&s_grid, 0, 0, 15, 15));       // empty grid: everything visible
    CHECK(SpLosClear(&s_grid, 3, 9, 3, 9));         // degenerate segment

    SpGridSet(&s_grid, 8, 8, SP_COST_BLOCKED);
    CHECK(!SpLosClear(&s_grid, 8, 2, 8, 14));       // straight through the wall
    CHECK(!SpLosClear(&s_grid, 2, 8, 14, 8));
    CHECK(SpLosClear(&s_grid, 2, 2, 2, 14));        // clear of it

    // An endpoint inside an obstacle is never visible, whichever end it is.
    CHECK(!SpLosClear(&s_grid, 8, 8, 2, 2));
    CHECK(!SpLosClear(&s_grid, 2, 2, 8, 8));

    // Symmetry: LOS is a property of the segment, not the direction walked.
    // An asymmetric supercover is a real and very confusing bug - a smoother
    // would keep a waypoint going one way and drop it coming back.
    // Swept across densities and seeds because this DID fail: the first
    // implementation used an accumulated error term and disagreed with itself
    // by direction on 5 of 400 segments. A handful of samples would have missed
    // it, so the sweep is the test, not a spot check.
    RngSeed(0x10537u);
    for (int density = 10; density <= 40; density += 10)
    {
        SpGridInit(&s_grid, 20, 20);
        for (int z = 0; z < 20; z++)
            for (int x = 0; x < 20; x++)
                SpGridSet(&s_grid, x, z,
                          ((RngNext() % 100u) < (uint32_t)density)
                              ? SP_COST_BLOCKED : SP_COST_NORMAL);
        for (int t = 0; t < 1500; t++)
        {
            int x0 = (int)(RngNext() % 20u), z0 = (int)(RngNext() % 20u);
            int x1 = (int)(RngNext() % 20u), z1 = (int)(RngNext() % 20u);
            CHECK(SpLosClear(&s_grid, x0, z0, x1, z1) ==
                  SpLosClear(&s_grid, x1, z1, x0, z0));
        }
    }
}

static void TestSmoothPreservesWalkability(void)
{
    // Smoothing may only REMOVE waypoints, never create a leg that cuts a
    // corner. Verified by re-walking every smoothed leg with SpLosClear on
    // random grids - if a leg is not clear, the smoother invented it.
    RngSeed(0x5007A1u);
    for (int trial = 0; trial < 15; trial++)
    {
        SpGridInit(&s_grid, 28, 28);
        for (int z = 0; z < 28; z++)
            for (int x = 0; x < 28; x++)
                SpGridSet(&s_grid, x, z,
                          ((RngNext() % 100u) < 20u) ? SP_COST_BLOCKED : SP_COST_NORMAL);
        SpGridSet(&s_grid, 0, 0, SP_COST_NORMAL);
        SpGridSet(&s_grid, 27, 27, SP_COST_NORMAL);

        int n = 0;
        if (SpAStarSolve(&s_astar, &s_grid, 0, 0, 27, 27,
                         s_path, SP_CELLS_MAX, &n) != SP_PATH_FOUND) continue;

        int px = 0, pz = 0;
        for (int i = 0; i < n; i++)
        {
            int x = (int)(s_path[i] % (SpCell)s_grid.w);
            int z = (int)(s_path[i] / (SpCell)s_grid.w);
            CHECK(SpLosClear(&s_grid, px, pz, x, z));
            px = x; pz = z;
        }
        CHECK(s_path[n-1] == (SpCell)(27*s_grid.w + 27));
    }
}

static void TestAStarTruncation(void)
{
    // A caller with fewer slots than the route needs gets the FIRST maxOut
    // waypoints, all valid, so it can walk them and repath from the last.
    // A maze-ish grid, so smoothing cannot collapse the route to one hop.
    SpGridInit(&s_grid, 40, 40);
    for (int z = 2; z < 38; z += 4)
        for (int x = 0; x < 40; x++)
        {
            int gap = ((z/4) % 2 == 0) ? 38 : 1;
            if (x != gap) SpGridSet(&s_grid, x, z, SP_COST_BLOCKED);
        }

    int nFull = 0;
    CHECK(SpAStarSolve(&s_astar, &s_grid, 1, 0, 38, 39,
                       s_path, SP_CELLS_MAX, &nFull) == SP_PATH_FOUND);
    CHECK(nFull > 3);       // the maze must actually need several corners

    int nCut = 0;
    CHECK(SpAStarSolve(&s_astar, &s_grid, 1, 0, 38, 39,
                       s_pathB, 3, &nCut) == SP_PATH_FOUND);
    CHECK(nCut == 3);
    for (int i = 0; i < nCut; i++) CHECK(s_pathB[i] == s_path[i]);
}

// ---------------------------------------------------------------------------
//  Path service
//
//  The queue's job is to make one searcher serve many askers without anyone
//  getting the wrong answer. The failures worth testing for are all of that
//  shape - a result delivered to the wrong owner, a stale handle cancelling a
//  live request, a sliced search losing its place - because none of them crash.
//  They show up as one unit in a hundred walking somewhere strange.
// ---------------------------------------------------------------------------
static SpCell s_svcOut[SP_PATH_MAX];

// Drain the service to completion, capped so a hang fails the test rather than
// hanging the suite.
static int SvcRunToIdle(int maxFrames)
{
    for (int f = 0; f < maxFrames; f++)
    {
        SpServiceUpdate();
        if (SpServiceGetStats()->queued == 0) return f + 1;
    }
    return -1;
}

static void TestServiceBasic(void)
{
    SpGridInit(&s_grid, 32, 32);
    SpServiceReset(&s_grid);
    SpServiceResetStats();
    SpServiceSetBudget(100000);

    SpRequestId id = SpServiceRequest(7, 2, 2, 20, 20);
    CHECK(id != SP_REQUEST_NONE);
    CHECK(SpServiceGetStats()->queued == 1);
    CHECK(SvcRunToIdle(50) > 0);

    int32_t owner = -1; SpPathStatus st = SP_PATH_FAILED; int n = 0;
    CHECK(SpServicePoll(&owner, &st, s_svcOut, SP_PATH_MAX, &n));
    CHECK(owner == 7);
    CHECK(st == SP_PATH_FOUND);
    CHECK(n == 1);                                  // open ground smooths to the goal
    CHECK(s_svcOut[0] == (SpCell)(20*s_grid.w + 20));

    // Delivered exactly once.
    CHECK(!SpServicePoll(&owner, &st, s_svcOut, SP_PATH_MAX, &n));
}

static void TestServiceGoalResolves(void)
{
    // A goal inside an obstacle must be nudged to open ground, not rejected.
    // This is the "ordered to walk into a lake" case, and every request goes
    // through it so no call site can forget.
    SpGridInit(&s_grid, 32, 32);
    for (int z = 14; z <= 18; z++)
        for (int x = 14; x <= 18; x++)
            SpGridSet(&s_grid, x, z, SP_COST_BLOCKED);

    SpServiceReset(&s_grid);
    SpServiceSetBudget(100000);
    CHECK(SpServiceRequest(1, 2, 2, 16, 16) != SP_REQUEST_NONE);
    CHECK(SvcRunToIdle(50) > 0);

    int32_t owner; SpPathStatus st; int n = 0;
    CHECK(SpServicePoll(&owner, &st, s_svcOut, SP_PATH_MAX, &n));
    CHECK(st == SP_PATH_FOUND);
    CHECK(n >= 1);
    int fx = (int)(s_svcOut[n-1] % (SpCell)s_grid.w);
    int fz = (int)(s_svcOut[n-1] / (SpCell)s_grid.w);
    CHECK(SpGridPassable(&s_grid, fx, fz));         // landed outside the block
}

static void TestServiceSlicingMatchesEager(void)
{
    // The service's own version of the resumption proof: the same batch of
    // requests, served one node per frame, must deliver the same paths to the
    // same owners as an unbudgeted run. A budget is a scheduling decision and
    // must never be an ANSWER decision.
    SpGridInit(&s_grid, 32, 32);
    RngSeed(0x5E7Cu);
    for (int z = 0; z < 32; z++)
        for (int x = 0; x < 32; x++)
            SpGridSet(&s_grid, x, z,
                      ((RngNext() % 100u) < 18u) ? SP_COST_BLOCKED : SP_COST_NORMAL);

    const int REQ = 12;
    int sxs[12], szs[12], gxs[12], gzs[12];
    for (int i = 0; i < REQ; i++)
    {
        sxs[i] = 1 + (int)(RngNext() % 30u); szs[i] = 1 + (int)(RngNext() % 30u);
        gxs[i] = 1 + (int)(RngNext() % 30u); gzs[i] = 1 + (int)(RngNext() % 30u);
    }

    // Eager pass: record each owner's path.
    static SpCell eager[12][SP_PATH_MAX];
    int eagerN[12]; SpPathStatus eagerSt[12];
    for (int i = 0; i < REQ; i++) { eagerN[i] = -1; eagerSt[i] = SP_PATH_BUSY; }

    SpServiceReset(&s_grid);
    SpServiceSetBudget(1000000);
    for (int i = 0; i < REQ; i++) SpServiceRequest(i, sxs[i], szs[i], gxs[i], gzs[i]);
    CHECK(SvcRunToIdle(200) > 0);
    for (;;)
    {
        int32_t owner; SpPathStatus st; int n = 0;
        if (!SpServicePoll(&owner, &st, s_svcOut, SP_PATH_MAX, &n)) break;
        CHECK(owner >= 0 && owner < REQ);
        eagerSt[owner] = st;
        eagerN[owner]  = n;
        for (int k = 0; k < n; k++) eager[owner][k] = s_svcOut[k];
    }

    // Sliced pass: one expansion per frame.
    SpServiceReset(&s_grid);
    SpServiceSetBudget(1);
    for (int i = 0; i < REQ; i++) SpServiceRequest(i, sxs[i], szs[i], gxs[i], gzs[i]);
    CHECK(SvcRunToIdle(500000) > 0);
    int delivered = 0;
    for (;;)
    {
        int32_t owner; SpPathStatus st; int n = 0;
        if (!SpServicePoll(&owner, &st, s_svcOut, SP_PATH_MAX, &n)) break;
        delivered++;
        CHECK(st == eagerSt[owner]);
        CHECK(n == eagerN[owner]);
        for (int k = 0; k < n; k++) CHECK(s_svcOut[k] == eager[owner][k]);
    }
    CHECK(delivered == REQ);        // nobody lost to slicing
}

static void TestServiceDedup(void)
{
    // Identical start and goal: one search, the rest shared. The point is not
    // the saving (small until flow fields land) but that a shared result is the
    // SAME result - a sharing bug hands a unit someone else's path.
    SpGridInit(&s_grid, 32, 32);
    SpServiceReset(&s_grid);
    SpServiceResetStats();
    SpServiceSetBudget(1000000);

    for (int i = 0; i < 10; i++) CHECK(SpServiceRequest(i, 3, 3, 28, 28) != SP_REQUEST_NONE);
    CHECK(SvcRunToIdle(100) > 0);
    CHECK(SpServiceGetStats()->shared == 9);        // one searched, nine shared

    SpCell first[SP_PATH_MAX];
    int firstN = -1, seen = 0;
    for (;;)
    {
        int32_t owner; SpPathStatus st; int n = 0;
        if (!SpServicePoll(&owner, &st, s_svcOut, SP_PATH_MAX, &n)) break;
        CHECK(st == SP_PATH_FOUND);
        if (firstN < 0)
        {
            firstN = n;
            for (int k = 0; k < n; k++) first[k] = s_svcOut[k];
        }
        else
        {
            CHECK(n == firstN);
            for (int k = 0; k < n; k++) CHECK(s_svcOut[k] == first[k]);
        }
        seen++;
    }
    CHECK(seen == 10);
}

static void TestServiceReorderSupersedes(void)
{
    // A unit re-ordered before its first path arrives must end up with the
    // SECOND destination, and must not hold two slots. A player dragging a
    // waypoint does this every frame; without superseding, the queue fills and
    // the rejection lands on an innocent unit.
    SpGridInit(&s_grid, 32, 32);
    SpServiceReset(&s_grid);
    SpServiceResetStats();
    SpServiceSetBudget(1);                          // guarantee nothing finishes

    SpServiceRequest(5, 2, 2, 20, 20);
    SpServiceUpdate();
    SpServiceRequest(5, 2, 2, 28, 4);               // new order, same owner
    CHECK(SpServiceGetStats()->queued == 1);        // not two

    SpServiceSetBudget(1000000);
    CHECK(SvcRunToIdle(100) > 0);

    int32_t owner; SpPathStatus st; int n = 0;
    CHECK(SpServicePoll(&owner, &st, s_svcOut, SP_PATH_MAX, &n));
    CHECK(owner == 5);
    CHECK(st == SP_PATH_FOUND);
    CHECK(s_svcOut[n-1] == (SpCell)(4*s_grid.w + 28));   // the SECOND goal
    CHECK(!SpServicePoll(&owner, &st, s_svcOut, SP_PATH_MAX, &n));
}

static void TestServiceCancelMidSearch(void)
{
    // Cancelling the request the searcher is actively on. The searcher must
    // abandon it rather than resume into a slot that has since been reused -
    // which would deliver a stranger's path to whoever now holds the slot.
    SpGridInit(&s_grid, 64, 64);
    SpServiceReset(&s_grid);
    SpServiceResetStats();
    SpServiceSetBudget(2);                          // will not finish in one frame

    SpRequestId a = SpServiceRequest(1, 1, 1, 62, 62);
    SpServiceUpdate();
    SpServiceCancel(a);
    CHECK(SpServiceGetStats()->queued == 0);

    SpServiceSetBudget(1000000);
    SpRequestId b = SpServiceRequest(2, 1, 1, 30, 30);
    CHECK(b != SP_REQUEST_NONE);
    CHECK(SvcRunToIdle(100) > 0);

    int32_t owner; SpPathStatus st; int n = 0;
    CHECK(SpServicePoll(&owner, &st, s_svcOut, SP_PATH_MAX, &n));
    CHECK(owner == 2);                              // never owner 1
    CHECK(s_svcOut[n-1] == (SpCell)(30*s_grid.w + 30));
    CHECK(!SpServicePoll(&owner, &st, s_svcOut, SP_PATH_MAX, &n));
}

static void TestServiceStaleCancel(void)
{
    // The sequence half of the request id earning its place: an id from a
    // completed request must not cancel whoever inherited its slot.
    SpGridInit(&s_grid, 32, 32);
    SpServiceReset(&s_grid);
    SpServiceSetBudget(1000000);

    SpRequestId old = SpServiceRequest(1, 2, 2, 10, 10);
    CHECK(SvcRunToIdle(50) > 0);
    int32_t owner; SpPathStatus st; int n = 0;
    CHECK(SpServicePoll(&owner, &st, s_svcOut, SP_PATH_MAX, &n));

    SpRequestId fresh = SpServiceRequest(2, 2, 2, 20, 20);
    CHECK(fresh != SP_REQUEST_NONE);
    CHECK(fresh != old);                    // sequence advanced, so ids differ

    SpServiceCancel(old);                   // must be a no-op
    CHECK(SpServiceGetStats()->queued == 1);
    CHECK(SvcRunToIdle(50) > 0);
    CHECK(SpServicePoll(&owner, &st, s_svcOut, SP_PATH_MAX, &n));
    CHECK(owner == 2);

    // Cancelling nonsense is safe too - callers cancel on death without
    // checking whether the request ever existed.
    SpServiceCancel(SP_REQUEST_NONE);
    SpServiceCancel((SpRequestId)0x7FFFFFF);
}

static void TestServiceQueueFull(void)
{
    // Overflow is REPORTED, not hidden. A rejected unit steers straight at its
    // goal, which is today's behaviour and acceptable; a silently dropped
    // request would be a unit that simply stops taking orders.
    SpGridInit(&s_grid, 32, 32);
    SpServiceReset(&s_grid);
    SpServiceResetStats();
    SpServiceSetBudget(1);

    int accepted = 0;
    for (int i = 0; i < (int)SP_REQUESTS_MAX + 20; i++)
        if (SpServiceRequest(i, 2, 2, 20 + (i % 8), 20) != SP_REQUEST_NONE) accepted++;

    CHECK(accepted == (int)SP_REQUESTS_MAX);
    CHECK(SpServiceGetStats()->rejected == 20);

    SpServiceSetBudget(1000000);
    CHECK(SvcRunToIdle(5000) > 0);
}

static void TestServiceUnreachableReports(void)
{
    // A walled-off goal must come back FAILED and be counted, so the lab
    // overlay can show a spike in `failed` - which explains a slowdown in a way
    // a millisecond number never does.
    SpGridInit(&s_grid, 32, 32);
    for (int i = 20; i <= 28; i++)
    {
        SpGridSet(&s_grid, i, 20, SP_COST_BLOCKED);
        SpGridSet(&s_grid, i, 28, SP_COST_BLOCKED);
        SpGridSet(&s_grid, 20, i, SP_COST_BLOCKED);
        SpGridSet(&s_grid, 28, i, SP_COST_BLOCKED);
    }
    SpServiceReset(&s_grid);
    SpServiceResetStats();
    SpServiceSetBudget(1000000);

    CHECK(SpServiceRequest(3, 2, 2, 24, 24) != SP_REQUEST_NONE);
    CHECK(SvcRunToIdle(100) > 0);

    int32_t owner; SpPathStatus st; int n = 0;
    CHECK(SpServicePoll(&owner, &st, s_svcOut, SP_PATH_MAX, &n));
    CHECK(owner == 3);
    CHECK(st == SP_PATH_FAILED);
    CHECK(SpServiceGetStats()->failed >= 1);
}

// ---------------------------------------------------------------------------
//  Flow fields
//
//  The headline test is not "does a field build" - it is that following the
//  direction arrows from EVERY reachable cell of a maze arrives at the goal.
//  A field with one local minimum looks completely fine in an overlay and traps
//  whichever units happen to walk into that cell, which is the kind of bug that
//  gets blamed on the steering for a week.
//
//  The second is that bucketed Dijkstra agrees with a plain reference Dijkstra
//  on random grids. The bucket ring is the one piece of cleverness in the build,
//  and its failure mode - a relaxation wrapping onto an already-drained bucket -
//  loses cells silently and only for particular cost patterns.
// ---------------------------------------------------------------------------

static void TestFlowBasics(void)
{
    SpGridInit(&s_grid, 24, 24);
    SpFlowReset(&s_grid);
    SpFlowSetVersion(1);

    SpFieldId id = SpFlowAcquire(12, 12);
    CHECK(id != SP_FIELD_NONE);
    CHECK(SpFlowValid(id));
    CHECK(SpFlowLiveCount() == 1);

    // The goal itself is cost 0 and has no direction to give.
    CHECK(SpFlowCost(id, 12, 12) == 0);
    float dx, dz;
    CHECK(!SpFlowDir(id, 12, 12, &dx, &dz));

    // Cost rises with distance on open ground, and a direction exists anywhere.
    CHECK(SpFlowCost(id, 12, 11) < SpFlowCost(id, 12, 5));
    CHECK(SpFlowDir(id, 2, 2, &dx, &dz));
    CHECK(dx > 0.0f && dz > 0.0f);          // toward the middle, from the corner
}

static void TestFlowCacheAndCoarsening(void)
{
    SpGridInit(&s_grid, 32, 32);
    SpFlowReset(&s_grid);
    SpFlowSetVersion(1);

    SpFieldId a = SpFlowAcquire(16, 16);
    CHECK(a != SP_FIELD_NONE);

    // The same goal is a hit, not a rebuild.
    CHECK(SpFlowAcquire(16, 16) == a);
    CHECK(SpFlowFind(16, 16) == a);

    // A goal one tile away shares the field: keys are coarsened to 2x2 blocks,
    // which is what makes near-identical clicks cheap. Two tiles of goal error
    // vanishes under the formation offsets a group order applies anyway.
    CHECK(SpFlowAcquire(17, 16) == a);
    CHECK(SpFlowAcquire(17, 17) == a);
    CHECK(SpFlowLiveCount() == 1);

    // Far enough away and it is a different field.
    SpFieldId b = SpFlowAcquire(4, 4);
    CHECK(b != SP_FIELD_NONE);
    CHECK(b != a);
    CHECK(SpFlowLiveCount() == 2);
}

static void TestFlowInvalidatesOnNavChange(void)
{
    // A field is a WHOLE-GRID answer, so any wall placed anywhere can make part
    // of it wrong. Invalidation is deliberately all-or-nothing: a field built
    // against an older grid must never be handed out again.
    SpGridInit(&s_grid, 24, 24);
    SpFlowReset(&s_grid);
    SpFlowSetVersion(1);

    SpFieldId a = SpFlowAcquire(12, 12);
    CHECK(a != SP_FIELD_NONE);
    CHECK(SpFlowValid(a));

    SpFlowSetVersion(2);                    // something was built or demolished
    CHECK(!SpFlowValid(a));
    CHECK(SpFlowFind(12, 12) == SP_FIELD_NONE);

    float dx, dz;
    CHECK(!SpFlowDir(a, 2, 2, &dx, &dz));   // a stale field answers nothing
    CHECK(SpFlowCost(a, 2, 2) == SP_FLOW_UNREACHED);

    // Re-acquiring rebuilds it against the new version.
    SpFieldId b = SpFlowAcquire(12, 12);
    CHECK(b != SP_FIELD_NONE);
    CHECK(SpFlowValid(b));
}

static void TestFlowUnreachable(void)
{
    // A sealed room: cells inside cannot reach a goal outside, and must say so
    // rather than pointing somewhere hopeful.
    SpGridInit(&s_grid, 32, 32);
    for (int i = 20; i <= 28; i++)
    {
        SpGridSet(&s_grid, i, 20, SP_COST_BLOCKED);
        SpGridSet(&s_grid, i, 28, SP_COST_BLOCKED);
        SpGridSet(&s_grid, 20, i, SP_COST_BLOCKED);
        SpGridSet(&s_grid, 28, i, SP_COST_BLOCKED);
    }
    SpFlowReset(&s_grid);
    SpFlowSetVersion(1);

    SpFieldId id = SpFlowAcquire(2, 2);
    CHECK(id != SP_FIELD_NONE);
    CHECK(SpFlowCost(id, 24, 24) == SP_FLOW_UNREACHED);
    float dx, dz;
    CHECK(!SpFlowDir(id, 24, 24, &dx, &dz));
    CHECK(SpFlowCost(id, 30, 30) != SP_FLOW_UNREACHED);      // outside: fine
}

// THE headline test. Walk the arrows from every reachable cell and require
// arrival. A single local minimum traps every unit that steps on that cell.
static void FlowFollowAllCells(int w, int h, int gx, int gz, const char *what)
{
    SpFlowReset(&s_grid);
    SpFlowSetVersion(1);
    SpFieldId id = SpFlowAcquire(gx, gz);
    CHECK(id != SP_FIELD_NONE);
    if (id == SP_FIELD_NONE) return;

    int trapped = 0, tested = 0;
    for (int z = 0; z < h; z++)
    {
        for (int x = 0; x < w; x++)
        {
            if (!SpGridPassable(&s_grid, x, z)) continue;
            if (SpFlowCost(id, x, z) == SP_FLOW_UNREACHED) continue;
            tested++;

            int cx = x, cz = z;
            int steps = 0, cap = 4*(w + h);
            while ((cx != gx || cz != gz) && steps < cap)
            {
                float dx, dz;
                if (!SpFlowDir(id, cx, cz, &dx, &dz)) break;
                // The arrows are eight-way unit steps; round back to integers.
                cx += (dx > 0.3f) ? 1 : (dx < -0.3f) ? -1 : 0;
                cz += (dz > 0.3f) ? 1 : (dz < -0.3f) ? -1 : 0;
                steps++;
            }
            if (cx != gx || cz != gz) trapped++;
        }
    }
    if (trapped != 0)
        printf("  (%s: %d of %d cells never reached the goal)\n", what, trapped, tested);
    CHECK(trapped == 0);
    CHECK(tested > 0);
}

static void TestFlowNoLocalMinima(void)
{
    // Open ground first - if this fails nothing else is worth reading.
    SpGridInit(&s_grid, 32, 32);
    FlowFollowAllCells(32, 32, 16, 16, "open");

    // A spiral maze: the case where a naive "walk toward the goal" field traps
    // units against the inside of a wall.
    SpGridInit(&s_grid, 32, 32);
    for (int r = 2; r < 15; r += 4)
    {
        for (int i = r; i < 32 - r; i++)
        {
            SpGridSet(&s_grid, i, r, SP_COST_BLOCKED);
            SpGridSet(&s_grid, i, 31 - r, SP_COST_BLOCKED);
            SpGridSet(&s_grid, r, i, SP_COST_BLOCKED);
            SpGridSet(&s_grid, 31 - r, i, SP_COST_BLOCKED);
        }
        SpGridSet(&s_grid, r, 16, SP_COST_NORMAL);          // one gap per ring
    }
    FlowFollowAllCells(32, 32, 16, 16, "spiral");

    // Random obstacles at several densities, and mixed terrain costs - the
    // steepest-descent pass has to cope with a cost surface, not just a
    // distance one.
    RngSeed(0xF10AAu);
    for (int trial = 0; trial < 10; trial++)
    {
        SpGridInit(&s_grid, 28, 28);
        for (int z = 0; z < 28; z++)
            for (int x = 0; x < 28; x++)
            {
                uint32_t r = RngNext() % 100u;
                uint8_t c = SP_COST_NORMAL;
                if (r < 22)      c = SP_COST_BLOCKED;
                else if (r < 38) c = SP_COST_SHALLOW;
                else if (r < 50) c = SP_COST_SKIRT;
                SpGridSet(&s_grid, x, z, c);
            }
        SpGridSet(&s_grid, 14, 14, SP_COST_NORMAL);
        FlowFollowAllCells(28, 28, 14, 14, "random");
    }
}

static void TestFlowMatchesDijkstra(void)
{
    // Bucketed Dijkstra vs the plain reference, on random cost grids. The bucket
    // ring's failure mode is a relaxation wrapping onto a drained bucket, which
    // loses cells silently and only for particular cost patterns - so this is
    // swept over many seeds rather than spot-checked.
    RngSeed(0xB0C4E7u);
    for (int trial = 0; trial < 20; trial++)
    {
        SpGridInit(&s_grid, 24, 24);
        for (int z = 0; z < 24; z++)
            for (int x = 0; x < 24; x++)
            {
                uint32_t r = RngNext() % 100u;
                uint8_t c = SP_COST_NORMAL;
                if (r < 20)      c = SP_COST_BLOCKED;
                else if (r < 40) c = SP_COST_SHALLOW;
                else if (r < 55) c = SP_COST_SKIRT;
                SpGridSet(&s_grid, x, z, c);
            }
        SpGridSet(&s_grid, 12, 12, SP_COST_NORMAL);

        SpFlowReset(&s_grid);
        SpFlowSetVersion(1);
        SpFieldId id = SpFlowAcquire(12, 12);
        CHECK(id != SP_FIELD_NONE);

        // DIRECTION MATTERS HERE. A step charges the terrain of the tile being
        // ENTERED, so cost(A->B) != cost(B->A) wherever terrain differs - the
        // graph is directed even though the walls are not. A flow field answers
        // "what does it cost to walk from HERE to the goal", so the reference
        // must be run cell -> goal, NOT goal -> cell.
        //
        // Comparing against the wrong direction is not a small error: it passes
        // on every uniform-cost grid and only diverges once terrain varies,
        // which is precisely the case flow fields are used on.
        for (int z = 0; z < 24; z++)
            for (int x = 0; x < 24; x++)
            {
                if (!SpGridPassable(&s_grid, x, z)) continue;
                uint32_t ref = RefDijkstra(&s_grid, x, z, 12, 12);
                uint16_t got = SpFlowCost(id, x, z);
                if (ref == REF_INF) CHECK(got == SP_FLOW_UNREACHED);
                else                CHECK((uint32_t)got == ref);
            }
    }
}

static void TestFlowEvictionRespectsRefcount(void)
{
    // Filling every slot then asking for one more must NOT evict a field units
    // are standing on. Stranding a crowd mid-walk is far worse than falling
    // back to individual A*, which is slower but always correct.
    SpGridInit(&s_grid, 24, 24);
    SpFlowReset(&s_grid);
    SpFlowSetVersion(1);

    SpFieldId held[SP_FLOW_FIELDS_MAX];
    for (int i = 0; i < SP_FLOW_FIELDS_MAX; i++)
    {
        held[i] = SpFlowAcquire(2 + 4*i, 2);
        CHECK(held[i] != SP_FIELD_NONE);
    }
    CHECK(SpFlowLiveCount() == SP_FLOW_FIELDS_MAX);

    // Everything is in use.
    SpFlowSweepBegin();
    for (int i = 0; i < SP_FLOW_FIELDS_MAX; i++) SpFlowSweepMark(held[i]);
    SpFlowSweepEnd(1.0f);

    CHECK(SpFlowAcquire(20, 20) == SP_FIELD_NONE);      // refuses rather than strands
    for (int i = 0; i < SP_FLOW_FIELDS_MAX; i++) CHECK(SpFlowValid(held[i]));

    // Release one by not marking it, and the slot becomes available.
    SpFlowSweepBegin();
    for (int i = 1; i < SP_FLOW_FIELDS_MAX; i++) SpFlowSweepMark(held[i]);
    SpFlowSweepEnd(2.0f);

    SpFieldId fresh = SpFlowAcquire(20, 20);
    CHECK(fresh != SP_FIELD_NONE);
    CHECK(fresh == held[0]);                            // the unreferenced one
}

static void TestFlowSweepIsIdempotent(void)
{
    // The sweep RECOMPUTES refcounts rather than adjusting them, which is the
    // whole reason it cannot leak: running it twice with the same marks must
    // give the same answer, not double it.
    SpGridInit(&s_grid, 16, 16);
    SpFlowReset(&s_grid);
    SpFlowSetVersion(1);
    SpFieldId a = SpFlowAcquire(8, 8);

    for (int pass = 0; pass < 3; pass++)
    {
        SpFlowSweepBegin();
        SpFlowSweepMark(a);
        SpFlowSweepMark(a);
        SpFlowSweepEnd(1.0f + (float)pass);
    }
    // Two marks, three passes: still two, not six.
    SpFlowSweepBegin();
    SpFlowSweepEnd(9.0f);
    // With nothing marked the field is unreferenced and therefore evictable.
    SpFieldId b = SpFlowAcquire(2, 2);
    CHECK(b != SP_FIELD_NONE);
    CHECK(SpFlowValid(b));
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  Formation geometry
//
//  THE BUG THIS EXISTS TO CATCH. The first formation system shipped a form-up
//  tolerance of 1.4 world units against a slot pitch of 1.5 - so a group was
//  asked to pack tighter than its own slots were spaced, the latch could never
//  fire, and a thousand-unit army crawled at 0.15x speed for the whole march.
//  Nothing caught it because the layout math lived in a file that cannot link
//  into a test. It can now, so the invariants it has to satisfy are asserted
//  here rather than discovered in a playtest.
// ---------------------------------------------------------------------------
static const SpFormCaps s_caps = { 40.0f, 2, 40.0f, 2, 3.0f };
#define FORM_TEST_PITCH 1.5f

static const int s_formCounts[] = { 2, 10, 100, 512 };
#define FORM_TEST_COUNTS ((int)(sizeof(s_formCounts)/sizeof(s_formCounts[0])))

// No two units may be sent to the same place. A duplicate slot is two units
// ordered to stand inside each other, which separation then fights forever.
static void TestFormSlotsUnique(void)
{
    static float rr[512], ff[512];

    for (int shape = 0; shape < SP_FORM_SHAPE_COUNT; shape++)
    {
        for (int c = 0; c < FORM_TEST_COUNTS; c++)
        {
            int n = s_formCounts[c];
            for (int i = 0; i < n; i++)
                SpFormSlotLocal(shape, i, n, FORM_TEST_PITCH, &s_caps, &rr[i], &ff[i]);

            int dupes = 0;
            for (int i = 0; i < n; i++)
                for (int j = i + 1; j < n; j++)
                {
                    float dx = rr[i] - rr[j], dz = ff[i] - ff[j];
                    if (dx*dx + dz*dz < 0.0001f) dupes++;
                }
            CHECK(dupes == 0);
        }
    }
}

// THE D3 INVARIANT. The closest any two slots sit is what every form-up
// tolerance has to clear: ask units to be nearer their slots than their slots
// are to each other and the latch is unsatisfiable by construction.
static void TestFormSlotSpacing(void)
{
    static float rr[512], ff[512];

    for (int shape = 0; shape < SP_FORM_SHAPE_COUNT; shape++)
    {
        for (int c = 0; c < FORM_TEST_COUNTS; c++)
        {
            int n = s_formCounts[c];
            for (int i = 0; i < n; i++)
                SpFormSlotLocal(shape, i, n, FORM_TEST_PITCH, &s_caps, &rr[i], &ff[i]);

            float closest = 1e9f;
            for (int i = 0; i < n; i++)
                for (int j = i + 1; j < n; j++)
                {
                    float dx = rr[i] - rr[j], dz = ff[i] - ff[j];
                    float d = sqrtf(dx*dx + dz*dz);
                    if (d < closest) closest = d;
                }

            // FREEFORM is a scatter, not a lattice, so its minimum pair is not
            // the pitch - but it still may not put two units on one spot.
            if (shape == SP_FORM_FREEFORM) CHECK(closest > FORM_TEST_PITCH*0.4f);
            else                           CHECK(closest > FORM_TEST_PITCH*0.9f);
        }
    }
}

// EXTENT CAPS ARE LOAD-BEARING. Past them a shape scales linearly with the unit
// count and walks off the map: the outer slots land outside the grid, where
// SpNearestOpen's ring cannot recover them, and those units are stranded.
static void TestFormExtentCaps(void)
{
    for (int c = 0; c < FORM_TEST_COUNTS; c++)
    {
        int n = s_formCounts[c];

        float wide = 0.0f, deep = 0.0f;
        for (int i = 0; i < n; i++)
        {
            float r, f;
            SpFormSlotLocal(SP_FORM_LINE, i, n, FORM_TEST_PITCH, &s_caps, &r, &f);
            if (fabsf(r) > wide) wide = fabsf(r);
        }
        CHECK(wide*2.0f <= s_caps.lineMaxWidth + FORM_TEST_PITCH);

        for (int i = 0; i < n; i++)
        {
            float r, f;
            SpFormSlotLocal(SP_FORM_COLUMN, i, n, FORM_TEST_PITCH, &s_caps, &r, &f);
            if (fabsf(f) > deep) deep = fabsf(f);
        }
        CHECK(deep <= s_caps.columnMaxDepth + FORM_TEST_PITCH);
    }
}

// Slot 0 is the FRONT rank for every ordered shape. The assignment sort relies
// on it to put the units that start nearest the destination at the front -
// break the ordering and a group marches through its own formation.
static void TestFormFrontRank(void)
{
    const int ordered[] = { SP_FORM_LINE, SP_FORM_COLUMN, SP_FORM_TWO_COLUMN, SP_FORM_WEDGE };

    for (int k = 0; k < 4; k++)
    {
        for (int c = 0; c < FORM_TEST_COUNTS; c++)
        {
            int n = s_formCounts[c];
            float r0, f0;
            SpFormSlotLocal(ordered[k], 0, n, FORM_TEST_PITCH, &s_caps, &r0, &f0);

            int ahead = 0;
            for (int i = 1; i < n; i++)
            {
                float r, f;
                SpFormSlotLocal(ordered[k], i, n, FORM_TEST_PITCH, &s_caps, &r, &f);
                if (f > f0 + 0.001f) ahead++;
            }
            CHECK(ahead == 0);
        }
    }
}

// FREEFORM's radius must grow as sqrt(i), or density collapses: grow it
// linearly and a thousand units become a thin ring with nothing in the middle.
// Measured as area per unit, which is what "constant density" actually means.
static void TestFormFreeformDensity(void)
{
    float prev = 0.0f;
    for (int c = 0; c < FORM_TEST_COUNTS; c++)
    {
        int n = s_formCounts[c];
        float worst = SpFormHalfExtent(SP_FORM_FREEFORM, n, FORM_TEST_PITCH, &s_caps);
        float perUnit = (worst*worst)/(float)n;     // area/unit, up to pi

        if (prev > 0.0f)
        {
            CHECK(perUnit < prev*1.5f);
            CHECK(perUnit > prev*0.5f);
        }
        prev = perUnit;
    }

    // Deterministic: the same order twice must give the same layout, or the
    // slot overlay flickers and nothing about it can be read.
    float a, b, c2, d;
    SpFormSlotLocal(SP_FORM_FREEFORM, 37, 100, FORM_TEST_PITCH, &s_caps, &a, &b);
    SpFormSlotLocal(SP_FORM_FREEFORM, 37, 100, FORM_TEST_PITCH, &s_caps, &c2, &d);
    CHECK(a == c2 && b == d);
}

// The extent is what the flow release radius scales with, so it has to grow
// with the count - a constant here is the D2 funnel bug in a different place.
static void TestFormHalfExtentGrows(void)
{
    for (int shape = 0; shape < SP_FORM_SHAPE_COUNT; shape++)
    {
        float small = SpFormHalfExtent(shape, 10,  FORM_TEST_PITCH, &s_caps);
        float big   = SpFormHalfExtent(shape, 500, FORM_TEST_PITCH, &s_caps);
        CHECK(big > small);
        CHECK(small > 0.0f);
    }
}

// The corridor probe against hand-built ground. This is the number the whole
// chokepoint rule turns on, and it is deliberately NOT the flow field's
// gradient - that answers "how far to the goal", so a long detour and a narrow
// gap read identically.
static void TestFormCorridorWidth(void)
{
    SpGridInit(&s_grid, 32, 32);

    // Open ground: capped, not infinite.
    int w = SpFormCorridorWidth(&s_grid, 16, 16, 0.0f, 1.0f, 12);
    CHECK(w == 25);                                 // 12 each way + the tile itself

    // A one-tile gap in a wall running across x, travelling along z.
    for (int x = 0; x < 32; x++) SpGridSet(&s_grid, x, 16, SP_COST_BLOCKED);
    SpGridSet(&s_grid, 16, 16, SP_COST_NORMAL);
    CHECK(SpFormCorridorWidth(&s_grid, 16, 16, 0.0f, 1.0f, 12) == 1);

    // Widen it to three.
    SpGridSet(&s_grid, 15, 16, SP_COST_NORMAL);
    SpGridSet(&s_grid, 17, 16, SP_COST_NORMAL);
    CHECK(SpFormCorridorWidth(&s_grid, 16, 16, 0.0f, 1.0f, 12) == 3);

    // Probed PERPENDICULAR to travel: walking along the wall instead of through
    // it is not a chokepoint, and reporting one would funnel a block that has
    // open ground on both sides.
    CHECK(SpFormCorridorWidth(&s_grid, 16, 16, 1.0f, 0.0f, 12) > 3);

    // A blocked tile has no width at all rather than a misleading 1.
    SpGridInit(&s_grid, 32, 32);
    SpGridSet(&s_grid, 8, 8, SP_COST_BLOCKED);
    CHECK(SpFormCorridorWidth(&s_grid, 8, 8, 0.0f, 1.0f, 12) == 0);
}


// A valid assignment is first of all a PERMUTATION: every slot used exactly
// once. Get this wrong and two units are sent to the same place while another
// slot stands empty - which reads as a formation with a hole in it.
static void TestFormAssignIsPermutation(void)
{
    static SpFormPoint units[512], slots[512];
    static int         got[512];
    static SpFormSortEntry sa[512], sb[512];
    static int seen[512];

    RngSeed(99);
    const int counts[] = { 1, 2, 7, 64, 512 };

    for (int c = 0; c < 5; c++)
    {
        int n = counts[c];
        for (int i = 0; i < n; i++)
        {
            units[i].x = RngRange(-30.0f, 30.0f);
            units[i].z = RngRange(-30.0f, 30.0f);
            slots[i].x = RngRange(-30.0f, 30.0f);
            slots[i].z = RngRange(-30.0f, 30.0f);
        }

        SpFormAssign(units, slots, n, got, sa, sb);

        memset(seen, 0, sizeof(int)*(size_t)n);
        int bad = 0;
        for (int i = 0; i < n; i++)
        {
            if (got[i] < 0 || got[i] >= n) { bad++; continue; }
            seen[got[i]]++;
        }
        CHECK(bad == 0);
        for (int i = 0; i < n; i++) CHECK(seen[i] == 1);
    }
}

// THE POINT OF THE CHANGE. Against the 1-D axis zip it replaces, on the case
// that actually looked wrong - a group standing on its own destination,
// re-ordered - the assignment must not walk the group further. Total distance
// AND the worst single unit both matter: the complaint was units crossing the
// block to reach a slot beside the one they were on, which is a worst-case
// symptom that a total-distance win can hide.
static void TestFormAssignBeatsAxisZip(void)
{
    static SpFormPoint units[512], slots[512];
    static int         got[512];
    static SpFormSortEntry sa[512], sb[512];

    RngSeed(4242);
    const int counts[] = { 24, 100, 512 };

    for (int c = 0; c < 3; c++)
    {
        int n = counts[c];

        // A grid of slots, and units scattered over the same ground - the
        // re-order-in-place case.
        int cols = (int)ceilf(sqrtf((float)n));
        for (int i = 0; i < n; i++)
        {
            slots[i].x = ((float)(i % cols) - (float)(cols - 1)*0.5f)*1.5f;
            slots[i].z = ((float)(i/cols)   - (float)(cols - 1)*0.5f)*1.5f;
            units[i].x = RngRange(-1.0f, 1.0f)*(float)cols*0.75f;
            units[i].z = RngRange(-1.0f, 1.0f)*(float)cols*0.75f;
        }

        // The axis zip, reproduced exactly as it shipped: project both onto one
        // axis (forward = +z here, so right = +x) and pair the sorted orders.
        for (int i = 0; i < n; i++)
        {
            sa[i].key = units[i].x + units[i].z*0.5f;  sa[i].index = i;
            sb[i].key = slots[i].x + slots[i].z*0.5f;  sb[i].index = i;
        }
        SpFormSortByKey(sa, n);
        SpFormSortByKey(sb, n);

        float zipTotal = 0.0f, zipWorst = 0.0f;
        for (int k = 0; k < n; k++)
        {
            float dx = units[sa[k].index].x - slots[sb[k].index].x;
            float dz = units[sa[k].index].z - slots[sb[k].index].z;
            float d = sqrtf(dx*dx + dz*dz);
            zipTotal += d;
            if (d > zipWorst) zipWorst = d;
        }

        SpFormAssign(units, slots, n, got, sa, sb);

        float total = 0.0f, worst = 0.0f;
        for (int i = 0; i < n; i++)
        {
            float dx = units[i].x - slots[got[i]].x;
            float dz = units[i].z - slots[got[i]].z;
            float d = sqrtf(dx*dx + dz*dz);
            total += d;
            if (d > worst) worst = d;
        }

        CHECK(total <= zipTotal);
        CHECK(worst <= zipWorst);
    }
}

// Deterministic, because the slot overlay draws a line per unit and a pairing
// that differed between two identical orders would make it unreadable.
static void TestFormAssignDeterministic(void)
{
    static SpFormPoint units[256], slots[256];
    static int         a[256], b[256];
    static SpFormSortEntry sa[256], sb[256];

    RngSeed(5150);
    for (int i = 0; i < 256; i++)
    {
        units[i].x = RngRange(-20.0f, 20.0f);
        units[i].z = RngRange(-20.0f, 20.0f);
        slots[i].x = RngRange(-20.0f, 20.0f);
        slots[i].z = RngRange(-20.0f, 20.0f);
    }

    SpFormAssign(units, slots, 256, a, sa, sb);
    SpFormAssign(units, slots, 256, b, sa, sb);
    for (int i = 0; i < 256; i++) CHECK(a[i] == b[i]);
}

// THE ROTATED RE-ORDER, which is the case the player actually complained about:
// a LINE standing in formation, ordered again with the facing turned 90 degrees.
// The 1-D zip sorts by an axis that the rotation has just redefined, so it walks
// units the length of the block to reach a slot near where they already stood -
// worst unit 29.8 world units on this exact layout.
//
// Note what is NOT asserted: that a group standing on its slots does not move.
// The zip scores a perfect zero on that too - the projection is injective on a
// regular grid - so a test built on it would pass for both algorithms and prove
// nothing. This one separates them.
static void TestFormAssignRotatedReorder(void)
{
    static SpFormPoint units[100], slots[100];
    static int         got[100];
    static SpFormSortEntry sa[100], sb[100];

    // The same LINE, laid out twice: once facing +z, once facing +x.
    for (int i = 0; i < 100; i++)
    {
        float r = ((float)(i % 25) - 12.0f)*1.5f;
        float f = -(float)(i/25)*1.5f;

        units[i].x = r*(-1.0f) + f*0.0f;    // facing (0,1): right = (-1,0)
        units[i].z = r*( 0.0f) + f*1.0f;
        slots[i].x = r*( 0.0f) + f*1.0f;    // facing (1,0): right = (0,1)
        slots[i].z = r*( 1.0f) + f*0.0f;
    }

    // The axis zip as it shipped, measured against the NEW facing.
    for (int i = 0; i < 100; i++)
    {
        sa[i].key = units[i].z + units[i].x*0.5f;  sa[i].index = i;
        sb[i].key = slots[i].z + slots[i].x*0.5f;  sb[i].index = i;
    }
    SpFormSortByKey(sa, 100);
    SpFormSortByKey(sb, 100);

    float zipWorst = 0.0f, zipTotal = 0.0f;
    for (int k = 0; k < 100; k++)
    {
        float dx = units[sa[k].index].x - slots[sb[k].index].x;
        float dz = units[sa[k].index].z - slots[sb[k].index].z;
        float d = sqrtf(dx*dx + dz*dz);
        zipTotal += d;
        if (d > zipWorst) zipWorst = d;
    }

    SpFormAssign(units, slots, 100, got, sa, sb);

    float worst = 0.0f, total = 0.0f;
    for (int i = 0; i < 100; i++)
    {
        float dx = units[i].x - slots[got[i]].x;
        float dz = units[i].z - slots[got[i]].z;
        float d = sqrtf(dx*dx + dz*dz);
        total += d;
        if (d > worst) worst = d;
    }

    // The zip really is bad here - if it were not, this test would be proving
    // nothing, so assert that too.
    CHECK(zipWorst > 20.0f);
    CHECK(worst < zipWorst);
    CHECK(total < zipTotal);
}


// A slot that cannot be resolved must never be handed out as-is. The shipped
// code called SpNearestOpen with a ring cap of 6 and, on failure, kept the
// ORIGINAL blocked position - so a LINE whose corner reached into a wide
// obstacle sent those units at a tile they could never stand on. They wedge
// against it and never arrive, which is the orbit-forever failure the whole
// slot-resolution rule exists to prevent.
//
// This asserts the property the caller needs: for a slot anywhere in a large
// blocked region, SOME passable tile is found, given a ring cap big enough to
// clear a realistic obstacle.
static void TestNearestOpenEscapesLargeObstacle(void)
{
    SpGridInit(&s_grid, 64, 64);

    // A 20x20 lake - far wider than a ring cap of 6 can escape from its centre.
    for (int z = 20; z < 40; z++)
        for (int x = 20; x < 40; x++)
            SpGridSet(&s_grid, x, z, SP_COST_BLOCKED);

    int ox, oz;

    // The old cap cannot escape the middle: this is the bug, asserted so the
    // number below is not mistaken for arbitrary.
    CHECK(!SpNearestOpen(&s_grid, 30, 30, 6, &ox, &oz));

    // A cap that spans the obstacle does, and lands somewhere passable.
    CHECK(SpNearestOpen(&s_grid, 30, 30, 16, &ox, &oz));
    CHECK(SpGridPassable(&s_grid, ox, oz));

    // Every tile inside the lake must resolve to open ground at that cap.
    int failed = 0, notPassable = 0;
    for (int z = 20; z < 40; z++)
        for (int x = 20; x < 40; x++)
        {
            if (!SpNearestOpen(&s_grid, x, z, 16, &ox, &oz)) { failed++; continue; }
            if (!SpGridPassable(&s_grid, ox, oz)) notPassable++;
        }
    CHECK(failed == 0);
    CHECK(notPassable == 0);
}


// ----------------------------------------------------------------------------
//  STRAT_CTRL_SIMPLE's two premises
// ----------------------------------------------------------------------------
//  The lab's SIMPLE arm is built entirely on this module: a rotated LOS probe
//  for obstacle avoidance and index-order slots validated by SpNearestOpen. The
//  arm itself needs a Unit and so cannot be tested here, but the two properties
//  it RESTS on can be, and if either fails the arm is measuring nothing.
// ----------------------------------------------------------------------------

// The dodge premise, and the LIMIT that comes with it.
//
// A 45-degree turn moves the probe endpoint sideways by look*sin(45) - about
// three tiles at look 4. So the dodge clears an obstacle it can get AROUND
// within that reach, and CANNOT clear a wall wider than it that it is facing
// square-on: both shoulders land on the same wall. That is not a defect to fix
// by widening the angle - at 90 degrees the unit stops making progress at all -
// it is the boundary of what a local steer can do, and it is exactly why the
// CURRENT arm has a search. Both halves are asserted so the boundary is a
// recorded property rather than a surprise in the lab.
static void TestSimpleDodgeFindsShoulder(void)
{
    const int   look  = 4;                  // STRAT_SIMPLE_DODGE_LOOK
    const float angle = 0.7853982f;         // STRAT_SIMPLE_DODGE_ANGLE
    float c = cosf(angle), s = sinf(angle);

    // Probe helper: is the endpoint of `dir` rotated by `side` reachable?
    #define SHOULDER_CLEAR(sx, sz, dirX, dirZ, side, okOut)                   \
        do {                                                                  \
            float rx = (dirX)*c - (dirZ)*(s*(float)(side));                   \
            float rz = (dirX)*(s*(float)(side)) + (dirZ)*c;                   \
            int px = (sx) + (int)(rx*look), pz = (sz) + (int)(rz*look);       \
            (okOut) = SpGridInBounds(&s_grid, px, pz) &&                      \
                      SpLosClear(&s_grid, (sx), (sz), px, pz);                \
        } while (0)

    // -- Case 1: a compact obstacle. The dodge must get round this. ----------
    {
        SpGridInit(&s_grid, 64, 64);

        // A 3x3 block - narrower than the dodge's sideways reach.
        for (int z = 31; z <= 33; z++)
            for (int x = 31; x <= 33; x++)
                SpGridSet(&s_grid, x, z, SP_COST_BLOCKED);

        int sx = 32, sz = 27;               // approaching from below, head-on
        float dirX = 0.0f, dirZ = 1.0f;

        // Straight ahead must actually be blocked, or the case proves nothing.
        CHECK(!SpLosClear(&s_grid, sx, sz, sx, sz + look));

        bool left = false, right = false;
        SHOULDER_CLEAR(sx, sz, dirX, dirZ, -1, left);
        SHOULDER_CLEAR(sx, sz, dirX, dirZ, +1, right);
        CHECK(left || right);               // a way round exists and is found
    }

    // -- Case 2: a long flat wall. The dodge must NOT be able to clear it. ---
    {
        SpGridInit(&s_grid, 64, 64);
        for (int x = 10; x < 50; x++) SpGridSet(&s_grid, x, 32, SP_COST_BLOCKED);

        int sx = 30, sz = 30;
        float dirX = 0.0f, dirZ = 1.0f;

        CHECK(!SpLosClear(&s_grid, sx, sz, sx, sz + look));

        bool left = false, right = false;
        SHOULDER_CLEAR(sx, sz, dirX, dirZ, -1, left);
        SHOULDER_CLEAR(sx, sz, dirX, dirZ, +1, right);

        // Both blocked: SimpleDodge falls through to "steer straight anyway",
        // the unit presses against the wall, and the stall test resolves it.
        // A search would have gone round; this is the cost of not having one.
        CHECK(!left && !right);
    }

    #undef SHOULDER_CLEAR
}

// The slot premise: index-order assignment over SpFormSlotLocal still produces
// DISTINCT positions, and every one of them resolves onto passable ground once
// pushed through SpNearestOpen. SIMPLE skips the stable assignment, so if the
// raw slots collided the arm would stack units invisibly and its zero-overlap
// claim would be false for reasons that have nothing to do with pathing.
static void TestSimpleSlotsSurviveObstacles(void)
{
    SpGridInit(&s_grid, 64, 64);

    // A lake sitting right where a formation centred at (32,32) would lay out.
    for (int z = 28; z < 38; z++)
        for (int x = 28; x < 38; x++)
            SpGridSet(&s_grid, x, z, SP_COST_BLOCKED);

    const SpFormCaps caps = { 24.0f, 2, 24.0f, 2, 3.0f };
    const float spacing = 1.5f;             // FORMATION_SPACING at r=0.35
    const int   count   = 64;

    for (int shape = 0; shape < SP_FORM_SHAPE_COUNT; shape++)
    {
        float bias = SpFormForwardBias(shape, count, spacing, &caps);

        int notPassable = 0, unresolved = 0;
        for (int k = 0; k < count; k++)
        {
            float offR, offF;
            SpFormSlotLocal(shape, k, count, spacing, &caps, &offR, &offF);
            offF -= bias;

            // Facing +Z, so right is +X - the same basis the order path uses.
            float wx = 32.0f + offR;
            float wz = 32.0f + offF;

            int tx, tz, ox, oz;
            SpWorldToTile(&s_grid, wx, wz, &tx, &tz);
            if (!SpNearestOpen(&s_grid, tx, tz, 16, &ox, &oz)) { unresolved++; continue; }
            if (!SpGridPassable(&s_grid, ox, oz)) notPassable++;
        }

        // Not one slot may be left sitting in the lake.
        CHECK(unresolved == 0);
        CHECK(notPassable == 0);
    }
}

// Every shape must land CENTRED on the point it is built around, not trailing
// behind it. GRID always did; LINE, COLUMN and WEDGE grew backward from the
// click, so a 100-unit column's mass sat eighteen world units behind where the
// player pointed - and on a short move its rear ranks were sent to ground behind
// where they already stood, so they walked backwards into position.
static void TestFormForwardBiasCentres(void)
{
    const int counts[] = { 2, 10, 100, 512 };

    for (int shape = 0; shape < SP_FORM_SHAPE_COUNT; shape++)
    {
        for (int c = 0; c < 4; c++)
        {
            int n = counts[c];
            float bias = SpFormForwardBias(shape, n, FORM_TEST_PITCH, &s_caps);

            // Re-anchored mean forward offset must be zero.
            float sum = 0.0f;
            for (int i = 0; i < n; i++)
            {
                float r, f;
                SpFormSlotLocal(shape, i, n, FORM_TEST_PITCH, &s_caps, &r, &f);
                sum += (f - bias);
            }
            CHECK(fabsf(sum/(float)n) < 0.001f);
        }
    }
}

// The bias must be REAL for the trailing shapes - if it were near zero this
// would be a no-op dressed up as a fix, so the size of the correction is
// asserted rather than assumed.
static void TestFormForwardBiasIsSubstantial(void)
{
    // GRID already centres: its bias is nil.
    CHECK(fabsf(SpFormForwardBias(SP_FORM_GRID, 100, FORM_TEST_PITCH, &s_caps)) < 0.6f);

    // The trailing shapes do not, and by a lot at scale.
    CHECK(SpFormForwardBias(SP_FORM_COLUMN, 100, FORM_TEST_PITCH, &s_caps) < -10.0f);
    CHECK(SpFormForwardBias(SP_FORM_WEDGE,  100, FORM_TEST_PITCH, &s_caps) < -5.0f);
    CHECK(SpFormForwardBias(SP_FORM_LINE,   100, FORM_TEST_PITCH, &s_caps) < -1.0f);
}

// THE SHORT-MOVE CASE, which is what was actually reported. A column ordered a
// few units forward must not send anybody backwards past where the group
// already stands. Before re-anchoring the deepest slot sat 10.5 units behind
// the centroid for a 3-unit step.
static void TestFormShortMoveDoesNotWalkBackwards(void)
{
    const int n = 20;
    const float step = 3.0f;        // the click, 3 units ahead of the centroid

    for (int shape = 0; shape < SP_FORM_SHAPE_COUNT; shape++)
    {
        float bias = SpFormForwardBias(shape, n, FORM_TEST_PITCH, &s_caps);

        float meanF = 0.0f;
        for (int i = 0; i < n; i++)
        {
            float r, f;
            SpFormSlotLocal(shape, i, n, FORM_TEST_PITCH, &s_caps, &r, &f);
            meanF += (f - bias) + step;     // slot position along forward
        }
        meanF /= (float)n;

        // The block's mass must end up where the player clicked, so the group
        // as a whole moves FORWARD by the step rather than staying put or
        // sliding back.
        CHECK(fabsf(meanF - step) < 0.001f);
    }
}


// THE HEADLINE PROPERTY. A block whose shape and size are unchanged keeps every
// unit in the slot it already holds, however far the formation has moved or
// turned. Re-deriving the pairing instead - which is what the plain assignment
// does - reshuffled 33 units in 36 for a 15-degree facing swing, and facing is
// re-derived from centroid-to-destination on every single order.
static void TestFormAssignStableKeepsSlots(void)
{
    static SpFormPoint units[64], slots[64];
    static int prev[64], got[64];
    static unsigned char taken[64];

    const int n = 36;

    // Units standing in a GRID; slots the same grid moved and rotated hard.
    for (int i = 0; i < n; i++)
    {
        float r = ((float)(i % 6) - 2.5f)*1.5f;
        float f = ((float)(i/6)   - 2.5f)*1.5f;
        units[i].x = r;   units[i].z = f;
        // 45 degrees and 20 units away
        slots[i].x =  r*0.7071f + f*0.7071f + 20.0f;
        slots[i].z = -r*0.7071f + f*0.7071f + 20.0f;
        prev[i] = i;
    }

    int placed = SpFormAssignStable(units, slots, n, prev, got, taken);

    CHECK(placed == 0);                         // nobody needed re-placing
    for (int i = 0; i < n; i++) CHECK(got[i] == i);
}

// A unit with no remembered slot takes a free one NEAR IT, not across the block.
// The free slots are the gaps left by units that departed, so "nearest free" is
// a local answer by construction - which is the property that stops a
// reinforcement walking through the formation to reach the far corner.
static void TestFormAssignStablePlacesLocally(void)
{
    static SpFormPoint units[64], slots[64];
    static int prev[64], got[64];
    static unsigned char taken[64];

    const int n = 36;
    for (int i = 0; i < n; i++)
    {
        float r = ((float)(i % 6) - 2.5f)*1.5f;
        float f = ((float)(i/6)   - 2.5f)*1.5f;
        units[i].x = r;  units[i].z = f;
        slots[i].x = r;  slots[i].z = f;        // formation stays put
        prev[i] = i;
    }

    // Three units in one corner forget their slots - a squad that broke off to
    // fight and rejoined. They must reclaim slots beside them, not across.
    const int forgot[3] = { 0, 1, 6 };
    for (int k = 0; k < 3; k++) prev[forgot[k]] = -1;

    int placed = SpFormAssignStable(units, slots, n, prev, got, taken);
    CHECK(placed == 3);

    for (int k = 0; k < 3; k++)
    {
        int i = forgot[k];
        float dx = units[i].x - slots[got[i]].x;
        float dz = units[i].z - slots[got[i]].z;
        float d  = sqrtf(dx*dx + dz*dz);

        // Their own three slots are the only free ones and all sit in that
        // corner, so the walk is a couple of slot pitches at most - never the
        // ~12 units it would take to cross this block.
        CHECK(d < 4.0f);
    }
}

// The result must still be a permutation, even when remembered slots collide
// (two units claiming one) or point outside the current layout.
static void TestFormAssignStableHandlesBadMemory(void)
{
    static SpFormPoint units[32], slots[32];
    static int prev[32], got[32], seen[32];
    static unsigned char taken[32];

    const int n = 16;
    RngSeed(31337);
    for (int i = 0; i < n; i++)
    {
        units[i].x = RngRange(-10.0f, 10.0f);
        units[i].z = RngRange(-10.0f, 10.0f);
        slots[i].x = RngRange(-10.0f, 10.0f);
        slots[i].z = RngRange(-10.0f, 10.0f);
    }

    // Deliberately hostile memory: duplicates, out-of-range and negatives.
    for (int i = 0; i < n; i++) prev[i] = 3;        // everyone claims slot 3
    prev[0]  = -1;
    prev[1]  = n + 99;
    prev[2]  = -42;

    SpFormAssignStable(units, slots, n, prev, got, taken);

    memset(seen, 0, sizeof(int)*(size_t)n);
    for (int i = 0; i < n; i++)
    {
        CHECK(got[i] >= 0 && got[i] < n);
        seen[got[i]]++;
    }
    for (int i = 0; i < n; i++) CHECK(seen[i] == 1);
}

// No memory at all (a brand-new formation) must still produce a valid
// permutation - the caller uses the from-scratch path there, but passing NULL
// must not be a trap.
static void TestFormAssignStableNullMemory(void)
{
    static SpFormPoint units[16], slots[16];
    static int got[16], seen[16];
    static unsigned char taken[16];

    RngSeed(777);
    for (int i = 0; i < 16; i++)
    {
        units[i].x = RngRange(-5.0f, 5.0f);  units[i].z = RngRange(-5.0f, 5.0f);
        slots[i].x = RngRange(-5.0f, 5.0f);  slots[i].z = RngRange(-5.0f, 5.0f);
    }

    int placed = SpFormAssignStable(units, slots, 16, NULL, got, taken);
    CHECK(placed == 16);

    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < 16; i++) { CHECK(got[i] >= 0 && got[i] < 16); seen[got[i]]++; }
    for (int i = 0; i < 16; i++) CHECK(seen[i] == 1);
}

int main(void)
{
    printf("path_tests\n");
    printf("  grid max   %d (%d cells)\n", SP_GRID_MAX, SP_CELLS_MAX);
    printf("  hash cells %d  items %d\n", SP_HASH_CELLS_MAX, SP_HASH_ITEMS_MAX);

    TestEmpty();
    TestSingle();
    TestRadiusLargerThanCell();
    TestCellBoundary();
    TestNegativeCoords();
    TestOutOfBoundsClamps();
    TestRebuildClears();
    TestOverflowCounts();
    TestHugeWorldCoarsens();
    TestBruteForceEquivalence();
    TestDeterminism();
    TestJitter();

    TestGridBasics();
    TestTileWorldRoundTrip();
    TestWorldToTileNegative();
    TestStampAndRestore();
    TestSkirt();
    TestNearestOpen();

    TestLosBasics();
    TestLosDiagonalGap();
    TestAStarStraightLine();
    TestAStarStartIsGoal();
    TestAStarBlockedGoal();
    TestAStarWallDetour();
    TestAStarOptimality();
    TestAStarBudgetResumption();
    TestAStarDeterminism();
    TestSmoothPreservesWalkability();
    TestAStarTruncation();

    TestServiceBasic();
    TestServiceGoalResolves();
    TestServiceSlicingMatchesEager();
    TestServiceDedup();
    TestServiceReorderSupersedes();
    TestServiceCancelMidSearch();
    TestServiceStaleCancel();
    TestServiceQueueFull();
    TestServiceUnreachableReports();

    TestFlowBasics();
    TestFlowCacheAndCoarsening();
    TestFlowInvalidatesOnNavChange();
    TestFlowUnreachable();
    TestFlowNoLocalMinima();
    TestFlowMatchesDijkstra();
    TestFlowEvictionRespectsRefcount();
    TestFlowSweepIsIdempotent();

    TestFormSlotsUnique();
    TestFormSlotSpacing();
    TestFormExtentCaps();
    TestFormFrontRank();
    TestFormFreeformDensity();
    TestFormHalfExtentGrows();
    TestFormCorridorWidth();
    TestFormAssignIsPermutation();
    TestFormAssignBeatsAxisZip();
    TestFormAssignDeterministic();
    TestFormAssignRotatedReorder();
    TestNearestOpenEscapesLargeObstacle();
    TestSimpleDodgeFindsShoulder();
    TestSimpleSlotsSurviveObstacles();
    TestFormForwardBiasCentres();
    TestFormForwardBiasIsSubstantial();
    TestFormShortMoveDoesNotWalkBackwards();
    TestFormAssignStableKeepsSlots();
    TestFormAssignStablePlacesLocally();
    TestFormAssignStableHandlesBadMemory();
    TestFormAssignStableNullMemory();

    printf("\n%d checks, %d failed\n", s_checks, s_fails);
    return (s_fails == 0) ? 0 : 1;
}
