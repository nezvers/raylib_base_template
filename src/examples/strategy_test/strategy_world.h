// ============================================================================
//  strategy_world.h  -  API between the strategy app state (strategy_test.c),
//  the world simulation (strategy_world.c) and the effect pool
//  (strategy_effects.c).
// ============================================================================

#ifndef STRATEGY_WORLD_H
#define STRATEGY_WORLD_H

#include "strategy_types.h"
#include "strategy_defs.h"

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

// Faction colors for the GUI (defined in strategy_world.c). Costs/stats/names
// come from the def tables in strategy_defs.h.
extern const Color strategyFactionColor[STRAT_FACTIONS];

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
