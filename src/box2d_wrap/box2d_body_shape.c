#include "box2d_wrap.h"


b2BodyId BodyCreate(
    WorldContext *ctx,
    b2Vec2 position,
    b2Vec2 size,
    b2BodyType type,
    bool fixed_rotation,
    const char *name
) {
    b2Vec2 pos = b2RectanglePosition(position, size);
    b2BodyDef body_def = b2DefaultBodyDef();
    body_def.position = pos;
    body_def.type = type;
    body_def.fixedRotation = fixed_rotation;
    body_def.name = name;
    b2BodyId result = b2CreateBody(ctx->world, &body_def);
    return result;
}

void BodyLinearVelocity(b2BodyId body, b2Vec2 velocity) {
    b2Body_SetLinearVelocity(body, velocity);
}

void BodyDestroy(b2BodyId body) {
    if (b2Body_IsValid(body)) {
        b2DestroyBody(body);
    }
}

void ShapeDestroy(b2ShapeId shape) {
    if (b2Shape_IsValid(shape)) {
        b2DestroyShape(shape, false);
    }
}

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
) {
    b2ShapeDef shape_def = b2DefaultShapeDef();
    shape_def.filter.categoryBits = categoryBits;
    shape_def.filter.maskBits = maskBits;
    shape_def.density = density;
    shape_def.userData = userData;
    shape_def.isSensor = isSensor;
    shape_def.enableSensorEvents = enableSensorEvents;
    shape_def.enablePreSolveEvents = enablePreSolveEvents;
    shape_def.enableContactEvents = enableContactEvents;
    shape_def.enableHitEvents = enableHitEvents;

    b2Polygon box = b2MakeBox(size.x * 0.5f - 1, size.y * 0.5f - 1);
    b2ShapeId result = b2CreatePolygonShape(body, &shape_def, &box);
    return result;
}

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
) {
    b2ShapeDef shape_def = b2DefaultShapeDef();
    shape_def.filter.categoryBits = categoryBits;
    shape_def.filter.maskBits = maskBits;
    shape_def.density = density;
    shape_def.userData = userData;
    shape_def.isSensor = isSensor;
    shape_def.enableSensorEvents = enableSensorEvents;
    shape_def.enablePreSolveEvents = enablePreSolveEvents;
    shape_def.enableContactEvents = enableContactEvents;
    shape_def.enableHitEvents = enableHitEvents;

    b2Circle circle;
    circle.radius = radius;
    b2ShapeId result = b2CreateCircleShape(body, &shape_def, &circle);
    return result;
}

b2ShapeId ShapeCreateCapsule(
    b2BodyId body,
    float radius,
    float height,
    unsigned long int categoryBits,       // entity kind
    unsigned long int maskBits,       // collides against
    float density,
    void *userData,
    bool isSensor,
    bool enableSensorEvents,
    bool enablePreSolveEvents,
    bool enableContactEvents,
    bool enableHitEvents
) {
    b2ShapeDef shape_def = b2DefaultShapeDef();
    shape_def.filter.categoryBits = categoryBits;
    shape_def.filter.maskBits = maskBits;
    shape_def.density = density;
    shape_def.userData = userData;
    shape_def.isSensor = isSensor;
    shape_def.enableSensorEvents = enableSensorEvents;
    shape_def.enablePreSolveEvents = enablePreSolveEvents;
    shape_def.enableContactEvents = enableContactEvents;
    shape_def.enableHitEvents = enableHitEvents;

    b2Capsule capsule;
    capsule.radius = radius;
    capsule.center1 = (b2Vec2){radius, height * 0.5f - radius};
    capsule.center2 = (b2Vec2){radius, -height * 0.5f + radius};
    b2ShapeId result = b2CreateCapsuleShape(body, &shape_def, &capsule);
    
    return result;
}


