#ifndef TILEMAP_RAYLIB_H
#define TILEMAP_RAYLIB_H

#include "tilemap.h"
#include "raylib.h"


// Remove name mangling for C++
#ifdef __cplusplus
extern "C" {
#endif

#ifdef STATIC_API
#define TMRAPI static
#else
#define TMRAPI
#endif

TMRAPI void DrawTileAtlas(TileAtlas *tile_atlas, TileID tile_id, Vector2 draw_pos, Texture2D *texture);
TMRAPI void DrawTile(Tile *tile, TileAtlas *tile_atlas, Vector2 draw_pos, Texture2D *texture, TileID subtile);
TMRAPI void DrawTileRand(Tile *tile, TileAtlas *tile_atlas, Vector2 draw_pos, TileRandType rand_type, uint32_t *seed, Texture2D *texture);
TMRAPI void DrawTilemapGrid(Tilemap *tilemap, Color color); // Draw 2D grid lines
TMRAPI void DrawTilemapTileId(Tilemap *tilemap, Font font, int32_t font_size, Color color); // Draw ID on tile positions for whole tilemap
TMRAPI void DrawTilemapCellRect(Tilemap *tilemap, vec2i world_pos, TileID tile_id, Font font, int32_t font_size, Color color); // Draw rectangle around tile and draw provided ID
TMRAPI void DrawTilemapSelection(Tilemap *tilemap, recti rect, Color color); // Draw lines around selection
TMRAPI void DrawTilemap(Tilemap *tilemap, Tileset *tileset, TileAtlas *tile_atlas, bool skip_zero, TileRandType rand_type,Texture2D *texture); // skip_zero = true if TILE_EMPTY doesn't map to tile_atlas
TMRAPI void DrawTilemapRecti(Tilemap *tilemap, Tileset *tileset, TileAtlas *tile_atlas, bool skip_zero, TileRandType rand_type, recti region_rect,Texture2D *texture); // Draw selected region. For optimization draw only what is on a screen.


#ifdef __cplusplus
}
#endif
#endif // TILEMAP_RAYLIB_H

/* ------------------------------------------- */


#ifdef TILEMAP_RAYLIB_IMPLEMENTATION
#undef TILEMAP_RAYLIB_IMPLEMENTATION

// Drawing a tile from atlas directly
TMRAPI void DrawTileAtlas(TileAtlas *tile_atlas, TileID tile_id, Vector2 draw_pos, Texture2D *texture) {
	Vector2 tex_pos = *(Vector2*)& tile_atlas->data[tile_id];
	Rectangle tex_rect = {tex_pos.x,tex_pos.y, tile_atlas->tile_size.x, tile_atlas->tile_size.y};

	DrawTextureRec(*texture, tex_rect, draw_pos, WHITE);
}

TMRAPI void DrawTile(Tile *tile, TileAtlas *tile_atlas, Vector2 draw_pos, Texture2D *texture, TileID subtile) {
    TileID tile_id = tile->data[subtile];
    DrawTileAtlas(tile_atlas, tile_id, draw_pos, texture);
}

TMRAPI void DrawTileRand(Tile *tile, TileAtlas *tile_atlas, Vector2 draw_pos, TileRandType rand_type, uint32_t *seed, Texture2D *texture) {
    TileID tile_id;
    switch(rand_type) {
    case TileRandType_NONE:
        tile_id = TileGetId(tile);
        break;
    case TileRandType_SEED:
        tile_id = TileGetRandomSeed(tile, seed);
        break;
    case TileRandType_XY:
        tile_id = TileGetRandomXY(tile, *seed, (int32_t)draw_pos.x, (int32_t)draw_pos.y);
        break;
    }
    DrawTileAtlas(tile_atlas, tile_id, draw_pos, texture);
}

// Draw 2D grid lines
TMRAPI void DrawTilemapGrid(Tilemap *tilemap, Color color) {
    int32_t map_width = tilemap->size.x * tilemap->tile_size.x;
    int32_t map_height = tilemap->size.y * tilemap->tile_size.y;

    // Vertical lines
    for (int32_t x = 0; x < tilemap->size.x + 1; x += 1) {
        int32_t cell_x = tilemap->position.x + x * tilemap->tile_size.x;
        DrawLine(cell_x, tilemap->position.y, (cell_x + 1), (tilemap->position.y + map_height), color);
    }

    // Horizontal lines
    for (int32_t y = 0; y < tilemap->size.y + 1; y += 1) {
        int32_t cell_y = tilemap->position.y + y * tilemap->tile_size.y;
        DrawLine(tilemap->position.x, cell_y, (tilemap->position.x + map_width), cell_y, color);
    }
}

// Draw ID on tile positions for whole tilemap
TMRAPI void DrawTilemapTileId(Tilemap *tilemap, Font font, int32_t font_size, Color color) {
    int32_t text_offset_y = (tilemap->tile_size.y - font_size) / 2;

    for (int32_t y = 0; y < tilemap->size.y; y += 1) {
        int32_t cell_y = tilemap->position.y + y * tilemap->tile_size.y;
        for (int32_t x; x < tilemap->size.x; x += 1) {
            int32_t cell_x = tilemap->position.x + x * tilemap->tile_size.x;
            int32_t cell_i = x + y * tilemap->size.x;
            assert(cell_i < (int32_t)tilemap->capacity);

            TileID cell_id = tilemap->grid[cell_i];
            if (cell_id == 0) {
                continue; // skip EMPTY
            }
            const char *text = TextFormat("%d", cell_id);
            Vector2 text_measure = MeasureTextEx(font, text, (int32_t)font_size, 0.0);
            int32_t text_offset_x = (tilemap->tile_size.x - (int32_t)text_measure.x) / 2;
            Vector2 text_position = {(float)(cell_x + text_offset_x), (float)(cell_y + text_offset_y + 1)};
            DrawTextEx(font, text, text_position, (float)font_size, 0.0, color);
        }
    }
}

// Draw rectangle around tile and draw provided ID
TMRAPI void DrawTilemapCellRect(Tilemap *tilemap, vec2i world_pos, TileID tile_id, Font font, int32_t font_size, Color color) {
    vec2i tile_pos = TilemapGetWorld2Tile(tilemap, world_pos);
    int32_t tile_x = tilemap->position.x + tile_pos.x * tilemap->tile_size.x;
    int32_t tile_y = tilemap->position.y + tile_pos.y * tilemap->tile_size.y;
    DrawRectangleLines(tile_x, tile_y, tilemap->tile_size.x, tilemap->tile_size.y, color);

    const char *text = TextFormat("%d", tile_id);
    Vector2 text_measure = MeasureTextEx(font, text, (float)font_size, 0.0);
    int32_t text_offset_x = (tilemap->tile_size.x - (int32_t)text_measure.x) / 2;
    int32_t text_offset_y = (tilemap->tile_size.y - font_size) / 2;
    Vector2 text_position = {(float)(tile_x + text_offset_x), (float)(tile_y + text_offset_y)};
    DrawTextEx(font, text, text_position, (float)font_size, 0.0, color);
}

// Draw lines around selection
TMRAPI void DrawTilemapSelection(Tilemap *tilemap, recti rect, Color color) {
    Rectangle rectangle = {
        (float)(tilemap->position.x + rect.x * tilemap->tile_size.x),
        (float)(tilemap->position.y + rect.y * tilemap->tile_size.y),
        (float)(rect.w * tilemap->tile_size.x),
        (float)(rect.h * tilemap->tile_size.y),
    };
    DrawRectangleLinesEx(rectangle, 1.0, color);
}


// skip_zero = true if TILE_EMPTY doesn't map to tile_atlas
TMRAPI void DrawTilemap(
    Tilemap *tilemap, 
    Tileset *tileset, 
    TileAtlas *tile_atlas, 
    bool skip_zero, 
    TileRandType rand_type,
    Texture2D *texture
) {
    rectf tex_rect = {0.0, 0.0, tile_atlas->tile_size.x, tile_atlas->tile_size.y};
    uint32_t seed = tileset->random_seed;

    for (int32_t y = 0; y < tilemap->size.y; y += 1) {
        int32_t cell_y = tilemap->position.y + y * tilemap->tile_size.y;
        for (int32_t x = 0; x < tilemap->size.x; x += 1) {
            int32_t cell_x = tilemap->position.x + x * tilemap->tile_size.x;
            int32_t cell_i = x + y * tilemap->size.x;
            TileID cell_id = tilemap->grid[cell_i];
            if (cell_id == TILE_EMPTY && skip_zero){
                continue;
            }

            TileID tile_id;
            switch(rand_type){
            case TileRandType_NONE:
                tile_id = TilesetGetId(tileset, cell_id);
                break;
            case TileRandType_SEED:
                tile_id = TilesetGetTileAltRandom(tileset, cell_id, &seed);
                break;
            case TileRandType_XY:
                tile_id = TilesetGetTileAltDeterministic(tileset, cell_id, x, y);
                break;
            }

            // Framework specific implementation
            Vector2 cell_pos = {(float)cell_x, (float)cell_y};
            Vector2 tex_pos = *(Vector2*)& tile_atlas->data[tile_id];
            tex_rect.x = tex_pos.x;
            tex_rect.y = tex_pos.y;
            DrawTextureRec(*texture, *(Rectangle*)&tex_rect, cell_pos, WHITE);
        }
    }
}

// Draw selected region. For optimization draw only what is on a screen.
TMRAPI void DrawTilemapRecti(
    Tilemap *tilemap, 
    Tileset *tileset, 
    TileAtlas *tile_atlas, 
    bool skip_zero, 
    TileRandType rand_type, 
    recti region_rect,
    Texture2D *texture
) {
    recti rect = region_rect;
    if (rect.x < 0) {
        rect.w += rect.x;
        rect.x = 0;
    }
    if (rect.y < 0) {
        rect.h += rect.y;
        rect.y = 0;
    }
    rect.w += rect.x;
    rect.h += rect.y;
    if (rect.w > tilemap->size.x) {
        rect.w = tilemap->size.x;
    }
    if (rect.h > tilemap->size.y) {
        rect.h = tilemap->size.y;
    }

    rectf tex_rect = {0.0, 0.0, tile_atlas->tile_size.x, tile_atlas->tile_size.y};
    uint32_t seed = tileset->random_seed;

    for (int32_t y = rect.y; y < rect.h; y += 1) {
        int32_t cell_y = tilemap->position.y + y * tilemap->tile_size.y;
        for (int32_t x = rect.x; x < rect.w; x += 1) {
            int32_t cell_x = tilemap->position.x + x * tilemap->tile_size.x;
            int32_t cell_i = x + y * tilemap->size.x;
            TileID cell_id = tilemap->grid[cell_i];
            if (cell_id == TILE_EMPTY && skip_zero){
                continue;
            }

            TileID tile_id;
            switch(rand_type){
            case TileRandType_NONE:
                tile_id = TilesetGetId(tileset, cell_id);
                break;
            case TileRandType_SEED:
                tile_id = TilesetGetTileAltRandom(tileset, cell_id, &seed);
                break;
            case TileRandType_XY:
                tile_id = TilesetGetTileAltDeterministic(tileset, cell_id, x, y);
                break;
            }

            // Framework specific implementation
            Vector2 cell_pos = {(float)cell_x, (float)cell_y};
            Vector2 tex_pos = *(Vector2*)& tile_atlas->data[tile_id];
            tex_rect.x = tex_pos.x;
            tex_rect.y = tex_pos.y;
            DrawTextureRec(*texture, *(Rectangle*)& tex_rect, cell_pos, WHITE);
        }
    }
}


#endif // TILEMAP_RAYLIB_IMPLEMENTATION
#undef TMRAPI