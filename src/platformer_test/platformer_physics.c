#include "raylib.h"
#include "../box2d_wrap/box2d_wrap.h"
#include "platformer_types.h"
#include "platformer_constants.h"
#include "common_types.h"


void PhysicsSensorBegin(b2SensorBeginTouchEvent event_begin);
void PhysicsSensorEnd(b2SensorEndTouchEvent event_end);
bool PhysicsPreSolve( b2ShapeId shapeIdA, b2ShapeId shapeIdB, b2Manifold* manifold, void* context );

void PhysicsWorldInit(WorldContext *ctx, void *user_data) {
    WorldContextInit(
        ctx,
        32,                 // units per meter
        (b2Vec2){0, 0},     // zero gravity, each object updates their velocity
        PhysicsSensorBegin,
        PhysicsSensorEnd
    );
    b2World_SetPreSolveCallback(ctx->world, PhysicsPreSolve, user_data);
}

void PhysicsPlatformInit(WorldContext *ctx, PlatformStatic *platform) {
    
}

void PhysicsSensorBegin(b2SensorBeginTouchEvent event_begin) {}
void PhysicsSensorEnd(b2SensorEndTouchEvent event_end) {}
bool PhysicsPreSolve( b2ShapeId shapeIdA, b2ShapeId shapeIdB, b2Manifold* manifold, void* context ) { return false;}