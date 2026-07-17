#ifndef TILEMAP_H
#define TILEMAP_H

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

// Remove name mangling for C++
#ifdef __cplusplus
extern "C" {
#endif

#ifdef STATIC_API
#define TMAPI static
#else
#define TMAPI
#endif



#ifdef __cplusplus
}
#endif
#endif // TILEMAP_H


/* ------------------------------------- */
#define TILEMAP_IMPLEMENTATION // TODO: remove - auto implement for prototype

#ifdef TILEMAP_IMPLEMENTATION
#undef TILEMAP_IMPLEMENTATION





#endif // TILEMAP_IMPLEMENTATION