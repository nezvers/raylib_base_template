#ifndef TILEMAP_EXAMPLE_H
#define TILEMAP_EXAMPLE_H


#include "tilemap.h"
#include "tilemap_raylib.h"

// Individual tile size in pixels
#define TILE_SIZE_X 16
#define TILE_SIZE_Y 16
// Tiles on texture columns and rows
#define ATLAS_SIZE_X 10
#define ATLAS_SIZE_Y 5
// In-game tilemap size
#define MAP_SIZE_X 10
#define MAP_SIZE_Y 10

// Holds texture positions for tiles
#define ATLAS_BUFFER_SIZE (ATLAS_SIZE_X * ATLAS_SIZE_Y + 1)
extern vec2 atlas_buffer[ATLAS_BUFFER_SIZE];

// Holds indexes for tile_atlas positions
// For this example same buffer sliced for each Tile (no alternative tiles)
#define TILE_BUFFER_SIZE (ATLAS_SIZE_X * ATLAS_SIZE_Y + 1)
extern TileID tile_buffer[TILE_BUFFER_SIZE];

// Holds indexes for Tiles
#define TILESET_BUFFER_SIZE (ATLAS_SIZE_X * ATLAS_SIZE_Y + 1)
extern TileID tileset_buffer[TILESET_BUFFER_SIZE];

#define TILE_LIST_BUFFER_SIZE (ATLAS_SIZE_X * ATLAS_SIZE_Y + 1)
extern Tile tile_list[TILE_LIST_BUFFER_SIZE];

#define TILEMAP_BUFFER_SIZE (MAP_SIZE_X * MAP_SIZE_Y)
extern TileID tilemap_buffer[TILEMAP_BUFFER_SIZE];

extern Texture tileset_texture;
extern TileAtlas tile_atlas;
// Allocated Tile array
extern Tileset tileset;
extern Tilemap tilemap;


#endif // TILEMAP_EXAMPLE_H