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

// File as struct array
SSAPI bool SimpleArrayReserve(const char *file_path, size_t element_size, size_t count);
SSAPI bool SimpleArrayAppend(const char *file_path, const void *element, size_t element_size);
SSAPI bool SimpleArrayLoad(const char *file_path, void *element, size_t element_size, size_t index);
SSAPI bool SimpleArraySave(const char *file_path, const void *element, size_t element_size, size_t index);
SSAPI size_t SimpleArrayCount(const char *file_path, size_t element_size);

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


// File as struct array

SSAPI size_t SimpleArrayCount(const char *file_path, size_t element_size)
{
    if (element_size == 0) return 0;

    FILE *file = fopen(file_path, "rb");
    if (!file) return 0;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }

    long size = ftell(file);
    fclose(file);

    if (size < 0) return 0;

    return (size_t)size / element_size;
}

SSAPI bool SimpleArrayReserve(const char *file_path, size_t element_size, size_t count)
{
    FILE *file = fopen(file_path, "wb");
    if (!file)
        return false;

    if (count != 0) {
        if (fseek(file, (long)(element_size * count - 1), SEEK_SET) != 0) {
            fclose(file);
            return false;
        }

        fputc(0, file);
    }

    fclose(file);
    return true;
}

SSAPI bool SimpleArrayAppend(const char *file_path, const void *element, size_t element_size)
{
    FILE *file = fopen(file_path, "ab");
    if (!file)
        return false;

    bool ok = fwrite(element, element_size, 1, file) == 1;

    fclose(file);
    return ok;
}

SSAPI bool SimpleArrayLoad(const char *file_path, void *element, size_t element_size, size_t index)
{
    FILE *file = fopen(file_path, "rb");
    if (!file)
        return false;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }

    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return false;
    }

    size_t count = (size_t)size / element_size;

    if (index >= count) {
        printf("SimpleArrayLoad: index out of bounds\n");
        fclose(file);
        return false;
    }

    fseek(file, (long)(index * element_size), SEEK_SET);

    bool ok = fread(element, element_size, 1, file) == 1;

    fclose(file);
    return ok;
}

SSAPI bool SimpleArraySave(const char *file_path, const void *element, size_t element_size, size_t index)
{
    FILE *file = fopen(file_path, "r+b");
    if (!file)
        return false;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }

    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return false;
    }

    size_t count = (size_t)size / element_size;

    if (index >= count) {
        printf("SimpleArraySave: index out of bounds\n");
        fclose(file);
        return false;
    }

    fseek(file, (long)(index * element_size), SEEK_SET);

    bool ok = fwrite(element, element_size, 1, file) == 1;

    fclose(file);
    return ok;
}



#endif // SIMPLE_SAVE_IMPLEMENTATION

#undef SSAPI