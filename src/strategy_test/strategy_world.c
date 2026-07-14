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
#include "raymath.h"
#include <math.h>
#include <stddef.h>

// Build costs, indexed [BuildingKind][ResourceKind] (wood, stone).
const int strategyBuildingCost[BLD_COUNT][RES_COUNT] = {
    { 5, 3 },   // BLD_HOUSE
    { 3, 0 },   // BLD_LOGGING
    { 2, 2 },   // BLD_QUARRY
};

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
    return (kind == NODE_TREE) ? RES_WOOD : RES_STONE;
}

// ----------------------------------------------------------------------------
//  Spawning
// ----------------------------------------------------------------------------
static Unit *UnitSpawn(int faction, Vector3 pos)
{
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world.units[i];
        if (u->active) continue;

        *u = (Unit){ 0 };
        u->active     = true;
        u->faction    = faction;
        u->pos        = pos;
        u->target     = pos;
        u->state      = UNIT_IDLE;
        u->maxHp      = STRAT_UNIT_HP;
        u->hp         = STRAT_UNIT_HP;
        u->targetUnit = -1;
        u->targetNode = -1;
        return u;
    }
    return NULL;
}

static Building *BuildingSpawn(BuildingKind kind, int faction, Vector3 pos)
{
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        Building *b = &world.buildings[i];
        if (b->active) continue;

        *b = (Building){ 0 };
        b->active  = true;
        b->kind    = kind;
        b->faction = faction;
        b->pos     = pos;
        b->maxHp   = 100.0f;
        b->hp      = 100.0f;
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
    world.placing = -1;
    world.aiTimer = STRAT_AI_PERIOD;
    EffectsReset();

    // Camera: perspective, fixed pitch; focus starts over the player base.
    world.camera.up         = (Vector3){ 0.0f, 1.0f, 0.0f };
    world.camera.fovy       = 45.0f;
    world.camera.projection = CAMERA_PERSPECTIVE;
    world.camFocus = (Vector2){ -14.0f, -12.0f };
    world.camZoom  = 1.0f;
    CameraRefresh();

    // Two rival bases in opposite corners, 6 units each in a loose row.
    BuildingSpawn(BLD_HOUSE, 0, (Vector3){ -14.0f, 0.0f, -14.0f });
    BuildingSpawn(BLD_HOUSE, 1, (Vector3){  14.0f, 0.0f,  14.0f });
    for (int i = 0; i < 6; i++)
    {
        UnitSpawn(0, (Vector3){ -14.0f + (float)i*1.2f - 3.0f, 0.0f, -11.0f });
        UnitSpawn(1, (Vector3){  14.0f - (float)i*1.2f + 3.0f, 0.0f,  11.0f });
    }

    // Resources scattered between the bases.
    NodeCluster(NODE_TREE, (Vector3){ -7.0f, 0.0f,  -3.0f }, 6, 2.5f, 12);
    NodeCluster(NODE_TREE, (Vector3){  6.0f, 0.0f,   9.0f }, 6, 2.5f, 12);
    NodeCluster(NODE_ROCK, (Vector3){ -3.0f, 0.0f,   8.0f }, 5, 2.0f, 10);
    NodeCluster(NODE_ROCK, (Vector3){  9.0f, 0.0f,  -7.0f }, 5, 2.0f, 10);

    // Starting stockpiles so building is possible right away.
    for (int f = 0; f < STRAT_FACTIONS; f++)
    {
        world.stockpile[f][RES_WOOD]  = 10;
        world.stockpile[f][RES_STONE] = 10;
    }
}

// ----------------------------------------------------------------------------
//  Orders: the ONLY way anything (mouse or AI) makes a unit act.
// ----------------------------------------------------------------------------
static void OrderMove(Unit *u, Vector3 dest)
{
    u->state      = UNIT_MOVE;
    u->target     = dest;
    u->targetUnit = -1;
    u->targetNode = -1;
}

static void OrderGather(Unit *u, int nodeIndex)
{
    u->state       = UNIT_GATHER;
    u->targetNode  = nodeIndex;
    u->targetUnit  = -1;
    u->gatherTimer = 0.0f;
}

static void OrderAttack(Unit *u, int unitIndex)
{
    u->state          = UNIT_ATTACK;
    u->targetUnit     = unitIndex;
    u->targetNode     = -1;
    u->attackCooldown = 0.0f;
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
static bool PlacementValid(BuildingKind kind, Vector3 pos)
{
    for (int r = 0; r < RES_COUNT; r++)
    {
        if (world.stockpile[0][r] < strategyBuildingCost[kind][r]) return false;
    }
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
    if (!PlacementGhost(&pos) || !PlacementValid(kind, pos)) return;

    for (int r = 0; r < RES_COUNT; r++)
    {
        world.stockpile[0][r] -= strategyBuildingCost[kind][r];
    }
    BuildingSpawn(kind, 0, pos);
    EffectSpawn(FX_RING, pos, RAYWHITE);
    for (int i = 0; i < 3; i++) EffectSpawn(FX_PUFF, (Vector3){ pos.x, 0.8f, pos.z }, LIGHTGRAY);
    world.placing = -1;
}

static void SelectionInput(void)
{
    Vector2 mouse = MouseGame();
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        if (MouseOnGui()) return;   // the build bar owns this click
        world.dragStart = mouse;
        world.dragging  = false;
    }
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !world.dragging)
    {
        if (Vector2Distance(mouse, world.dragStart) > 6.0f) world.dragging = true;
    }
    if (!IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) return;

    if (world.dragging)
    {
        // Box select: player units whose SCREEN projection is inside the rect.
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

    // Plain click: select the player unit under the cursor (or deselect all).
    Vector3 ground;
    if (!MouseGroundPoint(&ground)) return;
    int hit = PickUnit(ground, 0, 0.7f);

    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world.units[i];
        if (!u->active || u->faction != 0) continue;
        if (i == hit)    u->selected = true;
        else if (!shift) u->selected = false;
    }
    if (hit >= 0) EffectSpawn(FX_RING, world.units[hit].pos, GREEN);
}

// Right click: attack an enemy, gather a node, or move - checked in that
// priority so a click near a tree still prefers a unit standing beside it.
static void OrderInput(void)
{
    if (!IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) || MouseOnGui()) return;

    Vector3 ground;
    if (!MouseGroundPoint(&ground)) return;

    int enemy = PickUnit(ground, 1, 0.8f);
    int node  = (enemy < 0) ? PickNode(ground, 0.9f) : -1;

    bool any = false;
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world.units[i];
        if (!u->active || u->faction != 0 || !u->selected) continue;

        if (enemy >= 0)     OrderAttack(u, enemy);
        else if (node >= 0) OrderGather(u, node);
        else                OrderMove(u, ground);
        any = true;
    }
    if (!any) return;

    // Order feedback: red ring on an attack target, yellow on a resource,
    // lime ripple on plain ground.
    if (enemy >= 0)     EffectSpawn(FX_RING, world.units[enemy].pos, strategyFactionColor[1]);
    else if (node >= 0) EffectSpawn(FX_RING, world.nodes[node].pos, YELLOW);
    else                EffectSpawn(FX_RING, ground, LIME);
}

void StrategyWorldHandleInput(void)
{
    CameraPanZoom();

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

    float step = STRAT_UNIT_SPEED*dt;
    if (step > dist) step = dist;
    u->pos = Vector3Add(u->pos, Vector3Scale(delta, step/dist));
}

static int NearestOwnBuilding(const Unit *u)
{
    int best = -1;
    float bestDist = 1000000.0f;
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        Building *b = &world.buildings[i];
        if (!b->active || b->faction != u->faction) continue;

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

        float d = DistXZ(other->pos, u->pos);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

static void UnitKill(int index)
{
    Unit *u = &world.units[index];
    u->active   = false;
    u->selected = false;

    EffectSpawn(FX_RING, u->pos, strategyFactionColor[u->faction]);
    for (int i = 0; i < 4; i++)
    {
        EffectSpawn(FX_PUFF, (Vector3){ u->pos.x, 0.5f, u->pos.z }, GRAY);
    }
}

// Auto-aggro: idle units of BOTH factions engage hostiles on sight; enemy
// units also break off moving/working (the player keeps manual control).
static void UnitAggroScan(Unit *u)
{
    bool scan = (u->state == UNIT_IDLE) ||
                (u->faction == 1 && (u->state == UNIT_MOVE ||
                                     u->state == UNIT_GATHER ||
                                     u->state == UNIT_RETURN));
    if (!scan) return;

    int hostile = NearestHostile(u, STRAT_SIGHT_RANGE);
    if (hostile >= 0) OrderAttack(u, hostile);
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
                u->state = (u->carryAmount > 0) ? UNIT_RETURN : UNIT_IDLE;
                break;
            }
            if (DistXZ(u->pos, n->pos) > 1.0f)
            {
                MoveToward(u, n->pos, dt);
                break;
            }
            // In working range: one resource unit per GATHER_TIME tick.
            u->gatherTimer += dt;
            if (u->gatherTimer >= STRAT_GATHER_TIME)
            {
                u->gatherTimer -= STRAT_GATHER_TIME;
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
            int home = NearestOwnBuilding(u);
            if (home < 0)
            {
                u->carryAmount = 0;     // nowhere to drop off
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

            // Keep the loop going while the node is alive.
            bool nodeAlive = (u->targetNode >= 0) && world.nodes[u->targetNode].active;
            u->state = nodeAlive ? UNIT_GATHER : UNIT_IDLE;
        } break;

        case UNIT_ATTACK:
        {
            Unit *victim = (u->targetUnit >= 0) ? &world.units[u->targetUnit] : NULL;
            if ((victim == NULL) || !victim->active)
            {
                u->state = UNIT_IDLE;
                u->targetUnit = -1;
                break;
            }
            if (DistXZ(u->pos, victim->pos) > STRAT_ATTACK_RANGE)
            {
                MoveToward(u, victim->pos, dt);
                break;
            }
            u->attackCooldown -= dt;
            if (u->attackCooldown <= 0.0f)
            {
                u->attackCooldown = STRAT_ATTACK_PERIOD;
                victim->hp -= STRAT_ATTACK_DAMAGE;

                Vector3 from = (Vector3){ u->pos.x, 0.8f, u->pos.z };
                Vector3 to   = (Vector3){ victim->pos.x, 0.6f, victim->pos.z };
                EffectSpawnBeam(from, to, strategyFactionColor[u->faction]);
                EffectSpawn(FX_FLASH, to, RAYWHITE);

                if (victim->hp <= 0.0f)
                {
                    UnitKill(u->targetUnit);
                    u->state = UNIT_IDLE;
                    u->targetUnit = -1;
                }
            }
        } break;
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

// ----------------------------------------------------------------------------
//  Enemy AI: runs once per STRAT_AI_PERIOD. Only ISSUES ORDERS - movement,
//  gathering and combat are the shared unit code above. Auto-aggro (see
//  UnitAggroScan) already covers "attack player units on sight".
// ----------------------------------------------------------------------------
static void EnemyAiTick(void)
{
    // The enemy "home" anchor for wandering: its first standing building.
    Vector3 home = (Vector3){ 14.0f, 0.0f, 14.0f };
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        Building *b = &world.buildings[i];
        if (b->active && b->faction == 1)
        {
            home = b->pos;
            break;
        }
    }

    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world.units[i];
        if (!u->active || u->faction != 1 || u->state != UNIT_IDLE) continue;

        if (GetRandomValue(0, 99) < 60)
        {
            // Gather the nearest resource node.
            int best = -1;
            float bestDist = 1000000.0f;
            for (int n = 0; n < STRAT_MAX_NODES; n++)
            {
                if (!world.nodes[n].active) continue;
                float d = DistXZ(world.nodes[n].pos, u->pos);
                if (d < bestDist)
                {
                    bestDist = d;
                    best = n;
                }
            }
            if (best >= 0) OrderGather(u, best);
        }
        else
        {
            // Wander somewhere near home.
            Vector3 dest = home;
            dest.x += (float)GetRandomValue(-600, 600)*0.01f;
            dest.z += (float)GetRandomValue(-600, 600)*0.01f;
            OrderMove(u, dest);
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

    world.aiTimer -= dt;
    if (world.aiTimer <= 0.0f)
    {
        world.aiTimer += STRAT_AI_PERIOD;
        EnemyAiTick();
    }

    EffectsUpdate(dt);
}

// ----------------------------------------------------------------------------
//  Drawing: 3D world
// ----------------------------------------------------------------------------
static void DrawNode(const ResourceNode *n)
{
    if (n->kind == NODE_TREE)
    {
        DrawCylinder(n->pos, 0.12f, 0.16f, 0.8f, 6, BROWN);
        Vector3 crown = (Vector3){ n->pos.x, 0.8f, n->pos.z };
        DrawCylinder(crown, 0.0f, 0.55f, 1.2f, 6, (Color){ 60, 140, 60, 255 });
    }
    else
    {
        Vector3 body = (Vector3){ n->pos.x, 0.3f, n->pos.z };
        DrawCube(body, 0.9f, 0.6f, 0.8f, GRAY);
        DrawCubeWires(body, 0.9f, 0.6f, 0.8f, DARKGRAY);
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
    Color color = strategyFactionColor[u->faction];

    if (u->selected)
    {
        Vector3 ring = (Vector3){ u->pos.x, 0.02f, u->pos.z };
        DrawCircle3D(ring, 0.55f, (Vector3){ 1.0f, 0.0f, 0.0f }, 90.0f, GREEN);
    }

    DrawCylinder(u->pos, 0.28f, STRAT_UNIT_RADIUS, 0.8f, 8, color);
    Vector3 head = (Vector3){ u->pos.x, 0.95f, u->pos.z };
    DrawSphere(head, 0.18f, ColorBrightness(color, 0.3f));

    // Carried resources float above the head, tinted by kind.
    if (u->carryAmount > 0)
    {
        Color carry = (u->carryKind == RES_WOOD) ? BROWN : GRAY;
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
        if (b->active) DrawBuilding(b->kind, b->faction, b->pos, BEIGE);
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
            bool valid = PlacementValid((BuildingKind)world.placing, ghost);
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

    // Resource HUD: player big, enemy small below (visible AI progress).
    int size = (int)fmaxf(10.0f, gameSize.y*0.045f);
    DrawText(TextFormat("WOOD %d   STONE %d",
                        world.stockpile[0][RES_WOOD], world.stockpile[0][RES_STONE]),
             (int)(gameSize.x*0.02f), (int)(gameSize.y*0.02f), size, RAYWHITE);
    DrawText(TextFormat("enemy: wood %d stone %d",
                        world.stockpile[1][RES_WOOD], world.stockpile[1][RES_STONE]),
             (int)(gameSize.x*0.02f), (int)(gameSize.y*0.02f) + size + 4, size/2,
             Fade(strategyFactionColor[1], 0.8f));

    if (world.placing >= 0)
    {
        const char *hint = "LMB place - RMB/ESC cancel";
        DrawText(hint, (int)(gameSize.x*0.5f - (float)MeasureText(hint, size/2)*0.5f),
                 (int)(gameSize.y*0.9f), size/2, RAYWHITE);
    }
}
