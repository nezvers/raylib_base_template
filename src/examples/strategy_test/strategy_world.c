// ============================================================================
//  strategy_world.c  -  the RTS test world: map, camera, picking, selection,
//  orders, gathering, combat and the enemy AI.
//
//  Design notes:
//  - ONE unit state machine (UnitUpdate) serves both factions. The mouse and
//    the enemy AI both just WRITE order fields (state/target/targetUnit/
//    targetNode); nothing else differs per faction.
//  - Picking is the classic letterbox trap: the 3D scene renders into the
//    fixed game render-texture, but the mouse lives in real window pixels.
//    ALL conversions go through MouseGroundPoint()/WorldToGame() below -
//    if clicking ever feels "off", look there first.
// ============================================================================

#define SP_PROF_IMPLEMENTATION   // this TU owns the profiler storage
#include "../../strategy_path/strategy_path_prof.h"
#include "../../strategy_path/strategy_path.h"
#include "strategy_world.h"
#include "strategy_entity_anim.h"
#include "strategy_models.h"
#include "../../strategy_asset/strategy_catalog.h"
#include "../../strategy_map/strategy_map.h"
#include "../../strategy_map/strategy_map_catalog.h"
#include "../../screen_state/screen_state.h"
#include "../../settings_state/settings_state.h"
#include "raymath.h"
#include <math.h>
#include <stddef.h>

const Color strategyFactionColor[STRAT_FACTIONS] = {
    {  80, 140, 255, 255 },     // faction 0: player, blue
    { 230,  70,  70, 255 },     // faction 1: enemy, red
};

static StrategyWorld world;

// The battlefield the next Init builds. A NAME, not a pointer: the map catalog
// is rebuilt whenever the forge saves, which moves maps between slots and
// invalidates every pointer it has handed out. Empty = the built-in layout.
static char s_selectedMap[SGM_NAME_MAX];

StrategyWorld *StrategyWorldGet(void)
{
    return &world;
}

// ----------------------------------------------------------------------------
//  Unit roster  -  active list, free list, selection list
//
//  WHY. The unit pool is sized for the worst case (10,000 on desktop) but a
//  normal game holds fewer than a hundred. Every "for i < STRAT_MAX_UNITS"
//  therefore paid for 10,000 slots to visit 96, and the Unit struct is 248
//  bytes, so the scan is not merely 100x too long - it is 100x too long while
//  touching a different cache line every step. The lists make every pass cost
//  what is LIVE rather than what is POSSIBLE.
//
//  THE INVARIANT, and it is the only thing that can go wrong here:
//  s_active holds exactly the indices where units[i].active is true, once
//  each, and s_activeCount is its length. Two mutation points maintain it -
//  UnitSpawn and UnitKill - plus a full rebuild on world reset and bulk clear.
//  Nothing else may write units[i].active. RosterAssert() checks this in debug
//  builds; if a unit ever vanishes or double-updates, call it first.
//
//  ORDER IS NOT STABLE. UnitKill swaps the last entry into the dead slot's
//  place, so the active list is a set, not a sequence. Anything that needs
//  deterministic iteration order must sort - see the formation slot assignment
//  in the movement plan. Iterating it while spawning or killing is likewise
//  unsafe; the update loop below explains how it avoids that.
// ----------------------------------------------------------------------------
static int s_active[STRAT_MAX_UNITS];       // indices of live units, unordered
static int s_activeAt[STRAT_MAX_UNITS];     // unit index -> its slot in s_active, -1 if dead
static int s_activeCount;

static int s_free[STRAT_MAX_UNITS];         // stack of unused unit indices
static int s_freeCount;

static int s_selected[STRAT_MAX_UNITS];     // indices of selected units
static int s_selectedCount;
static bool s_selectionDirty = true;        // rebuild s_selected before next use

// -- Spatial hashes -----------------------------------------------------------
//  Rebuilt once per frame from the active roster (UnitHashRebuild), replacing
//  the two O(n^2) scans that dominated the frame at scale: unit separation and
//  the sight/aggro searches.
//
//  TWO HASHES, NOT ONE. Separation queries a 0.7-unit radius; sight queries 6
//  to 8. One hash cannot serve both - sized for separation a sight query sweeps
//  a 10x10 block of cells and walks a hundred chains, and sized for sight every
//  separation query drags in dozens of units that were never close enough to
//  matter. A second rebuild of the same positions is one extra linear pass,
//  which is far cheaper than either mis-sized query.
#define STRAT_SIGHT_CELL  8.0f  // ~ the largest sightRange in strategy_defs.c

// Frame counter, used only to stagger per-unit work across frames. Wrapping is
// harmless: every consumer takes it modulo a small stride.
static int s_frame;

static SpHash s_unitHash;       // separation: fine cells, tiny radius
static SpHash s_sightHash;      // aggro / targeting: coarse cells, wide radius

// Neighbour scratch for the sight queries. File-static rather than a local:
// SP_HASH_ITEMS_MAX is the unit cap, so at desktop settings this is 40 KB and
// has no business on the stack. The queries using it never nest.
static int32_t s_sightScratch[SP_HASH_ITEMS_MAX];

// ----------------------------------------------------------------------------
//  Navigation grid
//
//  TWO GRIDS. s_navStatic is terrain alone - whatever the authored map says,
//  and nothing else. s_nav is that plus every building and blocking node. A
//  demolition restores its footprint from s_navStatic rather than trying to
//  recompute what the tile "used to be", which is unanswerable once two
//  obstacles have overlapped a tile.
//
//  128 KB of BSS at desktop caps. That is the cost of never having to reason
//  about obstacle removal, and it is a bargain.
//
//  s_navVersion is bumped on every change. Nothing reads it yet; flow fields in
//  Phase 5 store the version they were built against and evict themselves when
//  it moves. It is here now because adding it later means auditing every
//  mutation site a second time.
// ----------------------------------------------------------------------------
static SpGrid   s_navStatic;    // terrain only
static SpGrid   s_nav;          // terrain + buildings + blocking nodes
static uint32_t s_navVersion;

// False while the world is being populated. Spawning happens in bulk during
// init - dozens of buildings and nodes - and each one would otherwise trigger a
// full skirt rebuild for a grid that is about to be rebuilt from scratch
// anyway. Init stamps nothing and calls NavRebuild once at the end.
static bool s_navReady;

// Paces the flow-field refcount sweep. See StrategyMoveFlowSweep.
static float s_flowSweepTimer;

// The grid must be able to hold any map the forge can author. Checked here
// because this is the one file that sees both headers.
_Static_assert(SP_GRID_MAX >= SGM_GRID_MAX, "nav grid smaller than map grid");

// Read-only access for strategy_move.c. Deliberately const: this file is the
// only writer, which is what keeps "who may change the grid" a question with
// one answer. The version lets a path notice the ground moved under it.
const SpGrid *StrategyNavGrid(void)    { return &s_nav; }
uint32_t      StrategyNavVersion(void) { return s_navVersion; }

// Full reset: every slot free, nothing active. The free stack is filled in
// DESCENDING order so the first pops are 0, 1, 2... - spawn order then matches
// the old linear scan exactly, which keeps save files, replays and the AI's
// index-based targeting behaving as they did.
static void RosterReset(void)
{
    s_activeCount = 0;
    s_freeCount   = 0;
    for (int i = STRAT_MAX_UNITS - 1; i >= 0; i--) s_free[s_freeCount++] = i;
    for (int i = 0; i < STRAT_MAX_UNITS; i++) s_activeAt[i] = -1;
    s_selectedCount  = 0;
    s_selectionDirty = true;
}

static void RosterAdd(int index)
{
    s_activeAt[index] = s_activeCount;
    s_active[s_activeCount++] = index;
}

// Swap-with-last removal: O(1), but it REORDERS the list. See the note above.
static void RosterRemove(int index)
{
    int slot = s_activeAt[index];
    if (slot < 0) return;                   // already dead: idempotent by design

    int last = s_active[--s_activeCount];
    s_active[slot]   = last;
    s_activeAt[last] = slot;
    s_activeAt[index] = -1;

    s_free[s_freeCount++] = index;
    s_selectionDirty = true;

    // The slot is about to be reused by the next spawn. A path left behind
    // would be inherited by whoever lands here, and a search still in flight
    // would deliver its result to them - a new unit walking a dead one's route.
    StrategyMoveForget(index);
}

// The selection list is rebuilt lazily rather than maintained on every
// `selected` write, because selection is touched from a dozen places (drag
// rect, shift-click, control groups, deselect-all, death) and a missed update
// in any one of them is a phantom or a ghost. A rebuild is O(live) and happens
// at most once per frame, against input that happens at human speed.
static const int *SelectedUnits(int *count)
{
    if (s_selectionDirty)
    {
        s_selectedCount = 0;
        for (int k = 0; k < s_activeCount; k++)
        {
            int i = s_active[k];
            if (world.units[i].selected) s_selected[s_selectedCount++] = i;
        }
        s_selectionDirty = false;
    }
    *count = s_selectedCount;
    return s_selected;
}

// Call after ANY write to units[].selected. Cheap enough to call redundantly.
static void SelectionTouch(void) { s_selectionDirty = true; }

// Invariant check, for when a unit vanishes or updates twice. Verifies that
// s_active and units[].active agree in BOTH directions, that s_activeAt is a
// correct inverse, and that active + free accounts for every slot exactly once.
//
// Off by default because it is O(pool) - running it every frame would reinstate
// exactly the full-array scan this phase removed. Turn it on from the path lab
// (or a debugger) when something is wrong; it traces the first failure and
// stops. Debug builds only.
#ifndef NDEBUG
bool strategyRosterAudit = false;

static bool RosterCheck(void)
{
    if (s_activeCount + s_freeCount != STRAT_MAX_UNITS)
    {
        TraceLog(LOG_ERROR, "ROSTER: %d active + %d free != %d",
                 s_activeCount, s_freeCount, STRAT_MAX_UNITS);
        return false;
    }
    for (int k = 0; k < s_activeCount; k++)
    {
        int i = s_active[k];
        if (i < 0 || i >= STRAT_MAX_UNITS)
        { TraceLog(LOG_ERROR, "ROSTER: bad index %d at %d", i, k); return false; }
        if (!world.units[i].active)
        { TraceLog(LOG_ERROR, "ROSTER: unit %d listed but dead", i); return false; }
        if (s_activeAt[i] != k)
        { TraceLog(LOG_ERROR, "ROSTER: unit %d at %d, activeAt says %d",
                   i, k, s_activeAt[i]); return false; }
    }
    // The other direction: nothing live may be missing from the list.
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        if (world.units[i].active && s_activeAt[i] < 0)
        { TraceLog(LOG_ERROR, "ROSTER: unit %d live but unlisted", i); return false; }
    }
    return true;
}
#endif

// Public view of the active list, for strategy_ai.c and strategy_test.c.
const int *StrategyActiveUnits(int *count)
{
    *count = s_activeCount;
    return s_active;
}

void StrategyMapSelect(const char *name)
{
    if (name == NULL) { s_selectedMap[0] = '\0'; return; }
    TextCopy(s_selectedMap, name);
}

const char *StrategyMapSelected(void)
{
    return s_selectedMap;
}

// ----------------------------------------------------------------------------
//  Small helpers
// ----------------------------------------------------------------------------
static float DistXZ(Vector3 a, Vector3 b)
{
    float dx = a.x - b.x;
    float dz = a.z - b.z;
    return sqrtf(dx*dx + dz*dz);
}

// Derive the camera from focus + zoom (fixed pitch, no rotation). Called
// whenever either changes; keeps pan/zoom code trivial.
static void CameraRefresh(void)
{
    Vector3 target = (Vector3){ world.camFocus.x, 0.0f, world.camFocus.y };
    Vector3 offset = (Vector3){ 0.0f, 14.0f, 10.0f };

    world.camera.target   = target;
    world.camera.position = Vector3Add(target, Vector3Scale(offset, world.camZoom));
}

// ----------------------------------------------------------------------------
//  Authored map: terrain queries and terrain drawing
//
//  The world holds a BORROWED pointer to the map it was built from, or NULL for
//  the built-in layout. Every one of these helpers answers the built-in case
//  first and permissively - no map means no authored obstacles, which is
//  exactly how the game behaved before maps existed.
// ----------------------------------------------------------------------------

// Can a building stand here? Snapped to the tile, matching PlacementGhost's
// roundf, so the ghost the player sees and the rule that rejects agree.
static bool MapBuildableAt(Vector3 pos)
{
    if (world.map == NULL) return true;
    int tx, tz;
    SgmWorldToTile(world.map, roundf(pos.x), roundf(pos.z), &tx, &tz);
    return SgmTileBuildable(world.map, tx, tz);
}

// The colour a terrain kind draws as. Ground/grass/dirt return false: they are
// the base plane and are not drawn as tiles at all, which keeps the common case
// free of thousands of overlapping quads.
static bool MapTerrainColor(int terrain, Color *out)
{
    switch (terrain)
    {
        case SGM_TERRAIN_SHALLOW: *out = (Color){  90, 150, 180, 255 }; return true;
        case SGM_TERRAIN_WATER:   *out = (Color){  40,  80, 140, 255 }; return true;
        case SGM_TERRAIN_ROCK:    *out = (Color){ 110, 105, 100, 255 }; return true;
        case SGM_TERRAIN_CLIFF:   *out = (Color){  85,  75,  70, 255 }; return true;
        default: return false;      // ground/grass/dirt: the base plane shows
    }
}

// Draw the authored terrain: one box per non-plain tile, at its elevation.
// VOID draws nothing at all - it is a hole, and the base plane is what would
// show through, so a void tile is punched by simply skipping it.
static void MapDrawTerrain(void)
{
    if (world.map == NULL) return;
    const SgmMap *m = world.map;

    for (int z = 0; z < m->gridH; z++)
    {
        for (int x = 0; x < m->gridW; x++)
        {
            const SgmTile *t = SgmTileAtConst(m, x, z);
            if (t == NULL || t->terrain == SGM_TERRAIN_VOID) continue;

            Color c;
            bool paint = MapTerrainColor(t->terrain, &c);
            if (!paint && t->height == 0) continue;     // plain ground: the plane

            if (!paint) c = (Color){ 100, 120, 88, 255 };   // raised plain ground

            // Elevation steps are half a world unit, so a 15-step cliff reads as
            // tall without dwarfing the units standing beside it.
            float h  = 0.5f*(float)t->height;
            Vector3 p = SgmTileToWorld(m, x, z);

            // Every tile is drawn from the plane UP, so a raised tile is a solid
            // column rather than a floating lid.
            float boxH = h + 0.12f;
            DrawCube((Vector3){ p.x, boxH*0.5f - 0.06f, p.z }, 1.0f, boxH, 1.0f, c);
        }
    }
}

// The gridlines. Replaces raylib's DrawGrid(), which is a fixed LIGHTGRAY at
// full alpha centred on the origin - it cannot express a non-square extent, and
// its opacity is the thing being complained about. Phase 5 makes the strength
// authored + player-tunable; for now it is a markedly lighter fixed value.
static void DrawGroundGrid(void)
{
    Color line = (Color){ 255, 255, 255, 40 };
    float hx = world.groundHalfX, hz = world.groundHalfZ;

    for (float x = -hx; x <= hx + 0.001f; x += 1.0f)
        DrawLine3D((Vector3){ x, 0.0f, -hz }, (Vector3){ x, 0.0f, hz }, line);
    for (float z = -hz; z <= hz + 0.001f; z += 1.0f)
        DrawLine3D((Vector3){ -hx, 0.0f, z }, (Vector3){ hx, 0.0f, z }, line);
}

// ----------------------------------------------------------------------------
//  Navigation grid: build, stamp, patch
//
//  Nothing STEERS by this yet - units still lerp straight at their targets.
//  The grid lands on its own so it can be proven correct against an overlay
//  that is already trusted (the forge's P view) before A* is written on top of
//  it. A pathfinding bug and a grid bug are indistinguishable from the outside,
//  so they get separated in time.
// ----------------------------------------------------------------------------

// Does a resource node block movement? Trees and rocks are solid objects a unit
// must walk around; wheat is knee-high and corpses are on the floor, so both
// stay walkable. Getting this wrong is very visible: blocking corpses would
// mean a battlefield slowly filling with invisible walls.
static bool NodeBlocks(NodeKind kind)
{
    return (kind == NODE_TREE) || (kind == NODE_ROCK);
}

// Half-extents in tiles for a building, from its footprint. Footprints are odd
// so the building sits centred on one tile; halving rounds down, which is
// exactly right: 3 -> 1 (the centre tile plus one either side), 1 -> 0.
static void BuildingHalfTiles(BuildingKind kind, int *hx, int *hz)
{
    const BuildingDef *bd = StrategyBuildingDef(kind);
    *hx = bd->footprintX/2;
    *hz = bd->footprintZ/2;
}

// Half the footprint in WORLD units, for interaction ranges. A 3x3 town hall
// reaches 1.5 units from its centre, so a worker asked to stand within 1.6 of
// the centre is being asked to stand inside the wall - it walks to the edge,
// never satisfies the check, and shuffles there forever. This is the number
// that fixes that.
static float BuildingHalfWorld(BuildingKind kind)
{
    const BuildingDef *bd = StrategyBuildingDef(kind);
    int f = (bd->footprintX > bd->footprintZ) ? bd->footprintX : bd->footprintZ;
    return 0.5f*(float)f;
}

// How close a unit must be to a building's CENTRE to interact with it: dropoff,
// build, repair, tend-equip. Half the footprint plus a unit's own diameter and
// a little slack, so a worker standing flush against the wall counts as
// arrived - which the old flat 1.6f/1.8f could not express, because a 3x3 town
// hall's wall is already 1.5 units out.
//
// Floored at STRAT_BUILD_RANGE so nothing gets HARDER to reach than it is
// today: a 1x1 house would otherwise want 1.3, and tightening an interaction
// radius is a gameplay change, not a bug fix.
#define STRAT_BLD_REACH_PAD  0.8f
static float BuildingReach(BuildingKind kind)
{
    float r = BuildingHalfWorld(kind) + STRAT_BLD_REACH_PAD;
    return (r > STRAT_BUILD_RANGE) ? r : STRAT_BUILD_RANGE;
}

// Stamp one building into the live grid. `blocked` false restores the tiles
// from terrain instead - that is the demolition path.
static void NavStampBuilding(const Building *b, bool blocked)
{
    int hx, hz;
    BuildingHalfTiles(b->kind, &hx, &hz);

    if (blocked) SpGridStampRect(&s_nav, b->pos.x, b->pos.z, hx, hz, SP_COST_BLOCKED);
    else         SpGridRestoreRect(&s_nav, &s_navStatic, b->pos.x, b->pos.z, hx, hz, 1);
}

static void NavStampNode(const ResourceNode *n, bool blocked)
{
    if (!NodeBlocks(n->kind)) return;   // wheat and corpses never stamped

    if (blocked) SpGridStampRect(&s_nav, n->pos.x, n->pos.z, 0, 0, SP_COST_BLOCKED);
    else         SpGridRestoreRect(&s_nav, &s_navStatic, n->pos.x, n->pos.z, 0, 0, 1);
}

// Rebuild both grids from scratch: terrain into s_navStatic, then a copy plus
// every live obstacle into s_nav. Called on world init and after any change
// too broad to patch.
static void NavRebuild(void)
{
    // world.map == NULL is the built-in layout, and it is answered HERE, once.
    // Every function downstream then works on a real grid and never NULL-checks
    // - which is the whole point, because a NULL check that has to be repeated
    // at thirty call sites is a bug waiting for the thirty-first.
    if (world.map != NULL)
    {
        const SgmMap *m = world.map;
        SpGridInit(&s_navStatic, m->gridW, m->gridH);

        for (int z = 0; z < m->gridH; z++)
        {
            for (int x = 0; x < m->gridW; x++)
            {
                const SgmTile *t = SgmTileAtConst(m, x, z);
                uint8_t cost = SP_COST_NORMAL;

                if (t == NULL || !SgmTilePassable(m, x, z))    cost = SP_COST_BLOCKED;
                else if (t->terrain == SGM_TERRAIN_SHALLOW)    cost = SP_COST_SHALLOW;

                SpGridSet(&s_navStatic, x, z, cost);
            }
        }
    }
    else
    {
        // The built-in battlefield is a bare STRAT_GROUND_HALF square with no
        // authored terrain at all, so the grid is simply open ground the same
        // size. Buildings and nodes still stamp into it.
        int side = (int)(2.0f*STRAT_GROUND_HALF);
        SpGridInit(&s_navStatic, side, side);
    }

    s_nav = s_navStatic;

    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        if (world.buildings[i].active) NavStampBuilding(&world.buildings[i], true);
    }
    for (int i = 0; i < STRAT_MAX_NODES; i++)
    {
        if (world.nodes[i].active) NavStampNode(&world.nodes[i], true);
    }

    SpGridBuildSkirt(&s_nav);
    s_navVersion++;
    SpFlowSetVersion(s_navVersion);
}

// Patch after an obstacle APPEARED. Only the skirt needs redoing; the stamp
// itself was already written.
//
// The skirt is recomputed WHOLE rather than locally, because a tile can be
// skirting two obstacles at once - so "lower the neighbours of what I just
// removed" is wrong in exactly the case that matters. The full pass is one
// byte-compare per cell and only runs on placement and demolition, never per
// frame.
static void NavPatch(void)
{
    SpGridBuildSkirt(&s_nav);
    s_navVersion++;
    // Every flow field is a whole-grid answer, so one new wall retires all of
    // them. Told here rather than at each caller: this and NavRebuild are the
    // only two places s_navVersion moves.
    SpFlowSetVersion(s_navVersion);
}

// Patch after an obstacle VANISHED. Restoring its footprint from terrain also
// wipes anything else that overlapped those tiles - a node under the edge of a
// demolished town hall, say - so every surviving obstacle whose own footprint
// touches the restored rect has to be stamped back.
//
// The re-stamp is gated on an AABB test rather than being a blanket rescan.
// STRAT_MAX_BUILDINGS is 24 and STRAT_MAX_NODES is 48 today, but both are
// flagged as far too small for a 10k-unit map; when they grow to hundreds this
// stays proportional to what is actually nearby.
static void NavClearArea(float wx, float wz, float halfWorldX, float halfWorldZ)
{
    // +2 world units of margin: the vanished obstacle's skirt reached one tile
    // past its footprint, and rounding to tiles can cost another.
    float rx = halfWorldX + 2.0f, rz = halfWorldZ + 2.0f;

    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        const Building *b = &world.buildings[i];
        if (!b->active) continue;
        float bh = BuildingHalfWorld(b->kind);
        if (fabsf(b->pos.x - wx) > rx + bh) continue;
        if (fabsf(b->pos.z - wz) > rz + bh) continue;
        NavStampBuilding(b, true);
    }
    for (int i = 0; i < STRAT_MAX_NODES; i++)
    {
        const ResourceNode *n = &world.nodes[i];
        if (!n->active || !NodeBlocks(n->kind)) continue;
        if (fabsf(n->pos.x - wx) > rx + 0.5f) continue;
        if (fabsf(n->pos.z - wz) > rz + 0.5f) continue;
        NavStampNode(n, true);
    }

    NavPatch();
}

// Mouse position in GAME-CANVAS pixels (the 3D scene's framebuffer space).
static Vector2 MouseGame(void)
{
    return Screen2Target(GetMousePosition());
}

// Cast the mouse into the world and intersect the y = 0 ground plane.
// Returns false when the ray misses the plane (looking at the sky).
static bool MouseGroundPoint(Vector3 *out)
{
    Vector2 mouse = MouseGame();
    Vector2 gameSize = ScreenStateTargetSize();
    Ray ray = GetScreenToWorldRayEx(mouse, world.camera,
                                    (int)gameSize.x, (int)gameSize.y);

    if (fabsf(ray.direction.y) < 0.0001f) return false;
    float t = -ray.position.y/ray.direction.y;
    if (t < 0.0f) return false;

    *out = Vector3Add(ray.position, Vector3Scale(ray.direction, t));
    return true;
}

// Project a world point back into game-canvas pixels (drag-select, HP bars).
static Vector2 WorldToGame(Vector3 p)
{
    Vector2 gameSize = ScreenStateTargetSize();
    return GetWorldToScreenEx(p, world.camera, (int)gameSize.x, (int)gameSize.y);
}

// True while the mouse hovers the build bar (Gui publishes its rectangle in
// REAL screen pixels) - world clicks must not fire "through" the buttons.
static bool MouseOnGui(void)
{
    Vector2 m = GetMousePosition();
    return CheckCollisionPointRec(m, world.guiBlock) ||
           CheckCollisionPointRec(m, world.guiBlock2);
}

static ResourceKind NodeResource(NodeKind kind)
{
    switch (kind)
    {
        case NODE_TREE:   return RES_WOOD;
        case NODE_ROCK:   return RES_STONE;
        case NODE_WHEAT:  return RES_FOOD;
        case NODE_CORPSE: return RES_FOOD;
        default:          return RES_WOOD;
    }
}

// Faction color with the neutral-animal guard: FACTION_NEUTRAL must never
// index strategyFactionColor[] (only 2 entries).
static Color UnitColor(const Unit *u)
{
    if (u->faction == FACTION_NEUTRAL) return BEIGE;
    return strategyFactionColor[u->faction];
}

// ----------------------------------------------------------------------------
//  Spawning
// ----------------------------------------------------------------------------
// Pops a free slot instead of scanning for one. The old linear scan was O(n)
// per spawn and therefore O(n^2) for a bulk spawn - at 10,000 units that was a
// visible hitch measured in seconds, not frames.
static Unit *UnitSpawn(int faction, UnitKind kind, Vector3 pos)
{
    if (s_freeCount > 0)
    {
        int i = s_free[--s_freeCount];
        Unit *u = &world.units[i];

        // Resolve stats ONCE: base def x this faction's difficulty mods.
        // (Training-building buffs multiply on top in BuildingsUpdate.)
        const UnitDef *def = StrategyUnitDef(kind);
        const FactionMods *m = &world.mods[faction];

        *u = (Unit){ 0 };
        u->active         = true;
        u->faction        = faction;
        u->kind           = kind;
        u->pos            = pos;
        u->target         = pos;
        u->state          = UNIT_IDLE;
        u->maxHp          = def->maxHp*m->hpMul;
        u->hp             = u->maxHp;
        u->damage         = def->damage*m->dmgMul;
        u->attackRange    = def->attackRange;
        u->attackPeriod   = def->attackPeriod;
        u->preferredRange = def->preferredRange;
        u->moveSpeed      = def->moveSpeed;
        u->sightRange     = def->sightRange*m->sightMul;
        u->gatherTime     = def->gatherTime*m->gatherMul;
        u->farmPeriod     = def->farmPeriod*m->gatherMul;
        u->targetUnit     = -1;
        u->targetNode     = -1;
        u->targetBuilding = -1;
        u->dropoffCache   = -1;
        u->formGroup      = -1;     // NOT 0: `(Unit){0}` above would otherwise
                                    // enrol every fresh unit in group 0

        RosterAdd(i);
        return u;
    }
    return NULL;       // pool full
}

// scaffold=true spawns an under-construction site: near-zero HP, non-functional
// until a worker builds it up. Pre-placed and AI buildings pass false (finished).
static Building *BuildingSpawn(BuildingKind kind, int faction, Vector3 pos, bool scaffold)
{
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        Building *b = &world.buildings[i];
        if (b->active) continue;

        *b = (Building){ 0 };
        b->active            = true;
        b->kind              = kind;
        b->faction           = faction;
        b->pos               = pos;
        b->maxHp             = StrategyBuildingDef(kind)->maxHp;
        b->underConstruction = scaffold;
        b->hp                = scaffold ? 1.0f : b->maxHp;
        b->buildProgress     = 0.0f;
        b->trainKind         = -1;
        b->trainProgress     = 0.0f;

        if (s_navReady) { NavStampBuilding(b, true); NavPatch(); }
        return b;
    }
    return NULL;
}

static void NodeSpawn(NodeKind kind, Vector3 pos, int amount)
{
    for (int i = 0; i < STRAT_MAX_NODES; i++)
    {
        ResourceNode *n = &world.nodes[i];
        if (n->active) continue;

        *n = (ResourceNode){ 0 };
        n->active    = true;
        n->kind      = kind;
        n->pos       = pos;
        n->remaining = amount;

        if (s_navReady) { NavStampNode(n, true); NavPatch(); }
        return;
    }
}

// The ONE way a node leaves the world. A raw `n->active = false` anywhere else
// leaves its stamp in the nav grid forever - a phantom obstacle standing where
// a tree used to be, invisible and permanent. Funnelling it through here is the
// only defence, since the compiler cannot catch the raw assignment.
static void NodeDespawn(int index)
{
    ResourceNode *n = &world.nodes[index];
    if (!n->active) return;

    bool blocked = NodeBlocks(n->kind);
    Vector3 pos  = n->pos;
    n->active = false;

    if (s_navReady && blocked)
    {
        // Restore the one tile from terrain, then let NavClearArea put back
        // whatever else was standing under it.
        SpGridRestoreRect(&s_nav, &s_navStatic, pos.x, pos.z, 0, 0, 1);
        NavClearArea(pos.x, pos.z, 0.5f, 0.5f);
    }
}

// How much a node yields. The built-in layout passes these amounts as literals
// (see SpawnBuiltinLayout); an authored placement carries a per-node override,
// and 0 - which is what the forge writes, since it does not author amounts -
// means "use the default for this kind". There is no NodeDef table to hang
// these on, so they live here beside the only other place they appear.
static int NodeAmount(int kind, int authored)
{
    if (authored > 0) return authored;
    switch (kind)
    {
        case NODE_ROCK:  return 100;
        case NODE_WHEAT: return 8;
        default:         return 12;    // NODE_TREE
    }
}

static void NodeCluster(NodeKind kind, Vector3 center, int count, float spread, int amount)
{
    for (int i = 0; i < count; i++)
    {
        Vector3 pos = center;
        pos.x += (float)GetRandomValue(-100, 100)*0.01f*spread;
        pos.z += (float)GetRandomValue(-100, 100)*0.01f*spread;
        NodeSpawn(kind, pos, amount);
    }
}

// The battlefield the game shipped with, spawned when no authored map is
// selected. Kept verbatim so that "no map" plays exactly as it always did.
// Runs AFTER the difficulty mods are installed, like every other spawn path.
static void SpawnBuiltinLayout(void)
{
    world.groundHalfX = STRAT_GROUND_HALF;
    world.groundHalfZ = STRAT_GROUND_HALF;
    world.camFocus    = (Vector2){ -14.0f, -12.0f };

    // Two rival bases in opposite corners: town hall FIRST (critical, trains
    // workers, and the AI's EnemyHome anchors to the first building), two
    // houses + barracks; 4 workers + 2 soldiers each.
    BuildingSpawn(BLD_TOWN_HALL, 0, (Vector3){ -14.0f, 0.0f, -14.0f }, false);
    BuildingSpawn(BLD_HOUSE,     0, (Vector3){ -17.0f, 0.0f, -14.0f }, false);
    BuildingSpawn(BLD_HOUSE,     0, (Vector3){ -17.0f, 0.0f, -11.0f }, false);
    BuildingSpawn(BLD_BARRACKS,  0, (Vector3){ -11.0f, 0.0f, -14.0f }, false);
    BuildingSpawn(BLD_TOWN_HALL, 1, (Vector3){  14.0f, 0.0f,  14.0f }, false);
    BuildingSpawn(BLD_HOUSE,     1, (Vector3){  17.0f, 0.0f,  14.0f }, false);
    BuildingSpawn(BLD_HOUSE,     1, (Vector3){  17.0f, 0.0f,  11.0f }, false);
    BuildingSpawn(BLD_BARRACKS,  1, (Vector3){  11.0f, 0.0f,  14.0f }, false);
    for (int i = 0; i < 6; i++)
    {
        UnitKind kind = (i < 4) ? KIND_WORKER : KIND_SOLDIER;
        UnitSpawn(0, kind, (Vector3){ -14.0f + (float)i*1.2f - 3.0f, 0.0f, -11.0f });
        UnitSpawn(1, kind, (Vector3){  14.0f - (float)i*1.2f + 3.0f, 0.0f,  11.0f });
    }

    // The player starts with a lone templar (no Chantry needed): it idles into
    // FOLLOW and blesses whichever worker starts gathering, seeding providence.
    UnitSpawn(0, KIND_TEMPLAR, (Vector3){ -13.0f, 0.0f, -10.0f });

    // Resources scattered between the bases; wheat near each base so both
    // factions can feed their training queue.
    NodeCluster(NODE_TREE,  (Vector3){ -7.0f, 0.0f,  -3.0f }, 6, 2.5f, 12);
    NodeCluster(NODE_TREE,  (Vector3){  6.0f, 0.0f,   9.0f }, 6, 2.5f, 12);
    NodeCluster(NODE_ROCK,  (Vector3){ -3.0f, 0.0f,   8.0f }, 5, 2.0f, 100);
    NodeCluster(NODE_ROCK,  (Vector3){  9.0f, 0.0f,  -7.0f }, 5, 2.0f, 100);
    NodeCluster(NODE_WHEAT, (Vector3){ -10.0f, 0.0f, -7.0f }, 5, 2.0f, 8);
    NodeCluster(NODE_WHEAT, (Vector3){  10.0f, 0.0f,  7.0f }, 5, 2.0f, 8);

    // Neutral animals wandering mid-map, huntable for food. Weak ones flee
    // when hit, strong ones fight back as a pack.
    for (int i = 0; i < STRAT_ANIMAL_COUNT; i++)
    {
        Vector3 pos = (Vector3){
            (float)GetRandomValue(-10, 10), 0.0f, (float)GetRandomValue(-10, 10),
        };
        UnitSpawn(FACTION_NEUTRAL, KIND_ANIMAL_WEAK, pos);
    }
    for (int i = 0; i < STRAT_ANIMAL_STRONG_COUNT; i++)
    {
        Vector3 pos = (Vector3){
            (float)GetRandomValue(-8, 8), 0.0f, (float)GetRandomValue(-8, 8),
        };
        UnitSpawn(FACTION_NEUTRAL, KIND_ANIMAL_STRONG, pos);
    }
}

// Spawn an authored battlefield.
//
// Two ordering constraints are load-bearing and are why this is a single
// straight-line pass rather than three loops by family:
//
//  1. DIFFICULTY MODS FIRST. UnitSpawn reads world.mods to resolve stats, so
//     every spawn here must run after StrategyWorldInit has installed them.
//     (Guaranteed by the call site, which is the last thing Init does.)
//
//  2. TOWN HALL FIRST, PER FACTION. strategy_ai.c anchors EnemyHome to the
//     first building it finds for its faction, so a faction's town hall must
//     occupy a lower buildings[] slot than its other buildings. SgmValidate
//     enforces town-hall-first in the authored list and the map forge refuses
//     to save a map that breaks it, so walking the list in order preserves it.
//     Walking by family would NOT: it would spawn every faction's buildings
//     interleaved and could seat a house ahead of a town hall.
//
// The runtime still plays two factions (STRAT_FACTIONS). Authored factions
// 2..8 are stored and validated but their placements are SKIPPED here - the
// agreed scope. Neutral placements (resource nodes, wild animals) always spawn.
static void SpawnFromMap(const SgmMap *m)
{
    world.map         = m;
    world.groundHalfX = 0.5f*(float)m->gridW;
    world.groundHalfZ = 0.5f*(float)m->gridH;

    // Focus on faction 0's start, so the player opens looking at their own base
    // wherever the author put it.
    if (m->factionCount > 0)
    {
        Vector3 home = SgmTileToWorld(m, m->starts[0].tileX, m->starts[0].tileZ);
        world.camFocus = (Vector2){ home.x, home.z };
    }

    for (int i = 0; i < m->placeCount; i++)
    {
        const SgmPlacement *pl = &m->places[i];
        Vector3 pos = SgmTileToWorld(m, pl->tileX, pl->tileZ);

        // A neutral placement belongs to nobody; an owned one belongs to a
        // faction the runtime may not be playing yet.
        bool neutral = (pl->faction == SGM_FACTION_NEUTRAL);
        if (!neutral && pl->faction >= STRAT_FACTIONS) continue;
        int faction = neutral ? FACTION_NEUTRAL : (int)pl->faction;

        switch (pl->family)
        {
            case SGM_PLACE_BUILDING:
                // Buildings are always owned; a neutral one is meaningless and
                // would index the stockpile at FACTION_NEUTRAL, which has no row.
                if (neutral) break;
                if (pl->kind < BLD_COUNT)
                    BuildingSpawn((BuildingKind)pl->kind, faction, pos, false);
                break;

            case SGM_PLACE_UNIT:
                if (pl->kind < UNIT_KIND_COUNT)
                    UnitSpawn(faction, (UnitKind)pl->kind, pos);
                break;

            case SGM_PLACE_NODE:
                // amount 0 means "the game's default for this kind", which is
                // what the forge writes unless the author overrode it.
                if (pl->kind < NODE_KIND_COUNT)
                    NodeSpawn((NodeKind)pl->kind, pos, NodeAmount(pl->kind, pl->amount));
                break;

            default: break;
        }
    }
}

void StrategyWorldInit(void)
{
    world = (StrategyWorld){ 0 };
    RosterReset();      // must follow the world wipe: it mirrors units[].active

    // Nav stamping stays OFF through the whole of init. The layouts below spawn
    // dozens of buildings and nodes, and each one would otherwise trigger a
    // full skirt rebuild for a grid that NavRebuild replaces wholesale at the
    // end of this function anyway.
    s_navReady = false;
    world.placing          = -1;
    world.selectedBuilding = -1;
    world.gameOver         = -1;
    EffectsReset();

    // Authored assets, if any. The catalog is shared with the showcase and the
    // forge, so this is a no-op when one of them already loaded it; it is here
    // so the game can be entered first and still draw bound roles.
    StrategyAssetSetFactionTint(StrategyFactionTint);
    StrategyBindingsSetRoleCounts(UNIT_KIND_COUNT, BLD_COUNT, NODE_KIND_COUNT);
    StrategyCatalogLoad();

    // Difficulty mods BEFORE any spawn (UnitSpawn reads them). The player and
    // neutral rows stay identity; only the AI faction is scaled. Hard is the
    // baseline; Normal/Easy weaken the AI and give the player a sell bonus.
    FactionMods identity = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f };
    world.mods[0] = world.mods[1] = world.mods[FACTION_NEUTRAL] = identity;
    switch (SettingsGet()->difficulty)
    {
        case 0:     // Easy
            world.mods[1] = (FactionMods){ 0.8f, 0.8f, 1.10f, 0.9f, 1.6f, 0.0f };
            world.mods[0].refundBonus = 0.2f;
            break;
        case 1:     // Normal
            world.mods[1] = (FactionMods){ 0.9f, 1.0f, 1.05f, 1.0f, 1.25f, 0.0f };
            world.mods[0].refundBonus = 0.1f;
            break;
        default:    // Hard: identity
            break;
    }
    world.aiPeriod = STRAT_AI_PERIOD*world.mods[1].aiPeriodMul;
    world.aiTimer  = world.aiPeriod;

    // Camera: perspective, fixed pitch. The FOCUS is not set here - it belongs
    // to the battlefield (the built-in looks at its corner base, an authored map
    // at faction 0's start), so each layout sets it and CameraRefresh runs once
    // afterwards. Likewise the extent: groundHalf* is still zero at this point.
    world.camera.up         = (Vector3){ 0.0f, 1.0f, 0.0f };
    world.camera.fovy       = 45.0f;
    world.camera.projection = CAMERA_PERSPECTIVE;
    world.camZoom  = 1.0f;

    // The battlefield itself. An authored map is looked up by NAME here rather
    // than held as a pointer, because the map catalog is rebuilt whenever the
    // forge saves; a name that no longer resolves falls back to the built-in
    // layout rather than failing to start.
    SgmCatalogLoad();
    const SgmMap *sel = (s_selectedMap[0] != '\0') ? SgmCatalogFind(s_selectedMap)
                                                   : NULL;
    if (sel != NULL) SpawnFromMap(sel);
    else             SpawnBuiltinLayout();
    CameraRefresh();    // the layout set camFocus; rebuild the camera from it

    // The battlefield is populated: derive the nav grid from it once, then let
    // later placements and demolitions patch incrementally.
    NavRebuild();
    s_navReady = true;

    // The path service holds a grid pointer and cell indices, both of which a
    // rebuild invalidates - and s_paths[] holds routes over the PREVIOUS map.
    // Resetting both here rather than at the top of init is deliberate: the
    // grid has to exist before the service can be pointed at it.
    SpServiceReset(&s_nav);
    SpFlowReset(&s_nav);
    SpFlowSetVersion(s_navVersion);
    StrategyMoveInit();

    // Starting stockpiles so building/training is possible right away. The
    // human player (faction 0) gets an easier-difficulty head start: Easy x3,
    // Normal x2, Hard x1. The enemy always starts on the base amounts.
    int playerMul = (SettingsGet()->difficulty == 0) ? 3
                  : (SettingsGet()->difficulty == 1) ? 2 : 1;
    for (int f = 0; f < STRAT_FACTIONS; f++)
    {
        int mul = (f == 0) ? playerMul : 1;
        world.stockpile[f][RES_WOOD]  = 10*mul;
        world.stockpile[f][RES_STONE] = 10*mul;
        world.stockpile[f][RES_FOOD]  = 5*mul;
    }
}

// ----------------------------------------------------------------------------
//  Orders: the ONLY way anything (mouse, GUI or AI) makes a unit act.
//  Exported so strategy_ai.c issues the exact same orders the mouse does.
// ----------------------------------------------------------------------------

// Defined with the movement code far below; declared here because the orders
// sit above it and every new destination must clear the arrival progression.
static void MoveArriveReset(Unit *u);

void StrategyOrderMove(Unit *u, Vector3 dest)
{
    u->state          = UNIT_MOVE;
    u->target         = dest;
    u->targetUnit     = -1;
    u->targetNode     = -1;
    u->targetBuilding = -1;
    MoveArriveReset(u);     // or a unit that settled on its last order arrives
                            // at this one instantly, without moving
}

void StrategyOrderGather(Unit *u, int nodeIndex)
{
    u->state          = UNIT_GATHER;
    u->targetNode     = nodeIndex;
    u->targetUnit     = -1;
    u->targetBuilding = -1;
    u->gatherTimer    = 0.0f;
}

void StrategyOrderAttack(Unit *u, int unitIndex)
{
    u->state          = UNIT_ATTACK;
    u->targetUnit     = unitIndex;
    u->targetNode     = -1;
    u->targetBuilding = -1;
    u->attackCooldown = 0.0f;
}

void StrategyOrderAttackBuilding(Unit *u, int bldIndex)
{
    u->state          = UNIT_ATTACK;
    u->targetBuilding = bldIndex;
    u->targetUnit     = -1;
    u->targetNode     = -1;
    u->attackCooldown = 0.0f;
}

// Assign a worker to tend a node-spawning building (farm/forestry). target is
// seeded to the building position, the "pick a new plant spot" sentinel.
void StrategyOrderFarm(Unit *u, int bldIndex)
{
    u->state          = UNIT_FARM;
    u->targetBuilding = bldIndex;
    u->target         = world.buildings[bldIndex].pos;
    u->targetUnit     = -1;
    u->targetNode     = -1;
    u->carryAmount    = 0;
    u->gatherTimer    = 0.0f;
    u->tendEquipped   = false;      // start the plant round-trip at the building
}

void StrategyOrderBuild(Unit *u, int bldIndex)
{
    u->state          = UNIT_BUILD;
    u->targetBuilding = bldIndex;
    u->targetUnit     = -1;
    u->targetNode     = -1;
}

void StrategyOrderRepair(Unit *u, int bldIndex)
{
    u->state          = UNIT_REPAIR;
    u->targetBuilding = bldIndex;
    u->targetUnit     = -1;
    u->targetNode     = -1;
}

// ----------------------------------------------------------------------------
//  Picking: nearest unit / node to a ground point, within a grab radius.
// ----------------------------------------------------------------------------
static int PickUnit(Vector3 ground, int faction, float radius)
{
    int best = -1;
    float bestDist = radius;
    for (int k = 0; k < s_activeCount; k++)
    {
        int i = s_active[k];
        Unit *u = &world.units[i];
        if (faction >= 0 && u->faction != faction) continue;

        float d = DistXZ(u->pos, ground);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

// Nearest building to a ground point within a grab radius; faction -1 = any.
static int PickBuilding(Vector3 ground, int faction, float radius)
{
    int best = -1;
    float bestDist = radius;
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        Building *b = &world.buildings[i];
        if (!b->active) continue;
        if (faction >= 0 && b->faction != faction) continue;

        float d = DistXZ(b->pos, ground);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

static int PickNode(Vector3 ground, float radius)
{
    int best = -1;
    float bestDist = radius;
    for (int i = 0; i < STRAT_MAX_NODES; i++)
    {
        ResourceNode *n = &world.nodes[i];
        if (!n->active) continue;

        float d = DistXZ(n->pos, ground);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

// Nearest active node of a kind (-1 = any) within radius; -1 when none.
int StrategyNearestNodeOfKind(Vector3 pos, int nodeKind, float radius)
{
    int best = -1;
    float bestDist = radius;
    for (int i = 0; i < STRAT_MAX_NODES; i++)
    {
        ResourceNode *n = &world.nodes[i];
        if (!n->active) continue;
        if (nodeKind >= 0 && (int)n->kind != nodeKind) continue;

        float d = DistXZ(n->pos, pos);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

// The node kind a dropoff building's accepted resource comes from, or -1.
// (WOOD->tree, STONE->rock, FOOD->wheat; corpses aren't a gather target.)
static int GatherNodeForBuilding(const BuildingDef *bd)
{
    if (bd->accepts[RES_WOOD])  return NODE_TREE;
    if (bd->accepts[RES_STONE]) return NODE_ROCK;
    if (bd->accepts[RES_FOOD])  return NODE_WHEAT;
    return -1;
}

// Assign a worker to work a gathering building: farms/forestries tend, town
// halls (accept everything) gather the nearest node of ANY kind, single-
// resource dropoffs gather that resource's node kind. Search is limited to
// STRAT_AUTO_GATHER_RANGE around the building. Returns false (worker left to
// the caller) when the building isn't a gatherer or nothing is in range.
static bool WorkerAutoGatherForBuilding(Unit *u, int bldIndex)
{
    if (bldIndex < 0) return false;
    Building *b = &world.buildings[bldIndex];
    if (!b->active || b->underConstruction) return false;

    const BuildingDef *bd = StrategyBuildingDef(b->kind);

    // Farm / forestry: reuse the tend (plant + harvest) behavior.
    if (bd->tendNode >= 0)
    {
        StrategyOrderFarm(u, bldIndex);
        return true;
    }

    // Town hall accepts all three -> gather whatever node is nearest.
    // A single-resource dropoff gathers only its own node kind.
    bool acceptsAll = bd->accepts[RES_WOOD] && bd->accepts[RES_STONE] &&
                      bd->accepts[RES_FOOD];
    int nodeKind = acceptsAll ? -1 : GatherNodeForBuilding(bd);
    if (!acceptsAll && nodeKind < 0) return false;     // not a gatherer

    int node = StrategyNearestNodeOfKind(b->pos, nodeKind, STRAT_AUTO_GATHER_RANGE);
    if (node < 0) return false;
    StrategyOrderGather(u, node);
    return true;
}

// ----------------------------------------------------------------------------
//  Worker job queue: build / repair / gather chained by Shift-RMB. One typed
//  queue, one dispatcher, popped by every terminal worker state.
// ----------------------------------------------------------------------------
// Turn one job into a concrete order (reuses the plain StrategyOrder* funcs).
static void WorkerDispatchJob(Unit *u, WorkerJob job)
{
    switch (job.kind)
    {
        case WJOB_BUILD:  StrategyOrderBuild(u, job.building);          break;
        case WJOB_REPAIR: StrategyOrderRepair(u, job.building);         break;
        case WJOB_GATHER: WorkerAutoGatherForBuilding(u, job.building); break;
    }
}

// True while a queued job still has something to do (skip dead/finished ones).
static bool WorkerJobStillValid(WorkerJob job)
{
    if (job.building < 0) return false;
    Building *b = &world.buildings[job.building];
    if (!b->active) return false;
    switch (job.kind)
    {
        case WJOB_BUILD:  return b->underConstruction;
        case WJOB_REPAIR: return !b->underConstruction && b->hp < b->maxHp;
        case WJOB_GATHER: return !b->underConstruction;
    }
    return false;
}

// Pop the front of the queue (shifting the rest down), skipping jobs whose
// target went invalid, and dispatch the first live one. Returns whether a job
// was started; false means the queue drained with nothing left to do.
static bool WorkerStartNextJob(Unit *u)
{
    while (u->jobQueueCount > 0)
    {
        WorkerJob job = u->jobQueue[0];
        for (int i = 1; i < u->jobQueueCount; i++) u->jobQueue[i - 1] = u->jobQueue[i];
        u->jobQueueCount--;

        if (!WorkerJobStillValid(job)) continue;
        WorkerDispatchJob(u, job);
        return true;
    }
    return false;
}

// Issue a worker job. append=false starts it now and clears the queue;
// append=true chains it after the current job, or starts it now if the worker
// isn't already running a queueable job. Every terminal state pops the queue.
void StrategyOrderJob(Unit *u, WorkerJobKind kind, int bldIndex, bool append)
{
    WorkerJob job = { kind, bldIndex };

    if (!append)
    {
        u->jobQueueCount = 0;
        WorkerDispatchJob(u, job);
        return;
    }

    // Chain only onto an in-progress build/repair; anything else starts now.
    bool queueable = (u->state == UNIT_BUILD || u->state == UNIT_REPAIR);
    if (!queueable)
    {
        WorkerDispatchJob(u, job);
        return;
    }
    if (u->targetBuilding == bldIndex && u->jobQueueCount == 0) return;  // dup
    if (u->jobQueueCount >= UNIT_MAX_JOB_QUEUE) return;
    u->jobQueue[u->jobQueueCount++] = job;
}

// ----------------------------------------------------------------------------
//  Population + training + building destruction (shared by GUI and AI)
// ----------------------------------------------------------------------------
int StrategyPopCap(int faction)
{
    int cap = 0;
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        Building *b = &world.buildings[i];
        if (b->active && !b->underConstruction && b->faction == faction)
        {
            cap += StrategyBuildingDef(b->kind)->popCap;
        }
    }
    return cap;
}

int StrategyPopUsed(int faction)
{
    int used = 0;
    for (int k = 0; k < s_activeCount; k++)
    {
        if (world.units[s_active[k]].faction == faction) used++;
    }
    return used;
}

// Composition census for the pop panel. kind < 0 counts every kind.
int StrategyCountUnits(int faction, int kind)
{
    int n = 0;
    for (int k = 0; k < s_activeCount; k++)
    {
        Unit *u = &world.units[s_active[k]];
        if (u->faction != faction) continue;
        if (kind >= 0 && (int)u->kind != kind) continue;
        n++;
    }
    return n;
}

int StrategyCountIdleWorkers(int faction)
{
    int n = 0;
    for (int k = 0; k < s_activeCount; k++)
    {
        Unit *u = &world.units[s_active[k]];
        if (u->faction == faction &&
            u->kind == KIND_WORKER && u->state == UNIT_IDLE) n++;
    }
    return n;
}

// Select the player's idle worker nearest the camera focus, deselect the rest,
// and pan the camera onto it. No-op when there is no idle worker.
void StrategySelectNearestIdleWorker(void)
{
    Vector3 focus = (Vector3){ world.camFocus.x, 0.0f, world.camFocus.y };
    int best = -1;
    float bestDist = 1000000.0f;
    for (int k = 0; k < s_activeCount; k++)
    {
        int i = s_active[k];
        Unit *u = &world.units[i];
        if (u->faction != 0) continue;
        if (u->kind != KIND_WORKER || u->state != UNIT_IDLE) continue;

        float d = DistXZ(u->pos, focus);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    if (best < 0) return;

    for (int k = 0; k < s_activeCount; k++)
    {
        Unit *u = &world.units[s_active[k]];
        if (u->faction == 0) u->selected = false;
    }
    world.units[best].selected = true;
    SelectionTouch();
    world.selectedBuilding = -1;

    world.camFocus = (Vector2){ world.units[best].pos.x, world.units[best].pos.z };
    CameraRefresh();
    EffectSpawn(FX_RING, world.units[best].pos, GREEN);
}

// Refund a to-be-cancelled trainee's paid cost back to the faction stockpile.
static void TrainRefund(Building *b, UnitKind kind)
{
    const BuildingDef *bd = StrategyBuildingDef(b->kind);
    const UnitDef *ud = StrategyUnitDef(kind);
    for (int r = 0; r < RES_COUNT; r++)
        world.stockpile[b->faction][r] += (int)ceilf((float)ud->cost[r]*bd->trainCostMul);
}

// Cancel one trainee: the last queued one if any, else the active trainee.
// Refunds its cost. Returns false when there was nothing to cancel.
bool StrategyTrainCancel(int bldIndex)
{
    Building *b = &world.buildings[bldIndex];
    if (!b->active) return false;

    if (b->trainQueueCount > 0)
    {
        TrainRefund(b, b->trainQueue[--b->trainQueueCount]);
        return true;
    }
    if (b->trainKind >= 0)
    {
        TrainRefund(b, (UnitKind)b->trainKind);
        b->trainKind     = -1;
        b->trainProgress = 0.0f;
        return true;
    }
    return false;
}

// Pop the next queued kind into the active trainee slot (queue shifts down).
// Assumes the building is idle and off cooldown; cost was paid on enqueue.
static void TrainDequeue(Building *b)
{
    if (b->trainQueueCount <= 0) return;
    b->trainKind     = (int)b->trainQueue[0];
    b->trainProgress = 0.0f;
    for (int i = 1; i < b->trainQueueCount; i++) b->trainQueue[i - 1] = b->trainQueue[i];
    b->trainQueueCount--;
}

// Enqueue a trainee: validate kind/pop/cost, pay up front, then either start
// immediately (idle + off cooldown) or append to the queue. The pop check
// counts already-queued trainees so a full queue can't overshoot the cap.
bool StrategyTrainStart(int bldIndex, UnitKind kind)
{
    Building *b = &world.buildings[bldIndex];
    if (!b->active || b->underConstruction) return false;

    const BuildingDef *bd = StrategyBuildingDef(b->kind);
    bool allowed = false;
    for (int i = 0; i < bd->trainableCount; i++)
    {
        if (bd->trainable[i] == kind) allowed = true;
    }
    if (!allowed) return false;

    // Everything already committed to this building: the active trainee plus
    // whatever is queued behind it. Keeps the pop cap honest for the queue.
    int committed = (b->trainKind >= 0 ? 1 : 0) + b->trainQueueCount;
    if (b->trainQueueCount >= BLD_MAX_QUEUE) return false;
    if (StrategyPopUsed(b->faction) + committed + 1 > StrategyPopCap(b->faction)) return false;

    const UnitDef *ud = StrategyUnitDef(kind);
    int cost[RES_COUNT];
    for (int r = 0; r < RES_COUNT; r++)
    {
        cost[r] = (int)ceilf((float)ud->cost[r]*bd->trainCostMul);
        if (world.stockpile[b->faction][r] < cost[r]) return false;
    }
    for (int r = 0; r < RES_COUNT; r++)
    {
        world.stockpile[b->faction][r] -= cost[r];
    }

    if (b->trainKind < 0 && b->trainCooldown <= 0.0f)
    {
        b->trainKind     = (int)kind;
        b->trainProgress = 0.0f;
    }
    else
    {
        b->trainQueue[b->trainQueueCount++] = kind;
    }
    return true;
}

// A faction is defeated when it has NO critical building AND no workers left
// (houses alone can't rebuild an economy). Called after every building
// destroy/sell and every unit kill.
static void CheckGameOver(void)
{
    if (world.gameOver >= 0) return;

    for (int f = 0; f < STRAT_FACTIONS; f++)
    {
        bool critical = false;
        bool workers  = false;
        for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
        {
            Building *b = &world.buildings[i];
            if (b->active && b->faction == f &&
                StrategyBuildingDef(b->kind)->critical)
            {
                critical = true;
                break;
            }
        }
        for (int k = 0; k < s_activeCount; k++)
        {
            Unit *u = &world.units[s_active[k]];
            if (u->faction == f && u->kind == KIND_WORKER)
            {
                workers = true;
                break;
            }
        }
        if (!critical && !workers)
        {
            world.gameOver = 1 - f;     // the OTHER faction wins
            return;
        }
    }
}

static void BuildingDestroy(int index)
{
    Building *b = &world.buildings[index];
    b->active = false;
    if (world.selectedBuilding == index) world.selectedBuilding = -1;

    // Unblock the nav grid before the effects fire - a destroyed building that
    // keeps blocking is a wall you can see through.
    if (s_navReady)
    {
        NavStampBuilding(b, false);
        float bh = BuildingHalfWorld(b->kind);
        NavClearArea(b->pos.x, b->pos.z, bh, bh);
    }

    EffectSpawn(FX_RING, b->pos, strategyFactionColor[b->faction]);
    EffectSpawn(FX_FLASH, (Vector3){ b->pos.x, 0.8f, b->pos.z }, RAYWHITE);
    for (int i = 0; i < 6; i++)
    {
        EffectSpawn(FX_PUFF, (Vector3){ b->pos.x, 0.8f, b->pos.z }, DARKGRAY);
    }
    CheckGameOver();
}

// Sell a building back: refund refundRate (+ the player-facing difficulty
// bonus) of every cost component, floored. The slot just deactivates.
bool StrategySellBuilding(int index)
{
    Building *b = &world.buildings[index];
    if (!b->active) return false;

    const BuildingDef *bd = StrategyBuildingDef(b->kind);
    float rate = bd->refundRate + world.mods[b->faction].refundBonus;
    for (int r = 0; r < RES_COUNT; r++)
    {
        world.stockpile[b->faction][r] += (int)floorf((float)bd->cost[r]*rate);
    }
    b->active = false;
    if (world.selectedBuilding == index) world.selectedBuilding = -1;

    if (s_navReady)
    {
        NavStampBuilding(b, false);
        float bh = BuildingHalfWorld(b->kind);
        NavClearArea(b->pos.x, b->pos.z, bh, bh);
    }

    EffectSpawn(FX_RING, b->pos, GOLD);
    for (int i = 0; i < 3; i++)
    {
        EffectSpawn(FX_PUFF, (Vector3){ b->pos.x, 0.8f, b->pos.z }, LIGHTGRAY);
    }
    CheckGameOver();    // selling your last town hall can lose the game
    return true;
}

// Count active nodes of a kind within radius of a point (node-tend cap).
static int NodesNear(NodeKind kind, Vector3 pos, float radius)
{
    int n = 0;
    for (int i = 0; i < STRAT_MAX_NODES; i++)
    {
        ResourceNode *nd = &world.nodes[i];
        if (nd->active && nd->kind == kind && DistXZ(nd->pos, pos) <= radius) n++;
    }
    return n;
}

// A ground spot is free to plant on when it is clear of every node, building
// and off the map edge. Used to scatter tended nodes without overlap.
static bool PlantSpotClear(Vector3 pos)
{
    if (fabsf(pos.x) > world.groundHalfX - 1.0f ||
        fabsf(pos.z) > world.groundHalfZ - 1.0f) return false;
    if (!MapBuildableAt(pos)) return false;     // no farms in the lake
    for (int i = 0; i < STRAT_MAX_NODES; i++)
        if (world.nodes[i].active &&
            DistXZ(world.nodes[i].pos, pos) < STRAT_TEND_SPACING) return false;
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
        if (world.buildings[i].active &&
            DistXZ(world.buildings[i].pos, pos) < STRAT_TEND_SPACING) return false;
    return true;
}

// Pick a free plant spot near a building's tend area, or fall back to the
// building itself when the area is packed. A few random tries is plenty.
static Vector3 PlantSpotNear(Vector3 center)
{
    for (int tries = 0; tries < 12; tries++)
    {
        Vector3 p = center;
        p.x += (float)GetRandomValue(-100, 100)*0.01f*STRAT_TEND_RANGE;
        p.z += (float)GetRandomValue(-100, 100)*0.01f*STRAT_TEND_RANGE;
        if (PlantSpotClear(p)) return p;
    }
    return center;
}

// Quarry: spend providence to conjure a fresh stone node beside the quarry.
// Returns false when the building isn't a finished quarry or providence is short.
bool StrategyQuarrySpawnStone(int bldIndex)
{
    Building *b = &world.buildings[bldIndex];
    if (!b->active || b->underConstruction || b->kind != BLD_QUARRY) return false;
    if (world.stockpile[b->faction][RES_PROVIDENCE] < STRAT_QUARRY_STONE_PROV)
        return false;

    world.stockpile[b->faction][RES_PROVIDENCE] -= STRAT_QUARRY_STONE_PROV;
    Vector3 p = b->pos;
    p.x += (float)GetRandomValue(-100, 100)*0.01f*STRAT_QUARRY_STONE_SPREAD;
    p.z += (float)GetRandomValue(-100, 100)*0.01f*STRAT_QUARRY_STONE_SPREAD;
    NodeSpawn(NODE_ROCK, p, STRAT_QUARRY_STONE_AMOUNT);
    EffectSpawn(FX_RING, p, GRAY);
    EffectSpawn(FX_FLASH, (Vector3){ p.x, 0.6f, p.z }, RAYWHITE);
    return true;
}

// Advance every in-progress trainee; on completion spawn the unit beside the
// building (separation shoves crowds apart) and start the anti-spam cooldown.
// Scaffolds do nothing here; the forestry auto-plants trees on its own timer.
static void BuildingsUpdate(float dt)
{
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        Building *b = &world.buildings[i];
        if (!b->active || b->underConstruction) continue;

        if (b->trainCooldown > 0.0f) b->trainCooldown -= dt;

        // Idle and off cooldown with a queue waiting: start the next trainee.
        if (b->trainKind < 0 && b->trainCooldown <= 0.0f && b->trainQueueCount > 0)
            TrainDequeue(b);
        if (b->trainKind < 0) continue;

        b->trainProgress += dt;
        if (b->trainProgress >= StrategyUnitDef((UnitKind)b->trainKind)->trainTime)
        {
            const BuildingDef *bd = StrategyBuildingDef(b->kind);
            Vector3 spawn = (Vector3){ b->pos.x + 1.6f, 0.0f, b->pos.z + 1.2f };
            Unit *u = UnitSpawn(b->faction, (UnitKind)b->trainKind, spawn);
            if (u != NULL)
            {
                // Training-building buffs (identity today: upgrade hook).
                u->maxHp  *= bd->buffHpMul;
                u->hp      = u->maxHp;
                u->damage *= bd->buffDmgMul;
                // Rally point: walk the fresh unit to the flag if one is set.
                if (b->hasRally) StrategyOrderMove(u, b->rally);
            }
            EffectSpawn(FX_RING, spawn, strategyFactionColor[b->faction]);
            EffectSpawn(FX_FLASH, (Vector3){ spawn.x, 0.6f, spawn.z }, RAYWHITE);
            b->trainKind     = -1;
            b->trainProgress = 0.0f;
            b->trainCooldown = bd->trainCooldown;
        }
    }
}

// ----------------------------------------------------------------------------
//  Input: camera, building placement, selection, orders
// ----------------------------------------------------------------------------
static void CameraPanZoom(void)
{
    float dt = GetFrameTime();
    float pan = 12.0f*world.camZoom*dt;

    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP))    world.camFocus.y -= pan;
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN))  world.camFocus.y += pan;
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT))  world.camFocus.x -= pan;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) world.camFocus.x += pan;
    world.camFocus.x = Clamp(world.camFocus.x, -world.groundHalfX, world.groundHalfX);
    world.camFocus.y = Clamp(world.camFocus.y, -world.groundHalfZ, world.groundHalfZ);

    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f)
    {
        world.camZoom *= 1.0f - wheel*0.1f;
        world.camZoom = Clamp(world.camZoom, 0.35f, 1.45f);
    }
    CameraRefresh();
}

// Placement validity: affordable, on the ground, and clear of everything.
// Can the faction currently pay for this building?
static bool Affordable(int faction, BuildingKind kind)
{
    for (int r = 0; r < RES_COUNT; r++)
        if (world.stockpile[faction][r] < StrategyBuildingDef(kind)->cost[r]) return false;
    return true;
}

// Placement, rewritten against real footprints.
//
// The old version compared CENTRE DISTANCES against three hand-typed numbers -
// 2.4 between buildings, 1.6 to a node, 1.2 to a unit - which are clearance
// values, not sizes. Nothing tied them to how big anything actually is, so a
// 3x3 town hall and a 1x1 house could be placed 2.4 apart and visually
// intersect. Now every rule is derived from footprintX/Z, and the rule the
// player is judged by is the same one the nav grid is stamped with.
static bool PlacementValid(int faction, BuildingKind kind, Vector3 pos)
{
    if (!Affordable(faction, kind)) return false;

    float half = BuildingHalfWorld(kind);
    if (fabsf(pos.x) > world.groundHalfX - half ||
        fabsf(pos.z) > world.groundHalfZ - half) return false;

    // Authored terrain: a map may forbid building on water, cliffs and voids.
    // Checked over the WHOLE footprint, not just the centre tile - a 3x3 hall
    // whose centre is on grass but whose corner hangs over a lake was legal
    // before, and then permanently blocked three tiles of water.
    int hx, hz, cx, cz;
    BuildingHalfTiles(kind, &hx, &hz);
    SpWorldToTile(&s_nav, roundf(pos.x), roundf(pos.z), &cx, &cz);

    for (int z = cz - hz; z <= cz + hz; z++)
    {
        for (int x = cx - hx; x <= cx + hx; x++)
        {
            // Buildability, not passability: shallow water is walkable but not
            // buildable, and the authored flags already carry that distinction.
            if (world.map != NULL)
            {
                if (!SgmTileBuildable(world.map, x, z)) return false;
            }
            else if (!SpGridInBounds(&s_nav, x, z)) return false;
        }
    }

    // Building-vs-building: the two footprints must not touch, plus one tile of
    // walkable gap so a worker can get between them rather than being sealed in
    // by a wall of houses.
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        const Building *b = &world.buildings[i];
        if (!b->active) continue;
        float gap = half + BuildingHalfWorld(b->kind) + 1.0f;
        if (fabsf(b->pos.x - pos.x) < gap && fabsf(b->pos.z - pos.z) < gap) return false;
    }

    // Nodes and units are round and roughly one unit across, so a centre
    // distance is the right test for them - just measured from the building's
    // EDGE now instead of its centre.
    for (int i = 0; i < STRAT_MAX_NODES; i++)
    {
        if (world.nodes[i].active && DistXZ(world.nodes[i].pos, pos) < half + 0.6f) return false;
    }
    for (int k = 0; k < s_activeCount; k++)
    {
        if (DistXZ(world.units[s_active[k]].pos, pos) < half + STRAT_UNIT_RADIUS) return false;
    }
    return true;
}

// Validate + pay + place in one call. Shared by the player's placement
// click and the enemy AI's house building.
bool StrategyTryBuild(int faction, BuildingKind kind, Vector3 pos)
{
    if (!PlacementValid(faction, kind, pos)) return false;

    for (int r = 0; r < RES_COUNT; r++)
    {
        world.stockpile[faction][r] -= StrategyBuildingDef(kind)->cost[r];
    }
    // Player buildings go up as scaffolds a worker must finish; the AI has no
    // build behavior, so its buildings spawn complete.
    BuildingSpawn(kind, faction, pos, faction == 0);
    EffectSpawn(FX_RING, pos, RAYWHITE);
    for (int i = 0; i < 3; i++) EffectSpawn(FX_PUFF, (Vector3){ pos.x, 0.8f, pos.z }, LIGHTGRAY);
    return true;
}

// Ghost position: mouse ground point snapped to the 1-unit grid.
static bool PlacementGhost(Vector3 *out)
{
    Vector3 ground;
    if (!MouseGroundPoint(&ground)) return false;
    out->x = roundf(ground.x);
    out->y = 0.0f;
    out->z = roundf(ground.z);
    return true;
}

static void PlacementInput(void)
{
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
    {
        world.placing = -1;     // cancel
        return;
    }
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || MouseOnGui()) return;

    Vector3 pos;
    BuildingKind kind = (BuildingKind)world.placing;
    if (!PlacementGhost(&pos)) return;
    if (!StrategyTryBuild(0, kind, pos)) return;

    // Hold Shift to keep placing more of the same building; a plain click
    // (or running out of resources) exits placement mode.
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (!shift || !Affordable(0, kind)) world.placing = -1;
}

// Select every own unit of `kind` whose projection lands within the visible
// game canvas (double-click "select all of type"). shift extends the current
// selection instead of replacing it.
static void SelectOnScreenOfKind(UnitKind kind, bool shift)
{
    Vector2 gameSize = ScreenStateTargetSize();
    Rectangle screen = { 0.0f, 0.0f, gameSize.x, gameSize.y };
    for (int k = 0; k < s_activeCount; k++)
    {
        Unit *u = &world.units[s_active[k]];
        if (u->faction != 0) continue;
        if (u->kind != kind)
        {
            if (!shift) u->selected = false;
            continue;
        }
        if (CheckCollisionPointRec(WorldToGame(u->pos), screen)) u->selected = true;
        else if (!shift) u->selected = false;
    }
    SelectionTouch();
}

static void SelectionInput(void)
{
    Vector2 mouse = MouseGame();
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        if (MouseOnGui())
        {
            // The build bar owns this click: without this flag the stale
            // dragStart would fake a drag and the release (the same one
            // that fires GuiButton) would box-select behind the panel.
            world.pressInWorld = false;
            world.dragging     = false;
            return;
        }
        world.pressInWorld = true;
        world.dragStart    = mouse;
        world.dragging     = false;
    }
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && world.pressInWorld && !world.dragging)
    {
        if (Vector2Distance(mouse, world.dragStart) > 6.0f) world.dragging = true;
    }
    if (!IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) return;
    if (!world.pressInWorld) return;    // this click belongs to the GUI
    world.pressInWorld = false;

    if (world.dragging)
    {
        // Box select: player units whose SCREEN projection is inside the rect.
        world.selectedBuilding = -1;
        Rectangle rect = {
            fminf(world.dragStart.x, mouse.x), fminf(world.dragStart.y, mouse.y),
            fabsf(mouse.x - world.dragStart.x), fabsf(mouse.y - world.dragStart.y),
        };
        for (int k = 0; k < s_activeCount; k++)
        {
            Unit *u = &world.units[s_active[k]];
            if (u->faction != 0) continue;

            bool inside = CheckCollisionPointRec(WorldToGame(u->pos), rect);
            if (inside)      u->selected = true;
            else if (!shift) u->selected = false;
        }
        SelectionTouch();
        world.dragging = false;
        return;
    }

    // Plain click: select the player unit under the cursor; with no unit hit,
    // try an own building; otherwise deselect all. Unit and building
    // selection are mutually exclusive.
    Vector3 ground;
    if (!MouseGroundPoint(&ground)) return;
    int hit = PickUnit(ground, 0, 0.7f);

    if (hit < 0)
    {
        world.selectedBuilding = PickBuilding(ground, 0, 1.4f);
        if (world.selectedBuilding >= 0)
        {
            for (int k = 0; k < s_activeCount; k++) world.units[s_active[k]].selected = false;
            SelectionTouch();
            EffectSpawn(FX_RING, world.buildings[world.selectedBuilding].pos, GREEN);
            return;
        }
    }
    else world.selectedBuilding = -1;

    // Double-click a unit -> select all on-screen units of that kind. Detected
    // by a second hit on the same-kind unit within the click window.
    static double lastClickTime = -1.0;
    static int    lastClickKind = -1;
    if (hit >= 0)
    {
        double now = GetTime();
        UnitKind kind = world.units[hit].kind;
        bool dbl = (now - lastClickTime < 0.3) && (lastClickKind == (int)kind);
        lastClickTime = now;
        lastClickKind = (int)kind;
        if (dbl)
        {
            SelectOnScreenOfKind(kind, shift);
            EffectSpawn(FX_RING, world.units[hit].pos, GREEN);
            return;
        }
    }
    else lastClickKind = -1;

    for (int k = 0; k < s_activeCount; k++)
    {
        int i = s_active[k];
        Unit *u = &world.units[i];
        if (u->faction != 0) continue;
        if (i == hit)    u->selected = true;
        else if (!shift) u->selected = false;
    }
    SelectionTouch();
    if (hit >= 0) EffectSpawn(FX_RING, world.units[hit].pos, GREEN);
}

// One selected unit's right-click routing. `hostile` covers enemy units AND
// animals (hunting); soldiers can't gather/farm/build, so those fall back to
// move. ownGather/ownScaffold/ownRepair are worker-only own-building jobs, all
// routed through StrategyOrderJob so Shift chains ANY of them (build, repair,
// or gather-assign) onto the worker's queue instead of replacing.
static void OrderUnitAt(Unit *u, int hostile, int enemyBld, int node,
                        int ownGather, int ownScaffold, int ownRepair,
                        bool shift, Vector3 ground)
{
    bool worker = (u->kind == KIND_WORKER);

    if (hostile >= 0)                     StrategyOrderAttack(u, hostile);
    else if (enemyBld >= 0)               StrategyOrderAttackBuilding(u, enemyBld);
    else if (node >= 0 && worker)         StrategyOrderGather(u, node);
    else if (ownScaffold >= 0 && worker)  StrategyOrderJob(u, WJOB_BUILD,  ownScaffold, shift);
    else if (ownRepair >= 0 && worker)    StrategyOrderJob(u, WJOB_REPAIR, ownRepair,   shift);
    else if (ownGather >= 0 && worker)    StrategyOrderJob(u, WJOB_GATHER, ownGather,   shift);
    else                                  StrategyOrderMove(u, ground);
}

// Right click: hostile unit/animal > enemy building > resource node >
// own building job > plain move - checked in that priority so a click near a
// tree still prefers the deer standing beside it.
static void OrderInput(void)
{
    if (!IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) || MouseOnGui()) return;

    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    Vector3 ground;
    if (!MouseGroundPoint(&ground)) return;

    // A selected training building takes the right-click as a rally order:
    // new trainees will walk to this spot instead of standing at the door.
    if (world.selectedBuilding >= 0)
    {
        Building *b = &world.buildings[world.selectedBuilding];
        if (StrategyBuildingDef(b->kind)->trainableCount > 0)
        {
            b->rally    = ground;
            b->hasRally = true;
            EffectSpawn(FX_RING, ground, GREEN);
            return;
        }
    }

    // Any non-player unit is a valid attack/hunt target.
    int hostile = PickUnit(ground, 1, 0.8f);
    if (hostile < 0) hostile = PickUnit(ground, FACTION_NEUTRAL, 0.8f);

    int enemyBld = (hostile < 0) ? PickBuilding(ground, 1, 1.4f) : -1;
    int node     = (hostile < 0 && enemyBld < 0) ? PickNode(ground, 0.9f) : -1;

    // Own building under the cursor -> build (scaffold), repair (damaged), or
    // gather work (finished tend/dropoff), in that priority. Worker-only;
    // OrderUnitAt gates by kind. A gatherer is a tend building OR any dropoff
    // (Logging/Quarry/Town Hall) - WorkerAutoGatherForBuilding sorts out which.
    int ownGather = -1, ownScaffold = -1, ownRepair = -1;
    if (hostile < 0 && enemyBld < 0 && node < 0)
    {
        int own = PickBuilding(ground, 0, 1.4f);
        if (own >= 0)
        {
            Building *b = &world.buildings[own];
            const BuildingDef *bd = StrategyBuildingDef(b->kind);
            bool gatherer = bd->tendNode >= 0 || bd->accepts[RES_WOOD] ||
                            bd->accepts[RES_STONE] || bd->accepts[RES_FOOD];
            if (b->underConstruction)      ownScaffold = own;
            else if (b->hp < b->maxHp)      ownRepair   = own;
            else if (gatherer)              ownGather   = own;
        }
    }

    int selCount = 0;
    const int *sel = SelectedUnits(&selCount);

    bool any = false;

    // A PLAIN GROUND MOVE IS THE ONE ORDER THAT BECOMES A GROUP ORDER. Every
    // other branch already targets a specific object - a unit, a node, a
    // building - so there is nothing to spread the group over and each unit
    // wants the identical destination anyway.
    //
    // For ground, the click is a POINT and the group needs an AREA: 500 units
    // sent to one spot are being asked to stand where about six fit. Phase 2
    // measured what that costs even with perfect settling - 300 units to one
    // point come to rest 7,316 overlapping pairs deep. The same steering with
    // formation slots gives zero.
    bool plainMove = (hostile < 0 && enemyBld < 0 && node < 0 &&
                      ownGather < 0 && ownScaffold < 0 && ownRepair < 0);

    // AN ATTACK ORDER AT DISTANCE IS A MARCH, AND MARCHES GET FORMATIONS.
    // Sending every selected unit at one enemy is what produces the overlapping
    // spearhead: they converge on a single point and stack, and a stack fights
    // far better than it should because only its front rank can be hit.
    //
    // Only at DISTANCE, though. Once the enemy is already close the players
    // means "hit that now", and forming up first would be a visible delay in
    // exactly the moment it is least wanted - so a near target keeps the direct
    // per-unit order, and the units break off into the fight the moment they
    // arrive anyway.
    bool attackMarch = false;
    if (hostile >= 0 && !shift && selCount > 1)
    {
        Vector3 c = { 0 };
        int n = 0;
        for (int k = 0; k < selCount; k++)
        {
            if (world.units[sel[k]].faction != 0) continue;
            c.x += world.units[sel[k]].pos.x;
            c.z += world.units[sel[k]].pos.z;
            n++;
        }
        if (n > 1)
        {
            c.x /= (float)n;
            c.z /= (float)n;
            attackMarch = (DistXZ(c, world.units[hostile].pos) > STRAT_FORM_MARCH_DIST);
        }
    }

    if ((plainMove || attackMarch) && !shift)
    {
        // Gather the player's own selected units and hand them over as a group.
        // Shift is excluded: it means "queue this job", which is a per-unit
        // notion the formation layer has nothing to say about.
        static int s_groupBuf[STRAT_MAX_UNITS];
        int n = 0;
        for (int k = 0; k < selCount; k++)
        {
            if (world.units[sel[k]].faction != 0) continue;
            s_groupBuf[n++] = sel[k];
        }

        // An attack march forms up on the TARGET's position rather than the
        // clicked ground. The units break off and engage as they close - under
        // whatever the current behaviour says - so this only has to get them
        // there as a block rather than as a queue.
        Vector3 dest = attackMarch ? world.units[hostile].pos : ground;
        if (n > 0) { StrategyOrderMoveGroup(s_groupBuf, n, dest); any = true; }
    }
    else
    {
        for (int k = 0; k < selCount; k++)
        {
            Unit *u = &world.units[sel[k]];
            if (u->faction != 0) continue;

            OrderUnitAt(u, hostile, enemyBld, node, ownGather, ownScaffold, ownRepair,
                        shift, ground);
            any = true;
        }
    }
    if (!any) return;

    // Order feedback: red ring on an attack target, yellow on a resource,
    // green on a build/repair/gather target, lime ripple on plain ground.
    if (hostile >= 0)         EffectSpawn(FX_RING, world.units[hostile].pos, RED);
    else if (enemyBld >= 0)   EffectSpawn(FX_RING, world.buildings[enemyBld].pos, RED);
    else if (node >= 0)       EffectSpawn(FX_RING, world.nodes[node].pos, YELLOW);
    else if (ownScaffold >= 0)EffectSpawn(FX_RING, world.buildings[ownScaffold].pos, GREEN);
    else if (ownRepair >= 0)  EffectSpawn(FX_RING, world.buildings[ownRepair].pos, GREEN);
    else if (ownGather >= 0)  EffectSpawn(FX_RING, world.buildings[ownGather].pos, GREEN);
    else                      EffectSpawn(FX_RING, ground, LIME);
}

// Control groups: ctrl+1..3 stamps the current selection as that group
// (removing units no longer selected); bare 1..3 recalls the group.
static void ControlGroupInput(void)
{
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

    for (int g = 1; g <= 3; g++)
    {
        if (!IsKeyPressed(KEY_ZERO + g)) continue;

        for (int k = 0; k < s_activeCount; k++)
        {
            Unit *u = &world.units[s_active[k]];
            if (u->faction != 0) continue;

            if (ctrl)
            {
                if (u->selected)               u->controlGroup = g;
                else if (u->controlGroup == g) u->controlGroup = 0;
            }
            else u->selected = (u->controlGroup == g);
        }
        if (!ctrl) { SelectionTouch(); world.selectedBuilding = -1; }
    }
}

// Formation hotkeys. Letters, per the house idiom - and two keys cycling two
// independent settings rather than one key cycling their product, because shape
// and break-off behaviour are orthogonal: fifteen combinations behind one key
// would be unusable.
static void FormationInput(void)
{
    if (IsKeyPressed(KEY_F))
        StrategyFormationShapeSet((FormationShape)((StrategyFormationShape() + 1) % FORM_COUNT));

    if (IsKeyPressed(KEY_V))
        StrategyFormationBehaviorSet(
            (FormationBehavior)((StrategyFormationBehavior() + 1) % FORM_BEHAVIOR_COUNT));
}

void StrategyWorldHandleInput(void)
{
    CameraPanZoom();
    ControlGroupInput();
    FormationInput();

    if (world.placing >= 0)
    {
        PlacementInput();
        return;     // placement mode owns the mouse
    }
    SelectionInput();
    OrderInput();
}

// ----------------------------------------------------------------------------
//  Unit behavior (faction-agnostic)
// ----------------------------------------------------------------------------
// Straight-line steering, unchanged in behaviour and now one line: the body
// moved to strategy_move.c so that pathed and unpathed movement share a single
// implementation of "step toward a point and face it". The name stays because
// five call sites here legitimately want exactly this and not a path - see the
// classification in strategy_move.c's header comment.
static void MoveToward(Unit *u, Vector3 dest, float dt)
{
    StrategyMoveDirect(u, dest, dt);
}

// ----------------------------------------------------------------------------
//  Arrival
// ----------------------------------------------------------------------------
//  Walk toward a final DESTINATION and decide when the walk is over. Returns
//  true once the unit has settled and the caller should leave the move state.
//
//  This is deliberately NOT what every MoveToward call site wants. A worker
//  closing on a tree, a soldier kiting to its preferred range and a templar
//  shadowing a target are all pursuing something that moves or that has its own
//  proximity test - they want plain steering. Only a walk to a fixed point on
//  the ground needs to decide it has finished, so only those callers get this.
//  Routing all sixteen sites through arrival logic would make workers settle
//  next to a tree they were supposed to chop.
//
//  WHY A PROGRESSION AND NOT A DISTANCE TEST. The old test was one line -
//  within 0.15 units, become idle - and it could not fire in a crowd, because
//  separation holds units at least 0.7 apart. So a pile of units all drove
//  inward forever. Three ways out are needed, because a crowd defeats any
//  single one:
//    - close enough (the ordinary case),
//    - hemmed in by units that have themselves finished (the chokepoint case),
//    - no longer making progress (the everything-else case).
static bool MoveArrive(Unit *u, int index, Vector3 dest, float dt)
{
    Vector3 delta = Vector3Subtract(dest, u->pos);
    delta.y = 0.0f;
    float dist = Vector3Length(delta);

    if (u->arrival == ARRIVE_SETTLED)
    {
        // Settled units stay settled unless shoved WELL clear of the target -
        // past the slowing band, not merely past the stop radius. A tighter
        // threshold makes units on the edge of a crowd flip state every few
        // frames, which looks exactly like the spiral this replaces.
        if (dist > STRAT_ARRIVE_RESUME)
        {
            u->arrival    = ARRIVE_SEEKING;
            u->stallTimer = 0.0f;
            u->lastProgressDist = dist;
            return false;
        }
        return true;
    }

    // -- Settle tests ---------------------------------------------------------
    if (dist <= STRAT_ARRIVE_STOP)
    {
        u->arrival = ARRIVE_SETTLED;
        u->vel = (Vector3){ 0.0f, 0.0f, 0.0f };
        return true;
    }

    // Hemmed in by units that have already finished. Without this a large group
    // ordered through a gap grinds against the back of the ones that made it -
    // they cannot reach the target, cannot settle, and push forever. The crowd
    // count only includes SETTLED neighbours, so this cannot cascade off units
    // that are merely slow.
    if (dist <= STRAT_ARRIVE_SLOW && u->crowd >= STRAT_SETTLE_CROWD)
    {
        u->arrival = ARRIVE_SETTLED;
        u->vel = (Vector3){ 0.0f, 0.0f, 0.0f };
        return true;
    }

    // Making no headway. Measured against distance-to-target rather than
    // distance travelled, because a unit circling its destination is moving
    // fast and getting nowhere - which is precisely the failure being fixed.
    //
    // ONLY WITHIN THE GIVE-UP BAND. Without that gate this rule fires on the
    // rear of any large column: those units are legitimately blocked for well
    // over half a second while the front sorts itself out, and settling them
    // parks a 500-unit order back at its start. The stall rule exists to
    // resolve units that cannot close the LAST few metres, not to abandon ones
    // that have not begun.
    if (dist <= STRAT_ARRIVE_GIVEUP)
    {
        u->stallTimer += dt;
        if (u->stallTimer >= 0.5f)
        {
            if (u->lastProgressDist - dist < STRAT_UNIT_RADIUS*0.5f)
            {
                u->arrival = ARRIVE_SETTLED;
                u->vel = (Vector3){ 0.0f, 0.0f, 0.0f };
                return true;
            }
            u->stallTimer = 0.0f;
            u->lastProgressDist = dist;
        }
    }
    else
    {
        // Outside the band the timer must not accumulate, or a unit that
        // spends ten seconds marching arrives with a primed stall timer and
        // settles on its first blocked frame.
        u->stallTimer       = 0.0f;
        u->lastProgressDist = dist;
    }

    if (dist < 0.001f) return true;

    // -- Drive ----------------------------------------------------------------
    // Speed ramps down across the approach band instead of stopping dead, so a
    // unit does not overshoot and bounce. The floor keeps it from creeping so
    // slowly that the stall test fires on an ordinary approach.
    float speed = u->moveSpeed;
    if (dist < STRAT_ARRIVE_SLOW)
    {
        u->arrival = ARRIVE_SLOWING;
        float t = dist/STRAT_ARRIVE_SLOW;
        speed *= (t < 0.35f) ? 0.35f : t;
    }
    else u->arrival = ARRIVE_SEEKING;

    // THE ARRIVAL TESTS ABOVE MEASURE AGAINST THE DESTINATION; THE DRIVE BELOW
    // STEERS AT THE NEXT WAYPOINT. Keeping those two apart is what lets a path
    // and the settling logic coexist. Deciding "am I there yet" against a
    // waypoint would settle a unit at the first corner of its route; steering
    // at the final goal would walk it into the wall the route exists to avoid.
    //
    // The speed ramp is applied by scaling dt rather than by reimplementing the
    // step, so the slowdown behaves identically whether or not a path is in
    // play - the ramp is an arrival behaviour, not a pathing one.
    float scaledDt = dt*(speed/u->moveSpeed);
    if (index >= 0) StrategyMoveTo(u, index, dest, scaledDt);
    else            StrategyMoveDirect(u, dest, scaledDt);
    return false;
}

// Begin a fresh walk. Every place that sets a new move destination must reset
// the arrival progression, or a unit that settled on its last order arrives
// instantly at its next one.
static void MoveArriveReset(Unit *u)
{
    u->arrival          = ARRIVE_SEEKING;
    u->stallTimer       = 0.0f;
    u->lastProgressDist = 1000000.0f;

    // Any new destination leaves the old formation. StrategyOrderMoveGroup
    // re-stamps this immediately AFTER calling the single-unit order, so a group
    // order still forms up - but every other order (attack, gather, a lone
    // right-click) drops the unit out of its block, which is what the player
    // means by giving it a different job.
    u->formGroup      = -1;
    u->formForming    = false;
    u->formEverFormed = false;
    u->formBrokeOff   = false;
}

// Nearest own building that ACCEPTS what the unit is carrying - wood only
// lands at logging camps / town halls, and so on.
// True when `index` is still a building this unit may deposit into. Used to
// decide whether a cached dropoff choice can be reused: buildings are sold,
// destroyed and completed mid-run, so the cache cannot simply be trusted.
static bool DropoffValid(const Unit *u, int index)
{
    if (index < 0 || index >= STRAT_MAX_BUILDINGS) return false;

    const Building *b = &world.buildings[index];
    return b->active && !b->underConstruction && b->faction == u->faction &&
           StrategyBuildingDef(b->kind)->accepts[u->carryKind];
}

static int NearestDropoff(const Unit *u)
{
    int best = -1;
    float bestDist = 1000000.0f;
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        Building *b = &world.buildings[i];
        if (!b->active || b->underConstruction || b->faction != u->faction) continue;
        if (!StrategyBuildingDef(b->kind)->accepts[u->carryKind]) continue;

        float d = DistXZ(b->pos, u->pos);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

// Cached NearestDropoff. The full search walks every building and runs from
// UNIT_RETURN, which every carrying worker is in for seconds at a time - so at
// scale it was a per-frame scan multiplied by the number of workers. The answer
// only changes when the chosen building stops accepting, so keep it.
//
// NOT NECESSARILY THE NEAREST any more, and that is the deliberate trade: a
// worker keeps walking to the depot it set out for even if a closer one is
// finished mid-trip. That reads as a worker following through rather than as a
// bug, and re-searching every frame to avoid it is what made this expensive.
static int NearestDropoffCached(Unit *u)
{
    if (DropoffValid(u, u->dropoffCache)) return u->dropoffCache;

    u->dropoffCache = NearestDropoff(u);
    return u->dropoffCache;
}

static int NearestHostile(const Unit *u, float range)
{
    int best = -1;
    float bestDist = range;

    // The sight hash, not the active roster. This scan used to walk every unit
    // in the world and it runs from UnitAggroScan, which is the first line of
    // every unit's update - so it was an O(n^2) pass hiding behind a name that
    // sounds like a lookup. At 10,000 units it cost more than separation did.
    int n = SpHashQuery(&s_sightHash, u->pos.x, u->pos.z, range,
                        s_sightScratch, SP_HASH_ITEMS_MAX);

    for (int q = 0; q < n; q++)
    {
        int i = s_sightScratch[q];
        Unit *other = &world.units[i];

        // The hash is rebuilt at the top of the frame, so a unit killed earlier
        // in this same update is still listed. Targeting a corpse would leave
        // the attacker swinging at nothing until its next scan.
        if (!other->active) continue;
        if (other->faction == u->faction) continue;
        if (other->faction == FACTION_NEUTRAL) continue;   // animals aren't hostile

        float d = DistXZ(other->pos, u->pos);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

// Kill a unit. A hunted animal leaves a food corpse node behind, and a worker
// that did the killing immediately starts harvesting it (hunting loop).
// A corpse borrows the unit's look for as long as its DIE animation runs. When
// the pool is full the OLDEST is recycled: a death that cannot be shown is
// better than dropping the newest one, which is the one the player is watching.
static void CorpseSpawn(const Unit *u)
{
    int slot = -1;
    float oldest = -1.0f;

    for (int i = 0; i < STRAT_MAX_CORPSES; i++)
    {
        if (!world.corpses[i].active) { slot = i; break; }
        if (world.corpses[i].t > oldest) { oldest = world.corpses[i].t; slot = i; }
    }
    if (slot < 0) return;

    world.corpses[slot] = (UnitCorpse){
        .active = true, .kind = u->kind, .faction = u->faction,
        .pos = u->pos, .yaw = u->anim.yaw, .t = 0.0f
    };
}

static void CorpsesUpdate(float dt)
{
    for (int i = 0; i < STRAT_MAX_CORPSES; i++)
    {
        UnitCorpse *c = &world.corpses[i];
        if (!c->active) continue;

        c->t += dt;

        // Retired against the ASSET's own DIE length, so an author lengthening
        // the animation lengthens the corpse without touching this code. No
        // asset, or nothing authored, means there is nothing to wait for.
        const SgaAsset *a = StrategyCatalogForRole(SGB_ROLE_UNIT, c->kind);
        float dur = (a != NULL) ? a->duration[SGA_STATE_DIE] : 0.0f;
        if ((dur <= 0.0f) || (c->t >= dur)) c->active = false;
    }
}

static void UnitKill(int index, Unit *killer)
{
    Unit *u = &world.units[index];

    // Copy the look out BEFORE the slot is freed. The slot itself is released
    // on the very next line, exactly as it always was.
    CorpseSpawn(u);

    u->active   = false;
    u->selected = false;
    RosterRemove(index);

    EffectSpawn(FX_RING, u->pos, UnitColor(u));
    for (int i = 0; i < 4; i++)
    {
        EffectSpawn(FX_PUFF, (Vector3){ u->pos.x, 0.5f, u->pos.z }, GRAY);
    }

    int corpseFood = StrategyUnitDef(u->kind)->corpseFood;
    if (corpseFood > 0)
    {
        NodeSpawn(NODE_CORPSE, u->pos, corpseFood);
        if (killer != NULL && killer->kind == KIND_WORKER)
        {
            int corpse = StrategyNearestNodeOfKind(u->pos, NODE_CORPSE, 2.0f);
            if (corpse >= 0) StrategyOrderGather(killer, corpse);
        }
    }
    CheckGameOver();    // losing the last worker can decide the game
}

// Weak-animal panic reflex: the victim AND every weak animal near it bolt
// away from the attacker together (the herd scatters as one).
static void AnimalPanic(int victimIndex, Vector3 threat)
{
    Vector3 origin = world.units[victimIndex].pos;

    int n = SpHashQuery(&s_sightHash, origin.x, origin.z, STRAT_FLEE_PACK_RADIUS,
                        s_sightScratch, SP_HASH_ITEMS_MAX);
    for (int q = 0; q < n; q++)
    {
        Unit *u = &world.units[s_sightScratch[q]];
        if (!u->active) continue;       // hash may hold a unit killed this frame
        if (u->kind != KIND_ANIMAL_WEAK) continue;
        if (DistXZ(u->pos, origin) > STRAT_FLEE_PACK_RADIUS) continue;

        Vector3 away = Vector3Subtract(u->pos, threat);
        away.y = 0.0f;
        if (Vector3Length(away) < 0.001f) away = (Vector3){ 1.0f, 0.0f, 0.0f };

        Vector3 dest = Vector3Add(u->pos,
                                  Vector3Scale(Vector3Normalize(away), STRAT_FLEE_DIST));
        dest.x = Clamp(dest.x, -world.groundHalfX + 1.0f, world.groundHalfX - 1.0f);
        dest.z = Clamp(dest.z, -world.groundHalfZ + 1.0f, world.groundHalfZ - 1.0f);

        u->state          = UNIT_FLEE;
        u->target         = dest;
        u->targetUnit     = -1;
        u->targetNode     = -1;
        u->targetBuilding = -1;
        MoveArriveReset(u);
    }
}

// Strong-animal pack reflex: the victim and its packmates all turn on the
// attacker (regular attack orders - the shared state machine does the rest).
static void AnimalRetaliate(int victimIndex, Unit *attacker)
{
    int attackerIndex = (int)(attacker - world.units);
    if (attackerIndex < 0 || attackerIndex >= STRAT_MAX_UNITS) return;

    Vector3 origin = world.units[victimIndex].pos;

    int n = SpHashQuery(&s_sightHash, origin.x, origin.z, STRAT_FLEE_PACK_RADIUS,
                        s_sightScratch, SP_HASH_ITEMS_MAX);
    for (int q = 0; q < n; q++)
    {
        Unit *u = &world.units[s_sightScratch[q]];
        if (!u->active) continue;       // hash may hold a unit killed this frame
        if (u->kind != KIND_ANIMAL_STRONG) continue;
        if (DistXZ(u->pos, origin) > STRAT_FLEE_PACK_RADIUS) continue;

        StrategyOrderAttack(u, attackerIndex);
    }
}

// ALL unit-vs-unit damage funnels through here so the animal reflexes fire
// even on a killing blow. Returns true when the victim died.
static bool UnitDamage(int victimIndex, Unit *attacker, float damage)
{
    Unit *v = &world.units[victimIndex];
    v->hp -= damage;

    // Before the death test on purpose: a killing blow should read as a death,
    // not as a flinch that is immediately replaced by one.
    if (v->hp > 0.0f) StrategyEntityAnimEvent(v, SGA_STATE_DAMAGED);

    if (v->kind == KIND_ANIMAL_WEAK)        AnimalPanic(victimIndex, attacker->pos);
    else if (v->kind == KIND_ANIMAL_STRONG) AnimalRetaliate(victimIndex, attacker);

    if (v->hp <= 0.0f)
    {
        UnitKill(victimIndex, attacker);
        return true;
    }
    return false;
}

// Auto-aggro: idle units of BOTH factions engage hostiles on sight; enemy
// units also break off moving/working (the player keeps manual control).
// Animals only react when hit; templars never fight.
//
// STAGGERED. Only one unit in STRAT_AGGRO_STRIDE scans on any given frame,
// chosen by index so the load is spread evenly rather than spiking when a wave
// spawns together. A unit therefore notices an enemy up to a quarter second
// late, which is well inside how long it already takes to walk into range - but
// it makes the whole pass 15x cheaper, and this pass runs for every unit every
// frame whether or not there is an enemy anywhere near.
static void UnitAggroScan(Unit *u, int index, int frame)
{
    if (((index + frame) % STRAT_AGGRO_STRIDE) != 0) return;

    if (u->kind == KIND_ANIMAL_WEAK || u->kind == KIND_ANIMAL_STRONG) return;
    if (u->kind == KIND_TEMPLAR || u->kind == KIND_TEMPLAR_HEALER) return;

    bool scan = (u->state == UNIT_IDLE) ||
                (u->faction == 1 && (u->state == UNIT_MOVE ||
                                     u->state == UNIT_GATHER ||
                                     u->state == UNIT_RETURN ||
                                     u->state == UNIT_FARM));

    // A player unit marching in formation scans too, but under the formation's
    // OWN break-off rule rather than the blanket one above - that is the whole
    // point of the setting. Without this a formation walks past an enemy army
    // without reacting, because plain UNIT_MOVE never scanned for faction 0.
    bool inFormation = (u->formGroup >= 0 && u->state == UNIT_MOVE && !u->formBrokeOff);
    if (inFormation)
    {
        switch (StrategyFormationBehavior())
        {
            case FORM_BEHAVIOR_HOLD:
                return;                 // the march IS the order; ignore everything
            case FORM_BEHAVIOR_SKIRMISH:
            case FORM_BEHAVIOR_ENGAGE:
                scan = true;
                break;
            default: break;
        }
    }

    if (!scan) return;

    // SKIRMISH peels a unit off only when the enemy is inside its OWN attack
    // range, not its much longer sight range. That difference is what keeps a
    // brush with one scout from dissolving the formation: units see far, but
    // only the ones actually able to strike stop marching.
    float range = u->sightRange;
    if (inFormation && StrategyFormationBehavior() == FORM_BEHAVIOR_SKIRMISH)
        range = u->attackRange;

    int hostile = NearestHostile(u, range);
    if (hostile < 0) return;

    if (inFormation)
    {
        // ENGAGE breaks the whole formation on first contact: one unit finding
        // an enemy releases every member of its group, so they commit together
        // instead of feeding in one at a time.
        if (StrategyFormationBehavior() == FORM_BEHAVIOR_ENGAGE)
            StrategyFormationBreak(u->formGroup);
        else
            u->formBrokeOff = true;     // SKIRMISH: just this one
    }

    StrategyOrderAttack(u, hostile);
}

// Templar target search. The blessing templar shadows own units that are
// WORKING (gather/farm/return); the healer shadows own WOUNDED units. Both
// fall back to any own non-templar unit so they never stand alone mid-map.
static int TemplarFindTarget(const Unit *u)
{
    bool healer = (u->kind == KIND_TEMPLAR_HEALER);
    int best = -1;
    float bestDist = healer ? 15.0f : 1000000.0f;

    // NOT hashed, and deliberately so. The blessing templar's search is
    // UNBOUNDED - it takes the nearest working unit anywhere on the map, and
    // the fallback below is unbounded too - so a radius query cannot express
    // it. Bounding it to make it hashable would change behaviour: a templar
    // with no worker nearby would stand still instead of walking across the map
    // to the far base, which is a gameplay change disguised as an optimization.
    //
    // It stays a full scan because templars are RARE - one or two per side, not
    // a per-unit cost like the aggro scan. If a build ever fields hundreds of
    // them this becomes the next thing to fix.
    for (int k = 0; k < s_activeCount; k++)
    {
        int i = s_active[k];
        Unit *other = &world.units[i];
        if (other->faction != u->faction || other == u) continue;
        if (other->kind == KIND_TEMPLAR || other->kind == KIND_TEMPLAR_HEALER) continue;

        if (healer)
        {
            if (other->hp >= other->maxHp) continue;
        }
        else
        {
            if (other->state != UNIT_GATHER && other->state != UNIT_FARM &&
                other->state != UNIT_RETURN) continue;
        }
        float d = DistXZ(other->pos, u->pos);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    if (best >= 0) return best;

    // Fallback: any own non-templar unit, unlimited range.
    bestDist = 1000000.0f;
    for (int k = 0; k < s_activeCount; k++)
    {
        int i = s_active[k];
        Unit *other = &world.units[i];
        if (other->faction != u->faction || other == u) continue;
        if (other->kind == KIND_TEMPLAR || other->kind == KIND_TEMPLAR_HEALER) continue;

        float d = DistXZ(other->pos, u->pos);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

static void UnitUpdate(int index, float dt)
{
    Unit *u = &world.units[index];

    UnitAggroScan(u, index, s_frame);

    switch (u->state)
    {
        case UNIT_IDLE:
            break;

        case UNIT_MOVE:
        {
            // MoveArrive, not MoveToward: this is a walk to a fixed point on
            // the ground, so it is the one case that has to decide it is over.
            // The old test here was `< 0.15f` against a 0.35 unit radius - in
            // any crowd that is unreachable, so the state never resolved.
            if (MoveArrive(u, index, u->target, dt)) u->state = UNIT_IDLE;
        } break;

        case UNIT_GATHER:
        {
            ResourceNode *n = (u->targetNode >= 0) ? &world.nodes[u->targetNode] : NULL;
            if ((n == NULL) || !n->active)
            {
                // Auto-retarget: hop to the nearest node of the SAME kind
                // (dead slots keep their kind) instead of idling.
                int next = (n != NULL)
                    ? StrategyNearestNodeOfKind(u->pos, n->kind, STRAT_RETARGET_RADIUS)
                    : -1;
                int carried = u->carryAmount;
                if (next >= 0 && carried < STRAT_CARRY_MAX) StrategyOrderGather(u, next);
                else u->state = (carried > 0) ? UNIT_RETURN : UNIT_IDLE;
                break;
            }
            if (DistXZ(u->pos, n->pos) > 1.0f)
            {
                StrategyMoveTo(u, index, n->pos, dt);
                break;
            }
            // In working range: one resource unit per gatherTime tick.
            u->gatherTimer += dt;
            if (u->gatherTimer >= u->gatherTime)
            {
                u->gatherTimer -= u->gatherTime;
                u->carryKind = NodeResource(n->kind);
                u->carryAmount++;
                n->remaining--;

                Color puff = (n->kind == NODE_TREE) ? BROWN : GRAY;
                EffectSpawn(FX_PUFF, (Vector3){ n->pos.x, 1.0f, n->pos.z }, puff);

                if (n->remaining <= 0)
                {
                    Vector3 np = n->pos;                // read before despawn
                    NodeDespawn(u->targetNode);         // depleted: vanish with a burst
                    EffectSpawn(FX_PUFF, (Vector3){ np.x, 0.6f, np.z }, puff);
                    EffectSpawn(FX_RING, np, puff);
                }
            }
            if (u->carryAmount >= STRAT_CARRY_MAX || !n->active)
            {
                u->state = (u->carryAmount > 0) ? UNIT_RETURN : UNIT_IDLE;
            }
        } break;

        case UNIT_RETURN:
        {
            int home = NearestDropoffCached(u);
            if (home < 0)
            {
                u->carryAmount = 0;     // nothing accepts this - dump it
                u->state = UNIT_IDLE;
                break;
            }
            Building *b = &world.buildings[home];
            if (DistXZ(u->pos, b->pos) > BuildingReach(b->kind))
            {
                StrategyMoveTo(u, index, b->pos, dt);
                break;
            }
            world.stockpile[u->faction][u->carryKind] += u->carryAmount;
            u->carryAmount = 0;
            EffectSpawn(FX_FLASH, (Vector3){ b->pos.x, 1.2f, b->pos.z },
                        strategyFactionColor[u->faction]);

            // Keep the loop going while the node is alive, else retarget.
            bool nodeAlive = (u->targetNode >= 0) && world.nodes[u->targetNode].active;
            if (nodeAlive) u->state = UNIT_GATHER;
            else
            {
                int next = (u->targetNode >= 0)
                    ? StrategyNearestNodeOfKind(u->pos, world.nodes[u->targetNode].kind,
                                                STRAT_RETARGET_RADIUS)
                    : -1;
                if (next >= 0) StrategyOrderGather(u, next);
                else           u->state = UNIT_IDLE;
            }
        } break;

        case UNIT_FARM:
        {
            // Node-tending: a worker assigned to a farm/forestry plants its
            // node kind in free ground nearby. Once TEND_MAX of them stand
            // around the building it harvests the nearest one to depletion
            // (carrying to a dropoff like any gather) then resumes planting.
            Building *tb = (u->targetBuilding >= 0)
                ? &world.buildings[u->targetBuilding] : NULL;
            const BuildingDef *bd = (tb && tb->active)
                ? StrategyBuildingDef(tb->kind) : NULL;
            if ((tb == NULL) || !tb->active || bd == NULL || bd->tendNode < 0)
            {
                u->state = UNIT_IDLE;
                u->targetBuilding = -1;
                break;
            }
            NodeKind nk = (NodeKind)bd->tendNode;

            // 1) Carrying a full load: walk it to the nearest accepting building.
            if (u->carryAmount >= STRAT_CARRY_MAX)
            {
                int home = NearestDropoffCached(u);
                if (home < 0) { u->carryAmount = 0; }   // nothing accepts it: dump
                else
                {
                    Building *h = &world.buildings[home];
                    if (DistXZ(u->pos, h->pos) > BuildingReach(h->kind)) { StrategyMoveTo(u, index, h->pos, dt); break; }
                    world.stockpile[u->faction][u->carryKind] += u->carryAmount;
                    u->carryAmount = 0;
                    EffectSpawn(FX_FLASH, (Vector3){ h->pos.x, 1.2f, h->pos.z },
                                strategyFactionColor[u->faction]);
                }
                break;
            }

            // 2) Harvesting an existing tended node.
            if (u->targetNode >= 0)
            {
                ResourceNode *n = &world.nodes[u->targetNode];
                if (!n->active) { u->targetNode = -1; break; }
                if (DistXZ(u->pos, n->pos) > 1.0f) { StrategyMoveTo(u, index, n->pos, dt); break; }
                u->gatherTimer += dt;
                if (u->gatherTimer >= u->gatherTime)
                {
                    u->gatherTimer -= u->gatherTime;
                    u->carryKind = NodeResource(n->kind);
                    u->carryAmount++;
                    n->remaining--;
                    EffectSpawn(FX_PUFF, (Vector3){ n->pos.x, 1.0f, n->pos.z },
                                (nk == NODE_TREE) ? BROWN : (Color){ 220, 190, 90, 255 });
                    if (n->remaining <= 0)
                    {
                        Vector3 np = n->pos;
                        NodeDespawn(u->targetNode);
                        u->targetNode = -1;
                        EffectSpawn(FX_RING, np, GRAY);
                    }
                }
                break;
            }

            // 3) Area full of nodes: switch to harvesting the nearest one.
            //    Only a worker NOT already carrying a sapling may switch - one
            //    that has equipped must finish planting its cycle first, so a
            //    node tipping the cap doesn't yank every tending worker off
            //    mid-trip to clump on one node.
            if (!u->tendEquipped &&
                NodesNear(nk, tb->pos, STRAT_TEND_RANGE + 2.0f) >= STRAT_TEND_MAX)
            {
                int near = StrategyNearestNodeOfKind(tb->pos, nk, STRAT_TEND_RANGE + 2.0f);
                if (near >= 0)
                {
                    u->targetNode   = near;
                    u->gatherTimer  = 0.0f;
                    u->tendEquipped = false;    // drop the sapling, go harvest
                    break;
                }
                // (fallthrough to planting if none found somehow)
            }

            // 4) Plant is a round-trip: first walk BACK to the building to grab
            //    a hat + sapling (equip leg), then carry them out to a free spot
            //    and plant (plant leg). tendEquipped marks which leg we're on;
            //    target == building pos is the "no plant spot yet" sentinel.
            if (!u->tendEquipped)
            {
                // Equip leg: return to the building and dwell to gear up.
                if (DistXZ(u->pos, tb->pos) > BuildingReach(tb->kind))
                {
                    StrategyMoveTo(u, index, tb->pos, dt);
                    break;
                }
                u->gatherTimer += dt;
                if (u->gatherTimer >= STRAT_TEND_EQUIP_TIME)
                {
                    u->gatherTimer  = 0.0f;
                    u->tendEquipped = true;
                    u->target       = PlantSpotNear(tb->pos);
                }
                break;
            }

            // Plant leg: walk to the chosen spot, then drop a fresh node.
            if (DistXZ(u->pos, u->target) > 0.6f) { MoveToward(u, u->target, dt); break; }
            u->gatherTimer += dt;
            if (u->gatherTimer >= STRAT_TEND_PERIOD)
            {
                u->gatherTimer = 0.0f;
                if (PlantSpotClear(u->target))
                    NodeSpawn(nk, u->target, bd->tendAmount);
                EffectSpawn(FX_PUFF, (Vector3){ u->target.x, 0.6f, u->target.z },
                            (nk == NODE_TREE) ? (Color){ 60, 140, 60, 255 }
                                              : (Color){ 220, 190, 90, 255 });
                u->tendEquipped = false;    // used the sapling: re-equip next
                u->target = tb->pos;        // sentinel -> pick a new spot next
            }
        } break;

        case UNIT_BUILD:
        {
            // Walk to the scaffold, then pour build-time into it; HP tracks
            // progress so the site visibly rises. Done -> functional, worker idle.
            Building *b = (u->targetBuilding >= 0)
                ? &world.buildings[u->targetBuilding] : NULL;
            if ((b == NULL) || !b->active || !b->underConstruction)
            {
                u->state = UNIT_IDLE;
                u->targetBuilding = -1;
                break;
            }
            if (DistXZ(u->pos, b->pos) > BuildingReach(b->kind))
            {
                StrategyMoveTo(u, index, b->pos, dt);
                break;
            }
            float buildTime = StrategyBuildingDef(b->kind)->buildTime;
            b->buildProgress += dt;
            b->hp = fminf(b->maxHp, 1.0f + (b->maxHp - 1.0f)*(b->buildProgress/buildTime));
            EffectSpawn(FX_PUFF, (Vector3){ b->pos.x, 0.9f, b->pos.z }, LIGHTGRAY);
            if (b->buildProgress >= buildTime)
            {
                b->underConstruction = false;
                b->hp = b->maxHp;
                EffectSpawn(FX_RING, b->pos, strategyFactionColor[b->faction]);
                int finished = u->targetBuilding;
                u->state = UNIT_IDLE;
                u->targetBuilding = -1;

                // Next queued job, else drain fallback: work the building we
                // just finished if it gathers, otherwise stay idle. Auto-gather
                // is player-only (the AI has no build behavior).
                if (!WorkerStartNextJob(u) && u->faction == 0)
                    WorkerAutoGatherForBuilding(u, finished);
            }
        } break;

        case UNIT_REPAIR:
        {
            // Restore a damaged own building over time, free. Full HP -> done:
            // advance the job queue, else resume gathering for it (repair
            // targets are often gatherers), else idle. Same tail on an already-
            // invalid target so a chain isn't stranded on a dead building.
            Building *b = (u->targetBuilding >= 0)
                ? &world.buildings[u->targetBuilding] : NULL;
            if ((b == NULL) || !b->active || b->underConstruction ||
                b->hp >= b->maxHp)
            {
                int repaired = u->targetBuilding;
                u->state = UNIT_IDLE;
                u->targetBuilding = -1;
                if (!WorkerStartNextJob(u) && u->faction == 0)
                    WorkerAutoGatherForBuilding(u, repaired);
                break;
            }
            if (DistXZ(u->pos, b->pos) > BuildingReach(b->kind))
            {
                StrategyMoveTo(u, index, b->pos, dt);
                break;
            }
            b->hp = fminf(b->maxHp, b->hp + STRAT_REPAIR_RATE*dt);
            EffectSpawn(FX_PUFF, (Vector3){ b->pos.x, 0.9f, b->pos.z },
                        (Color){ 200, 200, 120, 255 });
            if (b->hp >= b->maxHp)
            {
                int repaired = u->targetBuilding;
                u->state = UNIT_IDLE;
                u->targetBuilding = -1;
                if (!WorkerStartNextJob(u) && u->faction == 0)
                    WorkerAutoGatherForBuilding(u, repaired);
            }
        } break;

        case UNIT_ATTACK:
        {
            // Two target flavors share the chase/cooldown skeleton: a unit
            // (or animal) via targetUnit, or a building via targetBuilding.
            Vector3 targetPos = u->pos;
            float   range     = u->attackRange;
            bool    valid     = false;

            if (u->targetBuilding >= 0)
            {
                Building *b = &world.buildings[u->targetBuilding];
                valid     = b->active;
                targetPos = b->pos;
                range    += 1.0f;   // buildings are wide - hit from the edge
            }
            else if (u->targetUnit >= 0)
            {
                Unit *victim = &world.units[u->targetUnit];
                valid     = victim->active;
                targetPos = victim->pos;
            }
            if (!valid)
            {
                u->state          = UNIT_IDLE;
                u->targetUnit     = -1;
                u->targetBuilding = -1;
                break;
            }
            float dist = DistXZ(u->pos, targetPos);
            if (dist > range)
            {
                MoveToward(u, targetPos, dt);
                break;
            }
            // Kiting: a ranged unit keeps FIRING but backs off toward its
            // stand-off distance when the target crowds in.
            if (u->preferredRange > 0.0f && dist < u->preferredRange*0.6f)
            {
                Vector3 away = Vector3Subtract(u->pos, targetPos);
                away.y = 0.0f;
                if (Vector3Length(away) > 0.001f)
                {
                    Vector3 dest = Vector3Add(u->pos,
                                              Vector3Scale(Vector3Normalize(away), 2.0f));
                    MoveToward(u, dest, dt);
                }
            }
            u->attackCooldown -= dt;
            if (u->attackCooldown <= 0.0f)
            {
                u->attackCooldown = u->attackPeriod;

                Vector3 from = (Vector3){ u->pos.x, 0.8f, u->pos.z };
                Vector3 to   = (Vector3){ targetPos.x, 0.6f, targetPos.z };
                EffectSpawnBeam(from, to, UnitColor(u));
                EffectSpawn(FX_FLASH, to, RAYWHITE);

                if (u->targetBuilding >= 0)
                {
                    Building *b = &world.buildings[u->targetBuilding];
                    b->hp -= u->damage;
                    if (b->hp <= 0.0f)
                    {
                        BuildingDestroy(u->targetBuilding);
                        u->state          = UNIT_IDLE;
                        u->targetBuilding = -1;
                    }
                }
                else if (UnitDamage(u->targetUnit, u, u->damage))
                {
                    // The kill handler may have re-ordered u (corpse
                    // harvest); only idle it if it is still attacking.
                    if (u->state == UNIT_ATTACK)
                    {
                        u->state      = UNIT_IDLE;
                        u->targetUnit = -1;
                    }
                }
            }
        } break;

        case UNIT_FLEE:
        {
            // -1 means "no path, steer straight", and that is right here for
            // two reasons. A flee target is STRAT_FLEE_DIST directly away from
            // the threat - short, and by construction not around anything. And
            // panic is a PACK response: every weak animal within the pack
            // radius flees on the same frame, so pathing this would fire a
            // burst of searches for a run that lasts a second or two.
            if (MoveArrive(u, -1, u->target, dt)) u->state = UNIT_IDLE;
        } break;

        case UNIT_FOLLOW:
        {
            // Templar/healer shadowing its target; gatherTimer paces the
            // bless cadence. The heal/providence effect lands when the
            // blessing STARTS - UNIT_BLESS is just the sparkle pause.
            Unit *t = (u->targetUnit >= 0) ? &world.units[u->targetUnit] : NULL;
            bool healer = (u->kind == KIND_TEMPLAR_HEALER);
            if ((t == NULL) || !t->active || (healer && t->hp >= t->maxHp))
            {
                u->state      = UNIT_IDLE;  // retarget next frame
                u->targetUnit = -1;
                break;
            }
            if (DistXZ(u->pos, t->pos) > 1.5f)
            {
                MoveToward(u, t->pos, dt);
                break;
            }
            u->gatherTimer += dt;
            if (u->gatherTimer >= STRAT_BLESS_PERIOD)
            {
                u->gatherTimer = 0.0f;
                if (healer)
                {
                    if (world.stockpile[u->faction][RES_PROVIDENCE] < STRAT_HEAL_COST)
                        break;      // broke: keep following, try again later
                    world.stockpile[u->faction][RES_PROVIDENCE] -= STRAT_HEAL_COST;
                    t->hp = fminf(t->maxHp, t->hp + STRAT_HEAL_AMOUNT);
                    StrategyEntityAnimEvent(t, SGA_STATE_HEALED);
                }
                else world.stockpile[u->faction][RES_PROVIDENCE] += 1;

                u->state          = UNIT_BLESS;
                u->attackCooldown = STRAT_BLESS_TIME;
                EffectSpawnBless(t->pos);
            }
        } break;

        case UNIT_BLESS:
        {
            u->attackCooldown -= dt;
            if (u->attackCooldown <= 0.0f) u->state = UNIT_FOLLOW;
        } break;
    }

    // Idle templars pick someone to shadow (their version of auto-aggro).
    if (u->state == UNIT_IDLE &&
        (u->kind == KIND_TEMPLAR || u->kind == KIND_TEMPLAR_HEALER))
    {
        int t = TemplarFindTarget(u);
        if (t >= 0)
        {
            u->state      = UNIT_FOLLOW;
            u->targetUnit = t;
        }
    }

    // Never leave the ground plane.
    u->pos.x = Clamp(u->pos.x, -world.groundHalfX, world.groundHalfX);
    u->pos.z = Clamp(u->pos.z, -world.groundHalfZ, world.groundHalfZ);
}

// ----------------------------------------------------------------------------
//  Separation
// ----------------------------------------------------------------------------
//  Push overlapping units apart so groups spread instead of stacking. Three
//  things changed from the original pairwise version, and all three are needed
//  before a large crowd will come to rest:
//
//  1. HASHED, NOT O(n^2). The old loop compared every unit against every other
//     - 4,560 tests at 96 units, 50 million at 10,000. The hash asks only who
//     shares your patch of ground.
//
//  2. FORCE, NOT TELEPORT. The old loop wrote a->pos and b->pos INSIDE the pair
//     loop, so a unit's final position depended on how many neighbours were
//     visited after it. That order-dependence is systematic, not random, which
//     is what turned jitter into the whole mass slowly rotating. Now each unit
//     sums a force, and every position is integrated once, after all forces are
//     known - so the result cannot depend on visit order.
//
//  3. APPLY/RECEIVE ASYMMETRY. A settled unit stops APPLYING push but still
//     RECEIVES it. Without this, two neighbours that have both arrived keep
//     shoving each other forever and the pile never stops breathing.
//
//  The pass is one-directional: each unit queries its own neighbourhood and
//  accumulates only its OWN force. That does redundant work - every pair is
//  examined twice, once from each end - but it keeps the loop trivially
//  parallel-shaped and, more importantly, lets rule 3 apply per-unit. Halving
//  the work by writing both units from one visit is exactly the mutation that
//  made the old version order-dependent.
static void UnitHashRebuild(void)
{
    // Cell size is twice the query radius, so a query touches a 2x2 block of
    // cells instead of 3x3 - four chains to walk rather than nine.
    SpHashBegin(&s_unitHash, 2.0f*STRAT_SEP_RADIUS,
                world.groundHalfX + 2.0f, world.groundHalfZ + 2.0f);
    SpHashBegin(&s_sightHash, STRAT_SIGHT_CELL,
                world.groundHalfX + 2.0f, world.groundHalfZ + 2.0f);

    for (int k = 0; k < s_activeCount; k++)
    {
        int i = s_active[k];
        float x = world.units[i].pos.x, z = world.units[i].pos.z;
        SpHashInsert(&s_unitHash,  i, x, z);
        SpHashInsert(&s_sightHash, i, x, z);
    }
}

// A unit only pushes while it is actually walking somewhere. Everything else -
// idle, gathering, building, attacking in place - is stationary by intent, and
// a stationary unit that still applies push is a permanent engine with nothing
// pulling it back.
//
// This is deliberately derived from `state` rather than tracked in a field.
// `arrival` is only ever written by MoveArrive, which only the two UNIT_MOVE /
// UNIT_FLEE branches call, so a freshly spawned unit sits at ARRIVE_SEEKING (=0
// from `(Unit){0}`) forever and reads as a full-strength pusher. Deriving it
// means a new unit state cannot silently opt itself back into shoving.
static bool UnitPushes(const Unit *u)
{
    if (u->arrival == ARRIVE_SETTLED) return false;
    return (u->state == UNIT_MOVE) || (u->state == UNIT_FLEE) ||
           (u->state == UNIT_FOLLOW);
}

static void UnitSeparation(float dt)
{
    int32_t neighbors[STRAT_SEP_MAX_NEIGHBORS];

    for (int ka = 0; ka < s_activeCount; ka++)
    {
        int i = s_active[ka];
        Unit *a = &world.units[i];

        int n = SpHashQuery(&s_unitHash, a->pos.x, a->pos.z, STRAT_SEP_RADIUS,
                            neighbors, STRAT_SEP_MAX_NEIGHBORS);

        float fx = 0.0f, fz = 0.0f;
        int   settledNeighbors = 0;

        for (int q = 0; q < n; q++)
        {
            int j = neighbors[q];
            if (j == i) continue;               // the query returns self

            Unit *b = &world.units[j];
            if (!b->active) continue;           // killed after the rebuild
            float dx = a->pos.x - b->pos.x;
            float dz = a->pos.z - b->pos.z;
            float d  = sqrtf(dx*dx + dz*dz);

            // "Finished" for the chokepoint rule means anything not going to
            // move out of the way, which includes units that never had an order
            // at all - a squad grinding against idle bystanders is stuck just as
            // hard as one grinding against arrivals.
            if (!UnitPushes(b)) settledNeighbors++;

            if (d < 0.001f)
            {
                // Exactly coincident: no direction to derive, so take a stable
                // one from the pair itself. Deriving it from iteration order
                // (the old `i % 2`) means the pair parts differently depending
                // on which end the hash happened to return first, and that
                // flips every frame.
                SpSeparationJitter(i, j, &dx, &dz);
                d = 0.001f;
            }
            else
            {
                dx /= d;
                dz /= d;
            }

            // Normalized falloff: full strength at total overlap, zero at the
            // separation radius. Dividing by d instead would send the force to
            // infinity as units converge, which ejects them across the map.
            float strength = (STRAT_SEP_RADIUS - d)/STRAT_SEP_RADIUS;
            fx += dx*strength;
            fz += dz*strength;
        }

        a->crowd = settledNeighbors;

        // A parked unit does not push. It is still pushed - it just stopped
        // being an engine. This is the line that ends the oscillation.
        if (!UnitPushes(a)) { fx = 0.0f; fz = 0.0f; }

        // Deadband: forces this small are the residue of a crowd that has
        // effectively resolved, and applying them is what makes a dense pile
        // shimmer in place instead of looking still.
        if (fx*fx + fz*fz < STRAT_SEP_DEADBAND*STRAT_SEP_DEADBAND)
        {
            fx = 0.0f;
            fz = 0.0f;
        }

        a->vel.x += fx*STRAT_SEP_STRENGTH*dt;
        a->vel.z += fz*STRAT_SEP_STRENGTH*dt;

        // Exponential damping. Without it the accumulated velocity has no way
        // to decay and a resolved crowd keeps coasting apart.
        float damp = 1.0f - STRAT_SEP_DAMP*dt;
        if (damp < 0.0f) damp = 0.0f;       // guard a big dt (alt-tab, breakpoint)
        a->vel.x *= damp;
        a->vel.z *= damp;
    }

    // Integrate every unit ONCE, after every force is known. Splitting this out
    // of the loop above is the whole point: while forces are being summed, no
    // position moves, so nothing a unit reads can depend on who came first.
    for (int k = 0; k < s_activeCount; k++)
    {
        Unit *u = &world.units[s_active[k]];

        // Clamp to the unit's own speed so a shove out of a dense pile can
        // never outrun a walk - a unit rocketing out of a crowd reads as a bug
        // even when the maths is sound.
        float speed = sqrtf(u->vel.x*u->vel.x + u->vel.z*u->vel.z);
        if (speed > u->moveSpeed && speed > 0.0001f)
        {
            float s = u->moveSpeed/speed;
            u->vel.x *= s;
            u->vel.z *= s;
        }

        u->pos.x += u->vel.x*dt;
        u->pos.z += u->vel.z*dt;

        // Units cannot be shoved off the world. Buildings and terrain do not
        // block yet - that is the nav grid in the next phase.
        u->pos.x = Clamp(u->pos.x, -world.groundHalfX + 0.3f, world.groundHalfX - 0.3f);
        u->pos.z = Clamp(u->pos.z, -world.groundHalfZ + 0.3f, world.groundHalfZ - 0.3f);
    }
}

// ----------------------------------------------------------------------------
//  Stress-test spawning (path lab only)
//
//  Goes through the ordinary UnitSpawn so stress units are indistinguishable
//  from real ones - same stat resolution, same slots, same everything. A test
//  population that took a shortcut would measure the shortcut.
// ----------------------------------------------------------------------------
int StrategyDebugSpawnUnits(int faction, UnitKind kind, Vector3 center,
                            float radius, int count)
{
    int spawned = 0;
    for (int i = 0; i < count; i++)
    {
        // Sunflower spiral rather than random: it fills the disc evenly at any
        // count, so density does not depend on how many you asked for, and it
        // is deterministic - the same N always lays out the same way, which is
        // what makes two stress runs comparable.
        float t = (count > 1) ? (float)i/(float)(count - 1) : 0.0f;
        float r = radius*sqrtf(t);
        float a = (float)i*2.39996323f;     // golden angle
        Vector3 pos = (Vector3){ center.x + r*cosf(a), 0.0f, center.z + r*sinf(a) };

        pos.x = Clamp(pos.x, -world.groundHalfX + 0.5f, world.groundHalfX - 0.5f);
        pos.z = Clamp(pos.z, -world.groundHalfZ + 0.5f, world.groundHalfZ - 0.5f);

        if (UnitSpawn(faction, kind, pos) == NULL) break;   // pool full
        spawned++;
    }
    return spawned;
}

void StrategyDebugClearUnits(void)
{
    for (int k = 0; k < s_activeCount; k++) world.units[s_active[k]].active = false;
    RosterReset();
    world.selectedBuilding = -1;
}

void StrategyDebugNavStats(int *outBlocked, int *outSkirt)
{
    int blocked = 0, skirt = 0;
    int cells = s_nav.w*s_nav.h;
    for (int i = 0; i < cells; i++)
    {
        uint8_t c = s_nav.cost[i];
        if (c == SP_COST_BLOCKED)     blocked++;
        else if (c >= SP_COST_SKIRT)  skirt++;
    }
    if (outBlocked != NULL) *outBlocked = blocked;
    if (outSkirt != NULL)   *outSkirt   = skirt;
}

static bool s_navShow;

void StrategyDebugNavShow(bool on) { s_navShow = on; }
bool StrategyDebugNavShown(void)   { return s_navShow; }

// Called from inside StrategyWorldDraw3D's 3D pass, not by the lab directly.
static void NavDraw(void)
{
    // Flat quads a hair above the ground plane so they win the depth test
    // against it without z-fighting, and below anything standing on it.
    const float y = 0.03f;

    for (int z = 0; z < s_nav.h; z++)
    {
        for (int x = 0; x < s_nav.w; x++)
        {
            uint8_t c = s_nav.cost[z*s_nav.w + x];
            if (c == SP_COST_NORMAL) continue;      // plain ground: draw nothing

            Color col;
            if (c == SP_COST_BLOCKED)      col = (Color){ 200,  40,  40, 150 };
            else if (c == SP_COST_SHALLOW) col = (Color){  70, 140, 200, 100 };
            else                           col = (Color){ 220, 170,  40,  90 };

            Vector3 w = SpTileToWorld(&s_nav, x, z);
            DrawCube((Vector3){ w.x, y, w.z }, 0.94f, 0.01f, 0.94f, col);
        }
    }
}

// -- Path overlay -------------------------------------------------------------
static bool s_pathShow;
static bool s_flowShow;
static bool s_slotShow;

void StrategyDebugPathShow(bool on) { s_pathShow = on; }
bool StrategyDebugPathShown(void)   { return s_pathShow; }

void StrategyDebugFlowShow(bool on) { s_flowShow = on; }
bool StrategyDebugFlowShown(void)   { return s_flowShow; }

void StrategyDebugSlotShow(bool on) { s_slotShow = on; }
bool StrategyDebugSlotShown(void)   { return s_slotShow; }

void StrategyDebugPathBudgetSet(int nodesPerFrame) { SpServiceSetBudget(nodesPerFrame); }
int  StrategyDebugPathBudget(void)                 { return SpServiceBudget(); }

void StrategyDebugPathStats(int *outQueued, int *outActive,
                            int *outPending, int *outNodes)
{
    if (outQueued  != NULL) *outQueued  = SpProfGet(SP_COUNT_PATH_REQUESTS);
    if (outActive  != NULL) *outActive  = SpProfGet(SP_COUNT_PATH_ACTIVE);
    if (outPending != NULL) *outPending = SpProfGet(SP_COUNT_PATH_PENDING);
    if (outNodes   != NULL) *outNodes   = SpProfGet(SP_COUNT_ASTAR_NODES);
}

// Draws the route each SELECTED unit has left to walk. Selected only, and on
// purpose: at a thousand units every path drawn at once is a solid mat of lines
// that answers nothing. Selecting a handful and watching where they intend to
// go is what actually diagnoses a bad route.
static void PathDraw(void)
{
    const float y = 0.35f;      // above the nav tint, below unit bodies
    Vector3 wp[SP_PATH_MAX];

    int selCount = 0;
    const int *sel = SelectedUnits(&selCount);
    for (int k = 0; k < selCount; k++)
    {
        int i = sel[k];
        const Unit *u = &world.units[i];
        if (!u->active) continue;

        int n = StrategyMovePathOf(i, wp, SP_PATH_MAX);
        if (n <= 0) continue;

        Color col = strategyFactionColor[u->faction];
        Vector3 prev = (Vector3){ u->pos.x, y, u->pos.z };
        for (int j = 0; j < n; j++)
        {
            Vector3 cur = (Vector3){ wp[j].x, y, wp[j].z };
            DrawLine3D(prev, cur, col);
            // A marker at each corner: the count of these IS the smoothing
            // result, so a route that should be four hops and shows twenty
            // means string-pulling silently did nothing.
            DrawCube(cur, 0.16f, 0.16f, 0.16f, col);
            prev = cur;
        }
    }
}

// -- Flow field overlay -------------------------------------------------------
// Draws the direction field the FIRST selected unit is riding. One field, not
// all sixteen: overlapping arrow mats answer nothing, and the question is
// always "what is this group following". Nothing selected, or nothing selected
// that is on a field, draws nothing - which is itself the answer when a group
// that should be sharing a field is not.
static void FlowDraw(void)
{
    const SpGrid *g = StrategyNavGrid();
    if (g == NULL) return;

    int selCount = 0;
    const int *sel = SelectedUnits(&selCount);

    SpFieldId id = SP_FIELD_NONE;
    for (int k = 0; k < selCount && id == SP_FIELD_NONE; k++)
        id = StrategyMoveFieldOf(sel[k]);
    if (id == SP_FIELD_NONE || !SpFlowValid(id)) return;

    const float y = 0.30f;      // under the path overlay, over the nav tint

    // Every fourth cell. At 96x96 that is 576 arrows instead of 9,216, which is
    // the difference between reading the field and looking at fur.
    for (int tz = 0; tz < g->h; tz += 4)
    {
        for (int tx = 0; tx < g->w; tx += 4)
        {
            float dx, dz;
            if (!SpFlowDir(id, tx, tz, &dx, &dz)) continue;
            if (dx == 0.0f && dz == 0.0f) continue;         // the goal itself

            Vector3 c = SpCellToWorld(g, (SpCell)(tz*g->w + tx));

            // Cost tint: bright near the goal, dark far from it, so the shape
            // of the field reads at a glance without following any one arrow.
            uint16_t cost = SpFlowCost(id, tx, tz);
            float t = (float)cost/2000.0f;
            if (t > 1.0f) t = 1.0f;
            Color col = { (unsigned char)(60 + 195*(1.0f - t)), 220,
                          (unsigned char)(80 + 100*t), 200 };

            Vector3 a = { c.x - dx*0.35f, y, c.z - dz*0.35f };
            Vector3 b = { c.x + dx*0.35f, y, c.z + dz*0.35f };
            DrawLine3D(a, b, col);
            DrawCube(b, 0.12f, 0.02f, 0.12f, col);          // the head
        }
    }
}

// -- Formation slot overlay ---------------------------------------------------
// A line from each selected unit to the destination it was ACTUALLY given. The
// spread of the endpoints is the formation; whether the lines cross is whether
// slot assignment is spatially coherent. Crossed lines mean units walk through
// each other to reach their slots - a sort-key bug, invisible any other way.
static void SlotDraw(void)
{
    const float y = 0.40f;

    int selCount = 0;
    const int *sel = SelectedUnits(&selCount);
    for (int k = 0; k < selCount; k++)
    {
        int i = sel[k];
        const Unit *u = &world.units[i];
        if (!u->active) continue;

        Vector3 goal;
        if (!StrategyMoveGoalOf(i, &goal)) continue;

        Vector3 from = { u->pos.x, y, u->pos.z };
        Vector3 to   = { goal.x,   y, goal.z   };
        DrawLine3D(from, to, (Color){ 255, 200, 60, 160 });
        DrawCube(to, 0.22f, 0.02f, 0.22f, (Color){ 255, 200, 60, 220 });
    }
}

void StrategyWorldUpdate(float dt)
{
    SpProfResetFrameCounters();
    s_frame++;

#ifndef NDEBUG
    if (strategyRosterAudit && !RosterCheck()) strategyRosterAudit = false;  // trace once
#endif

    // BEFORE the state machine, so the sight/aggro queries inside UnitUpdate
    // see this frame's positions. Separation therefore reads positions that are
    // one step stale, which is harmless - it is a continuous force and a unit
    // moves at most speed*dt (about 0.07 units) between the rebuild and the
    // push. Aggro cannot tolerate the same staleness because it feeds targeting
    // decisions, not a nudge.
    //
    // The cost is that a unit killed during the update stays in the hash until
    // the next rebuild, so every consumer re-checks `active` on what it gets
    // back. That is one load on an index the query already touched.
    SpProfBegin(SP_PROF_NAV_HASH);
    UnitHashRebuild();
    SpProfEnd(SP_PROF_NAV_HASH);
    SpProfSet(SP_COUNT_HASH_DROPPED, s_unitHash.dropped);

    // Pathfinding runs BEFORE the state machine so a route that finishes this
    // frame is walked this frame rather than next - at a low budget that is the
    // difference between a visible hitch on every order and none.
    SpProfBegin(SP_PROF_ASTAR);
    StrategyMoveBeginFrame();
    SpServiceUpdate();
    StrategyMoveCollect();
    SpProfEnd(SP_PROF_ASTAR);

    // Form-up BEFORE the state machine, for the same reason as pathing: every
    // unit in a group must scale its speed against the SAME snapshot of how
    // scattered the group is. Computed inside UnitUpdate it would depend on
    // update order, and units early in the roster would pace themselves against
    // a group that had already half-moved.
    StrategyMoveFormUpdate();

    SpProfBegin(SP_PROF_UNIT_UPDATE);
    // Snapshot the count, and re-check `active` inside the loop. UnitUpdate can
    // kill - a unit dies mid-tick from an attack resolved in its own update -
    // and RosterRemove swaps the last entry down into the dead slot. Walking a
    // live s_activeCount would then skip whichever unit got swapped in.
    // Snapshotting means the swapped-in unit is visited at its old index
    // instead: it may be re-visited or missed for exactly one frame, which is
    // invisible, whereas iterating a shrinking list drops units silently.
    // Spawns are safe either way - they append past the snapshot and simply
    // start next frame, which is what the old scan did too.
    int liveUnits = s_activeCount;
    for (int k = 0; k < liveUnits && k < s_activeCount; k++)
    {
        int i = s_active[k];
        if (world.units[i].active) UnitUpdate(i, dt);
    }
    SpProfEnd(SP_PROF_UNIT_UPDATE);
    SpProfSet(SP_COUNT_UNITS_ACTIVE, s_activeCount);

    SpProfBegin(SP_PROF_SEPARATE);
    UnitSeparation(dt);
    SpProfEnd(SP_PROF_SEPARATE);

    StrategyMoveStats();

    // Flow refcounts, recomputed at 1 Hz. Cheap (O(live)) and immune to a
    // missed decrement, which is the whole reason it is a sweep.
    s_flowSweepTimer += dt;
    if (s_flowSweepTimer >= 1.0f)
    {
        s_flowSweepTimer = 0.0f;
        StrategyMoveFlowSweep((float)GetTime());
    }

    // Arrival census, for the clustering acceptance test. The fix is judged by
    // eye - a pile either stops or it does not - but the eye cannot tell a
    // still crowd from one drifting a millimetre a frame, so the overlay counts
    // too.
    //
    // `moving` is the number to watch: a settled unit leaves UNIT_MOVE the same
    // frame it parks, so an order that resolves drives this to zero and it
    // STAYS at zero. The old spiral pinned it at the group size forever.
    // `settling` catches the in-between case - units parked but still being
    // shoved back out - which is the state a half-working fix produces.
    {
        int moving = 0, settling = 0;
        for (int k = 0; k < s_activeCount; k++)
        {
            const Unit *u = &world.units[s_active[k]];
            if (u->state != UNIT_MOVE && u->state != UNIT_FLEE) continue;
            moving++;
            if (u->arrival != ARRIVE_SEEKING) settling++;
        }
        SpProfSet(SP_COUNT_UNITS_MOVING, moving);
        SpProfSet(SP_COUNT_UNITS_SETTLED, settling);
    }

    // After the sim, so the animation reflects the move that just happened
    // rather than last frame's.
    SpProfBegin(SP_PROF_ANIM);
    for (int k = 0; k < s_activeCount; k++)
    {
        Unit *u = &world.units[s_active[k]];

        StrategyEntityAnimUpdate(u, dt);
        StrategyEntityAnimRetire(u, StrategyCatalogForRole(SGB_ROLE_UNIT, u->kind));
    }
    SpProfEnd(SP_PROF_ANIM);

    CorpsesUpdate(dt);
    BuildingsUpdate(dt);

    // Enemy + animal brains live in strategy_ai.c (orders-only).
    SpProfBegin(SP_PROF_AI);
    world.aiTimer -= dt;
    if (world.aiTimer <= 0.0f)
    {
        world.aiTimer += world.aiPeriod;
        StrategyAiTick();
    }
    SpProfEnd(SP_PROF_AI);

    EffectsUpdate(dt);
}

// ----------------------------------------------------------------------------
//  Drawing: 3D world
// ----------------------------------------------------------------------------
static void DrawNode(const ResourceNode *n)
{
    const SgaAsset *asset = StrategyCatalogForRole(SGB_ROLE_NODE, n->kind);
    if (asset != NULL)
    {
        SgaStateSet set;
        StrategyAssetStateSetInit(&set);
        StrategyAssetStateSetAdd(&set, SGA_STATE_IDLE, (float)GetTime());
        StrategyAssetDrawStates(asset, FACTION_NEUTRAL, n->pos, 0.0f, 1.0f, &set);
        return;
    }

    switch (n->kind)
    {
        case NODE_TREE:
        {
            DrawCylinder(n->pos, 0.12f, 0.16f, 0.8f, 6, BROWN);
            Vector3 crown = (Vector3){ n->pos.x, 0.8f, n->pos.z };
            DrawCylinder(crown, 0.0f, 0.55f, 1.2f, 6, (Color){ 60, 140, 60, 255 });
        } break;

        case NODE_ROCK:
        {
            Vector3 body = (Vector3){ n->pos.x, 0.3f, n->pos.z };
            DrawCube(body, 0.9f, 0.6f, 0.8f, GRAY);
            DrawCubeWires(body, 0.9f, 0.6f, 0.8f, DARKGRAY);
        } break;

        case NODE_WHEAT:
        {
            // A patch of thin golden stalks.
            Color wheat = (Color){ 220, 190, 90, 255 };
            for (int i = 0; i < 4; i++)
            {
                Vector3 stalk = n->pos;
                stalk.x += ((float)(i%2) - 0.5f)*0.5f;
                stalk.z += ((float)(i/2) - 0.5f)*0.5f;
                DrawCylinder(stalk, 0.02f, 0.08f, 0.7f, 5, wheat);
            }
        } break;

        case NODE_CORPSE:
        {
            Vector3 body = (Vector3){ n->pos.x, 0.12f, n->pos.z };
            DrawCube(body, 0.7f, 0.24f, 0.5f, (Color){ 140, 60, 50, 255 });
        } break;

        default: break;     // NODE_KIND_COUNT is a sentinel, never a node
    }
}

static void DrawBuilding(BuildingKind kind, int faction, Vector3 pos, Color tint)
{
    // Same fork as DrawUnit. Buildings have no state machine, so they get one
    // IDLE slot - which is still enough for an authored building to breathe,
    // turn a wheel or flicker a fire.
    const SgaAsset *asset = StrategyCatalogForRole(SGB_ROLE_BUILDING, kind);
    if (asset != NULL)
    {
        // Alpha rides in tint.a here (the placement ghost and the scaffold both
        // fade that way), so it is passed through as the asset's alpha rather
        // than being lost.
        SgaStateSet set;
        StrategyAssetStateSetInit(&set);
        StrategyAssetStateSetAdd(&set, SGA_STATE_IDLE, (float)GetTime());
        StrategyAssetDrawStates(asset, faction, pos, 0.0f, tint.a/255.0f, &set);
        return;
    }

    switch (kind)
    {
        case BLD_HOUSE:
        {
            Vector3 body = (Vector3){ pos.x, 0.6f, pos.z };
            DrawCube(body, 1.4f, 1.2f, 1.4f, tint);
            Vector3 roofBase = (Vector3){ pos.x, 1.2f, pos.z };
            DrawCylinder(roofBase, 0.0f, 1.1f, 0.8f, 4,
                         Fade(strategyFactionColor[faction], tint.a/255.0f));
        } break;

        case BLD_LOGGING:
        {
            Vector3 body = (Vector3){ pos.x, 0.3f, pos.z };
            DrawCube(body, 1.8f, 0.6f, 1.3f, tint);
            // A "log" on top marks the wood dropoff.
            Vector3 log = (Vector3){ pos.x, 0.75f, pos.z };
            DrawCylinderEx((Vector3){ log.x - 0.7f, log.y, log.z },
                           (Vector3){ log.x + 0.7f, log.y, log.z },
                           0.18f, 0.18f, 8, Fade(BROWN, tint.a/255.0f));
        } break;

        case BLD_QUARRY:
        {
            Vector3 body = (Vector3){ pos.x, 0.25f, pos.z };
            DrawCube(body, 1.6f, 0.5f, 1.6f, tint);
            Vector3 block = (Vector3){ pos.x, 0.75f, pos.z };
            DrawCube(block, 0.6f, 0.5f, 0.6f, Fade(DARKGRAY, tint.a/255.0f));
        } break;

        case BLD_BARRACKS:
        {
            Vector3 body = (Vector3){ pos.x, 0.5f, pos.z };
            DrawCube(body, 2.0f, 1.0f, 1.4f, tint);
            Vector3 roof = (Vector3){ pos.x, 1.15f, pos.z };
            DrawCube(roof, 2.2f, 0.3f, 1.6f, Fade(DARKGRAY, tint.a/255.0f));
        } break;

        case BLD_FARM:
        {
            // Flat tilled pad with a few wheat posts.
            Vector3 pad = (Vector3){ pos.x, 0.08f, pos.z };
            DrawCube(pad, 2.0f, 0.16f, 2.0f, Fade((Color){ 150, 110, 60, 255 }, tint.a/255.0f));
            Color wheat = Fade((Color){ 220, 190, 90, 255 }, tint.a/255.0f);
            for (int i = 0; i < 4; i++)
            {
                Vector3 stalk = pos;
                stalk.x += ((float)(i%2) - 0.5f)*1.0f;
                stalk.z += ((float)(i/2) - 0.5f)*1.0f;
                DrawCylinder(stalk, 0.02f, 0.07f, 0.6f, 5, wheat);
            }
        } break;

        case BLD_TOWN_HALL:
        {
            // The biggest footprint on the map, with a faction-colored keep.
            Vector3 body = (Vector3){ pos.x, 0.7f, pos.z };
            DrawCube(body, 2.4f, 1.4f, 2.4f, tint);
            Vector3 keep = (Vector3){ pos.x, 1.7f, pos.z };
            DrawCube(keep, 1.2f, 0.6f, 1.2f,
                     Fade(strategyFactionColor[faction], tint.a/255.0f));
        } break;

        case BLD_CHANTRY:
        {
            // Pale tower with a gold spire.
            Vector3 body = (Vector3){ pos.x, 0.8f, pos.z };
            DrawCube(body, 1.2f, 1.6f, 1.2f, tint);
            Vector3 spire = (Vector3){ pos.x, 1.6f, pos.z };
            DrawCylinder(spire, 0.0f, 0.5f, 1.0f, 6, Fade(GOLD, tint.a/255.0f));
        } break;

        case BLD_FORESTRY:
        {
            // Low hut with a couple of little saplings sprouting on top.
            Vector3 body = (Vector3){ pos.x, 0.35f, pos.z };
            DrawCube(body, 1.4f, 0.7f, 1.4f, tint);
            Color leaf = Fade((Color){ 60, 140, 60, 255 }, tint.a/255.0f);
            for (int i = 0; i < 2; i++)
            {
                Vector3 sap = (Vector3){ pos.x + (i ? 0.4f : -0.4f), 0.75f, pos.z };
                DrawCylinder(sap, 0.0f, 0.28f, 0.7f, 6, leaf);
            }
        } break;

        default: break;
    }

    // Faction banner: a small colored post so ownership reads at a glance.
    Vector3 postTop = (Vector3){ pos.x + 0.8f, 1.6f, pos.z + 0.8f };
    Vector3 postBot = (Vector3){ pos.x + 0.8f, 0.0f, pos.z + 0.8f };
    DrawLine3D(postBot, postTop, Fade(strategyFactionColor[faction], tint.a/255.0f));
    DrawCube(postTop, 0.25f, 0.18f, 0.05f, Fade(strategyFactionColor[faction], tint.a/255.0f));
}

// ----------------------------------------------------------------------------
//  Render LOD. The authored path costs a per-part loop of immediate-mode
//  primitives with a matrix push/pop each, so it saturates the batcher long
//  before the simulation is in trouble. The stress lab needs to push the
//  renderer out of the way to measure the MOVER, so every unit draw runs
//  through this fork. The game never changes it and always pays AUTHORED.
// ----------------------------------------------------------------------------
static StrategyRenderLod s_renderLod = STRAT_LOD_AUTHORED;

void StrategyRenderLodSet(StrategyRenderLod lod) { s_renderLod = lod; }
StrategyRenderLod StrategyRenderLodGet(void)     { return s_renderLod; }

const char *StrategyRenderLodName(StrategyRenderLod lod)
{
    switch (lod)
    {
        case STRAT_LOD_AUTHORED:  return "AUTHORED";
        case STRAT_LOD_PRIMITIVE: return "PRIMITIVE";
        case STRAT_LOD_DOTS:      return "DOTS";
        case STRAT_LOD_NONE:      return "NONE";
        default:                  return "?";
    }
}

// ----------------------------------------------------------------------------
//  Distance culling
//
//  THE CHEAPEST WIN IN THE RENDER OVERHAUL, and it is cheap only because this
//  camera is constrained: fixed pitch, no rotation, zoom clamped to 0.35..1.45.
//  A general frustum cull would need six plane tests per unit; here the visible
//  ground is always a fixed shape around camFocus, so one squared-distance
//  compare rejects everything outside it.
//
//  The radius is DERIVED, not guessed, and it is derived PER UNIT OF ZOOM. The
//  camera offset is (0,14,10)*zoom with a 45-degree fovy, so the distance from
//  the focus to the farthest visible ground corner is exactly linear in zoom:
//
//      zoom 0.35 -> 9.0      zoom 1.00 -> 25.8
//      zoom 0.70 -> 18.0     zoom 1.45 -> 37.4      (= 25.8 * zoom throughout)
//
//  Hence the coefficient below, plus slack for a 16:10 window and a unit's own
//  height. Getting this wrong in the safe direction is nearly free to write and
//  nearly worthless: a flat 40 - the max-zoom figure, applied at every zoom -
//  culls 0% at full zoom-out, which is the case that needs it most.
//
//  Measured against the maps that exist: at maximum zoom the visible ground is
//  roughly 37x37 units, so on the 96x96 "long march" about 15% of the map is on
//  screen. The other 85% of units currently run the full authored part loop
//  every frame to produce nothing.
#define CULL_RADIUS_PER_ZOOM  28.0f

// Distance LOD tiers, as fractions of the SQUARED cull radius - so 0.35 is 59%
// of the way out, and 0.65 is 81%. Expressed against the cull radius rather
// than in world units so the tiers scale with zoom exactly as the cull does:
// the question a tier answers is "how big is this on screen", and at this
// camera that is entirely a function of the fraction of the view it sits at.
#define LOD_MID_FRAC  0.35f
#define LOD_FAR_FRAC  0.65f

// Squared, recomputed once per frame by CullBegin. Seeded wide rather than to
// zero: the HP-bar pass reads it from a DIFFERENT function than the one that
// sets it, and a zero here would silently hide every bar in the game if the 3D
// pass were ever skipped. Culling nothing is the safe failure.
static float s_cullR2 = 1.0e9f;

static void CullBegin(void)
{
    // The floor is not tuning slack - it is the guard for a camZoom that some
    // future caller sets to zero or negative. A unit popping in at the screen
    // edge is far worse than drawing a few extra.
    float r = CULL_RADIUS_PER_ZOOM*world.camZoom;
    if (r < 10.0f) r = 10.0f;
    s_cullR2 = r*r;
}

static bool CullTest(Vector3 pos)
{
    float dx = pos.x - world.camFocus.x;
    float dz = pos.z - world.camFocus.y;
    return (dx*dx + dz*dz) <= s_cullR2;
}

static void DrawUnit(const Unit *u)
{
    // BEFORE the LOD fork, the colour lookup and everything else: a culled unit
    // must cost one subtract, one multiply-add and one compare, or the cull is
    // not paying for itself.
    if (!CullTest(u->pos)) return;

    Color color = UnitColor(u);

    // Cheap tiers first: they skip the selection ring, the asset lookup and the
    // whole part loop, which is the entire point of having them.
    if (s_renderLod != STRAT_LOD_AUTHORED)
    {
        SpProfAdd(SP_COUNT_DRAWN_UNITS, 1);
        if (s_renderLod == STRAT_LOD_NONE) return;
        if (s_renderLod == STRAT_LOD_DOTS)
        {
            DrawCube((Vector3){ u->pos.x, 0.1f, u->pos.z }, 0.18f, 0.18f, 0.18f, color);
            return;
        }
        // PRIMITIVE: one box, one draw, no matrix push, no asset lookup.
        DrawCube((Vector3){ u->pos.x, 0.4f, u->pos.z }, 0.5f, 0.8f, 0.5f, color);
        if (u->selected)
        {
            DrawCubeWires((Vector3){ u->pos.x, 0.4f, u->pos.z }, 0.55f, 0.85f, 0.55f, GREEN);
        }
        return;
    }
    SpProfAdd(SP_COUNT_DRAWN_UNITS, 1);

    if (u->selected)
    {
        Vector3 ring = (Vector3){ u->pos.x, 0.02f, u->pos.z };
        DrawCircle3D(ring, 0.55f, (Vector3){ 1.0f, 0.0f, 0.0f }, 90.0f, GREEN);
    }

    // THE FORK. A role with a binding draws its authored asset; everything else
    // falls through to the hand-built primitives below, unchanged. That is what
    // makes this safe to land before a single asset exists: with no bindings,
    // every branch below is the one that runs.
    const SgaAsset *asset = StrategyCatalogForRole(SGB_ROLE_UNIT, u->kind);
    if (asset != NULL)
    {
        SgaStateSet set;
        StrategyEntityAnimSet(u, asset, &set);

        // Distance LOD. The tiers are in SQUARED distance from the focus, so
        // the whole decision costs no square root - the same reason the cull
        // above is squared.
        //
        // The thresholds are fractions of the cull radius rather than absolute
        // distances, so they follow the zoom the same way the cull does: at
        // full zoom-out a unit two thirds of the way to the screen edge is
        // genuinely tiny, and at full zoom-in the same fraction is a unit still
        // large enough to want its detail.
        float dx = u->pos.x - world.camFocus.x;
        float dz = u->pos.z - world.camFocus.y;
        float d2 = dx*dx + dz*dz;

        float minSize = 0.0f;
        bool  lowPoly = false;
        if (d2 > s_cullR2*LOD_FAR_FRAC)
        {
            minSize = 0.18f;    // drop buttons, eyes, buckles
            lowPoly = true;
        }
        else if (d2 > s_cullR2*LOD_MID_FRAC)
        {
            minSize = 0.08f;    // only the genuinely sub-pixel parts
        }

        StrategyAssetDrawStatesLod(asset, u->faction, u->pos, u->anim.yaw, 1.0f,
                                   &set, minSize, lowPoly);
    }
    // THE BUILT-IN PRIMITIVES ARE WHERE THE TRIANGLES ACTUALLY ARE, and that is
    // the opposite of what the plan assumed. Measured on the three assets that
    // exist: worker, soldier and ranged are each ONE visible 8-sided cylinder -
    // 32 triangles, two draw calls. The fallback below is 2-3 draws with a
    // DrawSphere head at raylib's default 16x16 rings, which is 512 triangles
    // on its own, and it serves four of the seven kinds (both templars, both
    // animals) because those have no binding.
    //
    // So the LOD tiers below apply to THIS path, not the authored one: a head
    // is under a pixel at distance and costs sixteen times the body.
    else
    {
        float ddx = u->pos.x - world.camFocus.x;
        float ddz = u->pos.z - world.camFocus.y;
        float dd2 = ddx*ddx + ddz*ddz;
        bool  lowPoly = (dd2 > s_cullR2*LOD_MID_FRAC);
        bool  noDetail = (dd2 > s_cullR2*LOD_FAR_FRAC);

        switch (u->kind)
        {
        case KIND_WORKER:
        {
            DrawCylinder(u->pos, 0.28f, STRAT_UNIT_RADIUS, 0.8f, 8, color);
            if (!noDetail)
            {
                Vector3 head = (Vector3){ u->pos.x, 0.95f, u->pos.z };
                if (lowPoly) DrawSphereEx(head, 0.18f, 6, 6, ColorBrightness(color, 0.3f));
                else         DrawSphere(head, 0.18f, ColorBrightness(color, 0.3f));
            }
        } break;

        case KIND_SOLDIER:
        {
            // Taller, wider, darker - reads as "military" at a glance.
            Color dark = ColorBrightness(color, -0.25f);
            DrawCylinder(u->pos, 0.34f, 0.45f, 1.1f, 8, dark);
            if (!noDetail)
            {
                Vector3 head = (Vector3){ u->pos.x, 1.28f, u->pos.z };
                if (lowPoly) DrawSphereEx(head, 0.2f, 6, 6, color);
                else         DrawSphere(head, 0.2f, color);
            }
        } break;

        case KIND_RANGED:
        {
            // Slighter than the soldier, with a "bow" post at the side.
            Color light = ColorBrightness(color, 0.15f);
            DrawCylinder(u->pos, 0.30f, 0.40f, 1.0f, 8, light);
            if (!noDetail)
            {
                Vector3 head = (Vector3){ u->pos.x, 1.18f, u->pos.z };
                if (lowPoly) DrawSphereEx(head, 0.18f, 6, 6, color);
                else         DrawSphere(head, 0.18f, color);
                DrawLine3D((Vector3){ u->pos.x + 0.35f, 0.25f, u->pos.z },
                           (Vector3){ u->pos.x + 0.35f, 1.05f, u->pos.z }, DARKBROWN);
            }
        } break;

        case KIND_TEMPLAR:
        case KIND_TEMPLAR_HEALER:
        {
            // White robe, gold (templar) or lime (healer) head, faction band.
            Color halo = (u->kind == KIND_TEMPLAR) ? GOLD : LIME;
            DrawCylinder(u->pos, 0.26f, 0.42f, 1.0f, 8, RAYWHITE);
            if (!noDetail)
            {
                Vector3 band = (Vector3){ u->pos.x, 0.55f, u->pos.z };
                DrawCube(band, 0.5f, 0.12f, 0.5f, color);
                Vector3 head = (Vector3){ u->pos.x, 1.16f, u->pos.z };
                if (lowPoly) DrawSphereEx(head, 0.17f, 6, 6, halo);
                else         DrawSphere(head, 0.17f, halo);
            }
            else
            {
                // The band is what makes a templar readable as YOURS at range,
                // so it survives when the head does not - it is 12 triangles
                // against the head's 512.
                DrawCube((Vector3){ u->pos.x, 0.55f, u->pos.z },
                         0.5f, 0.12f, 0.5f, color);
            }
        } break;

        case KIND_ANIMAL_WEAK:
        {
            // Small low critter, no head sphere.
            Vector3 body = (Vector3){ u->pos.x, 0.22f, u->pos.z };
            DrawCube(body, 0.55f, 0.4f, 0.35f, color);
        } break;

        case KIND_ANIMAL_STRONG:
        {
            // Bigger, darker beast - reads as "don't poke it".
            Vector3 body = (Vector3){ u->pos.x, 0.34f, u->pos.z };
            DrawCube(body, 0.9f, 0.65f, 0.55f, ColorBrightness(color, -0.35f));
            // The wireframe is pure silhouette detail and doubles this kind's
            // draw calls; at distance it is a smudge on top of a smudge.
            if (!noDetail) DrawCubeWires(body, 0.9f, 0.65f, 0.55f, DARKBROWN);
        } break;

        default: break;
        }
    }

    // Decorations sit OUTSIDE the fork: they describe what a unit is doing, not
    // what it looks like, so they belong to every unit whether its model is
    // authored or built in.
    //
    // Tending workers wear a "hat" shaped like the resource they plant: a green
    // cone (forestry -> wood) or a golden cone (farm -> wheat). Only worn once
    // equipped (after the trip back to the building).
    if (u->kind == KIND_WORKER && u->state == UNIT_FARM &&
        u->tendEquipped && u->targetBuilding >= 0)
    {
        const BuildingDef *tb =
            StrategyBuildingDef(world.buildings[u->targetBuilding].kind);
        if (tb->tendNode >= 0)
        {
            Color hat = (tb->tendNode == NODE_TREE)
                ? (Color){ 60, 140, 60, 255 } : (Color){ 220, 190, 90, 255 };
            Vector3 cap = (Vector3){ u->pos.x, 1.12f, u->pos.z };
            DrawCylinder(cap, 0.0f, 0.16f, 0.28f, 6, hat);
        }
    }

    // Carried resources float above the head, tinted by kind.
    if (u->carryAmount > 0)
    {
        Color carry = (u->carryKind == RES_WOOD)  ? BROWN
                    : (u->carryKind == RES_STONE) ? GRAY
                    : (Color){ 220, 190, 90, 255 };
        Vector3 pack = (Vector3){ u->pos.x, 1.35f, u->pos.z };
        DrawCube(pack, 0.2f, 0.2f, 0.2f, carry);
    }
}

void StrategyWorldDraw3D(void)
{
    SpProfBegin(SP_PROF_DRAW_WORLD);
    BeginMode3D(world.camera);

    // Plane slightly below the grid lines to avoid z-fighting. Sized from the
    // world's extent, so an authored map's ground reaches its own edges rather
    // than the built-in 50x50.
    DrawPlane((Vector3){ 0.0f, -0.01f, 0.0f },
              (Vector2){ 2.0f*world.groundHalfX, 2.0f*world.groundHalfZ },
              (Color){ 90, 110, 80, 255 });
    MapDrawTerrain();
    DrawGroundGrid();
    // After the terrain, so the cost tint reads ON TOP of the water and cliff
    // colours it is supposed to be checked against.
    if (s_navShow) NavDraw();

    // Paths above the nav tint but before the props, so a route reads against
    // the terrain it crosses rather than through a tree.
    if (s_pathShow) PathDraw();

    // Flow arrows go UNDER the paths: when both are on, the question is whether
    // an individual route agrees with the field, and the route has to be the
    // one on top for that comparison to be readable.
    if (s_flowShow) FlowDraw();
    if (s_slotShow) SlotDraw();

    for (int i = 0; i < STRAT_MAX_NODES; i++)
    {
        if (world.nodes[i].active) DrawNode(&world.nodes[i]);
    }
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        Building *b = &world.buildings[i];
        if (!b->active) continue;

        // A scaffold reads as a faint, translucent version of the finished
        // building wrapped in a wireframe frame so it's clearly "not done".
        Color tint = b->underConstruction ? Fade(BEIGE, 0.35f) : BEIGE;
        DrawBuilding(b->kind, b->faction, b->pos, tint);
        if (b->underConstruction)
        {
            Vector3 frame = (Vector3){ b->pos.x, 0.9f, b->pos.z };
            DrawCubeWires(frame, 2.0f, 1.8f, 2.0f, Fade(BROWN, 0.8f));
        }
        if (i == world.selectedBuilding)
        {
            Vector3 ring = (Vector3){ b->pos.x, 0.02f, b->pos.z };
            DrawCircle3D(ring, 1.5f, (Vector3){ 1.0f, 0.0f, 0.0f }, 90.0f, GREEN);

            // Rally flag for the selected training building.
            if (b->hasRally)
            {
                Vector3 top = (Vector3){ b->rally.x, 1.4f, b->rally.z };
                Vector3 bot = (Vector3){ b->rally.x, 0.0f, b->rally.z };
                DrawLine3D(bot, top, GREEN);
                DrawCube((Vector3){ b->rally.x + 0.2f, 1.25f, b->rally.z },
                         0.4f, 0.25f, 0.05f, GREEN);
                DrawCircle3D((Vector3){ b->rally.x, 0.02f, b->rally.z }, 0.5f,
                             (Vector3){ 1.0f, 0.0f, 0.0f }, 90.0f, Fade(GREEN, 0.6f));
            }
        }
    }
    SpProfEnd(SP_PROF_DRAW_WORLD);

    SpProfBegin(SP_PROF_DRAW_UNITS);
    CullBegin();        // once per frame: the radius only moves with the zoom
    for (int k = 0; k < s_activeCount; k++) DrawUnit(&world.units[s_active[k]]);
    SpProfEnd(SP_PROF_DRAW_UNITS);

    SpProfBegin(SP_PROF_DRAW_WORLD);

    // Corpses finish their DIE animation after the slot is gone. Drawn with the
    // units so they sort the same way; a corpse whose role lost its binding
    // simply stops drawing, since there is no built-in death pose to fall back
    // on.
    for (int i = 0; i < STRAT_MAX_CORPSES; i++)
    {
        const UnitCorpse *c = &world.corpses[i];
        if (!c->active) continue;

        const SgaAsset *ca = StrategyCatalogForRole(SGB_ROLE_UNIT, c->kind);
        if (ca == NULL) continue;

        SgaStateSet set;
        StrategyAssetStateSetInit(&set);
        StrategyAssetStateSetAdd(&set, SGA_STATE_DIE, c->t);
        StrategyAssetDrawStates(ca, c->faction, c->pos, c->yaw, 1.0f, &set);
    }

    // Placement ghost: green when the spot is valid, red when not.
    if (world.placing >= 0)
    {
        Vector3 ghost;
        if (PlacementGhost(&ghost))
        {
            bool valid = PlacementValid(0, (BuildingKind)world.placing, ghost);
            Color tint = Fade(valid ? GREEN : RED, 0.5f);
            DrawBuilding((BuildingKind)world.placing, 0, ghost, tint);
        }
    }

    EffectsDraw3D();
    EndMode3D();
    SpProfEnd(SP_PROF_DRAW_WORLD);
}

// ----------------------------------------------------------------------------
//  Drawing: 2D overlay in GAME-CANVAS space (drawn after EndMode3D)
// ----------------------------------------------------------------------------
void StrategyWorldDraw2DOverlay(void)
{
    Vector2 gameSize = ScreenStateTargetSize();

    // Drag-selection rectangle.
    if (world.dragging)
    {
        Vector2 mouse = MouseGame();
        Rectangle rect = {
            fminf(world.dragStart.x, mouse.x), fminf(world.dragStart.y, mouse.y),
            fabsf(mouse.x - world.dragStart.x), fabsf(mouse.y - world.dragStart.y),
        };
        DrawRectangleRec(rect, Fade(GREEN, 0.12f));
        DrawRectangleLinesEx(rect, 1.0f, GREEN);
    }

    // HP bars over damaged units.
    for (int k = 0; k < s_activeCount; k++)
    {
        Unit *u = &world.units[s_active[k]];
        if (u->hp >= u->maxHp) continue;
        // Same cull as the unit itself. WorldToGame is a full matrix projection
        // per unit, so at ten thousand units this loop costs as much as the
        // draw it annotates - and every one of those bars is for a unit that
        // was not drawn.
        if (!CullTest(u->pos)) continue;

        Vector2 sp = WorldToGame((Vector3){ u->pos.x, 1.5f, u->pos.z });
        float frac = u->hp/u->maxHp;
        DrawRectangle((int)sp.x - 12, (int)sp.y, 24, 4, Fade(RED, 0.8f));
        DrawRectangle((int)sp.x - 12, (int)sp.y, (int)(24.0f*frac), 4, GREEN);
    }

    // HP bars over damaged buildings (wider).
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        Building *b = &world.buildings[i];
        if (!b->active || b->hp >= b->maxHp) continue;

        Vector2 sp = WorldToGame((Vector3){ b->pos.x, 2.2f, b->pos.z });
        float frac = b->hp/b->maxHp;
        DrawRectangle((int)sp.x - 20, (int)sp.y, 40, 5, Fade(RED, 0.8f));
        DrawRectangle((int)sp.x - 20, (int)sp.y, (int)(40.0f*frac), 5, GREEN);
    }

    // Resource HUD: player big, enemy small below (visible AI progress).
    int size = (int)fmaxf(10.0f, gameSize.y*0.045f);
    DrawText(TextFormat("WOOD %d   STONE %d   FOOD %d   PROV %d   POP %d/%d",
                        world.stockpile[0][RES_WOOD], world.stockpile[0][RES_STONE],
                        world.stockpile[0][RES_FOOD], world.stockpile[0][RES_PROVIDENCE],
                        StrategyPopUsed(0), StrategyPopCap(0)),
             (int)(gameSize.x*0.02f), (int)(gameSize.y*0.02f), size, RAYWHITE);
    DrawText(TextFormat("enemy: wood %d stone %d food %d prov %d pop %d/%d",
                        world.stockpile[1][RES_WOOD], world.stockpile[1][RES_STONE],
                        world.stockpile[1][RES_FOOD], world.stockpile[1][RES_PROVIDENCE],
                        StrategyPopUsed(1), StrategyPopCap(1)),
             (int)(gameSize.x*0.02f), (int)(gameSize.y*0.02f) + size + 4, size/2,
             Fade(strategyFactionColor[1], 0.8f));

    if (world.placing >= 0)
    {
        const char *hint = "LMB place - Shift+LMB place more - RMB/ESC cancel";
        DrawText(hint, (int)(gameSize.x*0.5f - (float)MeasureText(hint, size/2)*0.5f),
                 (int)(gameSize.y*0.9f), size/2, RAYWHITE);
    }

    // Formation readout. Shown only while units are selected - it is a property
    // of the order you are about to give, so it is noise at every other moment.
    {
        int selCount = 0;
        SelectedUnits(&selCount);
        if (selCount > 0)
        {
            const char *txt = TextFormat("[F] %s   [V] %s",
                                         StrategyFormationShapeName(StrategyFormationShape()),
                                         StrategyFormationBehaviorName(StrategyFormationBehavior()));
            int fs = size/2;
            DrawText(txt, (int)(gameSize.x*0.5f - (float)MeasureText(txt, fs)*0.5f),
                     (int)(gameSize.y*0.945f), fs, (Color){ 190, 195, 205, 255 });
        }
    }

    // Victory/defeat banner: the sim keeps running underneath, R restarts.
    if (world.gameOver >= 0)
    {
        const char *msg = (world.gameOver == 0) ? "VICTORY" : "DEFEAT";
        Color tint = (world.gameOver == 0) ? GOLD : RED;
        int bigSize = (int)(gameSize.y*0.14f);
        DrawText(msg, (int)(gameSize.x*0.5f - (float)MeasureText(msg, bigSize)*0.5f),
                 (int)(gameSize.y*0.36f), bigSize, tint);
        const char *why = (world.gameOver == 0)
            ? "the enemy lost every critical building and worker"
            : "you lost every critical building and worker";
        DrawText(why, (int)(gameSize.x*0.5f - (float)MeasureText(why, size/2)*0.5f),
                 (int)(gameSize.y*0.36f) + bigSize + 8, size/2, LIGHTGRAY);
        const char *sub = "press R to restart";
        DrawText(sub, (int)(gameSize.x*0.5f - (float)MeasureText(sub, size)*0.5f),
                 (int)(gameSize.y*0.36f) + bigSize + size/2 + 16, size, RAYWHITE);
    }
}
