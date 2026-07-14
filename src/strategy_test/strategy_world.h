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
void StrategyWorldInit(void);           // reset + spawn the test map
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

// Nearest active node of a kind (-1 = any kind) within radius, else -1.
int StrategyNearestNodeOfKind(Vector3 pos, int nodeKind, float radius);

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
