// ============================================================================
//  strategy_world.h  -  API between the strategy app state (strategy_test.c),
//  the world simulation (strategy_world.c) and the effect pool
//  (strategy_effects.c).
// ============================================================================

#ifndef STRATEGY_WORLD_H
#define STRATEGY_WORLD_H

#include "strategy_types.h"
#include "strategy_defs.h"
// For SpGrid on the movement API below. strategy_path/ never includes anything
// from here, so the dependency runs one way only: the game knows about the nav
// module, the nav module stays headless.
#include "../../strategy_path/strategy_path.h"

// -- World (strategy_world.c) ------------------------------------------------
StrategyWorld *StrategyWorldGet(void);

// Choose the battlefield the NEXT StrategyWorldInit() builds. Follows the
// open-then-transition convention used by the forges (StrategyForgeOpenAsset):
// the caller selects, then transitions, because AppStateTransition runs Enter()
// synchronously and there is no chance to pass anything afterwards.
//
// The name is looked up in the map catalog at init time, not here, so a map
// saved between the selection and the transition is still picked up - and so a
// stale name can never become a dangling pointer.
//
// Passing NULL (or a name no map has) selects the BUILT-IN layout: the
// hardcoded two-base battlefield the game shipped with. That is also the state
// before anything ever calls this, so the game still runs with no maps
// authored.
void StrategyMapSelect(const char *name);

// The map name currently selected, or "" for the built-in layout.
const char *StrategyMapSelected(void);

void StrategyWorldInit(void);           // reset + spawn the selected map
void StrategyWorldHandleInput(void);    // camera, picking, selection, orders
void StrategyWorldUpdate(float dt);     // units, gathering, combat, AI, effects
void StrategyWorldDraw3D(void);         // Begin/EndMode3D + all world geometry
void StrategyWorldDraw2DOverlay(void);  // drag rect, HP bars, resource HUD (game space)

// -- Active unit roster -------------------------------------------------------
// The indices of every live unit, and how many there are. Iterate THIS instead
// of `for (i < STRAT_MAX_UNITS)`: the pool is sized for 10,000 but a normal
// game holds fewer than a hundred, so a full scan pays for the cap rather than
// the population - and touches a 248-byte struct at every step to do it.
//
//     int n; const int *live = StrategyActiveUnits(&n);
//     for (int k = 0; k < n; k++) { Unit *u = &world->units[live[k]]; ... }
//
// Every listed index is active, so the `if (!u->active) continue;` guard that
// used to open these loops is now dead code - drop it rather than keeping it
// "just in case", because a guard that never fires hides the day it should.
//
// TWO RULES. The order is NOT stable (removal swaps the last entry down), so
// anything needing deterministic order must sort. And the array is invalidated
// by any spawn or kill, so do not hold the pointer across one - re-fetch.
const int *StrategyActiveUnits(int *count);

// Debug builds only: set true to audit the roster invariant every frame. O(pool)
// per frame, so it is a diagnostic, not a safety net - it clears itself after
// tracing the first failure. Undefined in release; guard uses with NDEBUG.
#ifndef NDEBUG
extern bool strategyRosterAudit;
#endif

// Population: cap comes from standing houses, used counts own units.
int StrategyPopCap(int faction);
int StrategyPopUsed(int faction);

// Orders: the ONLY way anything (mouse, GUI or AI) makes a unit act.
void StrategyOrderMove(Unit *u, Vector3 dest);
void StrategyOrderGather(Unit *u, int nodeIndex);
void StrategyOrderAttack(Unit *u, int unitIndex);
void StrategyOrderAttackBuilding(Unit *u, int bldIndex);
void StrategyOrderFarm(Unit *u, int bldIndex);
void StrategyOrderBuild(Unit *u, int bldIndex);     // raise a scaffold to full
void StrategyOrderRepair(Unit *u, int bldIndex);    // restore a damaged building

// Queue a worker job (build / repair / gather-assign). append=false replaces
// the current job and clears the queue; append=true chains after it (Shift-RMB).
void StrategyOrderJob(Unit *u, WorkerJobKind kind, int bldIndex, bool append);

// Nearest active node of a kind (-1 = any kind) within radius, else -1.
int StrategyNearestNodeOfKind(Vector3 pos, int nodeKind, float radius);

// Composition census (player-facing pop panel). kind < 0 counts every kind.
int StrategyCountUnits(int faction, int kind);
int StrategyCountIdleWorkers(int faction);

// Select the idle worker nearest the camera focus and pan the camera to it.
// No-op when the player has no idle worker.
void StrategySelectNearestIdleWorker(void);

// Start training at a building: validates cost + pop cap, deducts, returns
// success. Shared by the GUI buttons and the enemy AI.
bool StrategyTrainStart(int bldIndex, UnitKind kind);

// Cancel one trainee (last queued, else the active one) and refund its cost.
bool StrategyTrainCancel(int bldIndex);

// Validate + pay + place a building; false when unaffordable or blocked.
bool StrategyTryBuild(int faction, BuildingKind kind, Vector3 pos);

// Sell a building: refund refundRate (+ difficulty bonus) of its cost.
bool StrategySellBuilding(int index);

// Quarry: spend providence to spawn a fresh stone node beside it.
bool StrategyQuarrySpawnStone(int bldIndex);

// Enemy + animal think tick (strategy_ai.c), called on the world's aiTimer.
void StrategyAiTick(void);

// -- Movement (strategy_move.c) -----------------------------------------------
//  Path following. The split of responsibility is the point:
//    src/strategy_path/   - WHERE to go. A cost grid in, tile indices out; has
//                           never heard of a Unit and links headless.
//    strategy_move.c      - WHO goes there and HOW. Needs Unit, so it lives
//                           here beside the world rather than in that module.
//
//  Two movers, and choosing between them is a real decision, not a style one:
//
//    StrategyMoveTo      - pathed. For a STATIC destination that may be far:
//                          a clicked point, a tree, a building. Asks the path
//                          service for a route, walks its waypoints, and falls
//                          back to straight steering whenever it has no usable
//                          route - queue full, search still running, no route
//                          exists. A unit is never stationary because
//                          pathfinding is busy.
//    StrategyMoveDirect  - straight steering, the behaviour that predates all
//                          of this. For a target that MOVES (chasing an enemy,
//                          shadowing an ally) or a shuffle of a unit or two
//                          (kiting backwards, stepping to a plant spot). A path
//                          to a moving target is stale before it arrives, and a
//                          path for a two-unit shuffle is a search that buys
//                          nothing - at ten thousand units either is fatal.
//
//  Neither decides that the unit has ARRIVED; callers keep their own proximity
//  tests exactly as they did when every one of these was a lerp.
void StrategyMoveTo(Unit *u, int index, Vector3 dest, float dt);
void StrategyMoveDirect(Unit *u, Vector3 dest, float dt);

// Lifecycle, all called from strategy_world.c.
void StrategyMoveInit(void);            // world load: drop every path
void StrategyMoveForget(int index);     // a unit died or was recycled
void StrategyMoveBeginFrame(void);      // refresh the per-frame repath budget
void StrategyMoveCollect(void);         // drain finished searches into paths
void StrategyMoveStats(void);           // overlay census

// The nav grid and its version, for strategy_move.c. Read-only: obstacles are
// stamped by strategy_world.c alone, which is what keeps "who may write the
// grid" answerable. The version bumps on every change, so a path built against
// an older one knows to re-ask.
const SpGrid *StrategyNavGrid(void);
uint32_t      StrategyNavVersion(void);

// Waypoints a unit still has left to walk, in world space, for the lab's P
// overlay. Returns how many were written.
int StrategyMovePathOf(int index, Vector3 *out, int maxOut);

// The flow field a unit is riding (SP_FIELD_NONE if none) and the destination
// it was actually given - its formation slot, not the clicked point. Both feed
// the lab's F and O overlays.
SpFieldId StrategyMoveFieldOf(int index);
bool      StrategyMoveGoalOf(int index, Vector3 *out);

// -- Group orders -------------------------------------------------------------
//  StrategyOrderMove stays the single-unit primitive - strategy_ai.c and every
//  internal caller keep working untouched. This sits ABOVE it: it computes a
//  formation, assigns each unit its own slot, and then calls StrategyOrderMove
//  per unit. UNIT_MOVE's handler never learns that formations exist, which is
//  what keeps the change survivable.
//
//  WHY A GROUP ORDER IS NOT JUST N SINGLE ORDERS. A click is one point, and 500
//  units ordered to one point are being asked to occupy space that physically
//  holds about six - so they queue and shove no matter how good the arrival
//  logic is. Phase 2 measured it: 300 units to one point settle, and settle
//  7,316 pairs deep inside each other. The identical steering code with
//  formation slots gives ZERO overlapping pairs. The order has to become an
//  AREA, and that is what this does.
void StrategyOrderMoveGroup(const int *units, int count, Vector3 dest);

// Same, with the formation's heading pinned by the player instead of inferred
// from the direction of travel. Used by the drag-to-orient order.
void StrategyOrderMoveGroupFacing(const int *units, int count, Vector3 dest,
                                  float faceX, float faceZ);

// -- Formations --------------------------------------------------------------
// Shape and break-off behaviour are SEPARATE AXES, deliberately: "what the block
// looks like" and "who peels off when shot at" are unrelated decisions, and any
// shape can be marched with any behaviour. Both are player settings applied to
// the NEXT group order; a group already marching keeps what it was given.
void              StrategyFormationShapeSet(FormationShape s);
FormationShape    StrategyFormationShape(void);
void              StrategyFormationBehaviorSet(FormationBehavior b);
FormationBehavior StrategyFormationBehavior(void);
const char       *StrategyFormationShapeName(FormationShape s);
const char       *StrategyFormationBehaviorName(FormationBehavior b);

// Release every member of a group from its formation. FORM_BEHAVIOR_ENGAGE uses
// it so first contact commits the whole block at once.
void StrategyFormationBreak(int groupId);

// Per-frame form-up pass: recomputes each live group's worst distance-to-slot
// and latches groups that have closed up. MUST run before the movement pass, so
// every unit in a group compares against the same snapshot.
void StrategyMoveFormUpdate(float dt);

// Drop the flow field a unit is riding, keeping its A* path. Called from the
// single funnel every new individual destination passes through: a field is a
// GROUP answer, so the moment a unit is sent somewhere of its own it is being
// steered at somebody else's destination.
void StrategyMoveDropField(int index);

// Order preview: a fading ring the size of the block plus one marker per slot,
// showing where a move order will put the group. Ticked once a frame; the query
// returns false when nothing is live.
void StrategyMovePreviewUpdate(float dt);
bool StrategyMovePreview(Vector3 *outCentre, float *outRadius, float *outFade,
                         const Vector3 **outSlots, int *outCount, Vector3 *outFace);

// Live preview for an order still being aimed: lays the slots out at `faceX/Z`
// and holds them on screen until StrategyMovePreviewRelease. Same layout code
// the real order uses, so what is aimed is what is given.
void StrategyMovePreviewAim(const int *units, int count, Vector3 dest,
                            float faceX, float faceZ);
void StrategyMovePreviewRelease(void);

// True when this group has arrived and its members should hold their slots, and
// when the group is a loose FREEFORM scatter that opts out of holding entirely.
bool StrategyFormationGroupHolding(int groupId);
bool StrategyFormationGroupLoose(int groupId);

// Restoring displacement toward a held slot, or zero. Added to `pos` directly
// rather than to `vel` - see the definition for why that is load-bearing.
Vector3 StrategyFormationHoldPull(int index, const Unit *u, float dt);

// Units sharing a destination, above which a flow field is built instead of a
// path each. A field is looked up FIRST regardless of size, so a lone unit sent
// where a crowd is already headed rides theirs for free; the threshold only
// governs whether a NEW field is worth building.
//
// Exposed because the A/B it enables is the most informative thing the lab can
// show: watch one field serve 2,000 units with astar at 0 ms, then raise the
// threshold past the group size and watch the queue pin.
void StrategyMoveFlowThresholdSet(int n);
int  StrategyMoveFlowThreshold(void);

// Recount which flow fields are still in use. Refcounts are RECOMPUTED, never
// hand-maintained: every exit path - death, re-order, arrival, world reset -
// would otherwise need a decrement, and missing one leaks a field until all
// slots pin and everything silently degrades to individual A*. A sweep is
// O(live) and structurally cannot leak.
void StrategyMoveFlowSweep(float now);

// Fields resident right now, for the overlay's `fields n/max` line.
int StrategyMoveFlowLive(void);

// Faction colors for the GUI (defined in strategy_world.c). Costs/stats/names
// come from the def tables in strategy_defs.h.
extern const Color strategyFactionColor[STRAT_FACTIONS];

// -- Render LOD ---------------------------------------------------------------
// The authored draw path costs a per-part loop of immediate-mode primitives,
// each with its own matrix push/pop, so the renderer saturates long before the
// simulation does. The stress lab forces a cheaper tier to get the renderer out
// of the way and measure the MOVER; the game itself never leaves AUTHORED.
typedef enum {
    STRAT_LOD_AUTHORED = 0,  // full authored asset - what the game ships
    STRAT_LOD_PRIMITIVE,     // one box per unit, no matrix push, no asset lookup
    STRAT_LOD_DOTS,          // one tiny box per unit
    STRAT_LOD_NONE,          // units not drawn at all - sim cost in isolation
    STRAT_LOD_COUNT
} StrategyRenderLod;

void StrategyRenderLodSet(StrategyRenderLod lod);
StrategyRenderLod StrategyRenderLodGet(void);
const char *StrategyRenderLodName(StrategyRenderLod lod);

// Spawn `count` units of one kind for a faction, scattered in a disc around
// `center`. Stress-test entry point: it goes through the same UnitSpawn as
// everything else and stops early when the pool is full. Returns how many
// actually spawned.
int StrategyDebugSpawnUnits(int faction, UnitKind kind, Vector3 center,
                            float radius, int count);

// Deactivate every unit. Stress-test reset; leaves buildings and nodes alone.
void StrategyDebugClearUnits(void);

// Show the navigation grid as flat coloured tiles: red blocked, blue shallow,
// amber obstacle skirt. The lab's G overlay.
//
// A FLAG rather than a draw call, because the tiles must land inside the
// world's own BeginMode3D pass - the caller does not have the camera, and
// opening a second 3D pass over the first is the pane-aspect trap all over
// again.
//
// This exists to be A/B'd against the map forge's passability view: the grid is
// derived independently of it, so if the two disagree, one of them is wrong -
// and finding that out by eye, against an overlay that already works, is far
// cheaper than finding it out later as a pathfinding bug.
void StrategyDebugNavShow(bool on);
bool StrategyDebugNavShown(void);

// Live obstacle count in the nav grid: blocked tiles, and tiles raised to the
// obstacle skirt. Lets the overlay show the grid reacting to a placement
// without the player having to spot a colour change.
void StrategyDebugNavStats(int *outBlocked, int *outSkirt);

// Draw the route each SELECTED unit has left to walk. The lab's P overlay, and
// a flag rather than a draw call for the same reason G is.
//
// Selected units only, deliberately. Every path at once, at a thousand units,
// is a mat of lines that answers no question; a handful selected and watched is
// what actually shows a bad route. The corner markers are the point - their
// COUNT is the smoothing result, so a crossing that should be four hops and
// draws twenty says string-pulling did nothing.
void StrategyDebugPathShow(bool on);
bool StrategyDebugPathShown(void);

// F: the direction field the first selected unit is riding, every fourth cell,
// tinted by distance-to-goal. One field at a time on purpose - sixteen overlaid
// arrow mats answer nothing. Nothing drawn means nothing selected is on a
// field, which is itself the answer when a group that should share one doesn't.
void StrategyDebugFlowShow(bool on);
bool StrategyDebugFlowShown(void);

// O: a line from each selected unit to the destination it was actually given -
// its formation SLOT, not the clicked point. The spread of the endpoints is the
// formation; whether the lines cross is whether slot assignment is spatially
// coherent, which nothing else makes visible.
void StrategyDebugSlotShow(bool on);
bool StrategyDebugSlotShown(void);

// Per-frame path service numbers for the overlay: how many searches are queued,
// how many units are walking a route, how many are waiting on one, and how many
// A* nodes the last frame cost. Counts come from the profiler counters, so they
// are live whether or not the profiler overlay is showing.
void StrategyDebugPathStats(int *outQueued, int *outActive,
                            int *outPending, int *outNodes);

// A* expansions allowed per frame across every queued search. The lab exposes
// this as a slider because the claim it tests - that a low budget makes paths
// arrive LATE but never makes a unit stand still - is the one thing about
// time-slicing that has to be checked by eye.
void StrategyDebugPathBudgetSet(int nodesPerFrame);
int  StrategyDebugPathBudget(void);

// -- Effects (strategy_effects.c) ---------------------------------------------
// Small procedural pool drawn inside BeginMode3D. No textures.
typedef enum {
    FX_RING = 0,    // expanding ground circle: move order, placement, death
    FX_PUFF,        // rising shrinking cube: chop/mine, death debris
    FX_FLASH,       // pulsing wire sphere: hit impact, resource deposit
    FX_BEAM,        // short-lived attack line between two points
} EffectKind;

#define STRAT_MAX_EFFECTS 96

void EffectsReset(void);
void EffectSpawn(EffectKind kind, Vector3 pos, Color color);
void EffectSpawnBeam(Vector3 from, Vector3 to, Color color);
void EffectSpawnBless(Vector3 pos);     // gold ring + puff burst (templars)
void EffectsUpdate(float dt);
void EffectsDraw3D(void);   // call between BeginMode3D/EndMode3D

#endif // STRATEGY_WORLD_H
