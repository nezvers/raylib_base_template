// ============================================================================
//  strategy_types.h  -  data model for the strategy (RTS) test state
//
//  Everything lives in fixed-size arrays inside one StrategyWorld singleton
//  (no malloc). A slot is "alive" when its .active flag is set; freeing is
//  just clearing the flag. Positions are Vector3 on the y = 0 ground plane.
// ============================================================================

#ifndef STRATEGY_TYPES_H
#define STRATEGY_TYPES_H

#include "raylib.h"
#include <stdbool.h>

// -- Capacities --------------------------------------------------------------
#define STRAT_MAX_UNITS      64
#define STRAT_MAX_BUILDINGS  16
#define STRAT_MAX_NODES      32
#define STRAT_FACTIONS       2      // 0 = player (blue), 1 = enemy (red)

// -- Tuning ------------------------------------------------------------------
#define STRAT_GROUND_HALF    25.0f  // ground spans [-HALF, +HALF] on x and z
#define STRAT_UNIT_SPEED     4.0f   // world units per second
#define STRAT_UNIT_RADIUS    0.35f
#define STRAT_UNIT_HP        30.0f
#define STRAT_ATTACK_RANGE   1.2f
#define STRAT_ATTACK_DAMAGE  10.0f
#define STRAT_ATTACK_PERIOD  1.0f   // seconds between hits
#define STRAT_SIGHT_RANGE    6.0f   // auto-aggro scan radius
#define STRAT_CARRY_MAX      5      // resource units carried before returning
#define STRAT_GATHER_TIME    0.8f   // seconds per chop/mine tick
#define STRAT_AI_PERIOD      1.0f   // seconds between enemy "think" ticks

typedef enum {
    RES_WOOD = 0,
    RES_STONE,
    RES_COUNT
} ResourceKind;

typedef enum {
    NODE_TREE = 0,      // yields RES_WOOD
    NODE_ROCK,          // yields RES_STONE
} NodeKind;

typedef enum {
    BLD_HOUSE = 0,
    BLD_LOGGING,
    BLD_QUARRY,
    BLD_COUNT
} BuildingKind;

typedef enum {
    UNIT_IDLE = 0,
    UNIT_MOVE,          // walking to .target
    UNIT_GATHER,        // walking to / working .targetNode
    UNIT_RETURN,        // carrying resources to the nearest own building
    UNIT_ATTACK,        // chasing / hitting .targetUnit
} UnitState;

typedef struct {
    bool         active;
    int          faction;
    Vector3      pos;
    Vector3      target;            // move destination (UNIT_MOVE)
    UnitState    state;
    float        hp, maxHp;
    float        attackCooldown;    // seconds until the next hit is allowed
    int          targetUnit;        // units[] index while UNIT_ATTACK (-1 = none)
    int          targetNode;        // nodes[] index while UNIT_GATHER (-1 = none)
    int          carryAmount;       // 0..STRAT_CARRY_MAX
    ResourceKind carryKind;
    float        gatherTimer;       // accumulates toward STRAT_GATHER_TIME
    bool         selected;          // player faction only
} Unit;

typedef struct {
    bool         active;
    BuildingKind kind;
    int          faction;
    Vector3      pos;
    float        hp, maxHp;
} Building;

typedef struct {
    bool     active;
    NodeKind kind;
    Vector3  pos;
    int      remaining;             // resource units left; 0 -> despawn
} ResourceNode;

typedef struct {
    Unit         units[STRAT_MAX_UNITS];
    Building     buildings[STRAT_MAX_BUILDINGS];
    ResourceNode nodes[STRAT_MAX_NODES];
    int          stockpile[STRAT_FACTIONS][RES_COUNT];

    // Camera: fixed-pitch RTS view. The camera is DERIVED every frame from
    // focus + zoom, so panning/zooming only touch these two fields.
    Camera3D camera;
    Vector2  camFocus;              // ground point the camera looks at (x, z)
    float    camZoom;               // scales the fixed offset (clamped)

    // Input / UI state
    bool      dragging;             // LMB held and moved past the drag threshold
    Vector2   dragStart;            // game-canvas pixels (Screen2Target space)
    int       placing;              // BuildingKind ghost being placed, -1 = none
    float     aiTimer;              // countdown to the next enemy think tick
    Rectangle guiBlock;             // REAL-screen px area where the GUI owns the
                                    //   mouse (build bar); world clicks ignore it
} StrategyWorld;

#endif // STRATEGY_TYPES_H
