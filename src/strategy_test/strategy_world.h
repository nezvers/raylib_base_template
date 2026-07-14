// ============================================================================
//  strategy_world.h  -  API between the strategy app state (strategy_test.c),
//  the world simulation (strategy_world.c) and the effect pool
//  (strategy_effects.c).
// ============================================================================

#ifndef STRATEGY_WORLD_H
#define STRATEGY_WORLD_H

#include "strategy_types.h"

// -- World (strategy_world.c) ------------------------------------------------
StrategyWorld *StrategyWorldGet(void);
void StrategyWorldInit(void);           // reset + spawn the test map
void StrategyWorldHandleInput(void);    // camera, picking, selection, orders
void StrategyWorldUpdate(float dt);     // units, gathering, combat, AI, effects
void StrategyWorldDraw3D(void);         // Begin/EndMode3D + all world geometry
void StrategyWorldDraw2DOverlay(void);  // drag rect, HP bars, resource HUD (game space)

// Cost table for the build buttons (defined in strategy_world.c).
extern const int strategyBuildingCost[BLD_COUNT][RES_COUNT];
extern const Color strategyFactionColor[STRAT_FACTIONS];

// -- Effects (strategy_effects.c) ---------------------------------------------
// Small procedural pool drawn inside BeginMode3D. No textures.
typedef enum {
    FX_RING = 0,    // expanding ground circle: move order, placement, death
    FX_PUFF,        // rising shrinking cube: chop/mine, death debris
    FX_FLASH,       // pulsing wire sphere: hit impact, resource deposit
    FX_BEAM,        // short-lived attack line between two points
} EffectKind;

#define STRAT_MAX_EFFECTS 64

void EffectsReset(void);
void EffectSpawn(EffectKind kind, Vector3 pos, Color color);
void EffectSpawnBeam(Vector3 from, Vector3 to, Color color);
void EffectsUpdate(float dt);
void EffectsDraw3D(void);   // call between BeginMode3D/EndMode3D

#endif // STRATEGY_WORLD_H
