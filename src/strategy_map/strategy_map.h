// ============================================================================
//  strategy_map.h  -  authored battlefields ("SGM"), saved as binary
//
//  The RTS battlefield used to be forty lines of hardcoded spawn calls inside
//  StrategyWorldInit(). This module is the data behind a battlefield that a
//  user AUTHORS in the map forge and saves to a file, which the showcase then
//  lists and the game plays.
//
//  A MAP IS A GRID PLUS A LIST. The grid carries terrain, elevation and
//  passability per tile; the list carries what stands on it (buildings, units,
//  resource nodes) and where each faction starts. Nothing here draws, and
//  nothing here calls raylib UI - this file and strategy_map_io.c link into a
//  headless test binary, which is what keeps the validation rules testable.
//
//  TILES ARE WORLD UNITS. The game already snaps building placement to whole
//  world units (PlacementGhost -> roundf), so one tile IS one world unit and
//  one visible gridline cell. The mapping is therefore a centring offset and
//  nothing more - see SgmTileToWorld. There is no second coordinate system to
//  keep in sync, deliberately.
//
//  TERRAIN AND PASSABILITY ARE SEPARATE FIELDS. "Deep water" and "hole in the
//  map" are both impassable and look nothing alike, and an author may well want
//  a passable shallow ford or a walkable-but-unbuildable ridge. Deriving one
//  from the other would make those unauthorable, so the terrain kind sets the
//  DEFAULT flags (SgmTerrainDefaultFlags) and the author may then paint over
//  them.
//
//  FACTIONS: the format carries 1..9 of them from day one. The runtime still
//  instantiates only two (see STRAT_FACTIONS); factions 2..8 are authored,
//  validated and stored, and the game ignores them until that work lands. This
//  is on purpose - it is the format that is expensive to change later, not the
//  spawn loop.
//
//  MEMORY: fixed-capacity and heap-free, house style, so an SgmMap is a plain
//  value the forge's undo ring can memcpy. At the desktop tier the grid
//  dominates: 128x128 tiles x 4 bytes = 64 KB, which is why a deeper undo ring
//  is affordable here than in the asset forge (an SgaAsset is ~450 KB).
//  Still: DO NOT PUT AN SgmMap ON THE STACK on Web, where the stack is 1 MB.
// ============================================================================

#ifndef STRATEGY_MAP_H
#define STRATEGY_MAP_H

#include "raylib.h"
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
//  Capacities. The #ifndef-guarded ones are raised by CMakeLists.txt on every
//  non-Web build; the values here are the WEB tier.
// ---------------------------------------------------------------------------
#define SGM_NAME_MAX        32      // map name buffer
#define SGM_DESC_MAX        96      // one-line description shown in the picker

#define SGM_FACTIONS_MAX     9      // the authored range is 1..9 (NOT tiered:
                                    //   it is the feature, not a budget)

#ifndef SGM_GRID_MAX
#define SGM_GRID_MAX       128      // max tiles per side (square cap)
#endif
#ifndef SGM_PLACES_MAX
#define SGM_PLACES_MAX     256      // buildings + units + nodes on one map
#endif
#ifndef SGM_MAPS_MAX
#define SGM_MAPS_MAX        32      // .sgm files held in memory at once
#endif

#define SGM_TILES_MAX  (SGM_GRID_MAX*SGM_GRID_MAX)

// The grid the GAME can currently host, in world units. STRAT_GROUND_HALF is
// 25.0f, so the playable square is 50x50. Kept as a plain number rather than an
// include so this module stays free of the game headers (headless tests).
#define SGM_WORLD_GRID   50

// ---------------------------------------------------------------------------
//  Terrain
//
//  The kind is a LOOK plus a default passability. Order is on-disk order: only
//  append.
// ---------------------------------------------------------------------------
typedef enum {
    SGM_TERRAIN_GROUND = 0,     // plain walkable, buildable ground
    SGM_TERRAIN_GRASS,          // cosmetic variation on ground
    SGM_TERRAIN_DIRT,           // cosmetic variation on ground
    SGM_TERRAIN_SHALLOW,        // shallow water: walkable, not buildable
    SGM_TERRAIN_WATER,          // deep water: blocks
    SGM_TERRAIN_ROCK,           // boulder field / large rocks: blocks
    SGM_TERRAIN_CLIFF,          // sheer face: blocks
    SGM_TERRAIN_VOID,           // hole in the map: blocks, and draws nothing
    SGM_TERRAIN_COUNT
} SgmTerrain;

// Per-tile flags. Passability and buildability are separate so a map can carry
// walkable-but-unbuildable ground (a ford, a bridge, a steep ridge).
#define SGM_TILE_PASSABLE   0x01
#define SGM_TILE_BUILDABLE  0x02

#define SGM_HEIGHT_MAX      15      // elevation steps; 0 is ground level

// One tile. Four bytes, no padding, and it is the on-disk layout too.
typedef struct {
    uint8_t terrain;    // SgmTerrain
    uint8_t height;     // 0..SGM_HEIGHT_MAX
    uint8_t flags;      // SGM_TILE_* bits
    uint8_t variant;    // visual variation seed, cosmetic only
} SgmTile;

// ---------------------------------------------------------------------------
//  Placements
// ---------------------------------------------------------------------------
// Which family a placement's `kind` indexes into. The map does NOT hardcode the
// game's enums - it stores the family plus the numeric kind, and the game maps
// them onto BuildingKind / UnitKind / NodeKind when it spawns. That keeps this
// module free of the game headers.
typedef enum {
    SGM_PLACE_BUILDING = 0,
    SGM_PLACE_UNIT,
    SGM_PLACE_NODE,
    SGM_PLACE_COUNT
} SgmPlaceFamily;

// Faction id meaning "nobody owns this" - resource nodes and wild animals.
// Distinct from any authored faction index (0..count-1).
#define SGM_FACTION_NEUTRAL  0xFF

typedef struct {
    int16_t tileX, tileZ;   // tile coordinates, so a placement is never off-grid
    uint8_t family;         // SgmPlaceFamily
    uint8_t kind;           // index within that family (game's enum value)
    uint8_t faction;        // 0..count-1, or SGM_FACTION_NEUTRAL
    uint8_t amount;         // resource nodes: yield bucket. 0 = the game default
} SgmPlacement;

// ---------------------------------------------------------------------------
//  Faction start
// ---------------------------------------------------------------------------
typedef struct {
    int16_t tileX, tileZ;   // where this faction's base is centred
    uint8_t colorIndex;     // index into the map forge's faction palette
    uint8_t reserved;       // keeps the struct 6 bytes and padding-free
} SgmFactionStart;

// ---------------------------------------------------------------------------
//  Gridline appearance, authored per map
//
//  The game drew raylib's DrawGrid(), which is a fixed LIGHTGRAY at full alpha
//  with no parameters - the reason gridlines read as too aggressive. A map
//  carries its own default; the player can still override it globally.
// ---------------------------------------------------------------------------
typedef enum {
    SGM_GRID_OFF = 0,
    SGM_GRID_SUBTLE,
    SGM_GRID_NORMAL,
    SGM_GRID_STRONG,
    SGM_GRID_STYLE_COUNT
} SgmGridStyle;

// ---------------------------------------------------------------------------
//  The map document
// ---------------------------------------------------------------------------
// Tagged (unlike the small structs above) so that a consumer can forward-declare
// `struct SgmMap` and hold a pointer to one without including this header. The
// game's StrategyWorld does exactly that: it borrows the map it was built from,
// but strategy_types.h must not pull in the map module.
typedef struct SgmMap {
    char    name[SGM_NAME_MAX];
    char    desc[SGM_DESC_MAX];

    int32_t gridW, gridH;           // used extent, <= SGM_GRID_MAX

    int32_t factionCount;           // 1..SGM_FACTIONS_MAX
    SgmFactionStart starts[SGM_FACTIONS_MAX];

    SgmTile tiles[SGM_TILES_MAX];   // row-major: index = z*gridW + x

    int32_t placeCount;
    SgmPlacement places[SGM_PLACES_MAX];

    int32_t gridStyle;              // SgmGridStyle, the map's authored default
} SgmMap;

// ---------------------------------------------------------------------------
//  Lifecycle
// ---------------------------------------------------------------------------
// A blank map: all ground, one faction, no placements. `w`/`h` are clamped into
// [8, SGM_GRID_MAX].
void SgmMapInit(SgmMap *m, const char *name, int w, int h);

// Resize the used extent, preserving the tiles that stay in range and filling
// any newly exposed ones with ground. Placements and starts that fall outside
// the new extent are dropped (starts are clamped inward instead).
void SgmMapResize(SgmMap *m, int w, int h);

// ---------------------------------------------------------------------------
//  Tiles
// ---------------------------------------------------------------------------
bool SgmInBounds(const SgmMap *m, int x, int z);

// NULL when out of bounds - callers must check, which is why these exist rather
// than exposing the raw index arithmetic at every site.
SgmTile *SgmTileAt(SgmMap *m, int x, int z);
const SgmTile *SgmTileAtConst(const SgmMap *m, int x, int z);

// The default flags for a terrain kind. Painting terrain applies these; the
// author may then override them with the passability tool.
uint8_t SgmTerrainDefaultFlags(int terrain);

// Does this terrain block movement by default? (A convenience over the flags.)
bool SgmTerrainBlocks(int terrain);

// Passability of a tile as authored, out-of-bounds counting as blocked.
bool SgmTilePassable(const SgmMap *m, int x, int z);
bool SgmTileBuildable(const SgmMap *m, int x, int z);

// Paint one tile's terrain, resetting its flags to that terrain's defaults.
void SgmPaintTerrain(SgmMap *m, int x, int z, int terrain);

// ---------------------------------------------------------------------------
//  Tile <-> world
//
//  One tile is one world unit; the grid is centred on the origin, so a map of
//  width w spans [-w/2, +w/2]. Tile centres land on whole world units when the
//  extent is even, which is what the game's roundf placement snap expects.
// ---------------------------------------------------------------------------
Vector3 SgmTileToWorld(const SgmMap *m, int x, int z);
void    SgmWorldToTile(const SgmMap *m, float wx, float wz, int *outX, int *outZ);

// ---------------------------------------------------------------------------
//  Placements
// ---------------------------------------------------------------------------
// Appends; returns the new index, or -1 when full or out of bounds.
int  SgmPlaceAdd(SgmMap *m, int family, int kind, int faction, int x, int z);

// Removes by index, preserving order (the order of buildings within a faction
// is meaningful - see the town-hall-first rule in SgmValidate).
void SgmPlaceRemove(SgmMap *m, int index);

// Topmost placement on a tile, or -1. Searches backwards so the most recently
// added wins, which is what clicking a stack expects.
int  SgmPlaceAt(const SgmMap *m, int x, int z);

// How many placements of a family the map holds - the editor's budget readout.
int  SgmPlaceCountOf(const SgmMap *m, int family);

// ---------------------------------------------------------------------------
//  Validation
//
//  Two tiers. `SgmValidate` reports everything wrong with a map; a map with any
//  ERROR is not playable and the forge refuses to mark it so. Warnings are
//  advice and never block.
// ---------------------------------------------------------------------------
typedef enum {
    SGM_OK = 0,
    SGM_ERR_FACTION_COUNT,      // outside 1..SGM_FACTIONS_MAX
    SGM_ERR_START_OOB,          // a faction's start tile is off-grid
    SGM_ERR_START_BLOCKED,      // a start sits on impassable/unbuildable ground
    SGM_ERR_START_OVERLAP,      // two starts are on top of each other
    SGM_ERR_NO_TOWN_HALL,       // a playing faction has no town hall
    SGM_ERR_TOWN_HALL_NOT_FIRST,// its town hall is not its first building
    SGM_ERR_OVER_BUDGET,        // more entities than the world can host
    SGM_ERR_PLACE_BLOCKED,      // something stands on impassable ground
    SGM_ERR_COUNT
} SgmIssueCode;

#define SGM_ISSUES_MAX  32

typedef struct {
    int  code;              // SgmIssueCode
    bool warning;           // advice, not a blocker
    int  faction;           // -1 when not faction-specific
    int  place;             // placement index, or -1
} SgmIssue;

typedef struct {
    int      count;
    int      errors;        // how many of them block play
    SgmIssue items[SGM_ISSUES_MAX];
} SgmReport;

// The world's per-kind capacities, passed in rather than included so this file
// stays free of the game headers. The caller hands over STRAT_MAX_*.
typedef struct {
    int buildings;
    int units;
    int nodes;
} SgmBudget;

// The building `kind` the town-hall rule looks for. Passed in for the same
// reason - the game owns BuildingKind, this module must not.
void SgmValidate(const SgmMap *m, SgmBudget budget, int townHallKind,
                 SgmReport *out);

// True when the map has no blocking errors.
bool SgmPlayable(const SgmMap *m, SgmBudget budget, int townHallKind);

// A human-readable line for one issue, for the forge's status strip.
const char *SgmIssueText(const SgmIssue *issue);

#endif // STRATEGY_MAP_H
