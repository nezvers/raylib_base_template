// ============================================================================
//  strategy_path.h  -  navigation and steering support, UI-free
//
//  WHAT THIS MODULE IS. Grid navigation, spatial queries and (later) A* and
//  flow fields, with NO knowledge of Unit, factions, orders or raylib UI. It is
//  a sibling of src/strategy_map/ and split off for the same reason: so it can
//  link into a headless test binary. The game feeds it flat floats and gets
//  back indices and directions.
//
//  IT MUST NOT INCLUDE strategy_types.h. That header pulls in strategy_asset.h
//  and from there the whole anim system, which would make this module
//  untestable and drag the test binary into the renderer. If you ever need a
//  Unit field in here, you need it in strategy_move.c instead.
//
//  raylib IS included, but only for Vector2/Vector3 and the maths inlines -
//  no window, no draw calls, no input. That is what map_tests already does.
//
//  TWO-TIER CAPS. Every SP_*_MAX below is the WEB default. Desktop raises them
//  from CMakeLists via target_compile_definitions, the same scheme as SGM_*,
//  SGA_* and STRAT_MAX_UNITS. Web stays small as a PERFORMANCE guard, not a
//  memory one - a browser cannot draw ten thousand procedural units at any
//  framerate, so there is no point sizing for them.
// ============================================================================

#ifndef STRATEGY_PATH_H
#define STRATEGY_PATH_H

#include "raylib.h"
#include <stdbool.h>
#include <stdint.h>

// -- Capacities ---------------------------------------------------------------
#ifndef SP_GRID_MAX
#define SP_GRID_MAX          128    // Web tier; desktop raised via CMake
#endif

#ifndef SP_HASH_CELLS_MAX
#define SP_HASH_CELLS_MAX    16384  // Web tier; desktop raised via CMake
#endif

#ifndef SP_HASH_ITEMS_MAX
#define SP_HASH_ITEMS_MAX    256    // Web tier; must be >= STRAT_MAX_UNITS
#endif

#ifndef SP_PATH_MAX
#define SP_PATH_MAX          12     // Web tier; desktop raised via CMake
#endif

#ifndef SP_REQUESTS_MAX
#define SP_REQUESTS_MAX      64     // Web tier; desktop raised via CMake
#endif

#define SP_CELLS_MAX         (SP_GRID_MAX*SP_GRID_MAX)

// -- Cell index width ---------------------------------------------------------
//  A 256x256 map - the largest SGM_GRID_MAX allows the forge to author - is
//  65,536 cells, which is ONE PAST what a uint16_t can address, and that is
//  before reserving a value for "no cell". So cell indices are uint32_t.
//
//  This was very nearly a uint16_t: at 65,535 the arithmetic still "works" and
//  the overflow only bites on the single far-corner cell of the single largest
//  map, which is exactly the kind of bug that ships. The memory it costs is
//  real but bounded - the A* side arrays double - and paying it is cheaper than
//  capping the grid below what the map format already permits.
typedef uint32_t SpCell;

#define SP_CELL_NONE  ((SpCell)0xFFFFFFFFu)

_Static_assert(SP_CELLS_MAX < SP_CELL_NONE, "grid too large for SpCell");

// -- Spatial hash -------------------------------------------------------------
//  Uniform grid over the XZ plane, rebuilt from scratch every frame. Rebuilding
//  beats incremental maintenance here: every unit moves every frame anyway, so
//  an update is a remove plus an insert, and the rebuild is one linear pass
//  with perfect locality.
//
//  INTRUSIVE HEAD/NEXT LISTS, not per-cell bucket arrays. A bucket array needs
//  a fixed per-cell capacity, which either wastes memory across a mostly-empty
//  map or silently drops units where they pile up - and RTS units pile up by
//  definition, so the failure would appear exactly where separation matters
//  most. head[cell] indexes the first item, next[item] chains the rest.
//
//  px/pz/id ARE COPIES, and that is the entire performance argument. The
//  neighbor loop is the hottest loop in the game; walking it through 248-byte
//  Unit structs means a cache miss per candidate. Three parallel arrays of
//  4-byte scalars keep a whole chain in a couple of cache lines.
typedef struct {
    float   cellSize;
    float   invCellSize;            // premultiplied: the query is per-neighbor
    int32_t w, h;
    float   originX, originZ;       // world position of cell (0,0)'s corner

    int32_t head[SP_HASH_CELLS_MAX];    // -1 = empty
    int32_t next[SP_HASH_ITEMS_MAX];    // -1 = end of chain
    float   px[SP_HASH_ITEMS_MAX];
    float   pz[SP_HASH_ITEMS_MAX];
    int32_t id[SP_HASH_ITEMS_MAX];      // caller's opaque handle (a unit index)

    int32_t count;                      // items inserted this rebuild
    int32_t dropped;                    // items refused: SP_HASH_ITEMS_MAX hit
} SpHash;

// Prepare for a rebuild. `cellSize` should be about twice the query radius, so
// a query touches a 2x2 block of cells rather than 3x3 - four chains to walk
// instead of nine, for the same answer.
//
// halfX/halfZ are the world half-extents to cover. Anything inserted outside
// is CLAMPED into the edge cells rather than dropped: a unit shoved past the
// map edge by a crowd must still push back, and a silently un-hashed unit is
// an invisible hole in the collision that only shows up under load.
void SpHashBegin(SpHash *h, float cellSize, float halfX, float halfZ);

// Insert one point. Returns false only when SP_HASH_ITEMS_MAX is exhausted, in
// which case `dropped` counts it - check that in the overlay rather than
// discovering it as mysterious clipping.
bool SpHashInsert(SpHash *h, int32_t id, float x, float z);

// Ids of every item within `radius` of (x,z), written to out[], capped at
// maxOut. Returns how many were written.
//
// TWO RULES. The result is UNORDERED - it follows insertion chains, so do not
// depend on the sequence for anything that must be deterministic across a
// rebuild. And when more than maxOut neighbors qualify the extras are
// TRUNCATED, not sampled: the cap exists so a death-ball has a flat worst case
// rather than a quadratic spike, and beyond eight pushers the summed direction
// barely moves.
int SpHashQuery(const SpHash *h, float x, float z, float radius,
                int32_t *out, int maxOut);

// Deterministic separation direction for a pair that is exactly coincident.
// Hashing both ids means the SAME pair always parts the same way, whatever
// order the query returned them in - which is what makes a pile settle instead
// of shimmering, and what lets the tests assert on it at all.
void SpSeparationJitter(int32_t idA, int32_t idB, float *outX, float *outZ);

// -- Navigation grid ----------------------------------------------------------
//  A per-tile traversal cost over the same tile lattice the map format uses:
//  one tile is one world unit, the grid is centred on the origin. That is not a
//  coincidence to be re-derived - SpTileToWorld/SpWorldToTile below MUST agree
//  with SgmTileToWorld/SgmWorldToTile exactly, and path_tests asserts it.
//
//  0 IS THE BLOCKED SENTINEL, folded into the cost byte rather than kept in a
//  separate passability array. Every A* neighbour test is then one load and one
//  compare, not two loads from two arrays that miss cache independently. It
//  also makes "blocked" and "expensive" the same axis, which is what lets the
//  obstacle skirt work at all.
//
//  COST BANDS:
//      0        blocked
//      1        normal ground
//      2        shallow water - passable, but a path prefers dry land
//      3..8     obstacle skirt: cells adjacent to something blocked. This is
//               the highest value-per-line feature in the grid. Without it
//               paths hug walls, and a 0.35-radius unit steering along a
//               wall-hugging path scrapes the corner and jams.
//      9..255   reserved
typedef struct {
    int32_t w, h;
    uint8_t cost[SP_CELLS_MAX];
} SpGrid;

#define SP_COST_BLOCKED   0
#define SP_COST_NORMAL    1
#define SP_COST_SHALLOW   2
#define SP_COST_SKIRT     3     // cost written into cells touching an obstacle

// Reset to `w`x`h` of SP_COST_NORMAL. Dimensions are clamped into
// [1, SP_GRID_MAX]; anything past the used extent keeps whatever it held, so
// never index outside w/h.
void SpGridInit(SpGrid *g, int w, int h);

bool SpGridInBounds(const SpGrid *g, int x, int z);

// Cost at a tile. Out of bounds reads as SP_COST_BLOCKED - callers rely on this
// to avoid bounds-checking every neighbour by hand.
uint8_t SpGridCost(const SpGrid *g, int x, int z);
void    SpGridSet(SpGrid *g, int x, int z, uint8_t cost);

static inline bool SpGridPassable(const SpGrid *g, int x, int z)
{
    return SpGridCost(g, x, z) != SP_COST_BLOCKED;
}

// -- Tile <-> world -----------------------------------------------------------
//  Byte-identical to the SGM pair. Kept here as well, rather than including
//  strategy_map.h, because this module must stay linkable without it - and
//  because a nav grid also exists for the built-in no-map layout, which has no
//  SgmMap to ask.
Vector3 SpTileToWorld(const SpGrid *g, int x, int z);
void    SpWorldToTile(const SpGrid *g, float wx, float wz, int *outX, int *outZ);

// -- Stamping -----------------------------------------------------------------
// Mark a rectangle of tiles, given a world-space centre and HALF-extents in
// tiles. `cost` is written to every covered tile; SP_COST_BLOCKED for an
// obstacle. Partially covered edge tiles are included - a building overlapping
// a tile by any amount blocks it, because a unit cannot stand in the leftover
// sliver anyway.
void SpGridStampRect(SpGrid *g, float wx, float wz, int halfX, int halfZ,
                     uint8_t cost);

// Copy a rectangle (plus `pad` tiles of margin) from `src` into `dst`. This is
// how a demolition undoes a stamp: restore from the terrain-only grid rather
// than trying to work out what the tile "should" be, which cannot be answered
// once two obstacles have overlapped.
void SpGridRestoreRect(SpGrid *dst, const SpGrid *src,
                       float wx, float wz, int halfX, int halfZ, int pad);

// Recompute the obstacle skirt across the whole grid: every passable tile that
// touches a blocked one (8-connected) is raised to at least SP_COST_SKIRT.
// Idempotent - re-running it never compounds, because it only ever raises a
// normal/shallow tile to exactly SP_COST_SKIRT.
void SpGridBuildSkirt(SpGrid *g);

// Nearest passable tile to (tx,tz), searched outward in square rings up to
// `maxRing`. Writes the result and returns true; returns false when nothing
// passable is within the ring cap.
//
// EVERY GOAL MUST GO THROUGH THIS. A destination inside a lake or a building is
// unreachable, and a unit ordered there walks at it forever - which is exactly
// the orbit-forever bug Phase 2 fixed, reintroduced through the back door.
bool SpNearestOpen(const SpGrid *g, int tx, int tz, int maxRing,
                   int *outX, int *outZ);

// -- Line of sight ------------------------------------------------------------
//  True when a unit can walk the straight segment between two tile centres
//  without clipping an obstacle. This is what turns a staircase of grid cells
//  into the 4-8 waypoints a path actually needs.
//
//  SUPERCOVER, NOT STANDARD BRESENHAM. A standard line steps diagonally through
//  the shared corner of two cells; supercover visits BOTH cells the line grazes.
//  With standard, a segment between two blocked tiles that touch only at a
//  corner reads as clear, and the smoother then hands a unit a path through a
//  gap of zero width. The unit wedges on the corner and never arrives, and the
//  path looks perfectly reasonable in the overlay. path_tests names this case.
bool SpLosClear(const SpGrid *g, int x0, int z0, int x1, int z1);

// -- A* -----------------------------------------------------------------------
//  One searcher, reused for every request. Not per-unit: the state below is
//  ~1.4 MB on desktop, and N concurrent searchers would cost N times that for
//  no more throughput than serializing them through one.
//
//  THE STAMP/GENERATION TRICK. gScore/cameFrom/heapPos hold stale data from the
//  previous search, and clearing 65k-entry arrays between searches costs more
//  than most searches do. Instead `generation` is bumped and a cell counts as
//  visited only when stamp[c] == generation. Search cost then scales with
//  EXPANDED NODES, not grid size - the difference between a 200-node search
//  costing 200 units of work and costing 65,536.
typedef struct {
    SpCell   heap[SP_CELLS_MAX];    // open set, min-heap on fScore
    int32_t  heapCount;
    uint32_t heapPos[SP_CELLS_MAX]; // cell -> index in heap, for decrease-key
    uint16_t gScore[SP_CELLS_MAX];
    uint16_t fScore[SP_CELLS_MAX];
    SpCell   cameFrom[SP_CELLS_MAX];
    uint32_t stamp[SP_CELLS_MAX];   // visit generation
    uint32_t closed[SP_CELLS_MAX];  // generation at which the cell was closed
    uint32_t generation;

    // Resume state: a search that runs out of budget continues from here.
    const SpGrid *grid;
    SpCell   start, goal;
    int32_t  goalX, goalZ;
    bool     active;
    int32_t  expanded;              // nodes expanded across ALL slices
} SpAStar;

typedef enum {
    SP_PATH_FAILED = 0,     // no route exists, or the search space ran out
    SP_PATH_FOUND,          // complete, waypoints written
    SP_PATH_BUSY            // budget exhausted; call again next frame to resume
} SpPathStatus;

// Begin a search. Any search in progress on `a` is discarded. Start and goal
// are TILE coordinates and must already be passable - resolve them through
// SpNearestOpen first.
void SpAStarBegin(SpAStar *a, const SpGrid *g, int sx, int sz, int gx, int gz);

// Advance the current search by at most `nodeBudget` expansions.
//
// On SP_PATH_FOUND the smoothed route is written to out[] as cell indices,
// EXCLUDING the start cell and including the goal, and *outCount is set. A
// route longer than maxOut is TRUNCATED to its first maxOut waypoints; the
// caller repaths from the last one, which is invisible in play because the
// world has usually changed by then anyway.
SpPathStatus SpAStarStep(SpAStar *a, int nodeBudget,
                         SpCell *out, int maxOut, int *outCount);

// Convenience: run a whole search to completion, no budget. For tests and for
// short paths where slicing is pointless. Never call this from a frame that
// also runs the budgeted queue - it is unbounded by construction.
SpPathStatus SpAStarSolve(SpAStar *a, const SpGrid *g, int sx, int sz,
                          int gx, int gz, SpCell *out, int maxOut, int *outCount);

// Greedy string-pull: drop every waypoint that the previous kept one can see
// straight through. Operates in place on `cells`/`count`. `start` is the cell
// the unit is standing on, which is not in the array but is the first anchor.
void SpSmoothPath(const SpGrid *g, SpCell start, SpCell *cells, int *count);

// -- Path service -------------------------------------------------------------
//  The queue that stands between "a unit wants a path" and the single searcher
//  above. It exists for three reasons, in order of how badly each is needed:
//
//  1. A FRAME BUDGET. Five hundred units right-clicked at once is five hundred
//     searches; run eagerly they are one enormous frame hitch. The service
//     spends at most SpServiceSetBudget() expansions per frame across all of
//     them, so the cost of an order is spread rather than paid at once.
//
//  2. GOAL DEDUPLICATION - the thing that makes big orders tractable at all.
//     Requests are keyed by (start cell, goal cell). Two units standing on the
//     same tile ordered to the same tile want literally the same answer, and in
//     a crowd that is most of them. This is a partial measure: full sharing
//     needs the flow fields of the next phase, where 500 units to one point
//     become ONE field. Here it just stops the queue exploding.
//
//  3. ONE OWNER FOR THE SEARCHER. The SpAStar state is a megabyte-plus of
//     static arrays and exactly one search can be in flight in it. Making that
//     a rule enforced by a queue beats making it a rule people remember.
//
//  Requests are served OLDEST FIRST and a request is never dropped for being
//  slow - only for the queue being full, which is reported rather than hidden.
typedef int32_t SpRequestId;
#define SP_REQUEST_NONE  ((SpRequestId)-1)

typedef struct {
    int32_t queued;         // requests waiting or in flight right now
    int32_t served;         // completed since the last stats reset
    int32_t failed;         // completed with no route
    int32_t shared;         // served from an identical in-flight request
    int32_t rejected;       // refused: the queue was full
    int32_t expanded;       // A* nodes expanded last frame
    int32_t slicedFrames;   // frames where the budget ran out mid-search
} SpServiceStats;

// Point the service at a grid and drop everything in flight. Call on world load
// and whenever the grid is REBUILT (not merely patched) - a queued search holds
// a grid pointer and cell indices that a resize invalidates.
void SpServiceReset(const SpGrid *g);

// Per-frame A* expansion budget shared by every queued request. Lower makes
// paths arrive later without ever stopping a unit; the lab exposes it as a
// slider precisely so that claim can be checked by eye.
void SpServiceSetBudget(int nodesPerFrame);
int  SpServiceBudget(void);

// Ask for a path from one tile to another. Both are resolved through
// SpNearestOpen internally, so a click in a lake is a valid request. Returns
// SP_REQUEST_NONE when the queue is full.
//
// `owner` is an opaque caller handle (a unit index) echoed back on completion;
// the service never interprets it. Re-requesting for an owner that already has
// a request CANCELS the old one - a unit only ever wants its latest order.
SpRequestId SpServiceRequest(int32_t owner, int sx, int sz, int gx, int gz);

// Abandon a request. Safe on an id that has already completed or was never
// issued, which is what lets a caller cancel on death without bookkeeping.
void SpServiceCancel(SpRequestId id);

// Spend this frame's budget. Call once per frame, before units move.
void SpServiceUpdate(void);

// Collect one finished path. Returns false when nothing is ready. Call in a
// loop until it returns false; each result is delivered exactly once.
//
// `outCount` of 0 with a true return and SP_PATH_FOUND means the unit is
// already standing on its goal - a success, not an error.
bool SpServicePoll(int32_t *outOwner, SpPathStatus *outStatus,
                   SpCell *out, int maxOut, int *outCount);

// Convert a path cell back to a world position to steer at.
Vector3 SpCellToWorld(const SpGrid *g, SpCell c);

const SpServiceStats *SpServiceGetStats(void);
void SpServiceResetStats(void);

#endif // STRATEGY_PATH_H
