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
AppState app_tilemap_example_resize = {NULL, NULL, NULL, Draw, NULL, "left mouse select, release to resize"};

#define TAU (PI*2)

static void Draw(){
    // Persistent variables to hold state
	static recti rect_state;
	static vec2i selection_state;
	static TileID temp_buffer[MAP_SIZE_X * MAP_SIZE_Y];
    const uint32_t TEMP_BUFFER_CAPACITY = (MAP_SIZE_X * MAP_SIZE_Y);

	bool size_error;

	InputState input_selection = GetInputState(
		IsMouseButtonPressed(MOUSE_BUTTON_LEFT), 
		IsMouseButtonDown(MOUSE_BUTTON_LEFT),
		IsMouseButtonReleased(MOUSE_BUTTON_LEFT)
	);

	if (is_editor_button_hover){
		input_selection = tmInputState_NONE;
		rect_state.w = 0;
		rect_state.h = 0;
	}

	Vector2 mouse_position = Screen2Target(GetMousePosition());
	vec2i mouse_position_i = {(int32_t)mouse_position.x, (int32_t)mouse_position.y};

	if (input_selection != tmInputState_NONE){
		CreateSelection(&tilemap, mouse_position_i, &selection_state, &rect_state, input_selection);
	}

	int32_t rect_area = rect_state.w * rect_state.h;
	size_error = rect_area > tilemap.capacity;

	if (input_selection == tmInputState_RELEASE){
		if (!size_error && rect_area > 0){
			TilemapResize(&tilemap, rect_state, temp_buffer, TEMP_BUFFER_CAPACITY);
		}
		rect_state.w = 0;
		rect_state.h = 0;
	}

	DrawTilemapGrid(&tilemap, LIGHTGRAY);
	bool skip_zero = true;
	DrawTilemap(&tilemap, &tileset, &tile_atlas, skip_zero, TileRandType_NONE, &tileset_texture);

	DrawTilemapSelection(&tilemap, rect_state, BLACK);

	if (size_error) {
		const char *text = TextFormat("ERROR: tilemap grid buffer overflow (%d > %d)", rect_area, tilemap.capacity);
		DrawText(text, 0, 0, 10, RED);
	}
}