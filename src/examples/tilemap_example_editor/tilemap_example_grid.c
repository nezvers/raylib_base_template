#include "raylib.h"
#include "../../app_state/app_state.h"
#include "../../screen_state/screen_state.h"
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
AppState app_tilemap_example_grid = {NULL, NULL, NULL, Draw, NULL, "Grid drawing example"};

static void Draw(){
    DrawTilemapGrid(&tilemap, LIGHTGRAY);
	DrawTilemapTileId(&tilemap, GetFontDefault(), 10, LIGHTGRAY);

	Vector2 mouse_position = Screen2Target(GetMousePosition());
	vec2i mouse_position_i = {(int32_t)mouse_position.x, (int32_t)mouse_position.y};
	// Read TileID under mouse position
	TileID tile_id = TilemapGetTileWorld(&tilemap, mouse_position_i);
	// Draw a cell aligned to grid and ID under mouse
	DrawTilemapCellRect(&tilemap, mouse_position_i, tile_id, GetFontDefault(), 10, GRAY);
}