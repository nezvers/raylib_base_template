#ifndef BOX2D_WRAP_H
#define BOX2D_WRAP_H

#include "box2d/box2d.h"

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

b2Vec2 b2RectanglePosition(b2Vec2 pos, b2Vec2 size);
b2Vec2 b2PositionForRectangle(b2Vec2 pos, b2Vec2 size);

#endif // BOX2D_WRAP_H