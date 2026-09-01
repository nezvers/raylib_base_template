// ============================================================================
//  map_tests.c  -  headless checks for authored maps (.sgm)
//
//  Covers the parts that are expensive to get wrong and invisible when they do:
//  the BINARY FORMAT (a map that loads cropped looks like an authoring mistake,
//  not an IO bug), the TILE<->WORLD mapping (an off-by-one there puts every
//  building half a tile out and nothing says so), and the VALIDATION RULES,
//  which exist precisely to catch maps that would misbehave only once played.
//
//  Headless: no window, no GL. src/strategy_map/ never calls raylib UI, which
//  is the whole reason it is a separate module from the forge.
//
//  simple_save.h is header-only and its IMPLEMENTATION normally lives in
//  examples/simple_save_example.c, which this binary does not link - so the
//  suite compiles it here instead, the same way sga_tests.c does.
// ============================================================================

#define SIMPLE_SAVE_IMPLEMENTATION
#include "simple_save.h"

#include "../src/strategy_map/strategy_map.h"
#include "../src/strategy_map/strategy_map_io.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

static int s_checks = 0, s_fails = 0;

#define CHECK(cond) do { \
    s_checks++; \
    if (!(cond)) { s_fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_NEAR(a, b) CHECK(fabsf((a) - (b)) < 0.0001f)

// Every test writes here so a crashed run leaves nothing behind in the real
// map directory.
#define TEST_NAME  "_sgmtest"

// The game's real capacities, as the forge will pass them.
static const SgmBudget BUDGET = { .buildings = 24, .units = 96, .nodes = 48 };
#define TOWN_HALL  5        // BLD_TOWN_HALL in strategy_types.h

// Maps are large; keep them out of the stack in the tests too.
static SgmMap s_a, s_b;

// ---------------------------------------------------------------------------
//  Init / resize
// ---------------------------------------------------------------------------
static void TestInit(void)
{
    SgmMapInit(&s_a, "hello", 32, 24);

    CHECK(strcmp(s_a.name, "hello") == 0);
    CHECK(s_a.gridW == 32);
    CHECK(s_a.gridH == 24);
    CHECK(s_a.factionCount == 1);
    CHECK(s_a.placeCount == 0);

    // A blank map must be WALKABLE. A zeroed flag byte would read as
    // "impassable ground", which is the bug this check exists for.
    CHECK(SgmTilePassable(&s_a, 0, 0));
    CHECK(SgmTileBuildable(&s_a, 0, 0));
    CHECK(SgmTilePassable(&s_a, 31, 23));

    // Out of bounds is blocked, never a read past the array.
    CHECK(!SgmTilePassable(&s_a, 32, 0));
    CHECK(!SgmTilePassable(&s_a, -1, 0));
    CHECK(SgmTileAt(&s_a, 32, 0) == NULL);

    // Clamped, not accepted verbatim.
    SgmMapInit(&s_a, "tiny", 1, 1);
    CHECK(s_a.gridW >= 8);
    SgmMapInit(&s_a, "huge", 9999, 9999);
    CHECK(s_a.gridW == SGM_GRID_MAX);
}

static void TestResize(void)
{
    SgmMapInit(&s_a, "r", 16, 16);

    // Mark a tile whose position must survive a stride change.
    SgmPaintTerrain(&s_a, 3, 5, SGM_TERRAIN_ROCK);
    SgmTileAt(&s_a, 3, 5)->height = 4;

    SgmMapResize(&s_a, 32, 32);      // grow: stride changes 16 -> 32
    CHECK(s_a.gridW == 32);
    CHECK(SgmTileAtConst(&s_a, 3, 5)->terrain == SGM_TERRAIN_ROCK);
    CHECK(SgmTileAtConst(&s_a, 3, 5)->height == 4);
    // Newly exposed ground must be walkable, not zeroed-to-blocked.
    CHECK(SgmTilePassable(&s_a, 20, 20));

    SgmMapResize(&s_a, 16, 16);      // shrink back
    CHECK(SgmTileAtConst(&s_a, 3, 5)->terrain == SGM_TERRAIN_ROCK);

    // A placement outside the new extent is dropped; a start is clamped inward.
    SgmMapResize(&s_a, 32, 32);
    SgmPlaceAdd(&s_a, SGM_PLACE_NODE, 0, SGM_FACTION_NEUTRAL, 30, 30);
    s_a.starts[0].tileX = 30;
    s_a.starts[0].tileZ = 30;
    CHECK(s_a.placeCount == 1);

    SgmMapResize(&s_a, 16, 16);
    CHECK(s_a.placeCount == 0);              // dropped
    CHECK(s_a.starts[0].tileX == 15);        // clamped
    CHECK(s_a.starts[0].tileZ == 15);
}

// ---------------------------------------------------------------------------
//  Terrain and passability
// ---------------------------------------------------------------------------
static void TestTerrain(void)
{
    SgmMapInit(&s_a, "t", 16, 16);

    SgmPaintTerrain(&s_a, 2, 2, SGM_TERRAIN_WATER);
    CHECK(!SgmTilePassable(&s_a, 2, 2));
    CHECK(!SgmTileBuildable(&s_a, 2, 2));

    // Shallow water is the tile that makes fords authorable: walkable, but
    // nothing may be built in it.
    SgmPaintTerrain(&s_a, 3, 3, SGM_TERRAIN_SHALLOW);
    CHECK(SgmTilePassable(&s_a, 3, 3));
    CHECK(!SgmTileBuildable(&s_a, 3, 3));

    CHECK(SgmTerrainBlocks(SGM_TERRAIN_CLIFF));
    CHECK(SgmTerrainBlocks(SGM_TERRAIN_VOID));
    CHECK(SgmTerrainBlocks(SGM_TERRAIN_ROCK));
    CHECK(!SgmTerrainBlocks(SGM_TERRAIN_GROUND));
    CHECK(!SgmTerrainBlocks(SGM_TERRAIN_SHALLOW));

    // Painting RESETS flags to the new terrain's defaults, so a tile carved
    // passable and then repainted as water is water again.
    SgmTileAt(&s_a, 2, 2)->flags = SGM_TILE_PASSABLE;
    CHECK(SgmTilePassable(&s_a, 2, 2));
    SgmPaintTerrain(&s_a, 2, 2, SGM_TERRAIN_WATER);
    CHECK(!SgmTilePassable(&s_a, 2, 2));

    // The author may still override afterwards - that is the point of the
    // separate flags field.
    SgmTileAt(&s_a, 2, 2)->flags |= SGM_TILE_PASSABLE;
    CHECK(SgmTilePassable(&s_a, 2, 2));
    CHECK(SgmTileAtConst(&s_a, 2, 2)->terrain == SGM_TERRAIN_WATER);
}

// ---------------------------------------------------------------------------
//  Tile <-> world
// ---------------------------------------------------------------------------
static void TestCoords(void)
{
    SgmMapInit(&s_a, "c", 50, 50);

    // The grid is centred on the origin: a 50-wide map spans [-25, +25], which
    // is exactly STRAT_GROUND_HALF.
    Vector3 lo = SgmTileToWorld(&s_a, 0, 0);
    CHECK_NEAR(lo.x, -24.5f);
    CHECK_NEAR(lo.z, -24.5f);
    CHECK_NEAR(lo.y, 0.0f);

    Vector3 hi = SgmTileToWorld(&s_a, 49, 49);
    CHECK_NEAR(hi.x, 24.5f);
    CHECK_NEAR(hi.z, 24.5f);

    // Round-trip every tile. This is the check that catches a truncating cast
    // in SgmWorldToTile - it only misbehaves left of the origin.
    for (int z = 0; z < 50; z += 7)
    {
        for (int x = 0; x < 50; x += 7)
        {
            Vector3 w = SgmTileToWorld(&s_a, x, z);
            int rx = -1, rz = -1;
            SgmWorldToTile(&s_a, w.x, w.z, &rx, &rz);
            CHECK(rx == x);
            CHECK(rz == z);
        }
    }

    // A point anywhere inside a tile lands on that tile, not just its centre.
    int tx = -1, tz = -1;
    SgmWorldToTile(&s_a, -24.9f, -24.1f, &tx, &tz);
    CHECK(tx == 0);
    CHECK(tz == 0);
}

// ---------------------------------------------------------------------------
//  Placements
// ---------------------------------------------------------------------------
static void TestPlacements(void)
{
    SgmMapInit(&s_a, "p", 16, 16);

    int i0 = SgmPlaceAdd(&s_a, SGM_PLACE_BUILDING, TOWN_HALL, 0, 4, 4);
    int i1 = SgmPlaceAdd(&s_a, SGM_PLACE_UNIT, 0, 0, 5, 5);
    int i2 = SgmPlaceAdd(&s_a, SGM_PLACE_NODE, 0, SGM_FACTION_NEUTRAL, 6, 6);
    CHECK(i0 == 0);
    CHECK(i1 == 1);
    CHECK(i2 == 2);
    CHECK(s_a.placeCount == 3);

    CHECK(SgmPlaceCountOf(&s_a, SGM_PLACE_BUILDING) == 1);
    CHECK(SgmPlaceCountOf(&s_a, SGM_PLACE_UNIT) == 1);
    CHECK(SgmPlaceCountOf(&s_a, SGM_PLACE_NODE) == 1);

    CHECK(SgmPlaceAt(&s_a, 5, 5) == 1);
    CHECK(SgmPlaceAt(&s_a, 9, 9) == -1);

    // Off-grid is refused rather than clamped - a clamp would stack everything
    // silently on an edge.
    CHECK(SgmPlaceAdd(&s_a, SGM_PLACE_UNIT, 0, 0, 99, 99) == -1);
    CHECK(s_a.placeCount == 3);

    // Removal preserves ORDER, because "the town hall is first" is a real rule.
    SgmPlaceRemove(&s_a, 0);
    CHECK(s_a.placeCount == 2);
    CHECK(s_a.places[0].family == SGM_PLACE_UNIT);
    CHECK(s_a.places[1].family == SGM_PLACE_NODE);

    // A stacked tile returns the most recent.
    SgmMapInit(&s_a, "p2", 16, 16);
    SgmPlaceAdd(&s_a, SGM_PLACE_NODE, 0, SGM_FACTION_NEUTRAL, 7, 7);
    int top = SgmPlaceAdd(&s_a, SGM_PLACE_UNIT, 0, 0, 7, 7);
    CHECK(SgmPlaceAt(&s_a, 7, 7) == top);
}

// ---------------------------------------------------------------------------
//  Validation
// ---------------------------------------------------------------------------
// A minimal map that PASSES, so each test below can break exactly one rule and
// attribute the failure to it.
static void BuildValid(SgmMap *m, int factions)
{
    SgmMapInit(m, "v", 32, 32);
    m->factionCount = factions;

    for (int f = 0; f < factions; f++)
    {
        m->starts[f].tileX = (int16_t)(2 + f*3);
        m->starts[f].tileZ = (int16_t)2;
        // Town hall FIRST for each faction - the rule the AI depends on.
        SgmPlaceAdd(m, SGM_PLACE_BUILDING, TOWN_HALL, f, 2 + f*3, 2);
        SgmPlaceAdd(m, SGM_PLACE_BUILDING, 0, f, 2 + f*3, 4);
    }
}

static SgmReport s_rep;

static bool HasCode(const SgmReport *r, int code)
{
    for (int i = 0; i < r->count; i++) if (r->items[i].code == code) return true;
    return false;
}

static void TestValidateBasics(void)
{
    BuildValid(&s_a, 2);
    SgmValidate(&s_a, BUDGET, TOWN_HALL, &s_rep);
    CHECK(s_rep.errors == 0);
    CHECK(SgmPlayable(&s_a, BUDGET, TOWN_HALL));

    // The whole authored range must pass.
    for (int n = 1; n <= SGM_FACTIONS_MAX; n++)
    {
        BuildValid(&s_a, n);
        CHECK(SgmPlayable(&s_a, BUDGET, TOWN_HALL));
    }

    // ...and only that range.
    BuildValid(&s_a, 2);
    s_a.factionCount = 0;
    SgmValidate(&s_a, BUDGET, TOWN_HALL, &s_rep);
    CHECK(HasCode(&s_rep, SGM_ERR_FACTION_COUNT));
    CHECK(!SgmPlayable(&s_a, BUDGET, TOWN_HALL));

    s_a.factionCount = SGM_FACTIONS_MAX + 1;
    SgmValidate(&s_a, BUDGET, TOWN_HALL, &s_rep);
    CHECK(HasCode(&s_rep, SGM_ERR_FACTION_COUNT));
}

static void TestValidateStarts(void)
{
    // A start on impassable ground.
    BuildValid(&s_a, 2);
    SgmPaintTerrain(&s_a, s_a.starts[1].tileX, s_a.starts[1].tileZ, SGM_TERRAIN_WATER);
    SgmValidate(&s_a, BUDGET, TOWN_HALL, &s_rep);
    CHECK(HasCode(&s_rep, SGM_ERR_START_BLOCKED));
    CHECK(!SgmPlayable(&s_a, BUDGET, TOWN_HALL));

    // Two starts on one tile.
    BuildValid(&s_a, 2);
    s_a.starts[1] = s_a.starts[0];
    SgmValidate(&s_a, BUDGET, TOWN_HALL, &s_rep);
    CHECK(HasCode(&s_rep, SGM_ERR_START_OVERLAP));

    // A start off the grid.
    BuildValid(&s_a, 2);
    s_a.starts[1].tileX = 999;
    SgmValidate(&s_a, BUDGET, TOWN_HALL, &s_rep);
    CHECK(HasCode(&s_rep, SGM_ERR_START_OOB));
}

static void TestValidateTownHall(void)
{
    // A faction with buildings but no town hall cannot play.
    SgmMapInit(&s_a, "th", 32, 32);
    s_a.factionCount = 1;
    s_a.starts[0].tileX = 2; s_a.starts[0].tileZ = 2;
    SgmPlaceAdd(&s_a, SGM_PLACE_BUILDING, 0, 0, 2, 2);      // a house, not a hall
    SgmValidate(&s_a, BUDGET, TOWN_HALL, &s_rep);
    CHECK(HasCode(&s_rep, SGM_ERR_NO_TOWN_HALL));
    CHECK(s_rep.errors > 0);

    // A town hall that is not the faction's FIRST building. This is the rule
    // that is invisible until you watch the AI walk to the wrong corner.
    SgmMapInit(&s_a, "th2", 32, 32);
    s_a.factionCount = 1;
    s_a.starts[0].tileX = 2; s_a.starts[0].tileZ = 2;
    SgmPlaceAdd(&s_a, SGM_PLACE_BUILDING, 0, 0, 2, 4);          // house first
    SgmPlaceAdd(&s_a, SGM_PLACE_BUILDING, TOWN_HALL, 0, 2, 2);  // hall second
    SgmValidate(&s_a, BUDGET, TOWN_HALL, &s_rep);
    CHECK(HasCode(&s_rep, SGM_ERR_TOWN_HALL_NOT_FIRST));
    CHECK(!SgmPlayable(&s_a, BUDGET, TOWN_HALL));

    // An empty map is only a WARNING: a sandbox with nothing placed yet is a
    // legitimate work in progress, not a broken document.
    SgmMapInit(&s_a, "th3", 32, 32);
    SgmValidate(&s_a, BUDGET, TOWN_HALL, &s_rep);
    CHECK(s_rep.errors == 0);
    CHECK(s_rep.count > 0);
    CHECK(SgmPlayable(&s_a, BUDGET, TOWN_HALL));
}

static void TestValidateBudget(void)
{
    // The node cap is the one authors actually hit - the shipped hardcoded map
    // already places 32 of the 48.
    SgmMapInit(&s_a, "b", 64, 64);
    s_a.factionCount = 1;
    s_a.starts[0].tileX = 1; s_a.starts[0].tileZ = 1;
    SgmPlaceAdd(&s_a, SGM_PLACE_BUILDING, TOWN_HALL, 0, 1, 1);

    for (int i = 0; i < BUDGET.nodes; i++)
        SgmPlaceAdd(&s_a, SGM_PLACE_NODE, 0, SGM_FACTION_NEUTRAL, 10 + (i % 40), 10 + (i/40));
    SgmValidate(&s_a, BUDGET, TOWN_HALL, &s_rep);
    CHECK(!HasCode(&s_rep, SGM_ERR_OVER_BUDGET));       // exactly at the cap is fine

    SgmPlaceAdd(&s_a, SGM_PLACE_NODE, 0, SGM_FACTION_NEUTRAL, 55, 55);
    SgmValidate(&s_a, BUDGET, TOWN_HALL, &s_rep);
    CHECK(HasCode(&s_rep, SGM_ERR_OVER_BUDGET));
    CHECK(!SgmPlayable(&s_a, BUDGET, TOWN_HALL));
}

static void TestValidateBlockedPlacement(void)
{
    BuildValid(&s_a, 1);
    SgmPlaceAdd(&s_a, SGM_PLACE_NODE, 0, SGM_FACTION_NEUTRAL, 10, 10);
    SgmPaintTerrain(&s_a, 10, 10, SGM_TERRAIN_CLIFF);
    SgmValidate(&s_a, BUDGET, TOWN_HALL, &s_rep);
    CHECK(HasCode(&s_rep, SGM_ERR_PLACE_BLOCKED));

    // A UNIT on walkable-but-unbuildable ground is fine; a BUILDING is not.
    BuildValid(&s_a, 1);
    SgmPlaceAdd(&s_a, SGM_PLACE_UNIT, 0, 0, 11, 11);
    SgmPaintTerrain(&s_a, 11, 11, SGM_TERRAIN_SHALLOW);
    SgmValidate(&s_a, BUDGET, TOWN_HALL, &s_rep);
    CHECK(!HasCode(&s_rep, SGM_ERR_PLACE_BLOCKED));

    BuildValid(&s_a, 1);
    SgmPlaceAdd(&s_a, SGM_PLACE_BUILDING, 0, 0, 12, 12);
    SgmPaintTerrain(&s_a, 12, 12, SGM_TERRAIN_SHALLOW);
    SgmValidate(&s_a, BUDGET, TOWN_HALL, &s_rep);
    CHECK(HasCode(&s_rep, SGM_ERR_PLACE_BLOCKED));
}

// ---------------------------------------------------------------------------
//  Round trip
// ---------------------------------------------------------------------------
static void TestRoundTrip(void)
{
    // Every field set to something that is NOT its default, so a field the
    // writer forgets shows up as a mismatch rather than passing by coincidence.
    SgmMapInit(&s_a, "rt", 40, 28);
    TextCopy(s_a.desc, "a description that must survive");
    s_a.factionCount = 5;
    s_a.gridStyle = SGM_GRID_STRONG;

    for (int f = 0; f < 5; f++)
    {
        s_a.starts[f].tileX = (int16_t)(3 + f*4);
        s_a.starts[f].tileZ = (int16_t)(6 + f);
        s_a.starts[f].colorIndex = (uint8_t)(8 - f);
    }

    for (int z = 0; z < 28; z++)
    {
        for (int x = 0; x < 40; x++)
        {
            SgmTile *t = SgmTileAt(&s_a, x, z);
            t->terrain = (uint8_t)((x + z) % SGM_TERRAIN_COUNT);
            t->height  = (uint8_t)((x*z) % (SGM_HEIGHT_MAX + 1));
            t->flags   = (uint8_t)(((x + z) % 2) ? SGM_TILE_PASSABLE
                                                 : (SGM_TILE_PASSABLE | SGM_TILE_BUILDABLE));
            t->variant = (uint8_t)(x % 7);
        }
    }

    for (int i = 0; i < 20; i++)
        SgmPlaceAdd(&s_a, i % SGM_PLACE_COUNT, i + 1, i % 5, i, i);

    CHECK(SgmMapSaveNamed(&s_a, TEST_NAME));
    CHECK(SgmMapLoad(&s_b, SgmMapPath(TEST_NAME)));
    CHECK(!SgmMapLoadTruncated());

    CHECK(s_b.gridW == s_a.gridW);
    CHECK(s_b.gridH == s_a.gridH);
    CHECK(s_b.factionCount == s_a.factionCount);
    CHECK(s_b.gridStyle == s_a.gridStyle);
    CHECK(s_b.placeCount == s_a.placeCount);
    CHECK(strcmp(s_b.desc, s_a.desc) == 0);

    for (int f = 0; f < 5; f++)
    {
        CHECK(s_b.starts[f].tileX == s_a.starts[f].tileX);
        CHECK(s_b.starts[f].tileZ == s_a.starts[f].tileZ);
        CHECK(s_b.starts[f].colorIndex == s_a.starts[f].colorIndex);
    }

    // Every tile, not a sample: a stride bug in the writer shows up as a
    // diagonal smear that a sparse check can miss.
    int mismatches = 0;
    for (int z = 0; z < 28; z++)
    {
        for (int x = 0; x < 40; x++)
        {
            const SgmTile *ta = SgmTileAtConst(&s_a, x, z);
            const SgmTile *tb = SgmTileAtConst(&s_b, x, z);
            if ((ta->terrain != tb->terrain) || (ta->height != tb->height) ||
                (ta->flags != tb->flags) || (ta->variant != tb->variant)) mismatches++;
        }
    }
    CHECK(mismatches == 0);

    for (int i = 0; i < s_a.placeCount; i++)
    {
        CHECK(s_b.places[i].tileX   == s_a.places[i].tileX);
        CHECK(s_b.places[i].tileZ   == s_a.places[i].tileZ);
        CHECK(s_b.places[i].family  == s_a.places[i].family);
        CHECK(s_b.places[i].kind    == s_a.places[i].kind);
        CHECK(s_b.places[i].faction == s_a.places[i].faction);
    }

    // The FILE is the identity, so the loaded name comes from the filename.
    CHECK(SgmMapLoadAll(&s_b, 1) >= 1);

    CHECK(SgmMapDelete(TEST_NAME));
    CHECK(SgmMapNameFree(TEST_NAME));
}

static void TestVersionRejected(void)
{
    SgmMapInit(&s_a, "ver", 16, 16);
    CHECK(SgmMapSaveNamed(&s_a, TEST_NAME));

    // Corrupt FIELD ZERO. The format has no magic header, so the version is the
    // only thing standing between a stale file and a garbage document.
    const char *path = SgmMapPath(TEST_NAME);
    FILE *f = fopen(path, "r+b");
    CHECK(f != NULL);
    if (f != NULL)
    {
        int32_t bogus = 999;
        fwrite(&bogus, sizeof(bogus), 1, f);
        fclose(f);
    }

    CHECK(!SgmMapLoad(&s_b, path));     // refused, not misread
    SgmMapDelete(TEST_NAME);
}

static void TestClampsHostileFile(void)
{
    // A file on disk is untrusted input even when this program wrote it: every
    // field must come back inside its legal range.
    SgmMapInit(&s_a, "clamp", 16, 16);
    s_a.factionCount = 3;
    SgmTileAt(&s_a, 1, 1)->terrain = 200;       // past SGM_TERRAIN_COUNT
    SgmTileAt(&s_a, 1, 1)->height  = 200;       // past SGM_HEIGHT_MAX
    SgmTileAt(&s_a, 1, 1)->flags   = 0xFF;      // undefined bits set

    CHECK(SgmMapSaveNamed(&s_a, TEST_NAME));
    CHECK(SgmMapLoad(&s_b, SgmMapPath(TEST_NAME)));

    const SgmTile *t = SgmTileAtConst(&s_b, 1, 1);
    CHECK(t->terrain < SGM_TERRAIN_COUNT);
    CHECK(t->height <= SGM_HEIGHT_MAX);
    CHECK((t->flags & ~(SGM_TILE_PASSABLE | SGM_TILE_BUILDABLE)) == 0);

    SgmMapDelete(TEST_NAME);
}

static void TestMissingFile(void)
{
    CHECK(!SgmMapLoad(&s_b, SgmMapPath("_definitely_not_here")));
    CHECK(!SgmMapDelete("_definitely_not_here"));
    CHECK(SgmMapNameFree("_definitely_not_here"));
}

int main(void)
{
    TestInit();
    TestResize();
    TestTerrain();
    TestCoords();
    TestPlacements();
    TestValidateBasics();
    TestValidateStarts();
    TestValidateTownHall();
    TestValidateBudget();
    TestValidateBlockedPlacement();
    TestRoundTrip();
    TestVersionRejected();
    TestClampsHostileFile();
    TestMissingFile();

    printf("map_tests: %d checks, %d failed\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
