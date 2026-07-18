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
AppState app_tilemap_example_paint = {NULL, NULL, NULL, Draw, NULL, "left mouse draw, right mouse copy, mouse scroll change ID"};

#define TAU (PI*2)

static void Draw(){
    static TileID tile_id;
	static vec2i position_state;
	Vector2 mouse_position = Screen2Target(GetMousePosition());
	vec2i mouse_position_i = {(int32_t)mouse_position.x, (int32_t)mouse_position.y};

	InputState input_paint = GetInputState(
		IsMouseButtonPressed(MOUSE_BUTTON_LEFT), 
		IsMouseButtonDown(MOUSE_BUTTON_LEFT),
		IsMouseButtonReleased(MOUSE_BUTTON_LEFT)
	);

	PaintTiles(&tilemap, mouse_position_i, &position_state, tile_id, input_paint );

	// Read TileID under mouse position
	TileID mouse_id = TilemapGetTileWorld(&tilemap, mouse_position_i);
	
	if (mouse_id != TILE_INVALID) {
		// Active while inside tilemap
		int32_t wheel = (int32_t)GetMouseWheelMove();
		TileID max_tiles = ATLAS_BUFFER_SIZE;
		if (wheel > 0) {
			tile_id = ((tile_id + 1) % max_tiles);
		}
		if (wheel < 0) {
			tile_id = ((tile_id - 1 + max_tiles) % max_tiles);
		}
		
		if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)){
			// Copy TileID under mouse
			tile_id = mouse_id;
		}
	}
		
	// Don't draw TILE_EMPTY ID
	bool skip_zero = true;
	DrawTilemapGrid(&tilemap, LIGHTGRAY);
	DrawTilemap(&tilemap, &tileset, &tile_atlas, skip_zero, TileRandType_NONE, &tileset_texture);

	if (mouse_id != TILE_INVALID) {
		// Draw a cell aligned to grid and ID under mouse
		DrawTilemapCellRect(&tilemap, mouse_position_i, tile_id, GetFontDefault(), 10, GRAY);
	}
}