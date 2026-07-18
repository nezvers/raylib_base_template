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
AppState app_tilemap_example_region = {NULL, NULL, NULL, Draw, NULL, "Reveal tiles with rectangle"};

#define TAU (PI*2)

static void Draw(){
    Vector2 mouse_position = Screen2Target(GetMousePosition());
	vec2i mouse_position_i = {(int32_t)mouse_position.x, (int32_t)mouse_position.y};
	// Translate position to tile coordinates
	vec2i tile_position = TilemapGetWorld2Tile(&tilemap, mouse_position_i);
	recti region = {tile_position.x - 5, tile_position.y - 4, 5, 4};
	// Don't draw TILE_EMPTY ID
	bool skip_zero = true;

	DrawTilemapGrid(&tilemap, LIGHTGRAY);
	// Draw only tiles inside region
	DrawTilemapRecti(&tilemap, &tileset, &tile_atlas, skip_zero, TileRandType_NONE, region, &tileset_texture);
	// Draw rectangle around tiles
	DrawTilemapSelection(&tilemap, region, GRAY);
}