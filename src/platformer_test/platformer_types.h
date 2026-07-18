#ifndef PLATFORMER_TYPES_H
#define PLATFORMER_TYPES_H

#include "box2d/box2d.h"
#include "raylib.h"
#include "../box2d_wrap/box2d_wrap.h"
#include "common_types.h"

enum ENTITY_KIND {
    ENTITY_KIND_NONE   = 0,
    ENTITY_KIND_SOLID  = 1 << 0,
    ENTITY_KIND_ACTOR  = 1 << 1,
    ENTITY_KIND_COIN   = 1 << 2,
    ENTITY_KIND_JUMPAD = 1 << 3,
    ENTITY_KIND_PLAYER = 1 << 4,
    ENTITY_KIND_ENEMY  = 1 << 5,
    ENTITY_KIND_PROP   = 1 << 6,
};

enum SENSOR_KIND {
    SENSOR_KIND_NONE,
    SENSOR_KIND_ACTOR,
    SENSOR_KIND_COIN,
    SENSOR_KIND_JUMPAD,
    SENSOR_KIND_GROUND,
    SENSOR_KIND_PROP,
    SENSOR_KIND_HURT,
};

typedef struct {
    Vector2 pos;
    Vector2 size;
    b2BodyId body;
    b2ShapeId shape;
    ContactContext contact;
} Props;
typedef Props PlatformStatic;

typedef struct {
    Props prop;
    SensorContext sensor;
} Box;

typedef struct {
    Vector2 pos;
    Vector2 size;
    b2BodyId body;
    SensorContext sensor;
    bool active;
    bool triggered;
} Trigger;
typedef Trigger Coin;
typedef Trigger Jumpad;


// Used for choosing input and behaviour
enum ACTOR_TYPE {
    ACTOR_TYPE_PLAYER,
    ACTOR_TYPE_ENEMY,
};

typedef struct {
    float jump_force;
    float speed_max;
    float acceleration;
    float deacceleration;
    uint32_t jump_count;
    vec2 size;
} ActorValues;

typedef struct {
    Vector2 pos;
    bool grounded;
    bool is_jumping;
    Vector2 velocity;
    uint32_t remaining_jumps;
} ActorState;

typedef struct {
    float x;
    bool jump;
} ActorInput;

typedef struct {
    enum ACTOR_TYPE type;
    ActorInput input;
    ActorState state;
    ActorValues values;

    b2BodyId body;
    b2ShapeId shape;
    ContactContext contact;
    SensorContext sensor_actor;
    // b2ShapeId shape_feet;
    // SensorContext sensor_ground;
    // SensorContext sensor_hurt;
} Actor;

#endif // PLATFORMER_TYPES_H
