// ============================================================================
//  strategy_defs.c  -  THE balance file: every per-kind number lives here.
//
//  cost[] order everywhere: { wood, stone, food, providence }.
//  Unused fields are 0 (a melee unit's preferredRange, a non-trainer's
//  trainable list, ...). buff/cost multipliers default to 1.0 - they are the
//  hooks for future upgrades, not live features.
// ============================================================================

#include "strategy_defs.h"

static const UnitDef unitDefs[UNIT_KIND_COUNT] = {
    [KIND_WORKER] = {
        .name = "WORKER",
        .maxHp = 30.0f, .damage = 4.0f, .attackRange = 1.2f, .attackPeriod = 1.0f,
        .moveSpeed = 4.0f, .sightRange = 6.0f, .gatherTime = 0.8f, .farmPeriod = 2.0f,
        .canGather = true,
        .cost = { 0, 0, 2, 0 }, .trainTime = 4.0f,
    },
    [KIND_SOLDIER] = {
        .name = "SOLDIER",
        .maxHp = 60.0f, .damage = 12.0f, .attackRange = 1.2f, .attackPeriod = 1.0f,
        .moveSpeed = 4.0f, .sightRange = 6.0f,
        .cost = { 2, 0, 2, 0 }, .trainTime = 6.0f,
    },
    [KIND_RANGED] = {
        .name = "RANGED",
        .maxHp = 40.0f, .damage = 7.0f, .attackRange = 5.5f, .attackPeriod = 1.2f,
        .moveSpeed = 4.0f, .sightRange = 7.0f,
        .preferredRange = 5.0f,     // kite: back off when closer than 60% of this
        .cost = { 3, 0, 2, 0 }, .trainTime = 6.0f,
    },
    [KIND_TEMPLAR] = {
        .name = "TEMPLAR",
        .maxHp = 35.0f, .moveSpeed = 4.2f, .sightRange = 8.0f,
        .cost = { 1, 0, 3, 0 }, .trainTime = 6.0f,
    },
    [KIND_TEMPLAR_HEALER] = {
        .name = "HEALER",
        .maxHp = 35.0f, .moveSpeed = 4.2f, .sightRange = 8.0f,
        .cost = { 1, 0, 3, 1 }, .trainTime = 7.0f,
    },
    [KIND_ANIMAL_WEAK] = {
        .name = "DEER",
        .maxHp = 15.0f, .moveSpeed = 4.6f,
        .corpseFood = 6,
    },
    [KIND_ANIMAL_STRONG] = {
        .name = "BOAR",
        .maxHp = 50.0f, .damage = 8.0f, .attackRange = 1.2f, .attackPeriod = 1.2f,
        .moveSpeed = 3.6f,
        .corpseFood = 10,
    },
};

static const BuildingDef buildingDefs[BLD_COUNT] = {
    [BLD_HOUSE] = {
        .name = "HOUSE", .cost = { 5, 3, 0, 0 }, .refundRate = 0.5f,
        .maxHp = 100.0f, .popCap = 4,
        .trainCostMul = 1.0f, .buffHpMul = 1.0f, .buffDmgMul = 1.0f,
    },
    [BLD_LOGGING] = {
        .name = "LOGGING", .cost = { 3, 0, 0, 0 }, .refundRate = 0.5f,
        .maxHp = 80.0f,
        .accepts = { [RES_WOOD] = true },
        .trainCostMul = 1.0f, .buffHpMul = 1.0f, .buffDmgMul = 1.0f,
    },
    [BLD_QUARRY] = {
        .name = "QUARRY", .cost = { 2, 2, 0, 0 }, .refundRate = 0.5f,
        .maxHp = 80.0f,
        .accepts = { [RES_STONE] = true },
        .trainCostMul = 1.0f, .buffHpMul = 1.0f, .buffDmgMul = 1.0f,
    },
    [BLD_BARRACKS] = {
        .name = "BARRACKS", .cost = { 6, 4, 0, 0 }, .refundRate = 0.5f,
        .maxHp = 140.0f,
        .trainable = { KIND_SOLDIER, KIND_RANGED }, .trainableCount = 2,
        .trainCooldown = 2.0f,
        .trainCostMul = 1.0f, .buffHpMul = 1.0f, .buffDmgMul = 1.0f,
    },
    [BLD_FARM] = {
        .name = "FARM", .cost = { 4, 1, 0, 0 }, .refundRate = 0.5f,
        .maxHp = 90.0f,
        .accepts = { [RES_FOOD] = true },
        .trainCostMul = 1.0f, .buffHpMul = 1.0f, .buffDmgMul = 1.0f,
    },
    [BLD_TOWN_HALL] = {
        .name = "TOWN HALL", .cost = { 8, 6, 0, 0 }, .refundRate = 0.5f,
        .maxHp = 300.0f, .critical = true, .popCap = 4,
        .trainable = { KIND_WORKER }, .trainableCount = 1,
        .trainCooldown = 1.5f,
        .accepts = { [RES_WOOD] = true, [RES_STONE] = true, [RES_FOOD] = true },
        .trainCostMul = 1.0f, .buffHpMul = 1.0f, .buffDmgMul = 1.0f,
    },
    [BLD_CHANTRY] = {
        .name = "CHANTRY", .cost = { 6, 3, 0, 0 }, .refundRate = 0.5f,
        .maxHp = 120.0f,
        .trainable = { KIND_TEMPLAR, KIND_TEMPLAR_HEALER }, .trainableCount = 2,
        .trainCooldown = 2.0f,
        .trainCostMul = 1.0f, .buffHpMul = 1.0f, .buffDmgMul = 1.0f,
    },
};

const UnitDef *StrategyUnitDef(UnitKind kind)
{
    return &unitDefs[kind];
}

const BuildingDef *StrategyBuildingDef(BuildingKind kind)
{
    return &buildingDefs[kind];
}
