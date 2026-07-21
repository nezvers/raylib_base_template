#ifndef SIMPLE_SAVE_H
#define SIMPLE_SAVE_H

#include <stddef.h>
#include <stdio.h>

// Remove name mangling for C++
#ifdef __cplusplus
extern "C" {
#endif

#ifdef STATIC_API
#define SSAPI static
#else
#define SSAPI
#endif

SSAPI bool SimpleSave(const char *file_path, char *buffer, size_t size);
SSAPI bool SimpleLoad(const char *file_path, char *buffer, size_t size);
SSAPI FILE* SimpleSaveOpen(const char *file_path); // TODO: add alternative to continue writing
SSAPI FILE* SimpleLoadOpen(const char *file_path);
SSAPI bool SimpleSaveBytes(FILE *file_out, char *buffer, size_t size);
SSAPI bool SimpleLoadBytes(FILE *file_in, char *buffer, size_t size);
SSAPI bool SimpleSaveBytesFrom(FILE *file_out, char *buffer, size_t size, size_t position);
SSAPI bool SimpleLoadBytesFrom(FILE *file_in, char *buffer, size_t size, size_t position);
SSAPI void SimpleFileClose(FILE *file);
SSAPI void SimpleDelete(const char *file_path);

#ifdef __cplusplus
}
#endif

#endif // SIMPLE_SAVE_H

/* ---------------------------------------- */

#ifdef SIMPLE_SAVE_IMPLEMENTATION
#undef SIMPLE_SAVE_IMPLEMENTATION

SSAPI bool SimpleSave(const char *file_path, char *buffer, size_t size) {
    FILE* file = SimpleSaveOpen(file_path);
    if (file == NULL) { return false; }
    bool result = SimpleSaveBytes(file, buffer, size);
    SimpleFileClose(file);
    return result;
}

SSAPI bool SimpleLoad(const char *file_path, char *buffer, size_t size) {
    FILE* file = SimpleLoadOpen(file_path);
    if (file == NULL) { return false; }
    bool result = SimpleLoadBytes(file, buffer, size);
    SimpleFileClose(file);
    return result;
}

SSAPI FILE* SimpleLoadOpen(const char *file_path) {
    FILE *file_in = NULL;
    file_in = fopen(file_path, "rb");
    if (!file_in) {
        printf("Failed to open input file - %s\n", file_path);
        goto defer;
    }
    return file_in;

defer:
    if (file_in) fclose(file_in);
    return NULL;
}

SSAPI FILE* SimpleSaveOpen(const char *file_path) {
    FILE *file_out = NULL;
    file_out = fopen(file_path, "wb");
    if (!file_out) {
        printf("Failed to create file - %s\n", file_path);
        goto defer;
    }
    return file_out;

defer:
    if (file_out) fclose(file_out);
    return NULL;
}

SSAPI bool SimpleLoadBytes(FILE *file_in, char *buffer, size_t size) {
    if (!fread(buffer, size, 1, file_in) != 0){
        printf("Failed to read a buffer\n");
        return false;
    }
    return true;
}

SSAPI bool SimpleSaveBytes(FILE *file_out, char *buffer, size_t size) {
    if (fwrite(buffer, size, 1, file_out) == 0){
        printf("Failed to write a buffer\n");
        return false;
    }
    return true;
}

SSAPI bool SimpleLoadBytesFrom(FILE *file_in, char *buffer, size_t size, size_t position) {
    if (fseek (file_in, position, SEEK_SET)){
        return false;
    }
    return fread(buffer, size, 1, file_in) != 0;
}

SSAPI bool SimpleSaveBytesFrom(FILE *file_out, char *buffer, size_t size, size_t position) {
    if (fseek (file_out, position, SEEK_SET)){
        return false;
    }
    if (fwrite(buffer, size, 1, file_out) == 0){
        printf("Failed to write a buffer\n");
        return false;
    }
    return true;
}

SSAPI void SimpleFileClose(FILE *file) {
    if (file) fclose(file);
}
SSAPI void SimpleDelete(const char *file_path) {
    if (file_path) remove(file_path);
}

#endif // SIMPLE_SAVE_IMPLEMENTATION

#undef SSAPI