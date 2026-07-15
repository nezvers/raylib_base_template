#ifndef SIMPLE_SAVE_H
#define SIMPLE_SAVE_H

#include <stddef.h>
#include <stdio.h>


FILE* SimpleLoadOpen(const char *input_file);
FILE* SimpleSaveOpen(const char *output_file);
bool SimpleLoadBytes(FILE *file_in, char *buffer, size_t size);
bool SimpleSaveBytes(FILE *file_out, char *buffer, size_t size);
bool SimpleLoadBytesFrom(FILE *file_in, char *buffer, size_t size, size_t position);
bool SimpleSaveBytes(FILE *file_out, char *buffer, size_t size, size_t position);
void SimpleFileClose(FILE *file);

#endif // SIMPLE_SAVE_H

#ifdef SIMPLE_SAVE_IMPLEMENTATION
#undef SIMPLE_SAVE_IMPLEMENTATION

FILE* SimpleLoadOpen(const char *input_file) {
    FILE *file_in = NULL;
    file_in = fopen(input_file, "rb");
    if (!file_in) {
        printf("Failed to open input file - %s\n", input_file);
        goto defer;
    }
    return file_in;

defer:
    if (file_in) fclose(file_in);
    return NULL;
}

FILE* SimpleSaveOpen(const char *output_file) {
    FILE *file_out = NULL;
    file_out = fopen(output_file, "rb");
    if (!file_out) {
        printf("Failed to create file - %s\n", output_file);
        goto defer;
    }
    return file_out;

defer:
    if (file_out) fclose(file_out);
    return NULL;
}

bool SimpleLoadBytes(FILE *file_in, char *buffer, size_t size) {
    if (!fread(buffer, size, 1, file_in) != 0){
        printf("Failed to read a buffer\n");
        return false;
    }
    return true;
}

bool SimpleSaveBytes(FILE *file_out, char *buffer, size_t size) {
    if (fwrite(buffer, size, 1, file_out) == 0){
        printf("Failed to write a buffer\n");
        return false;
    }
    return true;
}

bool SimpleLoadBytesFrom(FILE *file_in, char *buffer, size_t size, size_t position) {
    if (fseek (file_in, position, SEEK_SET)){
        return false;
    }
    return fread(buffer, size, 1, file_in) != 0;
}

bool SimpleSaveBytes(FILE *file_out, char *buffer, size_t size, size_t position) {
    if (fseek (file_out, position, SEEK_SET)){
        return false;
    }
    if (fwrite(buffer, size, 1, file_out) == 0){
        printf("Failed to write a buffer\n");
        return false;
    }
    return true;
}

void SimpleFileClose(FILE *file) {
    if (file) fclose(file);
}

#endif // SIMPLE_SAVE_IMPLEMENTATION