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


// Resets to a begining, intended for game loop start or end
TAAPI void TempAllocReset();
// Reserve a buffer from allicator
TAAPI void *TempAllocGet(unsigned int size);
// Store allocator position at current moment
TAAPI unsigned int TempAllocSave();
// Roll back to specific position
TAAPI void TempAllocLoad(unsigned int index);

#ifdef __cplusplus
}
#endif

#endif // TEMPORARY_ALLOCATOR_H

/* --------------------------------------- */

#ifdef TEMPORARY_ALLOCATOR_IMPLEMENTATION
#undef TEMPORARY_ALLOCATOR_IMPLEMENTATION

#include <assert.h>

#ifndef TEMPORARY_ALLOCATOR_SIZE
#define TEMPORARY_ALLOCATOR_SIZE 1024
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

TAAPI unsigned int TempAllocSave() {
    return temp_alloc_buffer.len;
}

TAAPI void TempAllocLoad(unsigned int index) {
    temp_alloc_buffer.len = index;
}


#endif // TEMPORARY_ALLOCATOR_IMPLEMENTATION

#undef TAAPI