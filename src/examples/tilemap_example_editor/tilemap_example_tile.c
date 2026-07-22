#include "raylib.h"
#include "../../app_state/app_state.h"
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
AppState app_tilemap_example_tile = {NULL, NULL, NULL, Draw, NULL, "Tile drawing example"};

#define TAU (PI*2)

static void Draw(){
    int32_t padding = 4 + (int32_t)(sin(GetExampleAnimationTime(0.2f) * TAU) * 4.5f);
	vec2 root_position = {10, 30};
	// 0th id is TILE_EMPTY
	int32_t id_offset = 1;
	for (int32_t y = 0; y < ATLAS_SIZE_Y; y += 1) {
		for (int32_t x = 0; x < ATLAS_SIZE_X; x += 1) {
			int32_t i = x + y * ATLAS_SIZE_X + id_offset;

			TileID atlas_id = TileGetId(&tile_list[i]);
			Vector2 draw_pos = {
				root_position.x + (float)(x * (TILE_SIZE_X + padding)), 
				root_position.y + (float)(y * (TILE_SIZE_Y + padding))
			};
			DrawTileAtlas(&tile_atlas, atlas_id, draw_pos, &tileset_texture);
		}
	}
}