#include "raylib.h"
#include "../app_state/app_state.h"
#include "../screen_state/screen_state.h"
#include <stddef.h>

// Forward declare functions
static void Enter();
static void Update();
static void Exit();
static void Draw();
static void Gui();
                               /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_tilemap_editor = {Enter, Exit, Update, Draw, Gui, "Placeholder"};

// Individual tile size in pixels
#define TILE_SIZE CLITERAL(Vector2){16, 16}
// Tiles on texture columns and rows
#define ATLAS_SIZE CLITERAL(Vector2){10, 5}
// In-game tilemap size
#define MAP_SIZE CLITERAL(Vector2){10, 10}

static void Enter(){
    ScreenState *screen_state = ScreenStateGet();
    screen_state->clear_color = WHITE;
}

static void Exit(){

}

static void Update(){

}

static void Draw(){
    
}

static void Gui() {

}