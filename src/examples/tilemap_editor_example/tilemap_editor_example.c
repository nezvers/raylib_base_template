#include "raylib.h"
#include "../app_state/app_state.h"
#include "../screen_state/screen_state.h"
#include <stddef.h>

#include "tilemap_example_common.h"

// Forward declare functions
static void Enter();
static void Update();
static void Exit();
static void Draw();
static void Gui();
                               /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_tilemap_editor = {Enter, Exit, Update, Draw, Gui, "Placeholder"};


// TODO: Refactor into state struct

// Holds texture positions for tiles
vec2 atlas_buffer[ATLAS_SIZE_X * ATLAS_SIZE_Y + 1];
// Holds indexes for tile_atlas positions
// For this example same buffer sliced for each Tile (no alternative tiles)
TileID tile_buffer[ATLAS_SIZE_X * ATLAS_SIZE_Y + 1];
// Holds indexes for Tiles
TileID tileset_buffer[ATLAS_SIZE_X * ATLAS_SIZE_Y + 1];

Texture tileset_texture;
TileAtlas tile_atlas;

// Allocated Tile array
Tileset tileset;
Tile tile_list[ATLAS_SIZE_X * ATLAS_SIZE_Y + 1];

Tilemap tilemap;
TileID tilemap_buffer[MAP_SIZE_X * MAP_SIZE_Y];


static void CreateTiles() {
    vec2 tile_size = {(float)TILE_SIZE_X, (float)TILE_SIZE_Y};
	TileAtlasInit(&tile_atlas, tile_size, atlas_buffer, ATLAS_BUFFER_SIZE);
	
	// Assign Tile array to a tileset
	uint32_t initial_length_tileset = TILE_LIST_BUFFER_SIZE;
	TilesetInit(&tileset, tile_list, TILE_LIST_BUFFER_SIZE, initial_length_tileset);
	
	// Represents TILE_EMPTY, use skip_zero flag
	vec2 tex_pos = {0.0, 0.0};
	// Each tile gets 1 TileAtlas position
	uint32_t initial_length = 1;
	TileAtlasInsert(&tile_atlas, tex_pos, 0);
	tile_buffer[0] = TILE_EMPTY;
	TileInit(&tile_list[0], tile_buffer, 1, initial_length);

	// Calculate actual tiles
	// for each tile in texture generate atlas position and assign ID to Tile
	for (int32_t i = 0; i < (ATLAS_SIZE_X * ATLAS_SIZE_Y); i += 1) {
		int32_t x = i % ATLAS_SIZE_X;
		int32_t y = i / ATLAS_SIZE_X;
		tex_pos = (vec2){
			(float)(x * TILE_SIZE_X),
			(float)(y * TILE_SIZE_Y),
		};

		uint32_t tile_i = (uint32_t)(i + 1);
		TileAtlasInsert(&tile_atlas, tex_pos, tile_i);
		// Assign TileID
		tile_buffer[tile_i] = (TileID)tile_i;
		TileInit(&tile_list[tile_i], &tile_buffer[tile_i], 1, initial_length);
    }
}

static void CreateTilemap() {
	tilemap = TilemapInit((vec2i){100, 100}, (vec2i){MAP_SIZE_X, MAP_SIZE_Y}, (vec2i){16,16}, tilemap_buffer, TILEMAP_BUFFER_SIZE);
	// reset map to predictable state
	TilemapClear(&tilemap);
	
    // Hardcoded tilemap
	TilemapSetTile(&tilemap, (vec2i){2, 2}, 1 + (TileID)ATLAS_SIZE_X * 0);
	TilemapSetTile(&tilemap, (vec2i){3, 2}, 2 + (TileID)ATLAS_SIZE_X * 0);
	TilemapSetTile(&tilemap, (vec2i){4, 2}, 3 + (TileID)ATLAS_SIZE_X * 0);
	TilemapSetTile(&tilemap, (vec2i){6, 2}, 4 + (TileID)ATLAS_SIZE_X * 0);

	TilemapSetTile(&tilemap, (vec2i){2, 3}, 1 + (TileID)ATLAS_SIZE_X * 1);
	TilemapSetTile(&tilemap, (vec2i){3, 3}, 2 + (TileID)ATLAS_SIZE_X * 1);
	TilemapSetTile(&tilemap, (vec2i){4, 3}, 3 + (TileID)ATLAS_SIZE_X * 1);
	TilemapSetTile(&tilemap, (vec2i){6, 3}, 4 + (TileID)ATLAS_SIZE_X * 1);

	TilemapSetTile(&tilemap, (vec2i){2, 4}, 1 + (TileID)ATLAS_SIZE_X * 2);
	TilemapSetTile(&tilemap, (vec2i){3, 4}, 2 + (TileID)ATLAS_SIZE_X * 2);
	TilemapSetTile(&tilemap, (vec2i){4, 4}, 3 + (TileID)ATLAS_SIZE_X * 2);
	TilemapSetTile(&tilemap, (vec2i){6, 4}, 4 + (TileID)ATLAS_SIZE_X * 2);
    
	TilemapSetTile(&tilemap, (vec2i){2, 6}, 1 + (TileID)ATLAS_SIZE_X * 3);
	TilemapSetTile(&tilemap, (vec2i){3, 6}, 2 + (TileID)ATLAS_SIZE_X * 3);
	TilemapSetTile(&tilemap, (vec2i){4, 6}, 3 + (TileID)ATLAS_SIZE_X * 3);

	TilemapSetTile(&tilemap, (vec2i){6, 6}, 4 + (TileID)ATLAS_SIZE_X * 3);
}

static void Enter(){
    ScreenState *screen_state = ScreenStateGet();
    screen_state->clear_color = WHITE;

    tileset_texture = LoadTexture(RESOURCES_PATH"/textures/tileset_template.png");

    CreateTiles();
    CreateTilemap();
}

static void Exit(){
    UnloadTexture(tileset_texture);
}

static void Update(){

}

static void Draw(){
    
}

static void Gui() {

}