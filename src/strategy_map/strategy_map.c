// ============================================================================
//  strategy_map.c  -  the map data model (see strategy_map.h)
//
//  UI-FREE ON PURPOSE. Nothing here draws or reads the mouse, so this file and
//  strategy_map_io.c link into the headless map_tests binary. The validation
//  rules in particular are the kind of thing that is worth testing and painful
//  to test through a GUI.
// ============================================================================

#include "strategy_map.h"
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------------------
//  Lifecycle
// ---------------------------------------------------------------------------
static int ClampInt(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// The smallest map worth authoring. Below this the centring maths is fine but
// there is no room for two bases and anything between them.
#define SGM_GRID_MIN  8

void SgmMapInit(SgmMap *m, const char *name, int w, int h)
{
    if (m == NULL) return;

    memset(m, 0, sizeof(*m));

    if ((name != NULL) && name[0]) TextCopy(m->name, name);
    else                           TextCopy(m->name, "untitled");

    m->gridW = ClampInt(w, SGM_GRID_MIN, SGM_GRID_MAX);
    m->gridH = ClampInt(h, SGM_GRID_MIN, SGM_GRID_MAX);

    // memset already made every tile GROUND/height 0/variant 0, but the flags
    // must match that terrain rather than being zero - a zeroed flag byte would
    // mean "impassable ground", which is not what a blank map is.
    uint8_t ground = SgmTerrainDefaultFlags(SGM_TERRAIN_GROUND);
    for (int i = 0; i < SGM_TILES_MAX; i++) m->tiles[i].flags = ground;

    // One faction, started near the middle. A one-faction map is legal (a
    // sandbox), so this is a complete, valid document as it stands.
    m->factionCount = 1;
    m->starts[0].tileX = (int16_t)(m->gridW/2);
    m->starts[0].tileZ = (int16_t)(m->gridH/2);
    for (int f = 0; f < SGM_FACTIONS_MAX; f++) m->starts[f].colorIndex = (uint8_t)f;

    m->gridStyle = SGM_GRID_SUBTLE;
}

void SgmMapResize(SgmMap *m, int w, int h)
{
    if (m == NULL) return;

    int nw = ClampInt(w, SGM_GRID_MIN, SGM_GRID_MAX);
    int nh = ClampInt(h, SGM_GRID_MIN, SGM_GRID_MAX);
    if ((nw == m->gridW) && (nh == m->gridH)) return;

    // Rebuild into a scratch grid rather than shuffling in place: the row
    // stride changes, so an in-place move would overwrite rows it still needs
    // to read whenever the map grows.
    static SgmTile scratch[SGM_TILES_MAX];      // static: 256 KB is not stack data
    uint8_t ground = SgmTerrainDefaultFlags(SGM_TERRAIN_GROUND);
    for (int i = 0; i < SGM_TILES_MAX; i++)
    {
        scratch[i] = (SgmTile){ 0 };
        scratch[i].flags = ground;
    }

    int copyW = (nw < m->gridW) ? nw : m->gridW;
    int copyH = (nh < m->gridH) ? nh : m->gridH;
    for (int z = 0; z < copyH; z++)
    {
        for (int x = 0; x < copyW; x++) scratch[z*nw + x] = m->tiles[z*m->gridW + x];
    }
    memcpy(m->tiles, scratch, sizeof(m->tiles));

    m->gridW = nw;
    m->gridH = nh;

    // Starts are CLAMPED inward - a faction losing its start entirely would
    // make the map invalid in a way the author cannot see from the grid.
    for (int f = 0; f < SGM_FACTIONS_MAX; f++)
    {
        m->starts[f].tileX = (int16_t)ClampInt(m->starts[f].tileX, 0, nw - 1);
        m->starts[f].tileZ = (int16_t)ClampInt(m->starts[f].tileZ, 0, nh - 1);
    }

    // Placements outside the new extent are DROPPED - unlike a start, there is
    // no sensible place to clamp a building to, and silently stacking them all
    // on the new edge would be worse than losing them.
    for (int i = m->placeCount - 1; i >= 0; i--)
    {
        if (!SgmInBounds(m, m->places[i].tileX, m->places[i].tileZ))
            SgmPlaceRemove(m, i);
    }
}

// ---------------------------------------------------------------------------
//  Tiles
// ---------------------------------------------------------------------------
bool SgmInBounds(const SgmMap *m, int x, int z)
{
    if (m == NULL) return false;
    return (x >= 0) && (z >= 0) && (x < m->gridW) && (z < m->gridH);
}

SgmTile *SgmTileAt(SgmMap *m, int x, int z)
{
    if (!SgmInBounds(m, x, z)) return NULL;
    return &m->tiles[z*m->gridW + x];
}

const SgmTile *SgmTileAtConst(const SgmMap *m, int x, int z)
{
    if (!SgmInBounds(m, x, z)) return NULL;
    return &m->tiles[z*m->gridW + x];
}

uint8_t SgmTerrainDefaultFlags(int terrain)
{
    switch (terrain)
    {
        case SGM_TERRAIN_GROUND:
        case SGM_TERRAIN_GRASS:
        case SGM_TERRAIN_DIRT:
            return SGM_TILE_PASSABLE | SGM_TILE_BUILDABLE;

        // Walkable, but nothing may be built in it. This is the tile that makes
        // fords and shorelines authorable.
        case SGM_TERRAIN_SHALLOW:
            return SGM_TILE_PASSABLE;

        case SGM_TERRAIN_WATER:
        case SGM_TERRAIN_ROCK:
        case SGM_TERRAIN_CLIFF:
        case SGM_TERRAIN_VOID:
        default:
            return 0;
    }
}

bool SgmTerrainBlocks(int terrain)
{
    return (SgmTerrainDefaultFlags(terrain) & SGM_TILE_PASSABLE) == 0;
}

bool SgmTilePassable(const SgmMap *m, int x, int z)
{
    const SgmTile *t = SgmTileAtConst(m, x, z);
    if (t == NULL) return false;        // off-grid is blocked
    return (t->flags & SGM_TILE_PASSABLE) != 0;
}

bool SgmTileBuildable(const SgmMap *m, int x, int z)
{
    const SgmTile *t = SgmTileAtConst(m, x, z);
    if (t == NULL) return false;
    return (t->flags & SGM_TILE_BUILDABLE) != 0;
}

void SgmPaintTerrain(SgmMap *m, int x, int z, int terrain)
{
    SgmTile *t = SgmTileAt(m, x, z);
    if (t == NULL) return;
    if ((terrain < 0) || (terrain >= SGM_TERRAIN_COUNT)) return;

    t->terrain = (uint8_t)terrain;
    t->flags   = SgmTerrainDefaultFlags(terrain);
}

// ---------------------------------------------------------------------------
//  Tile <-> world
// ---------------------------------------------------------------------------
Vector3 SgmTileToWorld(const SgmMap *m, int x, int z)
{
    if (m == NULL) return (Vector3){ 0.0f, 0.0f, 0.0f };
    return (Vector3){
        (float)x - (float)m->gridW*0.5f + 0.5f,
        0.0f,
        (float)z - (float)m->gridH*0.5f + 0.5f,
    };
}

void SgmWorldToTile(const SgmMap *m, float wx, float wz, int *outX, int *outZ)
{
    if (m == NULL) return;
    // floorf, not a cast: a cast truncates toward zero, so every tile left of
    // the origin would be off by one.
    int x = (int)floorf(wx + (float)m->gridW*0.5f);
    int z = (int)floorf(wz + (float)m->gridH*0.5f);
    if (outX != NULL) *outX = x;
    if (outZ != NULL) *outZ = z;
}

// ---------------------------------------------------------------------------
//  Placements
// ---------------------------------------------------------------------------
int SgmPlaceAdd(SgmMap *m, int family, int kind, int faction, int x, int z)
{
    if (m == NULL) return -1;
    if (m->placeCount >= SGM_PLACES_MAX) return -1;
    if (!SgmInBounds(m, x, z)) return -1;
    if ((family < 0) || (family >= SGM_PLACE_COUNT)) return -1;

    SgmPlacement *p = &m->places[m->placeCount];
    p->tileX   = (int16_t)x;
    p->tileZ   = (int16_t)z;
    p->family  = (uint8_t)family;
    p->kind    = (uint8_t)kind;
    p->faction = (uint8_t)faction;
    p->amount  = 0;

    return m->placeCount++;
}

void SgmPlaceRemove(SgmMap *m, int index)
{
    if (m == NULL) return;
    if ((index < 0) || (index >= m->placeCount)) return;

    // Order-preserving shift, not a swap-with-last: "the town hall is first"
    // is a real rule (see SgmValidate), so the order carries meaning.
    for (int i = index; i < m->placeCount - 1; i++) m->places[i] = m->places[i + 1];
    m->placeCount--;
    m->places[m->placeCount] = (SgmPlacement){ 0 };
}

int SgmPlaceAt(const SgmMap *m, int x, int z)
{
    if (m == NULL) return -1;
    // Backwards: the most recently added wins, which is what clicking a stack
    // expects.
    for (int i = m->placeCount - 1; i >= 0; i--)
    {
        if ((m->places[i].tileX == x) && (m->places[i].tileZ == z)) return i;
    }
    return -1;
}

int SgmPlaceCountOf(const SgmMap *m, int family)
{
    if (m == NULL) return 0;
    int n = 0;
    for (int i = 0; i < m->placeCount; i++) if (m->places[i].family == family) n++;
    return n;
}

// ---------------------------------------------------------------------------
//  Validation
// ---------------------------------------------------------------------------
static void Report(SgmReport *r, int code, bool warning, int faction, int place)
{
    if (r == NULL) return;
    if (!warning) r->errors++;
    if (r->count >= SGM_ISSUES_MAX) return;     // still counted, just not listed

    r->items[r->count].code    = code;
    r->items[r->count].warning = warning;
    r->items[r->count].faction = faction;
    r->items[r->count].place   = place;
    r->count++;
}

void SgmValidate(const SgmMap *m, SgmBudget budget, int townHallKind,
                 SgmReport *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (m == NULL) return;

    // -- faction count ------------------------------------------------------
    if ((m->factionCount < 1) || (m->factionCount > SGM_FACTIONS_MAX))
    {
        Report(out, SGM_ERR_FACTION_COUNT, false, -1, -1);
        return;     // every rule below indexes starts[] by it
    }

    // -- starts -------------------------------------------------------------
    for (int f = 0; f < m->factionCount; f++)
    {
        int sx = m->starts[f].tileX, sz = m->starts[f].tileZ;

        if (!SgmInBounds(m, sx, sz))
        {
            Report(out, SGM_ERR_START_OOB, false, f, -1);
            continue;       // the two rules below would read out of bounds
        }
        if (!SgmTilePassable(m, sx, sz) || !SgmTileBuildable(m, sx, sz))
            Report(out, SGM_ERR_START_BLOCKED, false, f, -1);

        for (int g = 0; g < f; g++)
        {
            if ((m->starts[g].tileX == sx) && (m->starts[g].tileZ == sz))
                Report(out, SGM_ERR_START_OVERLAP, false, f, -1);
        }
    }

    // -- budget -------------------------------------------------------------
    // The world hosts fixed-size arrays; a map that overruns them would lose
    // entities silently at load, so it is an error here instead.
    if (SgmPlaceCountOf(m, SGM_PLACE_BUILDING) > budget.buildings)
        Report(out, SGM_ERR_OVER_BUDGET, false, -1, -1);
    if (SgmPlaceCountOf(m, SGM_PLACE_UNIT) > budget.units)
        Report(out, SGM_ERR_OVER_BUDGET, false, -1, -1);
    if (SgmPlaceCountOf(m, SGM_PLACE_NODE) > budget.nodes)
        Report(out, SGM_ERR_OVER_BUDGET, false, -1, -1);

    // -- placements on blocked ground ---------------------------------------
    for (int i = 0; i < m->placeCount; i++)
    {
        const SgmPlacement *p = &m->places[i];
        if (!SgmInBounds(m, p->tileX, p->tileZ))
        {
            Report(out, SGM_ERR_PLACE_BLOCKED, false, -1, i);
            continue;
        }
        // A unit on unbuildable-but-passable ground is fine; only impassable
        // ground is wrong for anything. Buildings additionally want buildable.
        bool ok = SgmTilePassable(m, p->tileX, p->tileZ);
        if ((p->family == SGM_PLACE_BUILDING) && ok)
            ok = SgmTileBuildable(m, p->tileX, p->tileZ);
        if (!ok) Report(out, SGM_ERR_PLACE_BLOCKED, false, -1, i);
    }

    // -- town hall, and it must come first for its faction -------------------
    //
    // The AI anchors its home to the FIRST building it finds for a faction
    // (strategy_ai.c EnemyHome), and the game's own init spawns town halls
    // first for exactly this reason. A map that lists a house first would send
    // the AI to the wrong corner, which is invisible until you watch it play.
    for (int f = 0; f < m->factionCount; f++)
    {
        int firstBuilding = -1;
        bool hasHall = false;

        for (int i = 0; i < m->placeCount; i++)
        {
            const SgmPlacement *p = &m->places[i];
            if (p->family != SGM_PLACE_BUILDING) continue;
            if (p->faction != f) continue;

            if (firstBuilding < 0) firstBuilding = i;
            if (p->kind == townHallKind) hasHall = true;
        }

        if (firstBuilding < 0)
        {
            // No buildings at all. A faction with no base cannot play, but a
            // one-faction sandbox map may legitimately have nothing placed yet,
            // so this is advice rather than a blocker.
            Report(out, SGM_ERR_NO_TOWN_HALL, true, f, -1);
            continue;
        }
        if (!hasHall)
        {
            Report(out, SGM_ERR_NO_TOWN_HALL, false, f, -1);
            continue;
        }
        if (m->places[firstBuilding].kind != townHallKind)
            Report(out, SGM_ERR_TOWN_HALL_NOT_FIRST, false, f, firstBuilding);
    }
}

bool SgmPlayable(const SgmMap *m, SgmBudget budget, int townHallKind)
{
    // The report is 300-odd bytes; static keeps it off a 1 MB Web stack and
    // this is never called from two places at once.
    static SgmReport r;
    SgmValidate(m, budget, townHallKind, &r);
    return r.errors == 0;
}

const char *SgmIssueText(const SgmIssue *issue)
{
    if (issue == NULL) return "";
    switch (issue->code)
    {
        case SGM_OK:                     return "ok";
        case SGM_ERR_FACTION_COUNT:      return "faction count must be 1-9";
        case SGM_ERR_START_OOB:          return "a faction start is off the grid";
        case SGM_ERR_START_BLOCKED:      return "a faction start is on blocked ground";
        case SGM_ERR_START_OVERLAP:      return "two faction starts share a tile";
        case SGM_ERR_NO_TOWN_HALL:       return "a faction has no town hall";
        case SGM_ERR_TOWN_HALL_NOT_FIRST:return "a faction's town hall is not its first building";
        case SGM_ERR_OVER_BUDGET:        return "more entities than the world can host";
        case SGM_ERR_PLACE_BLOCKED:      return "something stands on blocked ground";
        default:                         return "unknown issue";
    }
}
