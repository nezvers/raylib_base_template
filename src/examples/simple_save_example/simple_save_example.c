#include "raylib.h"
#include "raygui.h"
#include "../../app_state/app_state.h"
#include "../../screen_state/screen_state.h"
#include <stddef.h>
#include "stdint.h"
#include "stdlib.h"

#define SIMPLE_SAVE_IMPLEMENTATION
#include "simple_save.h"

// Forward declare functions
static void Enter();
static void Update();
static void Exit();
static void Draw();
static void Gui();
                               /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_simple_save_example = {Enter, Exit, Update, Draw, Gui, "Simple Save"};

typedef struct {
    uint32_t rand_seed;
    char value_str[128];
} ExampleData;
static ExampleData example_data;
#define SAVE_FILE RESOURCES_PATH"save_file.sav"

static const char *message;
#define NAME_COUNT 9
static const char *name_list[NAME_COUNT] = {
    "Mugget",
    "Funger",
    "Pruck",
    "Clyt",
    "Seka",
    "Byat",
    "Lolly",
    "Chibby",
    "Eximo",
};


static uint32_t rand_int(uint32_t *seed) {
    /* Taken from OneLoneCoder: https://github.com/OneLoneCoder/Javidx9/blob/0c8ec20a9ed3b2daf76a925034ac5e7e6f4096e0/PixelGameEngine/SmallerProjects/OneLoneCoder_PGE_ProcGen_Universe.cpp#L183 */
    *seed += 0xe120fc15;
    uint64_t tmp;
    tmp = (uint64_t)*seed * 0x4a39b70d;
    uint32_t m1 = (tmp >> 32) ^ tmp;
    tmp = (uint64_t)m1 * 0x12fad5c9;
    uint32_t m2 = (tmp >> 32) ^ tmp;
    return m2;
}

static void Enter(){
    ScreenState *screen_state = ScreenStateGet();
    screen_state->clear_color = WHITE;
    example_data = (ExampleData){0};
}

static void Exit(){
    // Remove generated file
    SimpleDelete(SAVE_FILE);
}

static void Update(){

}

static void Draw(){
    
}

static void Gui() {
    if (GuiButton((Rectangle){10, 10, 100, 25}, "Load")) {
        if ( SimpleLoad(SAVE_FILE, (void*)&example_data, sizeof(example_data)) ) {
            message = "Success: Load";
        } else {
            message = "ERROR: Load failed - do fallback";
        }
    }
    if ( GuiButton((Rectangle){115, 10, 100, 25}, "Save") ) {
        if ( SimpleSave(SAVE_FILE, (void*)&example_data, sizeof(example_data))) {
            message = "Success: Save";
        } else {
            message = "ERROR: Save failed - do fallback";
        }
    }
    DrawText(message, 220, 10, 20, LIGHTGRAY);
    DrawText(TextFormat("Rand seed: %u", example_data.rand_seed), 10, 40, 20, LIGHTGRAY);

    if (GuiButton((Rectangle){10, 65, 100, 25}, "Rand Name")) {
        uint32_t i = rand_int(&example_data.rand_seed);
        const char *new_name = name_list[i % NAME_COUNT];
        sprintf(example_data.value_str, "%s", new_name);
    }
    DrawText(example_data.value_str, 10, 90, 20, LIGHTGRAY);
}


/* File as struct array
typedef struct {
    int id;
    float health;
} Enemy;

Enemy e;

// reserve space for 1024 enemies 
SimpleArrayReserve("enemies.bin", sizeof(Enemy), 1024);

// overwrite enemy #50 
SimpleArraySave("enemies.bin", &e, sizeof(Enemy), 50);

// read enemy #50 
SimpleArrayLoad("enemies.bin", &e, sizeof(Enemy), 50);

// append another enemy 
SimpleArrayAppend("enemies.bin", &e, sizeof(Enemy));

// query current number of elements 
size_t count = SimpleArrayCount("enemies.bin", sizeof(Enemy));

*/