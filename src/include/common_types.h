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

#endif // COMMON_TYPEDEF_DEFINED