#ifndef TEMPORARY_ALLOCATOR_H
#define TEMPORARY_ALLOCATOR_H

// Remove name mangling for C++
#ifdef __cplusplus
extern "C" {
#endif

#ifdef STATIC_API
#define TAAPI static
#else
#define TAAPI
#endif

TAAPI void TempAllocReset();
TAAPI void *TempAllocGet(unsigned int size);

#ifdef __cplusplus
}
#endif

#endif // TEMPORARY_ALLOCATOR_H

/* --------------------------------------- */

#ifdef TEMPORARY_ALLOCATOR_IMPLEMENTATION
#undef TEMPORARY_ALLOCATOR_IMPLEMENTATION

#include <assert.h>

#ifndef TEMPORARY_ALLOCATOR_SIZE 
static_assert(false)
#endif

typedef struct {
    char buffer[TEMPORARY_ALLOCATOR_SIZE];
    unsigned int len;
} TempBuffer_t;

static TempBuffer_t temp_alloc_buffer;

TAAPI void TempAllocReset() {
    temp_alloc_buffer.len = 0;
}

TAAPI void *TempAllocGet(unsigned int size) {
    assert((temp_alloc_buffer.len + size) <= TEMPORARY_ALLOCATOR_SIZE);
    void *ptr = &temp_alloc_buffer.buffer[temp_alloc_buffer.len];
    temp_alloc_buffer.len += size;
    return ptr;
}


#endif // TEMPORARY_ALLOCATOR_IMPLEMENTATION

#undef TAAPI