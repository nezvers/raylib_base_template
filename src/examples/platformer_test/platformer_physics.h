#ifndef PLATFORMER_PHYSICS_H
#define PLATFORMER_PHYSICS_H

#include "../../box2d_wrap/box2d_wrap.h"
#include "platformer_types.h"
#include "platformer_constants.h"
#include "common_types.h"

typedef void SensorBeginFcn( b2SensorBeginTouchEvent );
typedef void SensorEndFcn( b2SensorEndTouchEvent );
void PhysicsWorldInit(WorldContext *ctx, SensorBeginFcn *begin, SensorEndFcn *end, b2PreSolveFcn *pre, void *user_data);

#endif // PLATFORMER_PHYSICS_H