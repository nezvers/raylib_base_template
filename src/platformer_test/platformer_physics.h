#ifndef PLATFORMER_PHYSICS_H
#define PLATFORMER_PHYSICS_H

#include "../box2d_wrap/box2d_wrap.h"
#include "platformer_types.h"
#include "platformer_constants.h"
#include "common_types.h"

void PhysicsWorldInit(WorldContext *ctx, void *user_data);

#endif // PLATFORMER_PHYSICS_H