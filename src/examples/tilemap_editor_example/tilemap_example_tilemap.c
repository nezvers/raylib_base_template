#include "raylib.h"
#include "../app_state/app_state.h"
#include "../screen_state/screen_state.h"
#include <stddef.h>

#include "tilemap_example_common.h"
#include <math.h>

// Forward declare functions
static void Enter();
static void Update();
static void Exit();
static void Draw();
static void Gui();
                               /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_tilemap_example_tilemap = {NULL, NULL, NULL, Draw, NULL, "Tilemap drawing example"};

#define TAU (PI*2)

static void Draw(){
    bool skip_zero = true;
	DrawTilemap(&tilemap, &tileset, &tile_atlas, skip_zero, TileRandType_NONE, &tileset_texture);
}