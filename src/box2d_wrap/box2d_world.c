#include "box2d_wrap.h"

void WorldContextInit(
    WorldContext *ctx,
    float units_per_meter,
    b2Vec2 gravity,      // {0, -1200}
    void (*sensor_begin_callback)(b2SensorBeginTouchEvent),
    void (*sensor_end_callback)(b2SensorEndTouchEvent)
) {
    b2SetLengthUnitsPerMeter(units_per_meter);
    b2WorldDef world_def = b2DefaultWorldDef();
    world_def.gravity = gravity;
    ctx->world = b2CreateWorld(&world_def);
    ctx->time = 0;
    ctx->sensor_begin_callback = sensor_begin_callback;
    ctx->sensor_end_callback = sensor_end_callback;
}

void WorldContextDraw(WorldContext *ctx, b2DebugDraw *debug_draw) {
    if (debug_draw == NULL) { return; }
    b2World_Draw(ctx->world, debug_draw);
}

void WorldContextDestroy(WorldContext *ctx) {
    if (!b2World_IsValid(ctx->world)) { return; }
    ctx->time = 0;
    b2DestroyWorld(ctx->world);
}

void WorldContextUpdate(WorldContext *ctx, float delta_time) {
    const float PHYSICS_TIME_STEP = 1.f / 60.f;
    const unsigned int SUB_STEPS = 12;
    bool stepped = false;

    ctx->time += delta_time;
    while (ctx->time >= PHYSICS_TIME_STEP) {
        b2World_Step(ctx->world, PHYSICS_TIME_STEP, SUB_STEPS);
        ctx->time -= PHYSICS_TIME_STEP;
        stepped = true;
    }
    if (!stepped) { return; }

    b2SensorEvents sensor_events = b2World_GetSensorEvents(ctx->world);
    if (ctx->sensor_begin_callback != NULL) {
        for (int i = 0; i < sensor_events.beginCount; i += 1) {
            ctx->sensor_begin_callback(sensor_events.beginEvents[i]);
        }
    }
    if (ctx->sensor_end_callback != NULL) {
        for (int i = 0; i < sensor_events.endCount; i += 1) {
            ctx->sensor_end_callback(sensor_events.endEvents[i]);
        }
    }
}

b2Vec2 b2RectanglePosition(b2Vec2 pos, b2Vec2 size) {
    return (b2Vec2){pos.x + size.x * 0.5f, -pos.y - size.y * 0.5f};
}

b2Vec2 b2PositionForRectangle(b2Vec2 pos, b2Vec2 size) {
    // TODO: Need test if Y is correct
    return (b2Vec2){pos.x - size.x * 0.5f, -pos.y + size.y * 0.5f};
}