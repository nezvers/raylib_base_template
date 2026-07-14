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
#define STRAT_MAX_NODES      48
#define STRAT_FACTIONS       2      // 0 = player (blue), 1 = enemy (red)
#define FACTION_NEUTRAL      2      // animals: no stockpile, no color entry -
                                    //   always guard before indexing by faction

// -- Tuning ------------------------------------------------------------------
#define STRAT_GROUND_HALF    25.0f  // ground spans [-HALF, +HALF] on x and z
#define STRAT_UNIT_SPEED     4.0f   // world units per second
#define STRAT_UNIT_RADIUS    0.35f
#define STRAT_ATTACK_RANGE   1.2f
#define STRAT_ATTACK_PERIOD  1.0f   // seconds between hits
#define STRAT_SIGHT_RANGE    6.0f   // auto-aggro scan radius
#define STRAT_CARRY_MAX      5      // resource units carried before returning
#define STRAT_GATHER_TIME    0.8f   // seconds per chop/mine tick
#define STRAT_AI_PERIOD      1.0f   // seconds between enemy "think" ticks
#define STRAT_FARM_PERIOD    2.0f   // seconds per food unit while farming
#define STRAT_POP_PER_HOUSE  4      // each standing house adds this much pop cap
#define STRAT_RETARGET_RADIUS 10.0f // depleted node: search for the next one here
#define STRAT_ANIMAL_COUNT   6      // neutral critters spawned at init
#define STRAT_AI_ATTACK_SQUAD 4     // idle enemy soldiers needed for an attack wave
#define STRAT_CORPSE_FOOD    6      // food units in a hunted animal's corpse
#define STRAT_TRAIN_TIME_WORKER  4.0f
#define STRAT_TRAIN_TIME_SOLDIER 6.0f

typedef enum {
    RES_WOOD = 0,
    RES_STONE,
    RES_FOOD,
    RES_COUNT
} ResourceKind;

typedef enum {
    NODE_TREE = 0,      // yields RES_WOOD
    NODE_ROCK,          // yields RES_STONE
    NODE_WHEAT,         // yields RES_FOOD
    NODE_CORPSE,        // yields RES_FOOD (left behind by hunted animals)
} NodeKind;

typedef enum {
    BLD_HOUSE = 0,      // trains workers, raises the pop cap
    BLD_LOGGING,
    BLD_QUARRY,
    BLD_BARRACKS,       // trains soldiers
    BLD_FARM,           // workers assigned to it generate food (renewable)
    BLD_COUNT
} BuildingKind;

typedef enum {
    KIND_WORKER = 0,    // gathers/farms/hunts, weak in a fight
    KIND_SOLDIER,       // fights only, cannot gather
    KIND_ANIMAL,        // neutral critter (FACTION_NEUTRAL), huntable for food
    UNIT_KIND_COUNT
} UnitKind;

typedef enum {
    UNIT_IDLE = 0,
    UNIT_MOVE,          // walking to .target
    UNIT_GATHER,        // walking to / working .targetNode
    UNIT_RETURN,        // carrying resources to the nearest own building
    UNIT_ATTACK,        // chasing / hitting .targetUnit OR .targetBuilding
    UNIT_FARM,          // working .targetBuilding (a farm), food straight to stockpile
} UnitState;

typedef struct {
    bool         active;
    int          faction;
    UnitKind     kind;
    Vector3      pos;
    Vector3      target;            // move destination (UNIT_MOVE)
    UnitState    state;
    float        hp, maxHp;
    float        attackCooldown;    // seconds until the next hit is allowed
    int          targetUnit;        // units[] index while UNIT_ATTACK (-1 = none)
    int          targetNode;        // nodes[] index while UNIT_GATHER (-1 = none)
    int          targetBuilding;    // buildings[] index while attacking one or
                                    //   farming (-1 = none); never set with targetUnit
    int          carryAmount;       // 0..STRAT_CARRY_MAX
    ResourceKind carryKind;
    float        gatherTimer;       // accumulates toward STRAT_GATHER_TIME / FARM_PERIOD
    bool         selected;          // player faction only
    int          controlGroup;      // 0 = none, 1..3 = ctrl+digit group
} Unit;

typedef struct {
    bool         active;
    BuildingKind kind;
    int          faction;
    Vector3      pos;
    float        hp, maxHp;
    int          trainKind;         // UnitKind in production, -1 = idle
    float        trainProgress;     // seconds spent on the current trainee
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
    int       selectedBuilding;     // buildings[] index (player), -1 = none;
                                    //   mutually exclusive with unit selection
    bool      buildMenuOpen;        // command panel currently shows the build list
    int       gameOver;             // -1 = playing, else the WINNING faction index
    float     aiTimer;              // countdown to the next enemy think tick
    Rectangle guiBlock;             // REAL-screen px area where the GUI owns the
                                    //   mouse (command panel); world clicks ignore it
} StrategyWorld;

#endif // STRATEGY_TYPES_H
