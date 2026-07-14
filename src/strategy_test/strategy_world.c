// ============================================================================
//  strategy_world.c  -  the RTS test world: map, camera, picking, selection,
//  orders, gathering, combat and the enemy AI.
//
//  Design notes:
//  - ONE unit state machine (UnitUpdate) serves both factions. The mouse and
//    the enemy AI both just WRITE order fields (state/target/targetUnit/
//    targetNode); nothing else differs per faction.
//  - Picking is the classic letterbox trap: the 3D scene renders into the
//    fixed game render-texture, but the mouse lives in real window pixels.
//    ALL conversions go through MouseGroundPoint()/WorldToGame() below -
//    if clicking ever feels "off", look there first.
// ============================================================================

#include "strategy_world.h"
#include "../screen_state/screen_state.h"
#include "../settings_state/settings_state.h"
#include "raymath.h"
#include <math.h>
#include <stddef.h>

const Color strategyFactionColor[STRAT_FACTIONS] = {
    {  80, 140, 255, 255 },     // faction 0: player, blue
    { 230,  70,  70, 255 },     // faction 1: enemy, red
};

static StrategyWorld world;

StrategyWorld *StrategyWorldGet(void)
{
    return &world;
}

// ----------------------------------------------------------------------------
//  Small helpers
// ----------------------------------------------------------------------------
static float DistXZ(Vector3 a, Vector3 b)
{
    float dx = a.x - b.x;
    float dz = a.z - b.z;
    return sqrtf(dx*dx + dz*dz);
}

// Derive the camera from focus + zoom (fixed pitch, no rotation). Called
// whenever either changes; keeps pan/zoom code trivial.
static void CameraRefresh(void)
{
    Vector3 target = (Vector3){ world.camFocus.x, 0.0f, world.camFocus.y };
    Vector3 offset = (Vector3){ 0.0f, 14.0f, 10.0f };

    world.camera.target   = target;
    world.camera.position = Vector3Add(target, Vector3Scale(offset, world.camZoom));
}

// Mouse position in GAME-CANVAS pixels (the 3D scene's framebuffer space).
static Vector2 MouseGame(void)
{
    return Screen2Target(GetMousePosition());
}

// Cast the mouse into the world and intersect the y = 0 ground plane.
// Returns false when the ray misses the plane (looking at the sky).
static bool MouseGroundPoint(Vector3 *out)
{
    Vector2 mouse = MouseGame();
    Vector2 gameSize = ScreenStateTargetSize();
    Ray ray = GetScreenToWorldRayEx(mouse, world.camera,
                                    (int)gameSize.x, (int)gameSize.y);

    if (fabsf(ray.direction.y) < 0.0001f) return false;
    float t = -ray.position.y/ray.direction.y;
    if (t < 0.0f) return false;

    *out = Vector3Add(ray.position, Vector3Scale(ray.direction, t));
    return true;
}

// Project a world point back into game-canvas pixels (drag-select, HP bars).
static Vector2 WorldToGame(Vector3 p)
{
    Vector2 gameSize = ScreenStateTargetSize();
    return GetWorldToScreenEx(p, world.camera, (int)gameSize.x, (int)gameSize.y);
}

// True while the mouse hovers the build bar (Gui publishes its rectangle in
// REAL screen pixels) - world clicks must not fire "through" the buttons.
static bool MouseOnGui(void)
{
    return CheckCollisionPointRec(GetMousePosition(), world.guiBlock);
}

static ResourceKind NodeResource(NodeKind kind)
{
    switch (kind)
    {
        case NODE_TREE:   return RES_WOOD;
        case NODE_ROCK:   return RES_STONE;
        case NODE_WHEAT:  return RES_FOOD;
        case NODE_CORPSE: return RES_FOOD;
        default:          return RES_WOOD;
    }
}

// Faction color with the neutral-animal guard: FACTION_NEUTRAL must never
// index strategyFactionColor[] (only 2 entries).
static Color UnitColor(const Unit *u)
{
    if (u->faction == FACTION_NEUTRAL) return BEIGE;
    return strategyFactionColor[u->faction];
}

// ----------------------------------------------------------------------------
//  Spawning
// ----------------------------------------------------------------------------
static Unit *UnitSpawn(int faction, UnitKind kind, Vector3 pos)
{
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world.units[i];
        if (u->active) continue;

        // Resolve stats ONCE: base def x this faction's difficulty mods.
        // (Training-building buffs multiply on top in BuildingsUpdate.)
        const UnitDef *def = StrategyUnitDef(kind);
        const FactionMods *m = &world.mods[faction];

        *u = (Unit){ 0 };
        u->active         = true;
        u->faction        = faction;
        u->kind           = kind;
        u->pos            = pos;
        u->target         = pos;
        u->state          = UNIT_IDLE;
        u->maxHp          = def->maxHp*m->hpMul;
        u->hp             = u->maxHp;
        u->damage         = def->damage*m->dmgMul;
        u->attackRange    = def->attackRange;
        u->attackPeriod   = def->attackPeriod;
        u->preferredRange = def->preferredRange;
        u->moveSpeed      = def->moveSpeed;
        u->sightRange     = def->sightRange*m->sightMul;
        u->gatherTime     = def->gatherTime*m->gatherMul;
        u->farmPeriod     = def->farmPeriod*m->gatherMul;
        u->targetUnit     = -1;
        u->targetNode     = -1;
        u->targetBuilding = -1;
        return u;
    }
    return NULL;
}

// scaffold=true spawns an under-construction site: near-zero HP, non-functional
// until a worker builds it up. Pre-placed and AI buildings pass false (finished).
static Building *BuildingSpawn(BuildingKind kind, int faction, Vector3 pos, bool scaffold)
{
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        Building *b = &world.buildings[i];
        if (b->active) continue;

        *b = (Building){ 0 };
        b->active            = true;
        b->kind              = kind;
        b->faction           = faction;
        b->pos               = pos;
        b->maxHp             = StrategyBuildingDef(kind)->maxHp;
        b->underConstruction = scaffold;
        b->hp                = scaffold ? 1.0f : b->maxHp;
        b->buildProgress     = 0.0f;
        b->trainKind         = -1;
        b->trainProgress     = 0.0f;
        return b;
    }
    return NULL;
}

static void NodeSpawn(NodeKind kind, Vector3 pos, int amount)
{
    for (int i = 0; i < STRAT_MAX_NODES; i++)
    {
        ResourceNode *n = &world.nodes[i];
        if (n->active) continue;

        *n = (ResourceNode){ 0 };
        n->active    = true;
        n->kind      = kind;
        n->pos       = pos;
        n->remaining = amount;
        return;
    }
}

static void NodeCluster(NodeKind kind, Vector3 center, int count, float spread, int amount)
{
    for (int i = 0; i < count; i++)
    {
        Vector3 pos = center;
        pos.x += (float)GetRandomValue(-100, 100)*0.01f*spread;
        pos.z += (float)GetRandomValue(-100, 100)*0.01f*spread;
        NodeSpawn(kind, pos, amount);
    }
}

void StrategyWorldInit(void)
{
    world = (StrategyWorld){ 0 };
    world.placing          = -1;
    world.selectedBuilding = -1;
    world.gameOver         = -1;
    EffectsReset();

    // Difficulty mods BEFORE any spawn (UnitSpawn reads them). The player and
    // neutral rows stay identity; only the AI faction is scaled. Hard is the
    // baseline; Normal/Easy weaken the AI and give the player a sell bonus.
    FactionMods identity = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f };
    world.mods[0] = world.mods[1] = world.mods[FACTION_NEUTRAL] = identity;
    switch (SettingsGet()->difficulty)
    {
        case 0:     // Easy
            world.mods[1] = (FactionMods){ 0.8f, 0.8f, 1.10f, 0.9f, 1.6f, 0.0f };
            world.mods[0].refundBonus = 0.2f;
            break;
        case 1:     // Normal
            world.mods[1] = (FactionMods){ 0.9f, 1.0f, 1.05f, 1.0f, 1.25f, 0.0f };
            world.mods[0].refundBonus = 0.1f;
            break;
        default:    // Hard: identity
            break;
    }
    world.aiPeriod = STRAT_AI_PERIOD*world.mods[1].aiPeriodMul;
    world.aiTimer  = world.aiPeriod;

    // Camera: perspective, fixed pitch; focus starts over the player base.
    world.camera.up         = (Vector3){ 0.0f, 1.0f, 0.0f };
    world.camera.fovy       = 45.0f;
    world.camera.projection = CAMERA_PERSPECTIVE;
    world.camFocus = (Vector2){ -14.0f, -12.0f };
    world.camZoom  = 1.0f;
    CameraRefresh();

    // Two rival bases in opposite corners: town hall FIRST (critical, trains
    // workers, and the AI's EnemyHome anchors to the first building), two
    // houses + barracks; 4 workers + 2 soldiers each.
    BuildingSpawn(BLD_TOWN_HALL, 0, (Vector3){ -14.0f, 0.0f, -14.0f }, false);
    BuildingSpawn(BLD_HOUSE,     0, (Vector3){ -17.0f, 0.0f, -14.0f }, false);
    BuildingSpawn(BLD_HOUSE,     0, (Vector3){ -17.0f, 0.0f, -11.0f }, false);
    BuildingSpawn(BLD_BARRACKS,  0, (Vector3){ -11.0f, 0.0f, -14.0f }, false);
    BuildingSpawn(BLD_TOWN_HALL, 1, (Vector3){  14.0f, 0.0f,  14.0f }, false);
    BuildingSpawn(BLD_HOUSE,     1, (Vector3){  17.0f, 0.0f,  14.0f }, false);
    BuildingSpawn(BLD_HOUSE,     1, (Vector3){  17.0f, 0.0f,  11.0f }, false);
    BuildingSpawn(BLD_BARRACKS,  1, (Vector3){  11.0f, 0.0f,  14.0f }, false);
    for (int i = 0; i < 6; i++)
    {
        UnitKind kind = (i < 4) ? KIND_WORKER : KIND_SOLDIER;
        UnitSpawn(0, kind, (Vector3){ -14.0f + (float)i*1.2f - 3.0f, 0.0f, -11.0f });
        UnitSpawn(1, kind, (Vector3){  14.0f - (float)i*1.2f + 3.0f, 0.0f,  11.0f });
    }

    // Resources scattered between the bases; wheat near each base so both
    // factions can feed their training queue.
    NodeCluster(NODE_TREE,  (Vector3){ -7.0f, 0.0f,  -3.0f }, 6, 2.5f, 12);
    NodeCluster(NODE_TREE,  (Vector3){  6.0f, 0.0f,   9.0f }, 6, 2.5f, 12);
    NodeCluster(NODE_ROCK,  (Vector3){ -3.0f, 0.0f,   8.0f }, 5, 2.0f, 100);
    NodeCluster(NODE_ROCK,  (Vector3){  9.0f, 0.0f,  -7.0f }, 5, 2.0f, 100);
    NodeCluster(NODE_WHEAT, (Vector3){ -10.0f, 0.0f, -7.0f }, 5, 2.0f, 8);
    NodeCluster(NODE_WHEAT, (Vector3){  10.0f, 0.0f,  7.0f }, 5, 2.0f, 8);

    // Neutral animals wandering mid-map, huntable for food. Weak ones flee
    // when hit, strong ones fight back as a pack.
    for (int i = 0; i < STRAT_ANIMAL_COUNT; i++)
    {
        Vector3 pos = (Vector3){
            (float)GetRandomValue(-10, 10), 0.0f, (float)GetRandomValue(-10, 10),
        };
        UnitSpawn(FACTION_NEUTRAL, KIND_ANIMAL_WEAK, pos);
    }
    for (int i = 0; i < STRAT_ANIMAL_STRONG_COUNT; i++)
    {
        Vector3 pos = (Vector3){
            (float)GetRandomValue(-8, 8), 0.0f, (float)GetRandomValue(-8, 8),
        };
        UnitSpawn(FACTION_NEUTRAL, KIND_ANIMAL_STRONG, pos);
    }

    // Starting stockpiles so building/training is possible right away. The
    // human player (faction 0) gets an easier-difficulty head start: Easy x3,
    // Normal x2, Hard x1. The enemy always starts on the base amounts.
    int playerMul = (SettingsGet()->difficulty == 0) ? 3
                  : (SettingsGet()->difficulty == 1) ? 2 : 1;
    for (int f = 0; f < STRAT_FACTIONS; f++)
    {
        int mul = (f == 0) ? playerMul : 1;
        world.stockpile[f][RES_WOOD]  = 10*mul;
        world.stockpile[f][RES_STONE] = 10*mul;
        world.stockpile[f][RES_FOOD]  = 5*mul;
    }
}

// ----------------------------------------------------------------------------
//  Orders: the ONLY way anything (mouse, GUI or AI) makes a unit act.
//  Exported so strategy_ai.c issues the exact same orders the mouse does.
// ----------------------------------------------------------------------------
void StrategyOrderMove(Unit *u, Vector3 dest)
{
    u->state          = UNIT_MOVE;
    u->target         = dest;
    u->targetUnit     = -1;
    u->targetNode     = -1;
    u->targetBuilding = -1;
}

void StrategyOrderGather(Unit *u, int nodeIndex)
{
    u->state          = UNIT_GATHER;
    u->targetNode     = nodeIndex;
    u->targetUnit     = -1;
    u->targetBuilding = -1;
    u->gatherTimer    = 0.0f;
}

void StrategyOrderAttack(Unit *u, int unitIndex)
{
    u->state          = UNIT_ATTACK;
    u->targetUnit     = unitIndex;
    u->targetNode     = -1;
    u->targetBuilding = -1;
    u->attackCooldown = 0.0f;
}

void StrategyOrderAttackBuilding(Unit *u, int bldIndex)
{
    u->state          = UNIT_ATTACK;
    u->targetBuilding = bldIndex;
    u->targetUnit     = -1;
    u->targetNode     = -1;
    u->attackCooldown = 0.0f;
}

// Assign a worker to tend a node-spawning building (farm/forestry). target is
// seeded to the building position, the "pick a new plant spot" sentinel.
void StrategyOrderFarm(Unit *u, int bldIndex)
{
    u->state          = UNIT_FARM;
    u->targetBuilding = bldIndex;
    u->target         = world.buildings[bldIndex].pos;
    u->targetUnit     = -1;
    u->targetNode     = -1;
    u->carryAmount    = 0;
    u->gatherTimer    = 0.0f;
}

void StrategyOrderBuild(Unit *u, int bldIndex)
{
    u->state          = UNIT_BUILD;
    u->targetBuilding = bldIndex;
    u->targetUnit     = -1;
    u->targetNode     = -1;
}

void StrategyOrderRepair(Unit *u, int bldIndex)
{
    u->state          = UNIT_REPAIR;
    u->targetBuilding = bldIndex;
    u->targetUnit     = -1;
    u->targetNode     = -1;
}

// ----------------------------------------------------------------------------
//  Picking: nearest unit / node to a ground point, within a grab radius.
// ----------------------------------------------------------------------------
static int PickUnit(Vector3 ground, int faction, float radius)
{
    int best = -1;
    float bestDist = radius;
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world.units[i];
        if (!u->active) continue;
        if (faction >= 0 && u->faction != faction) continue;

        float d = DistXZ(u->pos, ground);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

// Nearest building to a ground point within a grab radius; faction -1 = any.
static int PickBuilding(Vector3 ground, int faction, float radius)
{
    int best = -1;
    float bestDist = radius;
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        Building *b = &world.buildings[i];
        if (!b->active) continue;
        if (faction >= 0 && b->faction != faction) continue;

        float d = DistXZ(b->pos, ground);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

static int PickNode(Vector3 ground, float radius)
{
    int best = -1;
    float bestDist = radius;
    for (int i = 0; i < STRAT_MAX_NODES; i++)
    {
        ResourceNode *n = &world.nodes[i];
        if (!n->active) continue;

        float d = DistXZ(n->pos, ground);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

// Nearest active node of a kind (-1 = any) within radius; -1 when none.
int StrategyNearestNodeOfKind(Vector3 pos, int nodeKind, float radius)
{
    int best = -1;
    float bestDist = radius;
    for (int i = 0; i < STRAT_MAX_NODES; i++)
    {
        ResourceNode *n = &world.nodes[i];
        if (!n->active) continue;
        if (nodeKind >= 0 && (int)n->kind != nodeKind) continue;

        float d = DistXZ(n->pos, pos);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

// ----------------------------------------------------------------------------
//  Population + training + building destruction (shared by GUI and AI)
// ----------------------------------------------------------------------------
int StrategyPopCap(int faction)
{
    int cap = 0;
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        Building *b = &world.buildings[i];
        if (b->active && !b->underConstruction && b->faction == faction)
        {
            cap += StrategyBuildingDef(b->kind)->popCap;
        }
    }
    return cap;
}

int StrategyPopUsed(int faction)
{
    int used = 0;
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        if (world.units[i].active && world.units[i].faction == faction) used++;
    }
    return used;
}

// Refund a to-be-cancelled trainee's paid cost back to the faction stockpile.
static void TrainRefund(Building *b, UnitKind kind)
{
    const BuildingDef *bd = StrategyBuildingDef(b->kind);
    const UnitDef *ud = StrategyUnitDef(kind);
    for (int r = 0; r < RES_COUNT; r++)
        world.stockpile[b->faction][r] += (int)ceilf((float)ud->cost[r]*bd->trainCostMul);
}

// Cancel one trainee: the last queued one if any, else the active trainee.
// Refunds its cost. Returns false when there was nothing to cancel.
bool StrategyTrainCancel(int bldIndex)
{
    Building *b = &world.buildings[bldIndex];
    if (!b->active) return false;

    if (b->trainQueueCount > 0)
    {
        TrainRefund(b, b->trainQueue[--b->trainQueueCount]);
        return true;
    }
    if (b->trainKind >= 0)
    {
        TrainRefund(b, (UnitKind)b->trainKind);
        b->trainKind     = -1;
        b->trainProgress = 0.0f;
        return true;
    }
    return false;
}

// Pop the next queued kind into the active trainee slot (queue shifts down).
// Assumes the building is idle and off cooldown; cost was paid on enqueue.
static void TrainDequeue(Building *b)
{
    if (b->trainQueueCount <= 0) return;
    b->trainKind     = (int)b->trainQueue[0];
    b->trainProgress = 0.0f;
    for (int i = 1; i < b->trainQueueCount; i++) b->trainQueue[i - 1] = b->trainQueue[i];
    b->trainQueueCount--;
}

// Enqueue a trainee: validate kind/pop/cost, pay up front, then either start
// immediately (idle + off cooldown) or append to the queue. The pop check
// counts already-queued trainees so a full queue can't overshoot the cap.
bool StrategyTrainStart(int bldIndex, UnitKind kind)
{
    Building *b = &world.buildings[bldIndex];
    if (!b->active || b->underConstruction) return false;

    const BuildingDef *bd = StrategyBuildingDef(b->kind);
    bool allowed = false;
    for (int i = 0; i < bd->trainableCount; i++)
    {
        if (bd->trainable[i] == kind) allowed = true;
    }
    if (!allowed) return false;

    // Everything already committed to this building: the active trainee plus
    // whatever is queued behind it. Keeps the pop cap honest for the queue.
    int committed = (b->trainKind >= 0 ? 1 : 0) + b->trainQueueCount;
    if (b->trainQueueCount >= BLD_MAX_QUEUE) return false;
    if (StrategyPopUsed(b->faction) + committed + 1 > StrategyPopCap(b->faction)) return false;

    const UnitDef *ud = StrategyUnitDef(kind);
    int cost[RES_COUNT];
    for (int r = 0; r < RES_COUNT; r++)
    {
        cost[r] = (int)ceilf((float)ud->cost[r]*bd->trainCostMul);
        if (world.stockpile[b->faction][r] < cost[r]) return false;
    }
    for (int r = 0; r < RES_COUNT; r++)
    {
        world.stockpile[b->faction][r] -= cost[r];
    }

    if (b->trainKind < 0 && b->trainCooldown <= 0.0f)
    {
        b->trainKind     = (int)kind;
        b->trainProgress = 0.0f;
    }
    else
    {
        b->trainQueue[b->trainQueueCount++] = kind;
    }
    return true;
}

// A faction is defeated when it has NO critical building AND no workers left
// (houses alone can't rebuild an economy). Called after every building
// destroy/sell and every unit kill.
static void CheckGameOver(void)
{
    if (world.gameOver >= 0) return;

    for (int f = 0; f < STRAT_FACTIONS; f++)
    {
        bool critical = false;
        bool workers  = false;
        for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
        {
            Building *b = &world.buildings[i];
            if (b->active && b->faction == f &&
                StrategyBuildingDef(b->kind)->critical)
            {
                critical = true;
                break;
            }
        }
        for (int i = 0; i < STRAT_MAX_UNITS; i++)
        {
            Unit *u = &world.units[i];
            if (u->active && u->faction == f && u->kind == KIND_WORKER)
            {
                workers = true;
                break;
            }
        }
        if (!critical && !workers)
        {
            world.gameOver = 1 - f;     // the OTHER faction wins
            return;
        }
    }
}

static void BuildingDestroy(int index)
{
    Building *b = &world.buildings[index];
    b->active = false;
    if (world.selectedBuilding == index) world.selectedBuilding = -1;

    EffectSpawn(FX_RING, b->pos, strategyFactionColor[b->faction]);
    EffectSpawn(FX_FLASH, (Vector3){ b->pos.x, 0.8f, b->pos.z }, RAYWHITE);
    for (int i = 0; i < 6; i++)
    {
        EffectSpawn(FX_PUFF, (Vector3){ b->pos.x, 0.8f, b->pos.z }, DARKGRAY);
    }
    CheckGameOver();
}

// Sell a building back: refund refundRate (+ the player-facing difficulty
// bonus) of every cost component, floored. The slot just deactivates.
bool StrategySellBuilding(int index)
{
    Building *b = &world.buildings[index];
    if (!b->active) return false;

    const BuildingDef *bd = StrategyBuildingDef(b->kind);
    float rate = bd->refundRate + world.mods[b->faction].refundBonus;
    for (int r = 0; r < RES_COUNT; r++)
    {
        world.stockpile[b->faction][r] += (int)floorf((float)bd->cost[r]*rate);
    }
    b->active = false;
    if (world.selectedBuilding == index) world.selectedBuilding = -1;

    EffectSpawn(FX_RING, b->pos, GOLD);
    for (int i = 0; i < 3; i++)
    {
        EffectSpawn(FX_PUFF, (Vector3){ b->pos.x, 0.8f, b->pos.z }, LIGHTGRAY);
    }
    CheckGameOver();    // selling your last town hall can lose the game
    return true;
}

// Count active nodes of a kind within radius of a point (node-tend cap).
static int NodesNear(NodeKind kind, Vector3 pos, float radius)
{
    int n = 0;
    for (int i = 0; i < STRAT_MAX_NODES; i++)
    {
        ResourceNode *nd = &world.nodes[i];
        if (nd->active && nd->kind == kind && DistXZ(nd->pos, pos) <= radius) n++;
    }
    return n;
}

// A ground spot is free to plant on when it is clear of every node, building
// and off the map edge. Used to scatter tended nodes without overlap.
static bool PlantSpotClear(Vector3 pos)
{
    float margin = STRAT_GROUND_HALF - 1.0f;
    if (fabsf(pos.x) > margin || fabsf(pos.z) > margin) return false;
    for (int i = 0; i < STRAT_MAX_NODES; i++)
        if (world.nodes[i].active &&
            DistXZ(world.nodes[i].pos, pos) < STRAT_TEND_SPACING) return false;
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
        if (world.buildings[i].active &&
            DistXZ(world.buildings[i].pos, pos) < STRAT_TEND_SPACING) return false;
    return true;
}

// Pick a free plant spot near a building's tend area, or fall back to the
// building itself when the area is packed. A few random tries is plenty.
static Vector3 PlantSpotNear(Vector3 center)
{
    for (int tries = 0; tries < 12; tries++)
    {
        Vector3 p = center;
        p.x += (float)GetRandomValue(-100, 100)*0.01f*STRAT_TEND_RANGE;
        p.z += (float)GetRandomValue(-100, 100)*0.01f*STRAT_TEND_RANGE;
        if (PlantSpotClear(p)) return p;
    }
    return center;
}

// Quarry: spend providence to conjure a fresh stone node beside the quarry.
// Returns false when the building isn't a finished quarry or providence is short.
bool StrategyQuarrySpawnStone(int bldIndex)
{
    Building *b = &world.buildings[bldIndex];
    if (!b->active || b->underConstruction || b->kind != BLD_QUARRY) return false;
    if (world.stockpile[b->faction][RES_PROVIDENCE] < STRAT_QUARRY_STONE_PROV)
        return false;

    world.stockpile[b->faction][RES_PROVIDENCE] -= STRAT_QUARRY_STONE_PROV;
    Vector3 p = b->pos;
    p.x += (float)GetRandomValue(-100, 100)*0.01f*STRAT_QUARRY_STONE_SPREAD;
    p.z += (float)GetRandomValue(-100, 100)*0.01f*STRAT_QUARRY_STONE_SPREAD;
    NodeSpawn(NODE_ROCK, p, STRAT_QUARRY_STONE_AMOUNT);
    EffectSpawn(FX_RING, p, GRAY);
    EffectSpawn(FX_FLASH, (Vector3){ p.x, 0.6f, p.z }, RAYWHITE);
    return true;
}

// Advance every in-progress trainee; on completion spawn the unit beside the
// building (separation shoves crowds apart) and start the anti-spam cooldown.
// Scaffolds do nothing here; the forestry auto-plants trees on its own timer.
static void BuildingsUpdate(float dt)
{
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        Building *b = &world.buildings[i];
        if (!b->active || b->underConstruction) continue;

        if (b->trainCooldown > 0.0f) b->trainCooldown -= dt;

        // Idle and off cooldown with a queue waiting: start the next trainee.
        if (b->trainKind < 0 && b->trainCooldown <= 0.0f && b->trainQueueCount > 0)
            TrainDequeue(b);
        if (b->trainKind < 0) continue;

        b->trainProgress += dt;
        if (b->trainProgress >= StrategyUnitDef((UnitKind)b->trainKind)->trainTime)
        {
            const BuildingDef *bd = StrategyBuildingDef(b->kind);
            Vector3 spawn = (Vector3){ b->pos.x + 1.6f, 0.0f, b->pos.z + 1.2f };
            Unit *u = UnitSpawn(b->faction, (UnitKind)b->trainKind, spawn);
            if (u != NULL)
            {
                // Training-building buffs (identity today: upgrade hook).
                u->maxHp  *= bd->buffHpMul;
                u->hp      = u->maxHp;
                u->damage *= bd->buffDmgMul;
                // Rally point: walk the fresh unit to the flag if one is set.
                if (b->hasRally) StrategyOrderMove(u, b->rally);
            }
            EffectSpawn(FX_RING, spawn, strategyFactionColor[b->faction]);
            EffectSpawn(FX_FLASH, (Vector3){ spawn.x, 0.6f, spawn.z }, RAYWHITE);
            b->trainKind     = -1;
            b->trainProgress = 0.0f;
            b->trainCooldown = bd->trainCooldown;
        }
    }
}

// ----------------------------------------------------------------------------
//  Input: camera, building placement, selection, orders
// ----------------------------------------------------------------------------
static void CameraPanZoom(void)
{
    float dt = GetFrameTime();
    float pan = 12.0f*world.camZoom*dt;

    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP))    world.camFocus.y -= pan;
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN))  world.camFocus.y += pan;
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT))  world.camFocus.x -= pan;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) world.camFocus.x += pan;
    world.camFocus.x = Clamp(world.camFocus.x, -STRAT_GROUND_HALF, STRAT_GROUND_HALF);
    world.camFocus.y = Clamp(world.camFocus.y, -STRAT_GROUND_HALF, STRAT_GROUND_HALF);

    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f)
    {
        world.camZoom *= 1.0f - wheel*0.1f;
        world.camZoom = Clamp(world.camZoom, 0.35f, 1.45f);
    }
    CameraRefresh();
}

// Placement validity: affordable, on the ground, and clear of everything.
// Can the faction currently pay for this building?
static bool Affordable(int faction, BuildingKind kind)
{
    for (int r = 0; r < RES_COUNT; r++)
        if (world.stockpile[faction][r] < StrategyBuildingDef(kind)->cost[r]) return false;
    return true;
}

static bool PlacementValid(int faction, BuildingKind kind, Vector3 pos)
{
    if (!Affordable(faction, kind)) return false;
    float margin = STRAT_GROUND_HALF - 1.5f;
    if (fabsf(pos.x) > margin || fabsf(pos.z) > margin) return false;

    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        if (world.buildings[i].active && DistXZ(world.buildings[i].pos, pos) < 2.4f) return false;
    }
    for (int i = 0; i < STRAT_MAX_NODES; i++)
    {
        if (world.nodes[i].active && DistXZ(world.nodes[i].pos, pos) < 1.6f) return false;
    }
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        if (world.units[i].active && DistXZ(world.units[i].pos, pos) < 1.2f) return false;
    }
    return true;
}

// Validate + pay + place in one call. Shared by the player's placement
// click and the enemy AI's house building.
bool StrategyTryBuild(int faction, BuildingKind kind, Vector3 pos)
{
    if (!PlacementValid(faction, kind, pos)) return false;

    for (int r = 0; r < RES_COUNT; r++)
    {
        world.stockpile[faction][r] -= StrategyBuildingDef(kind)->cost[r];
    }
    // Player buildings go up as scaffolds a worker must finish; the AI has no
    // build behavior, so its buildings spawn complete.
    BuildingSpawn(kind, faction, pos, faction == 0);
    EffectSpawn(FX_RING, pos, RAYWHITE);
    for (int i = 0; i < 3; i++) EffectSpawn(FX_PUFF, (Vector3){ pos.x, 0.8f, pos.z }, LIGHTGRAY);
    return true;
}

// Ghost position: mouse ground point snapped to the 1-unit grid.
static bool PlacementGhost(Vector3 *out)
{
    Vector3 ground;
    if (!MouseGroundPoint(&ground)) return false;
    out->x = roundf(ground.x);
    out->y = 0.0f;
    out->z = roundf(ground.z);
    return true;
}

static void PlacementInput(void)
{
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
    {
        world.placing = -1;     // cancel
        return;
    }
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || MouseOnGui()) return;

    Vector3 pos;
    BuildingKind kind = (BuildingKind)world.placing;
    if (!PlacementGhost(&pos)) return;
    if (!StrategyTryBuild(0, kind, pos)) return;

    // Hold Shift to keep placing more of the same building; a plain click
    // (or running out of resources) exits placement mode.
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (!shift || !Affordable(0, kind)) world.placing = -1;
}

// Select every own unit of `kind` whose projection lands within the visible
// game canvas (double-click "select all of type"). shift extends the current
// selection instead of replacing it.
static void SelectOnScreenOfKind(UnitKind kind, bool shift)
{
    Vector2 gameSize = ScreenStateTargetSize();
    Rectangle screen = { 0.0f, 0.0f, gameSize.x, gameSize.y };
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world.units[i];
        if (!u->active || u->faction != 0) continue;
        if (u->kind != kind)
        {
            if (!shift) u->selected = false;
            continue;
        }
        if (CheckCollisionPointRec(WorldToGame(u->pos), screen)) u->selected = true;
        else if (!shift) u->selected = false;
    }
}

static void SelectionInput(void)
{
    Vector2 mouse = MouseGame();
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        if (MouseOnGui())
        {
            // The build bar owns this click: without this flag the stale
            // dragStart would fake a drag and the release (the same one
            // that fires GuiButton) would box-select behind the panel.
            world.pressInWorld = false;
            world.dragging     = false;
            return;
        }
        world.pressInWorld = true;
        world.dragStart    = mouse;
        world.dragging     = false;
    }
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && world.pressInWorld && !world.dragging)
    {
        if (Vector2Distance(mouse, world.dragStart) > 6.0f) world.dragging = true;
    }
    if (!IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) return;
    if (!world.pressInWorld) return;    // this click belongs to the GUI
    world.pressInWorld = false;

    if (world.dragging)
    {
        // Box select: player units whose SCREEN projection is inside the rect.
        world.selectedBuilding = -1;
        Rectangle rect = {
            fminf(world.dragStart.x, mouse.x), fminf(world.dragStart.y, mouse.y),
            fabsf(mouse.x - world.dragStart.x), fabsf(mouse.y - world.dragStart.y),
        };
        for (int i = 0; i < STRAT_MAX_UNITS; i++)
        {
            Unit *u = &world.units[i];
            if (!u->active || u->faction != 0)
            {
                continue;
            }
            bool inside = CheckCollisionPointRec(WorldToGame(u->pos), rect);
            if (inside)      u->selected = true;
            else if (!shift) u->selected = false;
        }
        world.dragging = false;
        return;
    }

    // Plain click: select the player unit under the cursor; with no unit hit,
    // try an own building; otherwise deselect all. Unit and building
    // selection are mutually exclusive.
    Vector3 ground;
    if (!MouseGroundPoint(&ground)) return;
    int hit = PickUnit(ground, 0, 0.7f);

    if (hit < 0)
    {
        world.selectedBuilding = PickBuilding(ground, 0, 1.4f);
        if (world.selectedBuilding >= 0)
        {
            for (int i = 0; i < STRAT_MAX_UNITS; i++) world.units[i].selected = false;
            EffectSpawn(FX_RING, world.buildings[world.selectedBuilding].pos, GREEN);
            return;
        }
    }
    else world.selectedBuilding = -1;

    // Double-click a unit -> select all on-screen units of that kind. Detected
    // by a second hit on the same-kind unit within the click window.
    static double lastClickTime = -1.0;
    static int    lastClickKind = -1;
    if (hit >= 0)
    {
        double now = GetTime();
        UnitKind kind = world.units[hit].kind;
        bool dbl = (now - lastClickTime < 0.3) && (lastClickKind == (int)kind);
        lastClickTime = now;
        lastClickKind = (int)kind;
        if (dbl)
        {
            SelectOnScreenOfKind(kind, shift);
            EffectSpawn(FX_RING, world.units[hit].pos, GREEN);
            return;
        }
    }
    else lastClickKind = -1;

    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world.units[i];
        if (!u->active || u->faction != 0) continue;
        if (i == hit)    u->selected = true;
        else if (!shift) u->selected = false;
    }
    if (hit >= 0) EffectSpawn(FX_RING, world.units[hit].pos, GREEN);
}

// One selected unit's right-click routing. `hostile` covers enemy units AND
// animals (hunting); soldiers can't gather/farm/build, so those fall back to
// move. ownScaffold/ownRepair/ownFarm are worker-only own-building jobs.
static void OrderUnitAt(Unit *u, int hostile, int enemyBld, int node,
                        int ownFarm, int ownScaffold, int ownRepair, Vector3 ground)
{
    bool worker = (u->kind == KIND_WORKER);

    if (hostile >= 0)                     StrategyOrderAttack(u, hostile);
    else if (enemyBld >= 0)               StrategyOrderAttackBuilding(u, enemyBld);
    else if (node >= 0 && worker)         StrategyOrderGather(u, node);
    else if (ownScaffold >= 0 && worker)  StrategyOrderBuild(u, ownScaffold);
    else if (ownRepair >= 0 && worker)    StrategyOrderRepair(u, ownRepair);
    else if (ownFarm >= 0 && worker)      StrategyOrderFarm(u, ownFarm);
    else                                  StrategyOrderMove(u, ground);
}

// Right click: hostile unit/animal > enemy building > resource node >
// own farm > plain move - checked in that priority so a click near a tree
// still prefers the deer standing beside it.
static void OrderInput(void)
{
    if (!IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) || MouseOnGui()) return;

    Vector3 ground;
    if (!MouseGroundPoint(&ground)) return;

    // A selected training building takes the right-click as a rally order:
    // new trainees will walk to this spot instead of standing at the door.
    if (world.selectedBuilding >= 0)
    {
        Building *b = &world.buildings[world.selectedBuilding];
        if (StrategyBuildingDef(b->kind)->trainableCount > 0)
        {
            b->rally    = ground;
            b->hasRally = true;
            EffectSpawn(FX_RING, ground, GREEN);
            return;
        }
    }

    // Any non-player unit is a valid attack/hunt target.
    int hostile = PickUnit(ground, 1, 0.8f);
    if (hostile < 0) hostile = PickUnit(ground, FACTION_NEUTRAL, 0.8f);

    int enemyBld = (hostile < 0) ? PickBuilding(ground, 1, 1.4f) : -1;
    int node     = (hostile < 0 && enemyBld < 0) ? PickNode(ground, 0.9f) : -1;

    // Own building under the cursor -> build (scaffold), repair (damaged), or
    // farm work, in that priority. Worker-only; OrderUnitAt gates by kind.
    int ownFarm = -1, ownScaffold = -1, ownRepair = -1;
    if (hostile < 0 && enemyBld < 0 && node < 0)
    {
        int own = PickBuilding(ground, 0, 1.4f);
        if (own >= 0)
        {
            Building *b = &world.buildings[own];
            if (b->underConstruction)      ownScaffold = own;
            else if (b->hp < b->maxHp)      ownRepair   = own;
            else if (StrategyBuildingDef(b->kind)->tendNode >= 0) ownFarm = own;
        }
    }

    bool any = false;
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world.units[i];
        if (!u->active || u->faction != 0 || !u->selected) continue;

        OrderUnitAt(u, hostile, enemyBld, node, ownFarm, ownScaffold, ownRepair, ground);
        any = true;
    }
    if (!any) return;

    // Order feedback: red ring on an attack target, yellow on a resource,
    // green on a build/repair/farm target, lime ripple on plain ground.
    if (hostile >= 0)         EffectSpawn(FX_RING, world.units[hostile].pos, RED);
    else if (enemyBld >= 0)   EffectSpawn(FX_RING, world.buildings[enemyBld].pos, RED);
    else if (node >= 0)       EffectSpawn(FX_RING, world.nodes[node].pos, YELLOW);
    else if (ownScaffold >= 0)EffectSpawn(FX_RING, world.buildings[ownScaffold].pos, GREEN);
    else if (ownRepair >= 0)  EffectSpawn(FX_RING, world.buildings[ownRepair].pos, GREEN);
    else if (ownFarm >= 0)    EffectSpawn(FX_RING, world.buildings[ownFarm].pos, GREEN);
    else                      EffectSpawn(FX_RING, ground, LIME);
}

// Control groups: ctrl+1..3 stamps the current selection as that group
// (removing units no longer selected); bare 1..3 recalls the group.
static void ControlGroupInput(void)
{
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

    for (int g = 1; g <= 3; g++)
    {
        if (!IsKeyPressed(KEY_ZERO + g)) continue;

        for (int i = 0; i < STRAT_MAX_UNITS; i++)
        {
            Unit *u = &world.units[i];
            if (!u->active || u->faction != 0) continue;

            if (ctrl)
            {
                if (u->selected)               u->controlGroup = g;
                else if (u->controlGroup == g) u->controlGroup = 0;
            }
            else u->selected = (u->controlGroup == g);
        }
        if (!ctrl) world.selectedBuilding = -1;
    }
}

void StrategyWorldHandleInput(void)
{
    CameraPanZoom();
    ControlGroupInput();

    if (world.placing >= 0)
    {
        PlacementInput();
        return;     // placement mode owns the mouse
    }
    SelectionInput();
    OrderInput();
}

// ----------------------------------------------------------------------------
//  Unit behavior (faction-agnostic)
// ----------------------------------------------------------------------------
static void MoveToward(Unit *u, Vector3 dest, float dt)
{
    Vector3 delta = Vector3Subtract(dest, u->pos);
    delta.y = 0.0f;
    float dist = Vector3Length(delta);
    if (dist < 0.001f) return;

    float step = u->moveSpeed*dt;
    if (step > dist) step = dist;
    u->pos = Vector3Add(u->pos, Vector3Scale(delta, step/dist));
}

// Nearest own building that ACCEPTS what the unit is carrying - wood only
// lands at logging camps / town halls, and so on.
static int NearestDropoff(const Unit *u)
{
    int best = -1;
    float bestDist = 1000000.0f;
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        Building *b = &world.buildings[i];
        if (!b->active || b->underConstruction || b->faction != u->faction) continue;
        if (!StrategyBuildingDef(b->kind)->accepts[u->carryKind]) continue;

        float d = DistXZ(b->pos, u->pos);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

static int NearestHostile(const Unit *u, float range)
{
    int best = -1;
    float bestDist = range;
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *other = &world.units[i];
        if (!other->active || other->faction == u->faction) continue;
        if (other->faction == FACTION_NEUTRAL) continue;   // animals aren't hostile

        float d = DistXZ(other->pos, u->pos);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

// Kill a unit. A hunted animal leaves a food corpse node behind, and a worker
// that did the killing immediately starts harvesting it (hunting loop).
static void UnitKill(int index, Unit *killer)
{
    Unit *u = &world.units[index];
    u->active   = false;
    u->selected = false;

    EffectSpawn(FX_RING, u->pos, UnitColor(u));
    for (int i = 0; i < 4; i++)
    {
        EffectSpawn(FX_PUFF, (Vector3){ u->pos.x, 0.5f, u->pos.z }, GRAY);
    }

    int corpseFood = StrategyUnitDef(u->kind)->corpseFood;
    if (corpseFood > 0)
    {
        NodeSpawn(NODE_CORPSE, u->pos, corpseFood);
        if (killer != NULL && killer->kind == KIND_WORKER)
        {
            int corpse = StrategyNearestNodeOfKind(u->pos, NODE_CORPSE, 2.0f);
            if (corpse >= 0) StrategyOrderGather(killer, corpse);
        }
    }
    CheckGameOver();    // losing the last worker can decide the game
}

// Weak-animal panic reflex: the victim AND every weak animal near it bolt
// away from the attacker together (the herd scatters as one).
static void AnimalPanic(int victimIndex, Vector3 threat)
{
    Vector3 origin = world.units[victimIndex].pos;

    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world.units[i];
        if (!u->active || u->kind != KIND_ANIMAL_WEAK) continue;
        if (DistXZ(u->pos, origin) > STRAT_FLEE_PACK_RADIUS) continue;

        Vector3 away = Vector3Subtract(u->pos, threat);
        away.y = 0.0f;
        if (Vector3Length(away) < 0.001f) away = (Vector3){ 1.0f, 0.0f, 0.0f };

        Vector3 dest = Vector3Add(u->pos,
                                  Vector3Scale(Vector3Normalize(away), STRAT_FLEE_DIST));
        dest.x = Clamp(dest.x, -STRAT_GROUND_HALF + 1.0f, STRAT_GROUND_HALF - 1.0f);
        dest.z = Clamp(dest.z, -STRAT_GROUND_HALF + 1.0f, STRAT_GROUND_HALF - 1.0f);

        u->state          = UNIT_FLEE;
        u->target         = dest;
        u->targetUnit     = -1;
        u->targetNode     = -1;
        u->targetBuilding = -1;
    }
}

// Strong-animal pack reflex: the victim and its packmates all turn on the
// attacker (regular attack orders - the shared state machine does the rest).
static void AnimalRetaliate(int victimIndex, Unit *attacker)
{
    int attackerIndex = (int)(attacker - world.units);
    if (attackerIndex < 0 || attackerIndex >= STRAT_MAX_UNITS) return;

    Vector3 origin = world.units[victimIndex].pos;
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world.units[i];
        if (!u->active || u->kind != KIND_ANIMAL_STRONG) continue;
        if (DistXZ(u->pos, origin) > STRAT_FLEE_PACK_RADIUS) continue;

        StrategyOrderAttack(u, attackerIndex);
    }
}

// ALL unit-vs-unit damage funnels through here so the animal reflexes fire
// even on a killing blow. Returns true when the victim died.
static bool UnitDamage(int victimIndex, Unit *attacker, float damage)
{
    Unit *v = &world.units[victimIndex];
    v->hp -= damage;

    if (v->kind == KIND_ANIMAL_WEAK)        AnimalPanic(victimIndex, attacker->pos);
    else if (v->kind == KIND_ANIMAL_STRONG) AnimalRetaliate(victimIndex, attacker);

    if (v->hp <= 0.0f)
    {
        UnitKill(victimIndex, attacker);
        return true;
    }
    return false;
}

// Auto-aggro: idle units of BOTH factions engage hostiles on sight; enemy
// units also break off moving/working (the player keeps manual control).
// Animals only react when hit; templars never fight.
static void UnitAggroScan(Unit *u)
{
    if (u->kind == KIND_ANIMAL_WEAK || u->kind == KIND_ANIMAL_STRONG) return;
    if (u->kind == KIND_TEMPLAR || u->kind == KIND_TEMPLAR_HEALER) return;

    bool scan = (u->state == UNIT_IDLE) ||
                (u->faction == 1 && (u->state == UNIT_MOVE ||
                                     u->state == UNIT_GATHER ||
                                     u->state == UNIT_RETURN ||
                                     u->state == UNIT_FARM));
    if (!scan) return;

    int hostile = NearestHostile(u, u->sightRange);
    if (hostile >= 0) StrategyOrderAttack(u, hostile);
}

// Templar target search. The blessing templar shadows own units that are
// WORKING (gather/farm/return); the healer shadows own WOUNDED units. Both
// fall back to any own non-templar unit so they never stand alone mid-map.
static int TemplarFindTarget(const Unit *u)
{
    bool healer = (u->kind == KIND_TEMPLAR_HEALER);
    int best = -1;
    float bestDist = healer ? 15.0f : 1000000.0f;

    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *other = &world.units[i];
        if (!other->active || other->faction != u->faction || other == u) continue;
        if (other->kind == KIND_TEMPLAR || other->kind == KIND_TEMPLAR_HEALER) continue;

        if (healer)
        {
            if (other->hp >= other->maxHp) continue;
        }
        else
        {
            if (other->state != UNIT_GATHER && other->state != UNIT_FARM &&
                other->state != UNIT_RETURN) continue;
        }
        float d = DistXZ(other->pos, u->pos);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    if (best >= 0) return best;

    // Fallback: any own non-templar unit, unlimited range.
    bestDist = 1000000.0f;
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *other = &world.units[i];
        if (!other->active || other->faction != u->faction || other == u) continue;
        if (other->kind == KIND_TEMPLAR || other->kind == KIND_TEMPLAR_HEALER) continue;

        float d = DistXZ(other->pos, u->pos);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

static void UnitUpdate(int index, float dt)
{
    Unit *u = &world.units[index];

    UnitAggroScan(u);

    switch (u->state)
    {
        case UNIT_IDLE:
            break;

        case UNIT_MOVE:
        {
            MoveToward(u, u->target, dt);
            if (DistXZ(u->pos, u->target) < 0.15f) u->state = UNIT_IDLE;
        } break;

        case UNIT_GATHER:
        {
            ResourceNode *n = (u->targetNode >= 0) ? &world.nodes[u->targetNode] : NULL;
            if ((n == NULL) || !n->active)
            {
                // Auto-retarget: hop to the nearest node of the SAME kind
                // (dead slots keep their kind) instead of idling.
                int next = (n != NULL)
                    ? StrategyNearestNodeOfKind(u->pos, n->kind, STRAT_RETARGET_RADIUS)
                    : -1;
                int carried = u->carryAmount;
                if (next >= 0 && carried < STRAT_CARRY_MAX) StrategyOrderGather(u, next);
                else u->state = (carried > 0) ? UNIT_RETURN : UNIT_IDLE;
                break;
            }
            if (DistXZ(u->pos, n->pos) > 1.0f)
            {
                MoveToward(u, n->pos, dt);
                break;
            }
            // In working range: one resource unit per gatherTime tick.
            u->gatherTimer += dt;
            if (u->gatherTimer >= u->gatherTime)
            {
                u->gatherTimer -= u->gatherTime;
                u->carryKind = NodeResource(n->kind);
                u->carryAmount++;
                n->remaining--;

                Color puff = (n->kind == NODE_TREE) ? BROWN : GRAY;
                EffectSpawn(FX_PUFF, (Vector3){ n->pos.x, 1.0f, n->pos.z }, puff);

                if (n->remaining <= 0)
                {
                    n->active = false;     // depleted: vanish with a burst
                    EffectSpawn(FX_PUFF, (Vector3){ n->pos.x, 0.6f, n->pos.z }, puff);
                    EffectSpawn(FX_RING, n->pos, puff);
                }
            }
            if (u->carryAmount >= STRAT_CARRY_MAX || !n->active)
            {
                u->state = (u->carryAmount > 0) ? UNIT_RETURN : UNIT_IDLE;
            }
        } break;

        case UNIT_RETURN:
        {
            int home = NearestDropoff(u);
            if (home < 0)
            {
                u->carryAmount = 0;     // nothing accepts this - dump it
                u->state = UNIT_IDLE;
                break;
            }
            Building *b = &world.buildings[home];
            if (DistXZ(u->pos, b->pos) > 1.6f)
            {
                MoveToward(u, b->pos, dt);
                break;
            }
            world.stockpile[u->faction][u->carryKind] += u->carryAmount;
            u->carryAmount = 0;
            EffectSpawn(FX_FLASH, (Vector3){ b->pos.x, 1.2f, b->pos.z },
                        strategyFactionColor[u->faction]);

            // Keep the loop going while the node is alive, else retarget.
            bool nodeAlive = (u->targetNode >= 0) && world.nodes[u->targetNode].active;
            if (nodeAlive) u->state = UNIT_GATHER;
            else
            {
                int next = (u->targetNode >= 0)
                    ? StrategyNearestNodeOfKind(u->pos, world.nodes[u->targetNode].kind,
                                                STRAT_RETARGET_RADIUS)
                    : -1;
                if (next >= 0) StrategyOrderGather(u, next);
                else           u->state = UNIT_IDLE;
            }
        } break;

        case UNIT_FARM:
        {
            // Node-tending: a worker assigned to a farm/forestry plants its
            // node kind in free ground nearby. Once TEND_MAX of them stand
            // around the building it harvests the nearest one to depletion
            // (carrying to a dropoff like any gather) then resumes planting.
            Building *tb = (u->targetBuilding >= 0)
                ? &world.buildings[u->targetBuilding] : NULL;
            const BuildingDef *bd = (tb && tb->active)
                ? StrategyBuildingDef(tb->kind) : NULL;
            if ((tb == NULL) || !tb->active || bd == NULL || bd->tendNode < 0)
            {
                u->state = UNIT_IDLE;
                u->targetBuilding = -1;
                break;
            }
            NodeKind nk = (NodeKind)bd->tendNode;

            // 1) Carrying a full load: walk it to the nearest accepting building.
            if (u->carryAmount >= STRAT_CARRY_MAX)
            {
                int home = NearestDropoff(u);
                if (home < 0) { u->carryAmount = 0; }   // nothing accepts it: dump
                else
                {
                    Building *h = &world.buildings[home];
                    if (DistXZ(u->pos, h->pos) > 1.6f) { MoveToward(u, h->pos, dt); break; }
                    world.stockpile[u->faction][u->carryKind] += u->carryAmount;
                    u->carryAmount = 0;
                    EffectSpawn(FX_FLASH, (Vector3){ h->pos.x, 1.2f, h->pos.z },
                                strategyFactionColor[u->faction]);
                }
                break;
            }

            // 2) Harvesting an existing tended node.
            if (u->targetNode >= 0)
            {
                ResourceNode *n = &world.nodes[u->targetNode];
                if (!n->active) { u->targetNode = -1; break; }
                if (DistXZ(u->pos, n->pos) > 1.0f) { MoveToward(u, n->pos, dt); break; }
                u->gatherTimer += dt;
                if (u->gatherTimer >= u->gatherTime)
                {
                    u->gatherTimer -= u->gatherTime;
                    u->carryKind = NodeResource(n->kind);
                    u->carryAmount++;
                    n->remaining--;
                    EffectSpawn(FX_PUFF, (Vector3){ n->pos.x, 1.0f, n->pos.z },
                                (nk == NODE_TREE) ? BROWN : (Color){ 220, 190, 90, 255 });
                    if (n->remaining <= 0)
                    {
                        n->active = false;
                        u->targetNode = -1;
                        EffectSpawn(FX_RING, n->pos, GRAY);
                    }
                }
                break;
            }

            // 3) Area full of nodes: switch to harvesting the nearest one.
            if (NodesNear(nk, tb->pos, STRAT_TEND_RANGE + 2.0f) >= STRAT_TEND_MAX)
            {
                int near = StrategyNearestNodeOfKind(tb->pos, nk, STRAT_TEND_RANGE + 2.0f);
                if (near >= 0) { u->targetNode = near; u->gatherTimer = 0.0f; break; }
                // (fallthrough to planting if none found somehow)
            }

            // 4) Plant: walk to a chosen free spot, then drop a fresh node.
            //    target == building pos is the "no spot yet" sentinel.
            if (DistXZ(u->target, tb->pos) < 0.001f)
            {
                u->target = PlantSpotNear(tb->pos);
                u->gatherTimer = 0.0f;
            }
            if (DistXZ(u->pos, u->target) > 0.6f) { MoveToward(u, u->target, dt); break; }
            u->gatherTimer += dt;
            if (u->gatherTimer >= STRAT_TEND_PERIOD)
            {
                u->gatherTimer = 0.0f;
                if (PlantSpotClear(u->target))
                    NodeSpawn(nk, u->target, bd->tendAmount);
                EffectSpawn(FX_PUFF, (Vector3){ u->target.x, 0.6f, u->target.z },
                            (nk == NODE_TREE) ? (Color){ 60, 140, 60, 255 }
                                              : (Color){ 220, 190, 90, 255 });
                u->target = tb->pos;    // reset sentinel -> pick a new spot next
            }
        } break;

        case UNIT_BUILD:
        {
            // Walk to the scaffold, then pour build-time into it; HP tracks
            // progress so the site visibly rises. Done -> functional, worker idle.
            Building *b = (u->targetBuilding >= 0)
                ? &world.buildings[u->targetBuilding] : NULL;
            if ((b == NULL) || !b->active || !b->underConstruction)
            {
                u->state = UNIT_IDLE;
                u->targetBuilding = -1;
                break;
            }
            if (DistXZ(u->pos, b->pos) > STRAT_BUILD_RANGE)
            {
                MoveToward(u, b->pos, dt);
                break;
            }
            float buildTime = StrategyBuildingDef(b->kind)->buildTime;
            b->buildProgress += dt;
            b->hp = fminf(b->maxHp, 1.0f + (b->maxHp - 1.0f)*(b->buildProgress/buildTime));
            EffectSpawn(FX_PUFF, (Vector3){ b->pos.x, 0.9f, b->pos.z }, LIGHTGRAY);
            if (b->buildProgress >= buildTime)
            {
                b->underConstruction = false;
                b->hp = b->maxHp;
                EffectSpawn(FX_RING, b->pos, strategyFactionColor[b->faction]);
                u->state = UNIT_IDLE;
                u->targetBuilding = -1;
            }
        } break;

        case UNIT_REPAIR:
        {
            // Restore a damaged own building over time, free. Full HP -> idle.
            Building *b = (u->targetBuilding >= 0)
                ? &world.buildings[u->targetBuilding] : NULL;
            if ((b == NULL) || !b->active || b->underConstruction ||
                b->hp >= b->maxHp)
            {
                u->state = UNIT_IDLE;
                u->targetBuilding = -1;
                break;
            }
            if (DistXZ(u->pos, b->pos) > STRAT_BUILD_RANGE)
            {
                MoveToward(u, b->pos, dt);
                break;
            }
            b->hp = fminf(b->maxHp, b->hp + STRAT_REPAIR_RATE*dt);
            EffectSpawn(FX_PUFF, (Vector3){ b->pos.x, 0.9f, b->pos.z },
                        (Color){ 200, 200, 120, 255 });
            if (b->hp >= b->maxHp)
            {
                u->state = UNIT_IDLE;
                u->targetBuilding = -1;
            }
        } break;

        case UNIT_ATTACK:
        {
            // Two target flavors share the chase/cooldown skeleton: a unit
            // (or animal) via targetUnit, or a building via targetBuilding.
            Vector3 targetPos = u->pos;
            float   range     = u->attackRange;
            bool    valid     = false;

            if (u->targetBuilding >= 0)
            {
                Building *b = &world.buildings[u->targetBuilding];
                valid     = b->active;
                targetPos = b->pos;
                range    += 1.0f;   // buildings are wide - hit from the edge
            }
            else if (u->targetUnit >= 0)
            {
                Unit *victim = &world.units[u->targetUnit];
                valid     = victim->active;
                targetPos = victim->pos;
            }
            if (!valid)
            {
                u->state          = UNIT_IDLE;
                u->targetUnit     = -1;
                u->targetBuilding = -1;
                break;
            }
            float dist = DistXZ(u->pos, targetPos);
            if (dist > range)
            {
                MoveToward(u, targetPos, dt);
                break;
            }
            // Kiting: a ranged unit keeps FIRING but backs off toward its
            // stand-off distance when the target crowds in.
            if (u->preferredRange > 0.0f && dist < u->preferredRange*0.6f)
            {
                Vector3 away = Vector3Subtract(u->pos, targetPos);
                away.y = 0.0f;
                if (Vector3Length(away) > 0.001f)
                {
                    Vector3 dest = Vector3Add(u->pos,
                                              Vector3Scale(Vector3Normalize(away), 2.0f));
                    MoveToward(u, dest, dt);
                }
            }
            u->attackCooldown -= dt;
            if (u->attackCooldown <= 0.0f)
            {
                u->attackCooldown = u->attackPeriod;

                Vector3 from = (Vector3){ u->pos.x, 0.8f, u->pos.z };
                Vector3 to   = (Vector3){ targetPos.x, 0.6f, targetPos.z };
                EffectSpawnBeam(from, to, UnitColor(u));
                EffectSpawn(FX_FLASH, to, RAYWHITE);

                if (u->targetBuilding >= 0)
                {
                    Building *b = &world.buildings[u->targetBuilding];
                    b->hp -= u->damage;
                    if (b->hp <= 0.0f)
                    {
                        BuildingDestroy(u->targetBuilding);
                        u->state          = UNIT_IDLE;
                        u->targetBuilding = -1;
                    }
                }
                else if (UnitDamage(u->targetUnit, u, u->damage))
                {
                    // The kill handler may have re-ordered u (corpse
                    // harvest); only idle it if it is still attacking.
                    if (u->state == UNIT_ATTACK)
                    {
                        u->state      = UNIT_IDLE;
                        u->targetUnit = -1;
                    }
                }
            }
        } break;

        case UNIT_FLEE:
        {
            MoveToward(u, u->target, dt);
            if (DistXZ(u->pos, u->target) < 0.2f) u->state = UNIT_IDLE;
        } break;

        case UNIT_FOLLOW:
        {
            // Templar/healer shadowing its target; gatherTimer paces the
            // bless cadence. The heal/providence effect lands when the
            // blessing STARTS - UNIT_BLESS is just the sparkle pause.
            Unit *t = (u->targetUnit >= 0) ? &world.units[u->targetUnit] : NULL;
            bool healer = (u->kind == KIND_TEMPLAR_HEALER);
            if ((t == NULL) || !t->active || (healer && t->hp >= t->maxHp))
            {
                u->state      = UNIT_IDLE;  // retarget next frame
                u->targetUnit = -1;
                break;
            }
            if (DistXZ(u->pos, t->pos) > 1.5f)
            {
                MoveToward(u, t->pos, dt);
                break;
            }
            u->gatherTimer += dt;
            if (u->gatherTimer >= STRAT_BLESS_PERIOD)
            {
                u->gatherTimer = 0.0f;
                if (healer)
                {
                    if (world.stockpile[u->faction][RES_PROVIDENCE] < STRAT_HEAL_COST)
                        break;      // broke: keep following, try again later
                    world.stockpile[u->faction][RES_PROVIDENCE] -= STRAT_HEAL_COST;
                    t->hp = fminf(t->maxHp, t->hp + STRAT_HEAL_AMOUNT);
                }
                else world.stockpile[u->faction][RES_PROVIDENCE] += 1;

                u->state          = UNIT_BLESS;
                u->attackCooldown = STRAT_BLESS_TIME;
                EffectSpawnBless(t->pos);
            }
        } break;

        case UNIT_BLESS:
        {
            u->attackCooldown -= dt;
            if (u->attackCooldown <= 0.0f) u->state = UNIT_FOLLOW;
        } break;
    }

    // Idle templars pick someone to shadow (their version of auto-aggro).
    if (u->state == UNIT_IDLE &&
        (u->kind == KIND_TEMPLAR || u->kind == KIND_TEMPLAR_HEALER))
    {
        int t = TemplarFindTarget(u);
        if (t >= 0)
        {
            u->state      = UNIT_FOLLOW;
            u->targetUnit = t;
        }
    }

    // Never leave the ground plane.
    u->pos.x = Clamp(u->pos.x, -STRAT_GROUND_HALF, STRAT_GROUND_HALF);
    u->pos.z = Clamp(u->pos.z, -STRAT_GROUND_HALF, STRAT_GROUND_HALF);
}

// Push overlapping units apart so groups spread instead of stacking.
// O(n^2) over 64 units is nothing; no spatial structure needed.
static void UnitSeparation(void)
{
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *a = &world.units[i];
        if (!a->active) continue;

        for (int j = i + 1; j < STRAT_MAX_UNITS; j++)
        {
            Unit *b = &world.units[j];
            if (!b->active) continue;

            float minDist = 2.0f*STRAT_UNIT_RADIUS;
            float d = DistXZ(a->pos, b->pos);
            if (d >= minDist) continue;

            Vector3 push;
            if (d < 0.001f)
            {
                // Exactly stacked: separate along a per-pair fixed direction.
                push = (Vector3){ (i%2 == 0) ? 1.0f : -1.0f, 0.0f, 1.0f };
                push = Vector3Normalize(push);
            }
            else
            {
                push = Vector3Scale(Vector3Subtract(a->pos, b->pos), 1.0f/d);
            }
            float half = (minDist - d)*0.5f;
            a->pos = Vector3Add(a->pos, Vector3Scale(push, half));
            b->pos = Vector3Subtract(b->pos, Vector3Scale(push, half));
        }
    }
}

void StrategyWorldUpdate(float dt)
{
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        if (world.units[i].active) UnitUpdate(i, dt);
    }
    UnitSeparation();
    BuildingsUpdate(dt);

    // Enemy + animal brains live in strategy_ai.c (orders-only).
    world.aiTimer -= dt;
    if (world.aiTimer <= 0.0f)
    {
        world.aiTimer += world.aiPeriod;
        StrategyAiTick();
    }

    EffectsUpdate(dt);
}

// ----------------------------------------------------------------------------
//  Drawing: 3D world
// ----------------------------------------------------------------------------
static void DrawNode(const ResourceNode *n)
{
    switch (n->kind)
    {
        case NODE_TREE:
        {
            DrawCylinder(n->pos, 0.12f, 0.16f, 0.8f, 6, BROWN);
            Vector3 crown = (Vector3){ n->pos.x, 0.8f, n->pos.z };
            DrawCylinder(crown, 0.0f, 0.55f, 1.2f, 6, (Color){ 60, 140, 60, 255 });
        } break;

        case NODE_ROCK:
        {
            Vector3 body = (Vector3){ n->pos.x, 0.3f, n->pos.z };
            DrawCube(body, 0.9f, 0.6f, 0.8f, GRAY);
            DrawCubeWires(body, 0.9f, 0.6f, 0.8f, DARKGRAY);
        } break;

        case NODE_WHEAT:
        {
            // A patch of thin golden stalks.
            Color wheat = (Color){ 220, 190, 90, 255 };
            for (int i = 0; i < 4; i++)
            {
                Vector3 stalk = n->pos;
                stalk.x += ((float)(i%2) - 0.5f)*0.5f;
                stalk.z += ((float)(i/2) - 0.5f)*0.5f;
                DrawCylinder(stalk, 0.02f, 0.08f, 0.7f, 5, wheat);
            }
        } break;

        case NODE_CORPSE:
        {
            Vector3 body = (Vector3){ n->pos.x, 0.12f, n->pos.z };
            DrawCube(body, 0.7f, 0.24f, 0.5f, (Color){ 140, 60, 50, 255 });
        } break;
    }
}

static void DrawBuilding(BuildingKind kind, int faction, Vector3 pos, Color tint)
{
    switch (kind)
    {
        case BLD_HOUSE:
        {
            Vector3 body = (Vector3){ pos.x, 0.6f, pos.z };
            DrawCube(body, 1.4f, 1.2f, 1.4f, tint);
            Vector3 roofBase = (Vector3){ pos.x, 1.2f, pos.z };
            DrawCylinder(roofBase, 0.0f, 1.1f, 0.8f, 4,
                         Fade(strategyFactionColor[faction], tint.a/255.0f));
        } break;

        case BLD_LOGGING:
        {
            Vector3 body = (Vector3){ pos.x, 0.3f, pos.z };
            DrawCube(body, 1.8f, 0.6f, 1.3f, tint);
            // A "log" on top marks the wood dropoff.
            Vector3 log = (Vector3){ pos.x, 0.75f, pos.z };
            DrawCylinderEx((Vector3){ log.x - 0.7f, log.y, log.z },
                           (Vector3){ log.x + 0.7f, log.y, log.z },
                           0.18f, 0.18f, 8, Fade(BROWN, tint.a/255.0f));
        } break;

        case BLD_QUARRY:
        {
            Vector3 body = (Vector3){ pos.x, 0.25f, pos.z };
            DrawCube(body, 1.6f, 0.5f, 1.6f, tint);
            Vector3 block = (Vector3){ pos.x, 0.75f, pos.z };
            DrawCube(block, 0.6f, 0.5f, 0.6f, Fade(DARKGRAY, tint.a/255.0f));
        } break;

        case BLD_BARRACKS:
        {
            Vector3 body = (Vector3){ pos.x, 0.5f, pos.z };
            DrawCube(body, 2.0f, 1.0f, 1.4f, tint);
            Vector3 roof = (Vector3){ pos.x, 1.15f, pos.z };
            DrawCube(roof, 2.2f, 0.3f, 1.6f, Fade(DARKGRAY, tint.a/255.0f));
        } break;

        case BLD_FARM:
        {
            // Flat tilled pad with a few wheat posts.
            Vector3 pad = (Vector3){ pos.x, 0.08f, pos.z };
            DrawCube(pad, 2.0f, 0.16f, 2.0f, Fade((Color){ 150, 110, 60, 255 }, tint.a/255.0f));
            Color wheat = Fade((Color){ 220, 190, 90, 255 }, tint.a/255.0f);
            for (int i = 0; i < 4; i++)
            {
                Vector3 stalk = pos;
                stalk.x += ((float)(i%2) - 0.5f)*1.0f;
                stalk.z += ((float)(i/2) - 0.5f)*1.0f;
                DrawCylinder(stalk, 0.02f, 0.07f, 0.6f, 5, wheat);
            }
        } break;

        case BLD_TOWN_HALL:
        {
            // The biggest footprint on the map, with a faction-colored keep.
            Vector3 body = (Vector3){ pos.x, 0.7f, pos.z };
            DrawCube(body, 2.4f, 1.4f, 2.4f, tint);
            Vector3 keep = (Vector3){ pos.x, 1.7f, pos.z };
            DrawCube(keep, 1.2f, 0.6f, 1.2f,
                     Fade(strategyFactionColor[faction], tint.a/255.0f));
        } break;

        case BLD_CHANTRY:
        {
            // Pale tower with a gold spire.
            Vector3 body = (Vector3){ pos.x, 0.8f, pos.z };
            DrawCube(body, 1.2f, 1.6f, 1.2f, tint);
            Vector3 spire = (Vector3){ pos.x, 1.6f, pos.z };
            DrawCylinder(spire, 0.0f, 0.5f, 1.0f, 6, Fade(GOLD, tint.a/255.0f));
        } break;

        case BLD_FORESTRY:
        {
            // Low hut with a couple of little saplings sprouting on top.
            Vector3 body = (Vector3){ pos.x, 0.35f, pos.z };
            DrawCube(body, 1.4f, 0.7f, 1.4f, tint);
            Color leaf = Fade((Color){ 60, 140, 60, 255 }, tint.a/255.0f);
            for (int i = 0; i < 2; i++)
            {
                Vector3 sap = (Vector3){ pos.x + (i ? 0.4f : -0.4f), 0.75f, pos.z };
                DrawCylinder(sap, 0.0f, 0.28f, 0.7f, 6, leaf);
            }
        } break;

        default: break;
    }

    // Faction banner: a small colored post so ownership reads at a glance.
    Vector3 postTop = (Vector3){ pos.x + 0.8f, 1.6f, pos.z + 0.8f };
    Vector3 postBot = (Vector3){ pos.x + 0.8f, 0.0f, pos.z + 0.8f };
    DrawLine3D(postBot, postTop, Fade(strategyFactionColor[faction], tint.a/255.0f));
    DrawCube(postTop, 0.25f, 0.18f, 0.05f, Fade(strategyFactionColor[faction], tint.a/255.0f));
}

static void DrawUnit(const Unit *u)
{
    Color color = UnitColor(u);

    if (u->selected)
    {
        Vector3 ring = (Vector3){ u->pos.x, 0.02f, u->pos.z };
        DrawCircle3D(ring, 0.55f, (Vector3){ 1.0f, 0.0f, 0.0f }, 90.0f, GREEN);
    }

    switch (u->kind)
    {
        case KIND_WORKER:
        {
            DrawCylinder(u->pos, 0.28f, STRAT_UNIT_RADIUS, 0.8f, 8, color);
            Vector3 head = (Vector3){ u->pos.x, 0.95f, u->pos.z };
            DrawSphere(head, 0.18f, ColorBrightness(color, 0.3f));

            // Tending workers wear a "hat" shaped like the resource they plant:
            // a green cone (forestry -> wood) or a golden cone (farm -> wheat).
            if (u->state == UNIT_FARM && u->targetBuilding >= 0)
            {
                const BuildingDef *tb =
                    StrategyBuildingDef(world.buildings[u->targetBuilding].kind);
                if (tb->tendNode >= 0)
                {
                    Color hat = (tb->tendNode == NODE_TREE)
                        ? (Color){ 60, 140, 60, 255 } : (Color){ 220, 190, 90, 255 };
                    Vector3 cap = (Vector3){ u->pos.x, 1.12f, u->pos.z };
                    DrawCylinder(cap, 0.0f, 0.16f, 0.28f, 6, hat);
                }
            }
        } break;

        case KIND_SOLDIER:
        {
            // Taller, wider, darker - reads as "military" at a glance.
            Color dark = ColorBrightness(color, -0.25f);
            DrawCylinder(u->pos, 0.34f, 0.45f, 1.1f, 8, dark);
            Vector3 head = (Vector3){ u->pos.x, 1.28f, u->pos.z };
            DrawSphere(head, 0.2f, color);
        } break;

        case KIND_RANGED:
        {
            // Slighter than the soldier, with a "bow" post at the side.
            Color light = ColorBrightness(color, 0.15f);
            DrawCylinder(u->pos, 0.30f, 0.40f, 1.0f, 8, light);
            Vector3 head = (Vector3){ u->pos.x, 1.18f, u->pos.z };
            DrawSphere(head, 0.18f, color);
            DrawLine3D((Vector3){ u->pos.x + 0.35f, 0.25f, u->pos.z },
                       (Vector3){ u->pos.x + 0.35f, 1.05f, u->pos.z }, DARKBROWN);
        } break;

        case KIND_TEMPLAR:
        case KIND_TEMPLAR_HEALER:
        {
            // White robe, gold (templar) or lime (healer) head, faction band.
            Color halo = (u->kind == KIND_TEMPLAR) ? GOLD : LIME;
            DrawCylinder(u->pos, 0.26f, 0.42f, 1.0f, 8, RAYWHITE);
            Vector3 band = (Vector3){ u->pos.x, 0.55f, u->pos.z };
            DrawCube(band, 0.5f, 0.12f, 0.5f, color);
            Vector3 head = (Vector3){ u->pos.x, 1.16f, u->pos.z };
            DrawSphere(head, 0.17f, halo);
        } break;

        case KIND_ANIMAL_WEAK:
        {
            // Small low critter, no head sphere.
            Vector3 body = (Vector3){ u->pos.x, 0.22f, u->pos.z };
            DrawCube(body, 0.55f, 0.4f, 0.35f, color);
        } break;

        case KIND_ANIMAL_STRONG:
        {
            // Bigger, darker beast - reads as "don't poke it".
            Vector3 body = (Vector3){ u->pos.x, 0.34f, u->pos.z };
            DrawCube(body, 0.9f, 0.65f, 0.55f, ColorBrightness(color, -0.35f));
            DrawCubeWires(body, 0.9f, 0.65f, 0.55f, DARKBROWN);
        } break;

        default: break;
    }

    // Carried resources float above the head, tinted by kind.
    if (u->carryAmount > 0)
    {
        Color carry = (u->carryKind == RES_WOOD)  ? BROWN
                    : (u->carryKind == RES_STONE) ? GRAY
                    : (Color){ 220, 190, 90, 255 };
        Vector3 pack = (Vector3){ u->pos.x, 1.35f, u->pos.z };
        DrawCube(pack, 0.2f, 0.2f, 0.2f, carry);
    }
}

void StrategyWorldDraw3D(void)
{
    BeginMode3D(world.camera);

    // Plane slightly below the grid lines to avoid z-fighting.
    DrawPlane((Vector3){ 0.0f, -0.01f, 0.0f },
              (Vector2){ 2.0f*STRAT_GROUND_HALF, 2.0f*STRAT_GROUND_HALF },
              (Color){ 90, 110, 80, 255 });
    DrawGrid((int)(2.0f*STRAT_GROUND_HALF), 1.0f);

    for (int i = 0; i < STRAT_MAX_NODES; i++)
    {
        if (world.nodes[i].active) DrawNode(&world.nodes[i]);
    }
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        Building *b = &world.buildings[i];
        if (!b->active) continue;

        // A scaffold reads as a faint, translucent version of the finished
        // building wrapped in a wireframe frame so it's clearly "not done".
        Color tint = b->underConstruction ? Fade(BEIGE, 0.35f) : BEIGE;
        DrawBuilding(b->kind, b->faction, b->pos, tint);
        if (b->underConstruction)
        {
            Vector3 frame = (Vector3){ b->pos.x, 0.9f, b->pos.z };
            DrawCubeWires(frame, 2.0f, 1.8f, 2.0f, Fade(BROWN, 0.8f));
        }
        if (i == world.selectedBuilding)
        {
            Vector3 ring = (Vector3){ b->pos.x, 0.02f, b->pos.z };
            DrawCircle3D(ring, 1.5f, (Vector3){ 1.0f, 0.0f, 0.0f }, 90.0f, GREEN);

            // Rally flag for the selected training building.
            if (b->hasRally)
            {
                Vector3 top = (Vector3){ b->rally.x, 1.4f, b->rally.z };
                Vector3 bot = (Vector3){ b->rally.x, 0.0f, b->rally.z };
                DrawLine3D(bot, top, GREEN);
                DrawCube((Vector3){ b->rally.x + 0.2f, 1.25f, b->rally.z },
                         0.4f, 0.25f, 0.05f, GREEN);
                DrawCircle3D((Vector3){ b->rally.x, 0.02f, b->rally.z }, 0.5f,
                             (Vector3){ 1.0f, 0.0f, 0.0f }, 90.0f, Fade(GREEN, 0.6f));
            }
        }
    }
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        if (world.units[i].active) DrawUnit(&world.units[i]);
    }

    // Placement ghost: green when the spot is valid, red when not.
    if (world.placing >= 0)
    {
        Vector3 ghost;
        if (PlacementGhost(&ghost))
        {
            bool valid = PlacementValid(0, (BuildingKind)world.placing, ghost);
            Color tint = Fade(valid ? GREEN : RED, 0.5f);
            DrawBuilding((BuildingKind)world.placing, 0, ghost, tint);
        }
    }

    EffectsDraw3D();
    EndMode3D();
}

// ----------------------------------------------------------------------------
//  Drawing: 2D overlay in GAME-CANVAS space (drawn after EndMode3D)
// ----------------------------------------------------------------------------
void StrategyWorldDraw2DOverlay(void)
{
    Vector2 gameSize = ScreenStateTargetSize();

    // Drag-selection rectangle.
    if (world.dragging)
    {
        Vector2 mouse = MouseGame();
        Rectangle rect = {
            fminf(world.dragStart.x, mouse.x), fminf(world.dragStart.y, mouse.y),
            fabsf(mouse.x - world.dragStart.x), fabsf(mouse.y - world.dragStart.y),
        };
        DrawRectangleRec(rect, Fade(GREEN, 0.12f));
        DrawRectangleLinesEx(rect, 1.0f, GREEN);
    }

    // HP bars over damaged units.
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world.units[i];
        if (!u->active || u->hp >= u->maxHp) continue;

        Vector2 sp = WorldToGame((Vector3){ u->pos.x, 1.5f, u->pos.z });
        float frac = u->hp/u->maxHp;
        DrawRectangle((int)sp.x - 12, (int)sp.y, 24, 4, Fade(RED, 0.8f));
        DrawRectangle((int)sp.x - 12, (int)sp.y, (int)(24.0f*frac), 4, GREEN);
    }

    // HP bars over damaged buildings (wider).
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        Building *b = &world.buildings[i];
        if (!b->active || b->hp >= b->maxHp) continue;

        Vector2 sp = WorldToGame((Vector3){ b->pos.x, 2.2f, b->pos.z });
        float frac = b->hp/b->maxHp;
        DrawRectangle((int)sp.x - 20, (int)sp.y, 40, 5, Fade(RED, 0.8f));
        DrawRectangle((int)sp.x - 20, (int)sp.y, (int)(40.0f*frac), 5, GREEN);
    }

    // Resource HUD: player big, enemy small below (visible AI progress).
    int size = (int)fmaxf(10.0f, gameSize.y*0.045f);
    DrawText(TextFormat("WOOD %d   STONE %d   FOOD %d   PROV %d   POP %d/%d",
                        world.stockpile[0][RES_WOOD], world.stockpile[0][RES_STONE],
                        world.stockpile[0][RES_FOOD], world.stockpile[0][RES_PROVIDENCE],
                        StrategyPopUsed(0), StrategyPopCap(0)),
             (int)(gameSize.x*0.02f), (int)(gameSize.y*0.02f), size, RAYWHITE);
    DrawText(TextFormat("enemy: wood %d stone %d food %d prov %d pop %d/%d",
                        world.stockpile[1][RES_WOOD], world.stockpile[1][RES_STONE],
                        world.stockpile[1][RES_FOOD], world.stockpile[1][RES_PROVIDENCE],
                        StrategyPopUsed(1), StrategyPopCap(1)),
             (int)(gameSize.x*0.02f), (int)(gameSize.y*0.02f) + size + 4, size/2,
             Fade(strategyFactionColor[1], 0.8f));

    if (world.placing >= 0)
    {
        const char *hint = "LMB place - Shift+LMB place more - RMB/ESC cancel";
        DrawText(hint, (int)(gameSize.x*0.5f - (float)MeasureText(hint, size/2)*0.5f),
                 (int)(gameSize.y*0.9f), size/2, RAYWHITE);
    }

    // Victory/defeat banner: the sim keeps running underneath, R restarts.
    if (world.gameOver >= 0)
    {
        const char *msg = (world.gameOver == 0) ? "VICTORY" : "DEFEAT";
        Color tint = (world.gameOver == 0) ? GOLD : RED;
        int bigSize = (int)(gameSize.y*0.14f);
        DrawText(msg, (int)(gameSize.x*0.5f - (float)MeasureText(msg, bigSize)*0.5f),
                 (int)(gameSize.y*0.36f), bigSize, tint);
        const char *why = (world.gameOver == 0)
            ? "the enemy lost every critical building and worker"
            : "you lost every critical building and worker";
        DrawText(why, (int)(gameSize.x*0.5f - (float)MeasureText(why, size/2)*0.5f),
                 (int)(gameSize.y*0.36f) + bigSize + 8, size/2, LIGHTGRAY);
        const char *sub = "press R to restart";
        DrawText(sub, (int)(gameSize.x*0.5f - (float)MeasureText(sub, size)*0.5f),
                 (int)(gameSize.y*0.36f) + bigSize + size/2 + 16, size, RAYWHITE);
    }
}
