#include "raylib.h"
#include "raymath.h"
#include "../box2d_wrap/box2d_wrap.h"
#include "platformer_types.h"
#include "platformer_constants.h"
#include "common_types.h"
#include "platformer_physics.h"
#include "stddef.h"


// represents each level
typedef struct {
    Actor *actors;
    PlatformStatic *platforms;
    Box *boxes;
    Coin *coins;
    Jumpad *jumpads;

    u32 actor_count;
    u32 platform_count;
    u32 box_count;
    u32 coin_count;
    u32 jumpad_count;

    // Carry physics info
    WorldContext world_ctx;
    b2DebugDraw debug_draw;
    Actor *player; // Assigned in LevelActorInit
} LevelContext;

// Static size buffers for objects
static Actor actors[20];
static PlatformStatic platforms[20];
static Box boxes[20];
static Coin coins[20];
static Jumpad jumpads[20];

static LevelContext level;
static Actor *player; // Assigned in LevelLoad at initialization

typedef void RestartFcn(void);
static RestartFcn *restart_callback;
void LevelSetRestartCallback(RestartFcn *callback) {
    restart_callback = callback;
}
#define RESTART_FALL 200

#define SIGN_F(x) ((x) > 0 ? 1 : ((x) < 0) ? -1 : 0)

void LevelActorInit(Actor *actor);
void LevelPlatformInit(PlatformStatic *platform);
void LevelBoxInit(Box *box);
void LevelJumpadInit(Jumpad *jumpad);
void LevelCoinsInit(Coin *coin);

void LevelActorUpdate(f32 delta_time);
void LevelBoxesUpdate(f32 delta_time);
void LevelCoinsUpdate(f32 delta_time);

void LevelSensorBegin(b2SensorBeginTouchEvent event);
void LevelSensorEnd(b2SensorEndTouchEvent event);
bool LevelPreSolve( b2ShapeId shapeIdA, b2ShapeId shapeIdB, b2Manifold* manifold, void* context );

void LevelUpdate() {
    f32 delta_time = GetFrameTime(); // OPTION: turn into bullet time
    WorldContextUpdate(&level.world_ctx, delta_time);
    LevelBoxesUpdate(delta_time);
    LevelCoinsUpdate(delta_time);

    if (player != NULL) {
        bool right = IsKeyDown(KEY_D);
        bool left = IsKeyDown(KEY_A);
        player->input.x = (f32)(i32)right - (f32)(i32)left;
        player->input.jump = IsKeyDown(KEY_SPACE);
    }
    LevelActorUpdate(delta_time);

    if (player->state.pos.y > RESTART_FALL) {
        // Fell below floor
        if (restart_callback != NULL) { restart_callback();}
    }
}

void LevelDraw() {
    WorldContextDraw(&level.world_ctx, &level.debug_draw);
}

void LevelDestroy() {
    for (int i = 0; i < level.platform_count; i += 1) {
        ShapeDestroy(platforms[i].shape);
        BodyDestroy(platforms[i].body);
    }
    for (int i = 0; i < level.box_count; i += 1) {
        ShapeDestroy(boxes[i].prop.shape);
        ShapeDestroy(boxes[i].sensor.shape);
        BodyDestroy(boxes[i].prop.body);
    }
    for (int i = 0; i < level.jumpad_count; i += 1) {
        ShapeDestroy(jumpads[i].sensor.shape);
        BodyDestroy(jumpads[i].body);
    }
    for (int i = 0; i < level.coin_count; i += 1) {
        ShapeDestroy(coins[i].sensor.shape);
        BodyDestroy(coins[i].body);
    }
    for (int i = 0; i < level.actor_count; i += 1) {
        ShapeDestroy(actors[i].shape);
        ShapeDestroy(actors[i].sensor_actor.shape);
        BodyDestroy(actors[i].body);
    }
    WorldContextDestroy(&level.world_ctx);
    player = NULL;
}


// Load level
void LevelLoad_1() {
    /* PHYSICS FIRST */
    PhysicsWorldInit(&level.world_ctx, LevelSensorBegin, LevelSensorEnd, LevelPreSolve, &level);
    level.debug_draw = Box2dRaylibDebugDraw();

    /* Platforms */
    level.platforms = platforms;
    level.platform_count = 4;
    platforms[0] = (PlatformStatic){.pos = (Vector2){0, 170}, .size = (Vector2){320, 8}}; // bottom floor
    platforms[1] = (PlatformStatic){.pos = (Vector2){50, 170 - 32}, .size = (Vector2){48, 8}};
    platforms[2] = (PlatformStatic){.pos = (Vector2){150, 170 - 32}, .size = (Vector2){48, 8}};
    platforms[3] = (PlatformStatic){.pos = (Vector2){250, 170 - 32}, .size = (Vector2){48, 8}};
    for (int i = 0; i < level.platform_count; i += 1) {
        LevelPlatformInit(&platforms[i]);
    }

    /* Boxes */
    level.boxes = boxes;
    level.box_count = 3;
    boxes[0] = (Box){.prop = (Props){.pos = (Vector2){80, 130}, .size = (Vector2){8, 8}}};
    boxes[1] = (Box){.prop = (Props){.pos = (Vector2){180, 130}, .size = (Vector2){8, 8}}};
    boxes[2] = (Box){.prop = (Props){.pos = (Vector2){280, 130}, .size = (Vector2){8, 8}}};
    for (int i = 0; i < level.box_count; i += 1) {
        LevelBoxInit(&boxes[i]);
    }

    /* Jumpads */
    level.jumpads = jumpads;
    level.jumpad_count = 1;
    jumpads[0] = (Jumpad){.pos = (Vector2){110, 170 -1}, .size = (Vector2){8, 2}};
    for (int i = 0; i < level.jumpad_count; i += 1) {
        LevelJumpadInit(&jumpads[i]);
    }

    /* Coins */
    level.coins = coins;
    level.coin_count = 3;
    coins[0] = (Coin){.pos = (Vector2){65, 125}, .size = (Vector2){2, 2}};
    coins[1] = (Coin){.pos = (Vector2){165, 125}, .size = (Vector2){2, 2}};
    coins[2] = (Coin){.pos = (Vector2){265, 125}, .size = (Vector2){2, 2}};
    for (int i = 0; i < level.coin_count; i += 1) {
        LevelCoinsInit(&coins[i]);
    }
    
    /* Actors */
    level.actor_count = 1;
    level.actors = actors;
    actors[0] = (Actor){
        .type = ACTOR_TYPE_PLAYER,
        .values = (ActorValues){
            .jump_force = JUMP_FORCE,
            .speed_max = SPEED_MAX,
            .acceleration = ACCELERATION,
            .deacceleration = DEACCELERATION,
            .jump_count = JUMP_COUNT,
        },
        .state = (ActorState){
            .grounded = false,
            .pos = (Vector2){10, 30},
        },
        .contact = (ContactContext){.entity = &actors[0], .kind = ENTITY_KIND_ACTOR},
    };
    for (int i = 0; i < level.actor_count; i += 1) {
        LevelActorInit(&actors[i]);
        if (actors[i].type == ACTOR_TYPE_PLAYER) {
            player = &actors[i];
        }
    }
}


void LevelActorInit(Actor *actor) {
    b2Vec2 pos = *(b2Vec2*)& actor->state.pos;
    pos.y *= -1.f;                      // TRANSLATE to Box2D inverted Y

    b2BodyDef body_def = b2DefaultBodyDef();
    body_def.position = pos;
    body_def.type = b2_dynamicBody;
    body_def.fixedRotation = true;
    body_def.name = (actor->type == ACTOR_TYPE_PLAYER) ? "Player" : "Enemy";
    body_def.userData = actor;          // store reference back to Actor to access whole struct
    actor->body = b2CreateBody(level.world_ctx.world, &body_def);

    // TODO: make possible to have different shapes for each actor
    b2ShapeDef torso_def = b2DefaultShapeDef();
    // Belongs to categories
    torso_def.filter.categoryBits = ENTITY_KIND_ACTOR; // | (actor->type == ACTOR_TYPE_PLAYER) ? ENTITY_KIND_PLAYER : ENTITY_KIND_ENEMY;
    // Collides against categories
    torso_def.filter.maskBits = ENTITY_KIND_SOLID | ENTITY_KIND_PROP; // To collide against actors add ENTITY_KIND_ACTOR
    torso_def.enableSensorEvents = false;
    torso_def.enablePreSolveEvents = true;   // call LevelPreSolve
    torso_def.userData = &actor->contact;
    torso_def.material.friction = 0;
    torso_def.density = 1.0f;

    const f32 RADIUS = 3.f;
    const f32 HEIGHT = 16.f;
    // Capsule above position
    b2Vec2 p1 = b2Add(pos, (b2Vec2){0.f, -HEIGHT + RADIUS});
    b2Vec2 p2 = b2Add(pos, (b2Vec2){0.f, -RADIUS});
    b2Capsule capsule = (b2Capsule){p1, p2, RADIUS};
    actor->shape = b2CreateCapsuleShape(actor->body, &torso_def, &capsule);

    b2ShapeDef sensor_def = b2DefaultShapeDef();
    sensor_def.filter.categoryBits = ENTITY_KIND_ACTOR | ENTITY_KIND_PLAYER; //ENTITY_KIND_ACTOR | (actor->type == ACTOR_TYPE_PLAYER) ? ENTITY_KIND_PLAYER : ENTITY_KIND_ENEMY;
    sensor_def.filter.maskBits = ENTITY_KIND_JUMPAD | ENTITY_KIND_COIN; //(actor->type == ACTOR_TYPE_PLAYER) ? ENTITY_KIND_COIN : ENTITY_KIND_NONE;
    sensor_def.enableSensorEvents = true;
    sensor_def.userData = &actor->sensor_actor;

    actor->sensor_actor.shape = b2CreateCapsuleShape(actor->body, &sensor_def, &capsule);
    actor->sensor_actor.entity = actor;
    actor->sensor_actor.kind = SENSOR_KIND_ACTOR;
}

void LevelPlatformInit(PlatformStatic *platform) {
    platform->body = BodyCreate(
        &level.world_ctx,
        *(b2Vec2*)& platform->pos,
        *(b2Vec2*)& platform->size,
        b2_staticBody,
        true,            // fixed_rotation
        "Platform"
    );
    platform->shape = ShapeCreateBox(
        platform->body,
        *(b2Vec2*)& platform->size,
        ENTITY_KIND_SOLID,                                         // category
        (ENTITY_KIND_ACTOR | ENTITY_KIND_PLAYER | ENTITY_KIND_ENEMY | ENTITY_KIND_PROP), // mask
        1.f,                   // density
        &platform->contact,    // user_data
        false,                 // is_sensor
        false,                 // enable_sensor_events
        false,                 // enable_presolve_events
        false,                 // enable_contact_events
        false                  // enable_hit_events
    );
    platform->contact.entity = platform;
    platform->contact.kind = ENTITY_KIND_SOLID;
}

void LevelBoxInit(Box *box) {
    box->prop.body = BodyCreate(
        &level.world_ctx,
        *(b2Vec2*)& box->prop.pos,
        *(b2Vec2*)& box->prop.size,
        b2_dynamicBody,
        false, // fixed_rotation
        "Box"
    );
    box->prop.shape = ShapeCreateBox(
        box->prop.body,
        *(b2Vec2*)& box->prop.size,
        ENTITY_KIND_PROP,      // category
        (ENTITY_KIND_ACTOR | ENTITY_KIND_PROP | ENTITY_KIND_SOLID), // mask
        1.f,                   // density
        &box->prop.contact,    // user_data
        false,                 // is_sensor
        false,                 // enable_sensor_events
        false,                 // enable_presolve_events
        false,                 // enable_contact_events
        false                  // enable_hit_events
    );
    box->prop.contact.kind = ENTITY_KIND_PROP;
    box->prop.contact.entity = &box->prop; // MAYBE: reference box

    box->sensor.shape = ShapeCreateBox(
        box->prop.body,
        *(b2Vec2*)& box->prop.size,
        ENTITY_KIND_PROP,      // category
        ENTITY_KIND_JUMPAD,    // mask
        1.f,                   // density
        &box->sensor,          // user_data
        false,                 // is_sensor
        true,                  // enable_sensor_events
        false,                 // enable_presolve_events
        false,                 // enable_contact_events
        false                  // enable_hit_events
    );
    box->sensor.kind = SENSOR_KIND_PROP;
    box->sensor.entity = &box->prop;    // MAYBE: reference box
}

void LevelJumpadInit(Jumpad *jumpad) {
    jumpad->body = BodyCreate(
        &level.world_ctx,
        *(b2Vec2*)& jumpad->pos,
        *(b2Vec2*)& jumpad->size,
        b2_staticBody,
        true,               // fixed_rotation
        "Jumpad"
    );
    jumpad->sensor.shape = ShapeCreateBox(
        jumpad->body,
        *(b2Vec2*)& jumpad->size,
        ENTITY_KIND_JUMPAD,   // category
        (ENTITY_KIND_ACTOR | ENTITY_KIND_PROP), // mask
        1.f,                // density
        &jumpad->sensor,    // user_data
        true,               // is_sensor
        true,               // enable_sensor_events
        false,              // enable_presolve_events
        false,              // enable_contact_events
        false               // enable_hit_events
    );
    jumpad->sensor.entity = jumpad;
    jumpad->sensor.kind = SENSOR_KIND_JUMPAD;
    jumpad->active = true;
    jumpad->triggered = false;
}

void LevelCoinsInit(Coin *coin) {
    coin->body = BodyCreate(
        &level.world_ctx,
        *(b2Vec2*)& coin->pos,
        *(b2Vec2*)& coin->size,
        b2_staticBody,
        true,               // fixed_rotation
        "Coin"
    );
    coin->sensor.shape = ShapeCreateBox(
        coin->body,
        *(b2Vec2*)& coin->size,
        ENTITY_KIND_COIN,   // category
        ENTITY_KIND_PLAYER, // mask
        1.f,                // density
        &coin->sensor,    // user_data
        true,               // is_sensor
        true,               // enable_sensor_events
        false,              // enable_presolve_events
        false,              // enable_contact_events
        false               // enable_hit_events
    );
    coin->sensor.entity = coin;
    coin->sensor.kind = SENSOR_KIND_COIN;
    coin->active = true;
    coin->triggered = false;
}

void LevelBoxesUpdate(f32 delta_time) {
    // Reverse order to have option to remove by replacing with last
    for (int i = level.box_count -1; i > -1; i -= 1) {
        Box *box = &boxes[i];
        b2Vec2 velocity = b2Body_GetLinearVelocity(box->prop.body);
        velocity.y += GRAVITY * delta_time;
        b2Body_SetLinearVelocity(box->prop.body, velocity);
    }
}

void LevelCoinsUpdate(f32 delta_time) {
    // Reverse order to have option to remove by replacing with last
    for (int i = level.coin_count -1; i > -1; i -= 1) {
        Coin *coin = &coins[i];
        if (!coin->active) { continue; }
        if (!coin->triggered) { continue; }

        coin->active = false;
        ShapeDestroy(coin->sensor.shape);
        BodyDestroy(coin->body);

        // TODO: doesn't move underlying ID structs.
        // Replace with last
        // level.coin_count -= 1;
        // if (i == level.coin_count) { continue; }
        // coins[i] = coins[level.coin_count];
    }
}


void LevelActorUpdate(f32 delta_time) {
    // Reverse order to have option to remove by replacing with last
    for (int i = level.actor_count -1; i > -1; i -= 1) {
        Actor *actor = &actors[i];
        b2Vec2 pos_b2 = b2PositionForRectangle( b2Body_GetPosition(actor->body), (b2Vec2){0,0});
        // TODO: use sprite size
        actor->state.pos = *(Vector2*)& pos_b2;

        b2Vec2 velocity = b2Body_GetLinearVelocity(actor->body);
        b2Vec2 target_velocity = velocity;

        // Horizontal speed
        if (actor->input.x != 0) {
            target_velocity.x = Clamp(
                target_velocity.x + actor->input.x * actor->values.acceleration * delta_time,
                -actor->values.speed_max,
                actor->values.speed_max
            );
        } else {
            f32 abs_speed = fabsf(target_velocity.x);
            if (abs_speed > actor->values.deacceleration * delta_time) {
                f32 sign_speed = SIGN_F(target_velocity.x);
                target_velocity.x += -sign_speed * delta_time * actor->values.deacceleration;
            } else {
                target_velocity.x = 0;
            }
        }

        // Vertical speed
        target_velocity.y += GRAVITY * delta_time;

        // Jumping
        if (actor->state.grounded) {
            actor->state.remaining_jumps = actor->values.jump_count;
            if (actor->input.jump) {
                if (!actor->state.is_jumping) {
                    actor->state.grounded = false;
                    actor->state.is_jumping = true;
                    actor->state.remaining_jumps -= 1;
                    target_velocity.y = actor->values.jump_force;
                }
            }
            else if (actor->state.is_jumping) {
                actor->state.is_jumping = false;
            }
        } else {
            // Not grounded
            if (actor->input.jump) {
                if (!actor->state.is_jumping && actor->state.remaining_jumps > 0) {
                    // Double jump
                    actor->state.is_jumping = true;
                    actor->state.remaining_jumps -= 1;
                    target_velocity.y = actor->values.jump_force;
                }
            } else if (actor->state.is_jumping) {
                actor->state.is_jumping = false;
                if (target_velocity.y > actor->values.jump_force * 0.5f) {
                    // Jump release / variable jump height
                    target_velocity.y = actor->values.jump_force * 0.5f;
                }
            }
        }
        // NOTE: even with velocity up, next frame collision event sttill happen.
        // state.velocity used to skip state.grounded
        actor->state.velocity = *(Vector2*)& target_velocity;
        b2Body_SetLinearVelocity(actor->body, target_velocity);
        actor->state.grounded = false;
    }
}

void LevelSensorBegin(b2SensorBeginTouchEvent event) {
    SensorContext *sensor = b2Shape_GetUserData(event.sensorShapeId);
    SensorContext *visitor = b2Shape_GetUserData(event.visitorShapeId);

    switch(sensor->kind) {
        case SENSOR_KIND_NONE : break;
        case SENSOR_KIND_ACTOR: break;
        case SENSOR_KIND_COIN:
            Coin *coin = sensor->entity;
            if (coin->triggered) { return; }
            if (visitor->kind == SENSOR_KIND_ACTOR && visitor->entity == player) {
                coin->triggered = true;
                // TODO: SCORE !!!
            }
            break;
        case SENSOR_KIND_JUMPAD:
            if (visitor->kind == SENSOR_KIND_ACTOR) {
                Actor *actor = visitor->entity;
                b2Vec2 velocity = b2Body_GetLinearVelocity(actor->body);
                velocity.y = JUMP_FORCE * 1.3;
                b2Body_SetLinearVelocity(actor->body, velocity);
            }
            else if (visitor->kind == SENSOR_KIND_PROP) {
                // Box
                Props *prop = visitor->entity;
                b2Vec2 velocity = b2Body_GetLinearVelocity(prop->body);
                // TODO: differentiate props
                velocity.y = JUMP_FORCE * 1.3;
                b2Body_SetLinearVelocity(prop->body, velocity);
            }
            break;
        case SENSOR_KIND_GROUND:
            break;
    }
}

void LevelSensorEnd(b2SensorEndTouchEvent event) {
    if (!b2Shape_IsValid(event.sensorShapeId)) { return; }
    SensorContext *sensor = b2Shape_GetUserData(event.sensorShapeId);
    switch(sensor->kind) {
        case SENSOR_KIND_NONE : break;
        case SENSOR_KIND_GROUND : break;
    }
}

bool LevelPreSolve( b2ShapeId shapeIdA, b2ShapeId shapeIdB, b2Manifold* manifold, void* context ) {
    ContactContext *contactA = b2Shape_GetUserData(shapeIdA);
    ContactContext *contactB = b2Shape_GetUserData(shapeIdB);

    if (contactA->kind == ENTITY_KIND_ACTOR) {
        Actor *actor = contactA->entity;
        // WORKAROUND: after jumping up, this still triggers frame after
        if (!(actor->state.velocity.y > 0) && (manifold->normal.y > 0.5f)) {
            actor->state.grounded = true;
        }
    }
    
    if (contactB->kind == ENTITY_KIND_ACTOR) {
        Actor *actor = contactB->entity;
        // WORKAROUND: after jumping up, this still triggers frame after
        if (!(actor->state.velocity.y > 0) && (manifold->normal.y > 0.5f)) {
            actor->state.grounded = true;
        }
    }

    return true;
}

