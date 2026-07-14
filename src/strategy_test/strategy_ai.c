// ============================================================================
//  strategy_ai.c  -  enemy faction + neutral animal "brains"
//
//  Called once per STRAT_AI_PERIOD from StrategyWorldUpdate. The AI never
//  moves, fights or gathers by itself: it only ISSUES ORDERS through the same
//  StrategyOrder* functions the mouse uses, so both factions always share
//  identical mechanics. Auto-aggro (strategy_world.c) already covers
//  "attack player units on sight" - no combat logic lives here.
// ============================================================================

#include "strategy_world.h"
#include "raymath.h"
#include <math.h>

static float AiDistXZ(Vector3 a, Vector3 b)
{
    float dx = a.x - b.x;
    float dz = a.z - b.z;
    return sqrtf(dx*dx + dz*dz);
}

// The enemy's "home" anchor: its first standing building.
static Vector3 EnemyHome(const StrategyWorld *world)
{
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        const Building *b = &world->buildings[i];
        if (b->active && b->faction == 1) return b->pos;
    }
    return (Vector3){ 14.0f, 0.0f, 14.0f };
}

static int NearestUnitOfKind(const StrategyWorld *world, Vector3 pos,
                             int faction, UnitKind kind)
{
    int best = -1;
    float bestDist = 1000000.0f;
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        const Unit *u = &world->units[i];
        if (!u->active || u->faction != faction || u->kind != kind) continue;

        float d = AiDistXZ(u->pos, pos);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

static int NearestPlayerBuilding(const StrategyWorld *world, Vector3 pos)
{
    int best = -1;
    float bestDist = 1000000.0f;
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        const Building *b = &world->buildings[i];
        if (!b->active || b->faction != 0) continue;

        float d = AiDistXZ(b->pos, pos);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

// Idle animals amble to a random nearby spot now and then.
static void AnimalsTick(StrategyWorld *world)
{
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world->units[i];
        if (!u->active || u->faction != FACTION_NEUTRAL) continue;
        if (u->state != UNIT_IDLE || GetRandomValue(0, 99) >= 30) continue;

        Vector3 dest = u->pos;
        dest.x += (float)GetRandomValue(-400, 400)*0.01f;
        dest.z += (float)GetRandomValue(-400, 400)*0.01f;
        dest.x = Clamp(dest.x, -STRAT_GROUND_HALF + 1.0f, STRAT_GROUND_HALF - 1.0f);
        dest.z = Clamp(dest.z, -STRAT_GROUND_HALF + 1.0f, STRAT_GROUND_HALF - 1.0f);
        StrategyOrderMove(u, dest);
    }
}

// Enemy production: houses keep the worker count at 6, barracks always
// train soldiers when affordable (StrategyTrainStart validates everything).
static void EnemyTrainTick(StrategyWorld *world)
{
    int workers = 0;
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world->units[i];
        if (u->active && u->faction == 1 && u->kind == KIND_WORKER) workers++;
    }

    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        Building *b = &world->buildings[i];
        if (!b->active || b->faction != 1 || b->trainKind >= 0) continue;

        if (b->kind == BLD_HOUSE && workers < 6)
        {
            if (StrategyTrainStart(i, KIND_WORKER)) workers++;
        }
        else if (b->kind == BLD_BARRACKS)
        {
            StrategyTrainStart(i, KIND_SOLDIER);
        }
    }
}

// Idle enemy workers: food-poor -> corpses/wheat/hunting; otherwise any
// nearby resource; failing everything, wander near home.
static void EnemyWorkersTick(StrategyWorld *world)
{
    Vector3 home = EnemyHome(world);

    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world->units[i];
        if (!u->active || u->faction != 1) continue;
        if (u->kind != KIND_WORKER || u->state != UNIT_IDLE) continue;

        if (world->stockpile[1][RES_FOOD] < 6)
        {
            int corpse = StrategyNearestNodeOfKind(u->pos, NODE_CORPSE, 40.0f);
            if (corpse >= 0)
            {
                StrategyOrderGather(u, corpse);
                continue;
            }
            int wheat = StrategyNearestNodeOfKind(u->pos, NODE_WHEAT, 40.0f);
            if (wheat >= 0)
            {
                StrategyOrderGather(u, wheat);
                continue;
            }
            if (GetRandomValue(0, 99) < 40)
            {
                int animal = NearestUnitOfKind(world, u->pos, FACTION_NEUTRAL,
                                               KIND_ANIMAL);
                if (animal >= 0)
                {
                    StrategyOrderAttack(u, animal);   // hunt: corpse follows
                    continue;
                }
            }
        }

        int node = StrategyNearestNodeOfKind(u->pos, -1, 40.0f);
        if (node >= 0)
        {
            StrategyOrderGather(u, node);
        }
        else
        {
            Vector3 dest = home;
            dest.x += (float)GetRandomValue(-600, 600)*0.01f;
            dest.z += (float)GetRandomValue(-600, 600)*0.01f;
            StrategyOrderMove(u, dest);
        }
    }
}

// Enemy construction: the only thing the AI builds is HOUSES, and only when
// pop-capped - otherwise training stalls forever. A few random spots near
// home are tried; StrategyTryBuild rejects blocked/unaffordable ones.
static void EnemyBuildTick(StrategyWorld *world)
{
    if (StrategyPopUsed(1) + 1 <= StrategyPopCap(1)) return;

    Vector3 home = EnemyHome(world);
    for (int attempt = 0; attempt < 5; attempt++)
    {
        Vector3 pos = home;
        pos.x += (float)GetRandomValue(-700, 700)*0.01f;
        pos.z += (float)GetRandomValue(-700, 700)*0.01f;
        pos.x  = roundf(pos.x);
        pos.z  = roundf(pos.z);
        if (StrategyTryBuild(1, BLD_HOUSE, pos)) return;
    }
}

// Attack wave: once enough soldiers idle at home, send them all at the
// player building closest to the enemy base.
static void EnemyAttackTick(StrategyWorld *world)
{
    int idleSoldiers = 0;
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world->units[i];
        if (u->active && u->faction == 1 && u->kind == KIND_SOLDIER &&
            u->state == UNIT_IDLE) idleSoldiers++;
    }
    if (idleSoldiers < STRAT_AI_ATTACK_SQUAD) return;

    int target = NearestPlayerBuilding(world, EnemyHome(world));
    if (target < 0) return;

    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world->units[i];
        if (u->active && u->faction == 1 && u->kind == KIND_SOLDIER &&
            u->state == UNIT_IDLE)
        {
            StrategyOrderAttackBuilding(u, target);
        }
    }
}

void StrategyAiTick(void)
{
    StrategyWorld *world = StrategyWorldGet();

    AnimalsTick(world);
    EnemyBuildTick(world);
    EnemyTrainTick(world);
    EnemyWorkersTick(world);
    EnemyAttackTick(world);
}
