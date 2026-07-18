#ifndef TILEMAP_H
#define TILEMAP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// "common_types.h"
#ifndef COMMON_TYPEDEF_DEFINED
#define COMMON_TYPEDEF_DEFINED

typedef struct { float x; float y; } vec2;
typedef struct { int32_t x; int32_t y; } vec2i;
typedef struct {
    float x;
    float y;
    float w;
    float h;
} rectf;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} recti;
#endif // ---> COMMON_TYPEDEF_DEFINED

#ifndef TileID
    #define TileID unsigned char // default type
#endif
#ifndef TILE_INVALID
    #define TILE_INVALID 0xff    // default value 255, meaning valid are from 1 to 254
#endif
#define TILE_EMPTY 0

// Data about Texture positions
typedef struct {
    vec2 *data;
    uint32_t length;
    uint32_t capacity;
    vec2 tile_size;
} TileAtlas;

// Array of atlas position IDs. Tile->TileAtlas.data
// Can hold IDs for alternative tiles
typedef struct {
    TileID *data;
    uint32_t length;
    uint32_t capacity;
} Tile;

// Represents middle abstraction between Tilemap & Tile.
// It hold a set of tiles and used to calculate random alternative tiles
typedef struct {
    Tile *data;
    uint32_t length;
    uint32_t capacity;
    uint32_t random_seed;
} Tileset;

// Grid map of IDs. Tilemap->Tile
typedef struct {
    vec2i position;
    vec2i size;
    vec2i tile_size;
    TileID *grid; // pointer to a buffer
    uint32_t capacity; // size of the grid buffer
} Tilemap;

// Auto tiling rule
typedef struct {
    TileID id;
    vec2i offset;
    bool exclude;
} RuleMatch;

// Auto tiling rule
// tile_id - applied tile_id when rules are satisfied
// group_id - id checked with included & excluded arrays
typedef struct {
    TileID tile_id;
    RuleMatch *match;
    uint32_t capacity;
} AutotileRule;


typedef enum {
    // Default TileID
    TileRandType_NONE,
    // Using TileSet.random_seed. it is deterministic, if used same grid drawing conditions
    TileRandType_SEED,
    // Using deterministic Tileset.random_seed + cell X & Y
    TileRandType_XY,
} TileRandType;

// Used for tilemap editing interactions
typedef enum {
    tmInputState_NONE,
    tmInputState_START,
    tmInputState_HOLD,
    tmInputState_RELEASE,
} InputState;

// Used for tilemap editing interactions
typedef enum {
    tmInputDirection_NONE,
    tmInputDirection_INCREASE,
    tmInputDirection_DECREASE,
} InputType;

// Remove name mangling for C++
#ifdef __cplusplus
extern "C" {
#endif

#ifdef STATIC_API
#define TMAPI static
#else
#define TMAPI
#endif

TMAPI void TileAtlasInit(TileAtlas *tile_atlas, vec2 tile_size, vec2 *buffer, size_t size);
TMAPI void TileAtlasInsert(TileAtlas *tile_atlas, vec2 texture_position, uint32_t index);
TMAPI void TileAtlasRemoveIndex(TileAtlas *tile_atlas, uint32_t index);

TMAPI void TileInit(Tile *tile, TileID *index_buffer, size_t size, uint32_t initial_length);
TMAPI Tile TileNewDefault(TileID *index_buffer, size_t size, uint32_t initial_length, TileID tile_id);
TMAPI void TileAppend(Tile *tile, TileID tile_id);
TMAPI void TileRemoveId(Tile *tile, TileID tile_id);
TMAPI void TileRemoveIndex(Tile *tile, uint32_t index);
TMAPI TileID TileGetId(Tile *tile);                                 // Get default 0th ID or TILE_EMPTY if no IDs inside
TMAPI TileID TileGetRandomSeed(Tile *tile, uint32_t *seed);          // Seed is mutated // Use pointer to a seed copy when doing a batch of drawing to get repeatable results
TMAPI TileID TileGetRandomXY(Tile *tile, uint32_t seed, int32_t seed_x, int32_t seed_y);

TMAPI void TilesetInit(Tileset *tileset, Tile *buffer, size_t size, uint32_t initial_length);
TMAPI void TilesetAppend(Tileset *tileset, Tile tile);
TMAPI void TilesetInsert(Tileset *tileset, Tile tile, uint32_t index);
TMAPI void TilesetRemoveIndex(Tileset *tileset, uint32_t index);
TMAPI TileID TilesetGetId(Tileset *tileset, TileID tile_id);
TMAPI Tile TilesetGetTile(Tileset *tileset, TileID tile_id);
TMAPI TileID TilesetGetTileAltRandom(Tileset *tileset, TileID tile_id, uint32_t *seed); // Use copy of tileset.random_seed before each batch of fetching to get repeatable results
TMAPI TileID TilesetGetTileAltDeterministic(Tileset *tileset, TileID tile_id, int32_t seed_x, int32_t seed_y);

TMAPI Tilemap TilemapInit(vec2i position, vec2i size, vec2i tile_size, TileID *buffer, uint32_t capacity); // Optional initialization through a function
TMAPI recti TilemapRecti(Tilemap *tilemap); // Get rectangle representing tilemap's size
TMAPI void TilemapClear(Tilemap *tilemap); // Writes TILE_EMPTY on all cells
TMAPI TileID TilemapGetTile(Tilemap *tilemap, vec2i tile_pos); // Return TileID by using local tile coordinates
TMAPI TileID TilemapGetTileWorld(Tilemap *tilemap, vec2i world_pos); // Get TileID by world coordinates
TMAPI recti TilemapGetUsedRecti(Tilemap *tilemap); // Get rectangle region of populated tiles
TMAPI recti TilemapClampRecti(Tilemap *tilemap, recti relative_rect); // Get rectangle that is clipped to tilemap
TMAPI void TilemapSetTile(Tilemap *tilemap, vec2i tile_pos, TileID tile_id); // Set tile id in tile coordinates
TMAPI vec2i TilemapGetWorld2Tile(Tilemap *tilemap, vec2i world_pos); // Translate world coordinates to tile coordinates
TMAPI void TilemapSetTileWorld(Tilemap *tilemap, vec2i world_pos, TileID tile_id); // Set tile id in world coordinates
TMAPI vec2i TilemapGetTile2World(Tilemap *tilemap, vec2i tile_pos); // Translate tile coordinates to world coordinates
TMAPI void TilemapSetTileIdBlock(Tilemap *tilemap, int32_t left_x, int32_t top_y, int32_t columns, int32_t rows, TileID tile_id); // Fills TileID in region
TMAPI void TilemapResize(Tilemap *tilemap, recti relative_rect, TileID *temp_buffer, uint32_t capacity); // Change position & size to fit rectangle, excess gets clipped
TMAPI void TilemapGetRegionData(Tilemap *tilemap, recti rect_section, TileID *out_buffer, uint32_t capacity); // Copy TileID from region and place as 1D array in out_buffer
TMAPI void TilemapSetRegionData(Tilemap *tilemap, recti rect_section, TileID *in_buffer, uint32_t capacity, bool write_empty); // Copy TileID from in_buffer assuming data is 1D array representing provided region in tile coordinates
TMAPI void TilemapSetData(Tilemap *tilemap, TileID *in_buffer, uint32_t capacity, uint32_t data_width, uint32_t data_height); // Copy TileID as 1D array from in_buffer starting from 0th index

TMAPI void AutotileRuleUpdateCell(Tilemap *tilemap_in, Tilemap *tilemap_out, AutotileRule *rules, uint32_t rule_count, vec2i tile_pos); // Process single cell for tilemap_out
TMAPI void AutotileRuleUpdateTilemap(Tilemap *tilemap_in, Tilemap *tilemap_out, AutotileRule *rules, uint32_t rule_count);
TMAPI void AutotileRuleUpdateRect(Tilemap *tilemap_in, Tilemap *tilemap_out, AutotileRule *rules, uint32_t rule_count, recti region);
TMAPI void AutotileRuleUpdateNeighbours(Tilemap *tilemap_in, Tilemap *tilemap_out, AutotileRule *rules, uint32_t rule_count, vec2i tile_pos);

TMAPI uint32_t tmRndState(uint32_t *nProcGen);                     // Random number using seed state: determenistic using same seed state
TMAPI int32_t tmRndInt(uint32_t *seed, int32_t min, int32_t max);  // Random signed integer range number using seed state
TMAPI int tmRndCash(uint32_t seed, int x, int y);                  // cash stands for chaos hash :D

TMAPI InputState GetInputState(bool is_pressed, bool is_held, bool is_released);
TMAPI InputType GetInputType(bool is_increase, bool is_decrease);
TMAPI void CreateSelection(Tilemap *tilemap, vec2i input_position, vec2i *position_state, recti *rect_state, InputState input_state); // Draw a rectangle around tiles from press to release
TMAPI void DragTiles(Tilemap *tilemap,Tilemap *temp_tilemap_out,vec2i input_position,vec2i *drag_start_position,recti *selection_rect,InputState input_state,bool remove_from_source,bool write_empty,TileID *temp_buffer,uint32_t capacity); // Copy tiles inside map_rect and drop them when released
TMAPI void EditTiles(Tilemap *tilemap,vec2i input_position,TileID *tile_id_state,InputType input_type); // Increase or decrease tile ID under input_position 
TMAPI void PaintTiles(Tilemap *tilemap,vec2i input_position,vec2i *state_position,TileID tile_id_input,InputState input_state); // Draw with tile_id
TMAPI void MoveTilemap(Tilemap *tilemap,vec2i input_position,vec2i *drag_start_position,vec2i *map_start_position,InputState input_state,bool grid_lock); // Drag'n'Drop a tilemap

#ifdef __cplusplus
}
#endif
#endif // TILEMAP_H

/* ------------------------------------- */

#ifdef TILEMAP_IMPLEMENTATION
#undef TILEMAP_IMPLEMENTATION

#include "assert.h"

// clip off that is not inside the clip recti
static void RectiClipRecti(recti *clip, recti *rectangle) {
    if (rectangle->x < clip->x) {
        uint32_t diff = clip->x - rectangle->x;
        rectangle->w -= diff;
        rectangle->x = 0;
    }
    if (rectangle->y < clip->y) {
        uint32_t diff = clip->y - rectangle->y;
        rectangle->h -= diff;
        rectangle->y = 0;
    }
    if (rectangle->x + rectangle->w > clip->w) {
        uint32_t diff = (rectangle->x + rectangle->w) - clip->w;
        rectangle->w -= diff;
    }
    if (rectangle->y + rectangle->h > clip->h) {
        uint32_t diff = (rectangle->y + rectangle->h) - clip->h;
        rectangle->h -= diff;
    }
    if (rectangle->w < 0) {
        rectangle->w = 0;
    }
    if (rectangle->h < 0) {
        rectangle->h = 0;
    }
}

// Create a rectangle from two points
static recti RectiFromRange(vec2i from, vec2i to) {
    recti result = {};

    if (to.x < from.x){
        result.x = to.x;
        result.w = from.x - to.x;
    } else
    {
        result.x = from.x;
        result.w = to.x - from.x;
    }

    if (to.y < from.y){
        result.y = to.y;
        result.h = from.y - to.y;
    } else
    {
        result.y = from.y;
        result.h = to.y - from.y;
    }

    result.w += 1;
    result.h += 1;
    return result;
}

// Random number using seed state: determenistic using same seed state
TMAPI uint32_t tmRndState(uint32_t *nProcGen) {
    /* Taken from OneLoneCoder: https://github.com/OneLoneCoder/Javidx9/blob/0c8ec20a9ed3b2daf76a925034ac5e7e6f4096e0/PixelGameEngine/SmallerProjects/OneLoneCoder_PGE_ProcGen_Universe.cpp#L183 */
    *nProcGen += 0xe120fc15;
    uint64_t tmp;
    tmp = (uint64_t)*nProcGen * 0x4a39b70d;
    uint32_t m1 = (tmp >> 32) ^ tmp;
    tmp = (uint64_t)m1 * 0x12fad5c9;
    uint32_t m2 = (tmp >> 32) ^ tmp;
    return m2;
}

// Random signed integer range number using seed state
TMAPI int32_t tmRndInt(uint32_t *seed, int32_t min, int32_t max) {
    uint32_t num = tmRndState(seed);
    int32_t result = (int32_t)(num % (uint32_t)(max - min)) + min;
    return result;
}

// cash stands for chaos hash :D
TMAPI int tmRndCash(uint32_t seed, int x, int y) {
    /* https://stackoverflow.com/a/37221804 */
    int h = seed + x*374761393 + y*668265263; // all constants are prime
    h = (h^(h >> 13))*1274126177;
    return h^(h >> 16);
}

/* ----- ATLAS ----- */
TMAPI void TileAtlasInit(TileAtlas *tile_atlas, vec2 tile_size, vec2 *buffer, size_t size) {
    tile_atlas->data = buffer;
    tile_atlas->length = 0;
    tile_atlas->capacity = size;
    tile_atlas->tile_size = tile_size;
}

TMAPI void TileAtlasInsert(TileAtlas *tile_atlas, vec2 texture_position, uint32_t index) {
    if (tile_atlas->length > tile_atlas->capacity - 1){
        assert(false);
        return;
    }
    if (index > tile_atlas->length || index < 0){
        assert(false);
        return;
    }
    if (index == tile_atlas->length) {
        tile_atlas->data[index] = texture_position;
        tile_atlas->length += 1;
        return;
    }

    for (uint32_t i = tile_atlas->length; i > index; i -= 1) {
        tile_atlas->data[i] = tile_atlas->data[i - 1];
    }
    
    tile_atlas->data[index] = texture_position;
    tile_atlas->length += 1;
}

TMAPI void TileAtlasRemoveIndex(TileAtlas *tile_atlas, uint32_t index) {
    for (uint32_t i = index; i < tile_atlas->length -1; i += 1) {
        tile_atlas->data[i] = tile_atlas->data[i +1];
    }
    tile_atlas->length -= 1;
}

/* ----- TILES ----- */

TMAPI void TileInit(Tile *tile, TileID *index_buffer, size_t size, uint32_t initial_length) {
    tile->data = index_buffer;
    tile->length = initial_length;
    tile->capacity = size;
}

TMAPI Tile TileNewDefault(TileID *index_buffer, size_t size, uint32_t initial_length, TileID tile_id) {
    Tile tile = {
        index_buffer,
        initial_length,
        size,
    };
    tile.data[0] = tile_id;
    return tile;
}

TMAPI void TileAppend(Tile *tile, TileID tile_id) {
    if (tile->length > tile->capacity -1) {
        assert(false);
        return;
    }
    tile->data[tile->length] = tile_id;
    tile->length += 1;
}

TMAPI void TileRemoveId(Tile *tile, TileID tile_id) {
    bool is_removed;
    uint32_t i = 0;
    for (; i < tile->length; ) {
        if (tile->data[i] == tile_id) {
            is_removed = true;
            break;
        }
        i += 1;
    }
    if (!is_removed){
        return;
    }

    for (; i < tile->length -1; ) {
        tile->data[i] = tile->data[i+1];
        i += 1;
    }
    tile->length -= 1;
}

TMAPI void TileRemoveIndex(Tile *tile, uint32_t index) {
    for (uint32_t i = index; i < tile->length -1; i += 1) {
        tile->data[i] = tile->data[i +1];
    }
    tile->length -= 1;
}

// Get default 0th ID or TILE_EMPTY if no IDs inside
TMAPI TileID TileGetId(Tile *tile) {
    if (tile->length == 0) {
        assert(false);
        return TILE_EMPTY;
    }
    TileID result = tile->data[0];
    return result;
}

// Use pointer to a seed copy when doing a batch of drawing to get repeatable results
// Seed is mutated
TMAPI TileID TileGetRandomSeed(Tile *tile, uint32_t *seed) {
    if (tile->length == 0) {
        assert(false);
        return TILE_EMPTY;
    }
    uint32_t index_rnd = (uint32_t)tmRndInt(seed, 0, tile->length);
    TileID result = tile->data[index_rnd];
    return result;
}

TMAPI TileID TileGetRandomXY(Tile *tile, uint32_t seed, int32_t seed_x, int32_t seed_y) {
    if (tile->length == 0) {
        assert(false);
        return TILE_EMPTY;
    }
    uint32_t index_rnd = tmRndCash(seed, seed_x, seed_y) % tile->length;
    TileID result = tile->data[index_rnd];
    return result;
}

/* ----- Tile Set ----- */

TMAPI void TilesetInit(Tileset *tileset, Tile *buffer, size_t size, uint32_t initial_length) {
    tileset->data = buffer;
    tileset->length = initial_length;
    tileset->capacity = size;
    tileset->random_seed = 0;
}

TMAPI void TilesetAppend(Tileset *tileset, Tile tile) {
    if (tileset->length > tileset->capacity -1) {
        assert(false);
        return;
    }
    tileset->data[tileset->length] = tile;
    tileset->length += 1;
}

TMAPI void TilesetInsert(Tileset *tileset, Tile tile, uint32_t index) {
    if (tileset->length > tileset->capacity -1) {
        assert(false);
        return;
    }
    for (uint32_t i = tileset->length; i > index; i -= 1) {
        tileset->data[i] = tileset->data[i - 1];
    }
    
    tileset->data[index] = tile;
    tileset->length += 1;
}

TMAPI void TilesetRemoveIndex(Tileset *tileset, uint32_t index) {
    for (uint32_t i = index; i < tileset->length -1; i += 1) {
        tileset->data[i] = tileset->data[i +1];
    }
    tileset->length -= 1;
}

TMAPI TileID TilesetGetId(Tileset *tileset, TileID tile_id) {
    assert(tile_id != TILE_INVALID);
    if (tile_id > (TileID)(tileset->length - 1)) {
        return TILE_EMPTY;
    }
    TileID result = tileset->data[tile_id].data[0];
    return result;
}

TMAPI Tile TilesetGetTile(Tileset *tileset, TileID tile_id) {
    assert(tile_id != TILE_INVALID);
    if (tile_id > (TileID)(tileset->length - 1)) {
        return (Tile){NULL};
    }
    Tile result = tileset->data[tile_id];
    return result;
}

// Seed is mutated
// Use copy of tileset.random_seed before each batch of fetching to get repeatable results
TMAPI TileID TilesetGetTileAltRandom(Tileset *tileset, TileID tile_id, uint32_t *seed) {
    assert(tile_id != TILE_INVALID);
    if (tile_id > (TileID)(tileset->length - 1)) {
        return TILE_EMPTY;
    }
    Tile *tile = &tileset->data[tile_id];
    uint32_t index_rnd = (uint32_t)tmRndInt(seed, 0, (int32_t)(tile->length - 1));
    TileID result = tile->data[index_rnd];
    return result;
}

TMAPI TileID TilesetGetTileAltDeterministic(Tileset *tileset, TileID tile_id, int32_t seed_x, int32_t seed_y) {
    assert(tile_id != TILE_INVALID);
    if (tile_id > (TileID)(tileset->length - 1)) {
        return TILE_EMPTY;
    }
    Tile *tile = &tileset->data[tile_id];
    uint32_t index_rnd = tmRndCash(tileset->random_seed, (int32_t)seed_x, (int32_t)seed_y) % tile->length;
    TileID result = tile->data[index_rnd];
    return result;
}

/* ----- Tile Map ----- */

// Optional initialization through a function
TMAPI Tilemap TilemapInit(vec2i position, vec2i size, vec2i tile_size, TileID *buffer, uint32_t capacity) {
    return (Tilemap){ position, size, tile_size, buffer, capacity };
}

// Get rectangle representing tilemap's size
TMAPI recti TilemapRecti(Tilemap *tilemap) {
    return (recti){0, 0, tilemap->size.x, tilemap->size.y};
}

// Writes TILE_EMPTY on all cells
TMAPI void TilemapClear(Tilemap *tilemap) {
    for (uint32_t i = 0; i < tilemap->capacity; i += 1){
        tilemap->grid[i] = TILE_EMPTY;
    }
}

// Return TileID by using local tile coordinates
TMAPI TileID TilemapGetTile(Tilemap *tilemap, vec2i tile_pos) {
    if ( tile_pos.x < 0 || tile_pos.y < 0 ) { return TILE_INVALID; }
    if (tile_pos.x > tilemap->size.x - 1 || tile_pos.y > tilemap->size.y - 1) { return TILE_INVALID; }
    TileID result = tilemap->grid[tilemap->size.x * tile_pos.y + tile_pos.x];
    return result;
}

// Get TileID by world coordinates
TMAPI TileID TilemapGetTileWorld(Tilemap *tilemap, vec2i world_pos) {
    vec2i relative_pos = {world_pos.x - tilemap->position.x, world_pos.y - tilemap->position.y};
    vec2i tile_pos = {relative_pos.x / tilemap->tile_size.x, relative_pos.y / tilemap->tile_size.y};
    TileID result = TilemapGetTile(tilemap, tile_pos);
    return result;
}

// Get rectangle region of populated tiles 
TMAPI recti TilemapGetUsedRecti(Tilemap *tilemap) {
    int32_t left = tilemap->size.x - 1;
    int32_t top = tilemap->size.y - 1;
    int32_t right = 0;
    int32_t bottom = 0;
    for (int32_t y = 0; y < tilemap->size.y; y += 1) {
        for (int32_t x = 0; x < tilemap->size.x; x += 1) {
            if (tilemap->grid[tilemap->size.x * y + x] != TILE_EMPTY) {
                if (x > right) {
                    right = x;
                }
                if (y > bottom) {
                    bottom = y;
                }
                if (x < left) {
                    left = x;
                }
                if (y < top) {
                    top = y;
                }
            }
        }
    }

    if (left == tilemap->size.x && right == 0) {
        // No used tiles
        recti result = { tilemap->position.x, tilemap->position.y, 0, 0 };
        return result;
    }

    recti result = { left, top, right - left +1, bottom - top +1 };
    return result;
}

// Get rectangle that is clipped to tilemap
TMAPI recti TilemapClampRecti(Tilemap *tilemap, recti relative_rect) {
    recti result = relative_rect;
    if (result.x + result.w < 0         \
        || result.y + result.h < 0      \
        || result.x >= tilemap->size.x   \
        || result.y >= tilemap->size.y
    ) {
        result.w = 0.0;
        result.h = 0.0;
        return result;
    }

    if (result.x < 0){
        result.w -= result.x;
        result.x = 0.0;
    }
    if (result.y < 0){
        result.h -= result.y;
        result.y = 0.0;
    }
    if (result.x + result.w < 0){
        result.w -= tilemap->size.x - (result.x + result.w);
    }
    if (result.y + result.h < 0){
        result.h -= tilemap->size.y - (result.y + result.h);
    }

    return result;
}

// Set tile id in tile coordinates
TMAPI void TilemapSetTile(Tilemap *tilemap, vec2i tile_pos, TileID tile_id) {
    if (tile_id == TILE_INVALID) {
        return;
    }
    bool x_inside = tile_pos.x > -1 && tile_pos.x < tilemap->size.x;
    bool y_inside = tile_pos.y > -1 && tile_pos.y < tilemap->size.y;
    if (!x_inside || !y_inside){
        return;
    }
    int32_t pos = tile_pos.x + tile_pos.y * tilemap->size.x;
    tilemap->grid[pos] = tile_id;
}

// Translate world coordinates to tile coordinates
TMAPI vec2i TilemapGetWorld2Tile(Tilemap *tilemap, vec2i world_pos) {
    int32_t x = world_pos.x - tilemap->position.x;
    int32_t y = world_pos.y - tilemap->position.y;
    if (x < 0){
        x -= tilemap->tile_size.x;
    }
    if (y < 0){
        y -= tilemap->tile_size.y;
    }
    
    x /= tilemap->tile_size.x;
    y /= tilemap->tile_size.y;
    vec2i result = {x, y};
    return result;
}

// Set tile id in world coordinates
TMAPI void TilemapSetTileWorld(Tilemap *tilemap, vec2i world_pos, TileID tile_id) {
    vec2i tile_pos = TilemapGetWorld2Tile(tilemap, world_pos);
    TilemapSetTile(tilemap, tile_pos, tile_id);
}

// Translate tile coordinates to world coordinates
TMAPI vec2i TilemapGetTile2World(Tilemap *tilemap, vec2i tile_pos) {
    int32_t x = tile_pos.x * tilemap->tile_size.x + tilemap->position.x;
    int32_t y = tile_pos.y * tilemap->tile_size.y + tilemap->position.y;
    vec2i result = {x, y};
    return result;
}

// Fills TileID in region
TMAPI void TilemapSetTileIdBlock(Tilemap *tilemap, int32_t left_x, int32_t top_y, int32_t columns, int32_t rows, TileID tile_id) {
    if (tile_id == TILE_INVALID) {
        return;
    }

    vec2i from = {
        left_x > 0 ? left_x : 0,
        top_y > 0 ? top_y : 0,
    };
    // excluding
    vec2i to = {
        (from.x + columns) < tilemap->size.x ? (from.x + columns) : tilemap->size.x,
        (from.y + rows) < tilemap->size.y ? (from.y + rows) : tilemap->size.y,
    };

    for (int32_t y = from.y; y < to.y; y += 1) {
        for (int32_t x = from.x; x < to.x; x += 1) {
            int32_t i = x + y * tilemap->size.x;
            tilemap->grid[i] = tile_id;
        }
    }
}

// Change position & size to fit rectangle, excess gets clipped
// Requires a temporary buffer to hold used rectangle
TMAPI void TilemapResize(Tilemap *tilemap, recti relative_rect, TileID *temp_buffer, uint32_t capacity) {
    recti used_rect = TilemapGetUsedRecti(tilemap);
    assert(capacity >= used_rect.w * used_rect.h);

    TilemapGetRegionData(tilemap, used_rect, temp_buffer, capacity);
    // TODO: assert that current tilemap.grid is big enough for the new size
    tilemap->position.x += relative_rect.x * tilemap->tile_size.x;
    tilemap->position.y += relative_rect.y * tilemap->tile_size.y;
    tilemap->size.x = relative_rect.w;
    tilemap->size.y = relative_rect.h;
    TilemapClear(tilemap);

    used_rect.x -= relative_rect.x;
    used_rect.y -= relative_rect.y;
    bool write_empty = true;
    TilemapSetRegionData(tilemap, used_rect, temp_buffer, capacity, write_empty);
}

// Copy TileID from region and place as 1D array in out_buffer
TMAPI void TilemapGetRegionData(Tilemap *tilemap, recti rect_section, TileID *out_buffer, uint32_t capacity) {
    recti rect = rect_section;
    assert(rect.w * rect.h <= capacity);
    assert(rect.x >= 0);
    assert(rect.y >= 0);
    assert(rect.x + rect.w <= tilemap->size.x);
    assert(rect.y + rect.h <= tilemap->size.y);

    // place int 1D buffer
    uint32_t i = 0;
    for (int32_t y = rect.y; y < rect.y + rect.h; y += 1) {
        for (int32_t x = rect.x; x < rect.x + rect.w; x += 1) {
            TileID value = tilemap->grid[x + y * tilemap->size.x];
            out_buffer[i] = value;
            i += 1;
        }
    }
}

// Copy TileID from in_buffer assuming data is 1D array representing provided region in tile coordinates
TMAPI void TilemapSetRegionData(Tilemap *tilemap, recti rect_section, TileID *in_buffer, uint32_t capacity, bool write_empty) {
    recti rect = rect_section;
    if (rect.w < 1 || rect.h < 1) {return;}
    if (rect.x > tilemap->size.x - 1) {return;}
    if (rect.y > tilemap->size.y - 1) {return;}

    int32_t right = rect.x + rect.w;
    if (right - 1 < 0) {return;}
    int32_t bottom = rect.y + rect.h;
    if (bottom - 1 < 0) {return;}

    int32_t tile_x = rect.x >= 0 ? rect.x : 0;
    int32_t tile_y = rect.y >= 0 ? rect.y : 0;
    int32_t tile_r = right < tilemap->size.x ? right : tilemap->size.x;
    int32_t tile_b = bottom < tilemap->size.y ? bottom : tilemap->size.y;

    for (int32_t y = tile_y; y < tile_b; y += 1) {
        int32_t diff_y = y - rect.y;
        for (int32_t x = tile_x; x < tile_r; x += 1) {
            int32_t diff_x = x - rect.x;
            uint32_t data_i = (uint32_t)(diff_x + diff_y * rect.w);
            TileID value = in_buffer[data_i];
            if ((value == TILE_EMPTY) && !write_empty){
                continue;
            }
            uint32_t tile_i = (uint32_t)(x + y * tilemap->size.x);
            tilemap->grid[tile_i] = value;
        }
    }
}

// Copy TileID as 1D array from in_buffer starting from 0th index
TMAPI void TilemapSetData(Tilemap *tilemap, TileID *in_buffer, uint32_t capacity, uint32_t data_width, uint32_t data_height) {
    uint32_t width = data_width < (uint32_t)tilemap->size.x ? data_width : (uint32_t)tilemap->size.x;
    uint32_t height = data_height < (uint32_t)tilemap->size.y ? data_height : (uint32_t)tilemap->size.y;

    for (uint32_t y = 0; y < height; y += 1) {
        for (uint32_t x = 0; x < width; x += 1) {
            uint32_t data_i = x + y * data_width;
            uint32_t tile_i = x + y * (uint32_t)tilemap->size.x;
            TileID value = in_buffer[data_i];
            tilemap->grid[tile_i] = value;
        }
    }
}

/* ----- Ruletile ------ */

// Process single cell for tilemap_out
TMAPI void AutotileRuleUpdateCell(Tilemap *tilemap_in, Tilemap *tilemap_out, AutotileRule *rules, uint32_t rule_count, vec2i tile_pos) {
    assert(tile_pos.x >= 0);
    assert(tile_pos.y >= 0);
    assert(tile_pos.x < tilemap_in->tile_size.x);
    assert(tile_pos.y < tilemap_in->tile_size.y);
    assert(tile_pos.x < tilemap_out->tile_size.x);
    assert(tile_pos.y < tilemap_out->tile_size.y);

    AutotileRule *rule;
    TileID test_id;
    bool is_matching;
    vec2i match_pos;
    RuleMatch *match_rule;
    
    int32_t index_out = tile_pos.x + tile_pos.y * tilemap_out->size.x;
    // TODO: remove
    TileID tile_in = TilemapGetTile(tilemap_in, tile_pos);
    if (tile_in != TILE_EMPTY) {
        tile_in = tile_in;
    }

    for (int32_t i; i < rule_count; i += 1) {
        rule = &rules[i];
        
        is_matching = true;
        for (int32_t j = 0; j < (int32_t)rule->capacity; j += 1) {
            match_rule = &rule->match[j];
            match_pos = (vec2i){tile_pos.x + match_rule->offset.x, tile_pos.y + match_rule->offset.y};
            
            // Check X boundry
            if (match_pos.x < 0 || match_pos.x > (tilemap_in->size.x - 1)) {
                if (!match_rule->exclude || match_rule->id == TILE_EMPTY) {
                    is_matching = false;
                    break;
                }
                continue;
            }
            // Check Y boundry
            if (match_pos.y < 0 || match_pos.y > (tilemap_in->size.y - 1)) {
                if (!match_rule->exclude || match_rule->id == TILE_EMPTY) {
                    is_matching = false;
                    break;
                }
                continue;
            }

            test_id = TilemapGetTile(tilemap_in, match_pos);
            if (test_id != match_rule->id && !match_rule->exclude) {
                is_matching = false;
                break;
            }
            if (test_id == match_rule->id && match_rule->exclude) {
                is_matching = false;
                break;
            }
        }
        if (!is_matching) {
            continue;
        }

        // Match found
        // TODO: optionaly random skip
        tilemap_out->grid[index_out] = rule->tile_id;
        break;
    }
}

TMAPI void AutotileRuleUpdateTilemap(Tilemap *tilemap_in, Tilemap *tilemap_out, AutotileRule *rules, uint32_t rule_count) {
    assert((tilemap_in->size.x == tilemap_out->size.x) && (tilemap_in->size.y == tilemap_out->size.y));
    assert(tilemap_in->capacity >= tilemap_out->capacity);

    vec2i tile_pos;
    for (int32_t y = 0; y < tilemap_in->size.y; y += 1) {
        for (int32_t x = 0; x < tilemap_in->size.x; x += 1) {
            tile_pos = (vec2i){x, y};
            AutotileRuleUpdateCell(tilemap_in, tilemap_out, rules, rule_count, tile_pos);
        }
    }
}

TMAPI void AutotileRuleUpdateRect(Tilemap *tilemap_in, Tilemap *tilemap_out, AutotileRule *rules, uint32_t rule_count, recti region) {
    recti rect = TilemapClampRecti(tilemap_in, region);
    vec2i tile_pos;
    for (int32_t y = rect.y; y < (rect.y + rect.h); y += 1) {
        for (int32_t x = rect.x; x < (rect.x + rect.x); x += 1) {
            tile_pos = (vec2i){x, y};
            AutotileRuleUpdateCell(tilemap_in, tilemap_out, rules, rule_count, tile_pos);
        }
    }
}

TMAPI void AutotileRuleUpdateNeighbours(Tilemap *tilemap_in, Tilemap *tilemap_out, AutotileRule *rules, uint32_t rule_count, vec2i tile_pos) {
    vec2i neighbour_list[] = {
        {tile_pos.x - -1, tile_pos.y - -1},
        {tile_pos.x - 0,  tile_pos.y - -1},
        {tile_pos.x - 1,  tile_pos.y - -1},
        {tile_pos.x - -1, tile_pos.y - 0},
        {tile_pos.x - 1,  tile_pos.y - 0},
        {tile_pos.x - -1, tile_pos.y - 1},
        {tile_pos.x - 0,  tile_pos.y - 1},
        {tile_pos.x - 1,  tile_pos.y - 1},
    };
    const char neighbour_count = 8;

    vec2i test_pos;
    for (char i = 0; i < neighbour_count; i += 1) {
        test_pos = neighbour_list[i];
        if (test_pos.x < 0 || test_pos.y < 0 ) {
            continue;
        }
        if (test_pos.x > tilemap_in->size.x -1 || test_pos.y > tilemap_in->size.y -1) {
            continue;
        }
        AutotileRuleUpdateCell(tilemap_in, tilemap_out, rules, rule_count, test_pos);
    }
}

/* ----- Tilemap editing ----- */

TMAPI InputState GetInputState(bool is_pressed, bool is_held, bool is_released) {
    if (is_pressed) {
        return tmInputState_START;
    }
    if (is_held) {
        return tmInputState_HOLD;
    }
    if (is_released) {
        return tmInputState_RELEASE;
    }
    return tmInputState_NONE;
}

TMAPI InputType GetInputType(bool is_increase, bool is_decrease) {
    if (is_increase) {
        return tmInputDirection_INCREASE;
    }
    if (is_decrease) {
        return tmInputDirection_DECREASE;
    }
    return tmInputDirection_NONE;
}

// Draw a rectangle around tiles from press to release
TMAPI void CreateSelection(
    Tilemap *tilemap, 
    vec2i input_position,
    vec2i *position_state,
    recti *rect_state,
    InputState input_state
) {
    if (input_state == tmInputState_START) {
        *position_state = TilemapGetWorld2Tile(tilemap, input_position);
    } else
    if (input_state == tmInputState_HOLD){
        vec2i drag_position = TilemapGetWorld2Tile(tilemap, input_position);
        *rect_state = RectiFromRange(*position_state, drag_position);
    }
}

// Copy tiles inside map_rect and drop them when released
TMAPI void DragTiles(
    Tilemap *tilemap,
    Tilemap *temp_tilemap_out,
    vec2i input_position,
    vec2i *drag_start_position,
    recti *selection_rect,
    InputState input_state,
    bool remove_from_source,
    bool write_empty,
    TileID *temp_buffer,
    uint32_t capacity
){
    if (selection_rect->w == 0 || selection_rect->h == 0){
        return;
    }
    if (input_state == tmInputState_START) {
        // trim selection outside borders
        recti tilemap_rect = TilemapRecti(tilemap);
        RectiClipRecti(&tilemap_rect, selection_rect);

        if (selection_rect->w == 0 || selection_rect->h == 0){
            // no selection
            return;
        }

        TilemapGetRegionData(tilemap, *selection_rect, temp_buffer, capacity);
        
        vec2i selection_position = {selection_rect->x, selection_rect->y};
        vec2i map_position = {tilemap->position.x + tilemap->tile_size.x * selection_position.x, tilemap->position.y + tilemap->tile_size.y * selection_position.y};
        vec2i map_size = {selection_rect->w, selection_rect->h};
        *temp_tilemap_out = TilemapInit(map_position, map_size, tilemap->tile_size, temp_buffer, capacity);
        // Alternatively - TilemapSetRegionData
        TilemapSetData(temp_tilemap_out, temp_buffer, capacity, (uint32_t)map_size.x, (uint32_t)map_size.y);

        vec2i tile_pos = TilemapGetWorld2Tile(tilemap, input_position);
        *drag_start_position = (vec2i){selection_position.x - tile_pos.x, selection_position.y - tile_pos.y};
    } else if (input_state == tmInputState_HOLD){

        vec2i tile_pos = TilemapGetWorld2Tile(tilemap, input_position);
        vec2i tile_offset = {tile_pos.x + drag_start_position->x, tile_pos.y + drag_start_position->y};
        temp_tilemap_out->position = (vec2i){tilemap->position.x + tile_offset.x * tilemap->tile_size.x, tilemap->position.y + tile_offset.y * tilemap->tile_size.y};
    } else if (input_state == tmInputState_RELEASE){
        // Place tile data
        vec2i tile_pos = TilemapGetWorld2Tile(tilemap, input_position);
        vec2i tile_offset = {tile_pos.x + drag_start_position->x, tile_pos.y + drag_start_position->y};
        temp_tilemap_out->position = (vec2i){tilemap->position.x + tile_offset.x * tilemap->tile_size.x, tilemap->position.y + tile_offset.y * tilemap->tile_size.y};

        if (remove_from_source) {
            TilemapSetTileIdBlock(tilemap, selection_rect->x, selection_rect->y, selection_rect->w, selection_rect->h, TILE_EMPTY);
        }

        recti data_rect = {
            tile_offset.x,
            tile_offset.y,
            selection_rect->w,
            selection_rect->h,
        };
        TilemapSetRegionData(tilemap, data_rect, temp_buffer, capacity, write_empty);

        temp_tilemap_out->size = (vec2i){0, 0};
        selection_rect->w = 0.0;
        selection_rect->h = 0.0;
    }
}

// Increase or decrease tile ID under input_position 
TMAPI void EditTiles(
    Tilemap *tilemap,
    vec2i input_position,
    TileID *tile_id_state,
    InputType input_type
) {
    if (input_type == tmInputDirection_NONE){
        return;
    }

    TileID tile_id = TilemapGetTileWorld(tilemap, input_position);
    if (tile_id == TILE_INVALID){
        return;
    }

    if (input_type == tmInputDirection_INCREASE && (tile_id + 1) != TILE_INVALID){
        *tile_id_state = tile_id + 1;
    } else if (input_type == tmInputDirection_DECREASE && (tile_id - 1) != TILE_INVALID){
        *tile_id_state = tile_id - 1;
    }

    TilemapSetTileWorld(tilemap, input_position, *tile_id_state);
}

// Draw with tile_id
TMAPI void PaintTiles(
    Tilemap *tilemap,
    vec2i input_position,
    vec2i *state_position,
    TileID tile_id_input,
    InputState input_state
) {
    if (tile_id_input == TILE_INVALID){
        return;
    }

    if (input_state == tmInputState_START){
        *state_position = TilemapGetWorld2Tile(tilemap, input_position);
        TileID tile_id = TilemapGetTile(tilemap, *state_position);
        if (tile_id == TILE_INVALID){
            return;
        }
        TilemapSetTile(tilemap, *state_position, tile_id_input);
    } else if (input_state == tmInputState_HOLD){
        vec2i new_position = TilemapGetWorld2Tile(tilemap, input_position);
        if ((new_position.x == state_position->x) && (new_position.y == state_position->y)){
            return;
        }
        *state_position = new_position;
        TileID tile_id = TilemapGetTile(tilemap, *state_position);
        if (tile_id == TILE_INVALID){
            return;
        }
        TilemapSetTile(tilemap, *state_position, tile_id_input);
    }
}

// Drag'n'Drop a tilemap
TMAPI void MoveTilemap(
    Tilemap *tilemap,
    vec2i input_position,
    vec2i *drag_start_position,
    vec2i *map_start_position,
    InputState input_state,
    bool grid_lock
){
    if (input_state == tmInputState_START) {
        *map_start_position = tilemap->position;
        *drag_start_position = input_position;
    } else if (input_state == tmInputState_HOLD) {
        vec2i drag_difference = {input_position.x - drag_start_position->x, input_position.y - drag_start_position->y};
        if (!grid_lock) {
            tilemap->position = (vec2i){map_start_position->x + drag_difference.x, map_start_position->y + drag_difference.y};
            return;
        }
        vec2i tile_difference = {drag_difference.x / tilemap->tile_size.x, drag_difference.y / tilemap->tile_size.y};
        if (drag_difference.x < 0 ) {
            tile_difference.x -= 1;
        }
        if (drag_difference.y < 0 ) {
            tile_difference.y -= 1;
        }

        tilemap->position = (vec2i){map_start_position->x + tile_difference.x * tilemap->tile_size.x, map_start_position->y + tile_difference.y * tilemap->tile_size.y};
    }
}

#endif // TILEMAP_IMPLEMENTATION
