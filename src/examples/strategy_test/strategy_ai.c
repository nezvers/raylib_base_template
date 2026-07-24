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

// Enemy production: the town hall keeps the worker count at 6, barracks keep
// melee and ranged roughly even, the chantry maintains one templar + one
// healer (StrategyTrainStart validates cost/pop/cooldown/trainable).
static void EnemyTrainTick(StrategyWorld *world)
{
    int count[UNIT_KIND_COUNT] = { 0 };
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world->units[i];
        if (u->active && u->faction == 1) count[u->kind]++;
    }

    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        Building *b = &world->buildings[i];
        if (!b->active || b->faction != 1 || b->trainKind >= 0) continue;

        if (b->kind == BLD_TOWN_HALL && count[KIND_WORKER] < 6)
        {
            if (StrategyTrainStart(i, KIND_WORKER)) count[KIND_WORKER]++;
        }
        else if (b->kind == BLD_BARRACKS)
        {
            UnitKind want = (count[KIND_RANGED] < count[KIND_SOLDIER])
                                ? KIND_RANGED : KIND_SOLDIER;
            if (StrategyTrainStart(i, want)) count[want]++;
        }
        else if (b->kind == BLD_CHANTRY)
        {
            if (count[KIND_TEMPLAR] < 1)
            {
                if (StrategyTrainStart(i, KIND_TEMPLAR)) count[KIND_TEMPLAR]++;
            }
            else if (count[KIND_TEMPLAR_HEALER] < 1)
            {
                if (StrategyTrainStart(i, KIND_TEMPLAR_HEALER)) count[KIND_TEMPLAR_HEALER]++;
            }
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
                                               KIND_ANIMAL_WEAK);
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

// Enemy construction: a HOUSE when pop-capped (else training stalls forever),
// or ONE chantry once the economy is up. A few random spots near home are
// tried; StrategyTryBuild rejects blocked/unaffordable ones.
static void EnemyBuildTick(StrategyWorld *world)
{
    BuildingKind want;
    if (StrategyPopUsed(1) + 1 > StrategyPopCap(1))
    {
        want = BLD_HOUSE;
    }
    else
    {
        bool hasChantry = false;
        int workers = 0;
        for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
        {
            Building *b = &world->buildings[i];
            if (b->active && b->faction == 1 && b->kind == BLD_CHANTRY) hasChantry = true;
        }
        for (int i = 0; i < STRAT_MAX_UNITS; i++)
        {
            Unit *u = &world->units[i];
            if (u->active && u->faction == 1 && u->kind == KIND_WORKER) workers++;
        }
        // Economy up = full worker crew and a resource buffer beyond the cost.
        if (hasChantry || workers < 5 ||
            world->stockpile[1][RES_WOOD] < 8 || world->stockpile[1][RES_STONE] < 5) return;
        want = BLD_CHANTRY;
    }

    Vector3 home = EnemyHome(world);
    for (int attempt = 0; attempt < 5; attempt++)
    {
        Vector3 pos = home;
        pos.x += (float)GetRandomValue(-700, 700)*0.01f;
        pos.z += (float)GetRandomValue(-700, 700)*0.01f;
        pos.x  = roundf(pos.x);
        pos.z  = roundf(pos.z);
        if (StrategyTryBuild(1, want, pos)) return;
    }
}

// Attack wave: once enough fighters (melee + ranged) idle at home, send them
// all at the player building closest to the enemy base.
static bool AiIsFighter(const Unit *u)
{
    return u->kind == KIND_SOLDIER || u->kind == KIND_RANGED;
}

static void EnemyAttackTick(StrategyWorld *world)
{
    int idleFighters = 0;
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world->units[i];
        if (u->active && u->faction == 1 && AiIsFighter(u) &&
            u->state == UNIT_IDLE) idleFighters++;
    }
    if (idleFighters < STRAT_AI_ATTACK_SQUAD) return;

    int target = NearestPlayerBuilding(world, EnemyHome(world));
    if (target < 0) return;

    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world->units[i];
        if (u->active && u->faction == 1 && AiIsFighter(u) &&
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
