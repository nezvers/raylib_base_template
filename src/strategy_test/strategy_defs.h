// ============================================================================
//  strategy_defs.h  -  const definition tables for unit and building kinds
//
//  ONE row per kind: stats, costs, training rules, dropoff acceptance.
//  Balance tuning happens in strategy_defs.c and nowhere else. Instances
//  never point at these rows - UnitSpawn resolves a def row through the
//  faction difficulty mods into plain floats on the Unit (see
//  strategy_types.h header comment).
// ============================================================================

#ifndef STRATEGY_DEFS_H
#define STRATEGY_DEFS_H

#include "strategy_types.h"

typedef struct {
    const char *name;
    float maxHp, damage, attackRange, attackPeriod;
    float moveSpeed, sightRange, gatherTime, farmPeriod;
    float preferredRange;           // >0 = ranged kiting stand-off distance
    bool  canGather;
    int   corpseFood;               // >0: killed unit leaves a food corpse node
    int   cost[RES_COUNT];          // global train cost (not building-tied)
    float trainTime;                // seconds to train one
} UnitDef;

#define BLD_MAX_TRAINABLE 4
typedef struct {
    const char *name;
    int      cost[RES_COUNT];       // wood, stone, food, providence
    float    refundRate;            // base sell refund fraction of cost
    float    maxHp;
    float    buildTime;             // worker-seconds to raise the scaffold to full
    bool     critical;              // losing ALL critical buildings (and all
                                    //   workers) defeats the faction
    int      popCap;                // population added while standing
    UnitKind trainable[BLD_MAX_TRAINABLE];
    int      trainableCount;
    float    trainCooldown;         // anti-spam pause after each trainee
    bool     accepts[RES_COUNT];    // dropoff: which carried resources land here
    int      produces[RES_COUNT];   // reserved (planks/bricks later), all 0

    // Node-tending buildings: an assigned worker plants this node kind nearby.
    // tendNode < 0 means the building does not tend (not a farm/forestry).
    int      tendNode;              // NodeKind to plant, or -1
    int      tendAmount;           // resource units in each planted node
    float    trainCostMul;          // cost reduction hook, 1.0 default
    float    buffHpMul, buffDmgMul; // applied to units trained here
} BuildingDef;

const UnitDef     *StrategyUnitDef(UnitKind kind);
const BuildingDef *StrategyBuildingDef(BuildingKind kind);

#endif // STRATEGY_DEFS_H
