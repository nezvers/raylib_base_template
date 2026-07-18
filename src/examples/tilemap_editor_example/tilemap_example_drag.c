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
AppState app_tilemap_example_drag = {NULL, NULL, NULL, Draw, NULL, "left mouse select, right mouse drag,\n\thold CTRL to remove source, ALT to write empty tiles"};

#define TAU (PI*2)

static void Draw() {
	// Persistent variables to hold state
	static vec2i selection_state;
	static vec2i drag_pos_state;
	static vec2i map_pos_state;
	static recti rect_state;
	static Tilemap temp_tilemap;
	static TileID temp_buffer[MAP_SIZE_X * MAP_SIZE_Y];

	InputState input_selection = GetInputState(
		IsMouseButtonPressed(MOUSE_BUTTON_LEFT), 
		IsMouseButtonDown(MOUSE_BUTTON_LEFT),
		IsMouseButtonReleased(MOUSE_BUTTON_LEFT)
	);

	InputState input_drag = GetInputState(
		IsMouseButtonPressed(MOUSE_BUTTON_RIGHT), 
		IsMouseButtonDown(MOUSE_BUTTON_RIGHT),
		IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)
	);

	if (rect_state.w == 0 || rect_state.h == 0){
		// no drag without selection
		input_drag = tmInputState_NONE;
	}
	
	Vector2 mouse_position = Screen2Target(GetMousePosition());
	vec2i mouse_position_i = {(int32_t)mouse_position.x, (int32_t)mouse_position.y};

	if (input_drag != tmInputState_NONE){
		bool write_empty = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
		bool remove_source = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
		
		DragTiles(
			&tilemap,
			&temp_tilemap,
			mouse_position_i,
			&drag_pos_state,
			&rect_state,
			input_drag,
			remove_source,
			write_empty,
			temp_buffer,
			(MAP_SIZE_X * MAP_SIZE_Y) // capacity
		);

		// Don't allow to change selection
		if (input_selection != tmInputState_NONE){
			input_selection = tmInputState_NONE;
		}
	}

	if (input_selection != tmInputState_NONE && !is_editor_button_hover){
		CreateSelection(&tilemap, mouse_position_i, &selection_state, &rect_state, input_selection);
	}

	DrawTilemapGrid(&tilemap, LIGHTGRAY);
	bool skip_zero = true;
	DrawTilemap(&tilemap, &tileset, &tile_atlas, skip_zero, TileRandType_NONE, &tileset_texture);
	if(temp_tilemap.size.x != 0 && rect_state.w != 0){
		recti temp_rect = TilemapRecti(&temp_tilemap);
		DrawTilemapSelection(&temp_tilemap, temp_rect, GRAY);
		DrawTilemap(&temp_tilemap, &tileset, &tile_atlas, skip_zero, TileRandType_NONE, &tileset_texture);
	}

	DrawTilemapSelection(&tilemap, rect_state, BLACK);
}