#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H

#include <stdint.h>

typedef uint32_t u32;
typedef int32_t  i32;
typedef float    f32;
typedef double   f64;

typedef struct { f32 x; f32 y; } vec2;
typedef struct {
    f32 x;
    f32 y;
    f32 w;
    f32 h;
} rectf;

#endif // COMMON_TYPES_H