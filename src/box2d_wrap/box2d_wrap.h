#ifndef BOX2D_WRAP_H
#define BOX2D_WRAP_H

#include "box2d/box2d.h"
#include "stdbool.h"

typedef struct {
    b2WorldId world;
    float time;
    float units_per_meter;
    void (*sensor_begin_callback)(b2SensorBeginTouchEvent);
    void (*sensor_end_callback)(b2SensorEndTouchEvent);
} WorldContext;

typedef struct {
    void *entity;
    unsigned int kind;
    b2ShapeId shape;
} SensorContext;

typedef struct {
    void *entity;
    unsigned int kind;       // bit flags recommended
} ContactContext;

// WORLD
void WorldContextInit(
    WorldContext *ctx,
    float units_per_meter,
    b2Vec2 gravity,      // {0, -1200}
    void (*sensor_begin_callback)(b2SensorBeginTouchEvent),
    void (*sensor_end_callback)(b2SensorEndTouchEvent)
);

void WorldContextUpdate(WorldContext *ctx, float delta_time);
void WorldContextDraw(WorldContext *ctx, b2DebugDraw *debug_draw);
void WorldContextCleanup(WorldContext *ctx);

// BODY
b2BodyId BodyCreate(
    WorldContext *ctx,
    b2Vec2 position,
    b2Vec2 size,
    b2BodyType type,
    bool fixed_rotation,
    const char *name
);

void BodyLinearVelocity(b2BodyId body, b2Vec2 velocity);

// SHAPE
b2ShapeId ShapeCreateBox(
    b2BodyId body,
    b2Vec2 size, 
    unsigned long int categoryBits,       // entity kind
    unsigned long int maskBits,       // collides against
    float density,
    void *userData, // rawptr(uintptr(SensorKind.coin))
    bool isSensor,
    bool enableSensorEvents,
    bool enablePreSolveEvents,
    bool enableContactEvents,
    bool enableHitEvents
);

b2ShapeId ShapeCreateCircle(
    b2BodyId body,
    float radius, 
    unsigned long int categoryBits,       // entity kind
    unsigned long int maskBits,       // collides against
    float density,
    void *userData,
    bool isSensor,
    bool enableSensorEvents,
    bool enablePreSolveEvents,
    bool enableContactEvents,
    bool enableHitEvents
);

b2ShapeId ShapeCreateCapsule(
    b2BodyId body,
    float radius,
    float height,
    unsigned long int categoryBits,       // entity kind
    unsigned long int maskBits,       // collides against
    float density,
    void* userData,
    bool isSensor,
    bool enableSensorEvents,
    bool enablePreSolveEvents,
    bool enableContactEvents,
    bool enableHitEvents
);

// Utils
b2Vec2 b2RectanglePosition(b2Vec2 pos, b2Vec2 size);
b2Vec2 b2PositionForRectangle(b2Vec2 pos, b2Vec2 size);
void BodyDestroy(b2BodyId body);
void ShapeDestroy(b2ShapeId shape);


#endif // BOX2D_WRAP_H