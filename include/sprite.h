#ifndef SPRITE_H
#define SPRITE_H

// "common_types.h"
#ifndef COMMON_TYPEDEF_DEFINED
#define COMMON_TYPEDEF_DEFINED
#include <stdint.h>

typedef struct { float x; float y; } vec2;
typedef struct { int32_t x; int32_t y; } vec2i;
typedef struct {
    float x;
    float y;
    float w;
    float h;
} rectf;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} recti;
#endif // ---> COMMON_TYPEDEF_DEFINED

// Animation frame list, asuming all frames have same region size
typedef struct {
    vec2 *data; // Frame position array
    uint32_t count;
    vec2 size; // width & height
} Frames;

typedef struct {
    Frames **frames; // Array of Frame pointers
    uint32_t count;
    uint32_t animation_index;
    uint32_t image_index;
    float frame_rate;
    float time;
} AnimationSet;


typedef struct {
    AnimationSet animation_set;
    vec2 position;
    vec2 origin;
    vec2 offset;
    vec2 scale;
    float rotation;
} Sprite;

/* _________________________________________________________________ */

// Remove name mangling for C++
#ifdef __cplusplus
extern "C" {
#endif

#ifdef STATIC_API
#define SAPI static
#else
#define SAPI
#endif

SAPI void ChangeAnimation(AnimationSet *animation_set, uint32_t new_animation);
SAPI void UpdateAnimation(AnimationSet *animation_set, float delta_time);
SAPI void UpdateSprite(Sprite *sprite, float delta_time);
SAPI rectf GetAnimationFrame(AnimationSet *animation_set);
SAPI void GetSpriteFrame(Sprite *sprite, rectf *sprite_rect, rectf *texture_rect);

#ifdef __cplusplus
}
#endif

#endif // SPRITE_H


/* _________________________________________________________________ */

#ifdef SPRITE_IMPLEMENTATION
#undef SPRITE_IMPLEMENTATION

#include <assert.h>

SAPI void ChangeAnimation(AnimationSet *animation_set, uint32_t new_animation) {
    assert(new_animation < animation_set->count);
    animation_set->animation_index = new_animation;
    animation_set->image_index = 0;
    animation_set->time = 0;
}

SAPI void UpdateAnimation(AnimationSet *animation_set, float delta_time) {
    animation_set->time += delta_time * animation_set->frame_rate;
    if (animation_set->time < 1) { return; }

    const Frames *frame = animation_set->frames[animation_set->animation_index];
    const uint32_t image_count = frame->count;
    const uint32_t increment = animation_set->time;
    animation_set->time -= increment;
    animation_set->image_index = (animation_set->image_index + increment) % image_count;
}

SAPI void UpdateSprite(Sprite *sprite, float delta_time) {
    UpdateAnimation(&sprite->animation_set, delta_time);
}

SAPI rectf GetAnimationFrame(AnimationSet *animation_set) {
    const Frames *frame = animation_set->frames[animation_set->animation_index];
    const vec2 pos = frame->data[animation_set->image_index];
    const vec2 size = frame->size;
    const rectf result = {pos.x, pos.y, size.x, size.y};
    return result;
}

// texture_out = source rectangle from texture
// sprite_out = destination rectangle on screen
SAPI void GetSpriteFrame(Sprite *sprite, rectf *texture_out, rectf *sprite_out) {
    *texture_out = GetAnimationFrame(&sprite->animation_set);
    *sprite_out = (rectf){sprite->position.x, sprite->position.y, texture_out->w, texture_out->h};
}

#endif

#undef SAPI 