#include "platformer_physics.h"
#include "../box2d_wrap/box2d_wrap.h"


void PhysicsWorldInit(WorldContext *ctx, SensorBeginFcn *begin, SensorEndFcn *end, b2PreSolveFcn *pre, void *user_data) {
    void (*sensor_begin_callback)(b2SensorBeginTouchEvent);
    void (*sensor_end_callback)(b2SensorEndTouchEvent);
    WorldContextInit(
        ctx,
        32,                 // units per meter
        (b2Vec2){0, 0},     // zero gravity, each object updates their velocity
        begin,
        end
    );
    b2World_SetPreSolveCallback(ctx->world, pre, user_data);
}