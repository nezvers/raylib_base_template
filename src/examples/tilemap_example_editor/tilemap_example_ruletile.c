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
AppState app_tilemap_example_ruletile = {Enter, NULL, NULL, Draw, NULL, "Rule tile: Tilemap -> Tileset -> Tile -> TileAtlas\nIncomplete rule set"};

#define RIGHTi {1,0}
#define LEFTi  {-1,0}
#define UPi    {0,-1}
#define DOWNi  {0,1}
#define ZEROi  {0,0}

// Holds TileID of tileset groups (in example just one)
Tilemap group_tilemap;
#define GROUP_BUFFER_SIZE (MAP_SIZE_X * MAP_SIZE_Y)
TileID group_buffer[GROUP_BUFFER_SIZE];
#define solid_id 1 // Set group 1 at the same positions

// Can be procedurally generated or through custom editor
AutotileRule rules[] = {
        {1, (RuleMatch[]){
            {solid_id, RIGHTi, false},
            {solid_id, ZEROi, false},
            {solid_id, DOWNi, false},
            {solid_id, LEFTi, true},
            {solid_id, UPi, true},
            }, 5
        }, // Top-left corner
        {3, (RuleMatch[]){
            {solid_id, ZEROi, false},
            {solid_id, LEFTi, false},
            {solid_id, DOWNi, false},
            {solid_id, RIGHTi, true},
            {solid_id, UPi, true},
            }, 5
        }, // Top-right corner
        {21, (RuleMatch[]){
            {solid_id, ZEROi, false},
            {solid_id, RIGHTi, false},
            {solid_id, UPi, false},
            {solid_id, LEFTi, true},
            {solid_id, DOWNi, true},
            }, 5
        }, // Bottom-left corner
        {23, (RuleMatch[]){
            {solid_id, ZEROi, false},
            {solid_id, LEFTi, false},
            {solid_id, UPi, false},
            {solid_id, RIGHTi, true},
            {solid_id, DOWNi, true},
            }, 5
        }, // Bottom-right corner
        {2, (RuleMatch[]){
            {solid_id, ZEROi, false},
            {solid_id, RIGHTi, false},
            {solid_id, LEFTi, false},
            {solid_id, DOWNi, false},
            {solid_id, UPi, true},
            }, 5
        }, // Top-middle
        {11, (RuleMatch[]){
            {solid_id, ZEROi, false},
            {solid_id, RIGHTi, false},
            {solid_id, DOWNi, false},
            {solid_id, UPi, false},
            {solid_id, LEFTi, true},
            }, 5
        }, // Left-middle
        {13, (RuleMatch[]){
            {solid_id, ZEROi, false},
            {solid_id, DOWNi, false},
            {solid_id, UPi, false},
            {solid_id, LEFTi, false},
            {solid_id, RIGHTi, true},
            }, 5
        }, // Right-middle
        {22, (RuleMatch[]){
            {solid_id, ZEROi, false},
            {solid_id, UPi, false},
            {solid_id, LEFTi, false},
            {solid_id, RIGHTi, false},
            {solid_id, DOWNi, true},
            }, 5
        }, // Bottom-middle
        {4, (RuleMatch[]){
            {solid_id, ZEROi, false},
            {solid_id, DOWNi, false},
            {solid_id, UPi, true},
            {solid_id, LEFTi, true},
            {solid_id, RIGHTi, true},
            }, 5
        }, // Top single
        {14, (RuleMatch[]){
            {solid_id, ZEROi, false},
            {solid_id, DOWNi, false},
            {solid_id, UPi, false},
            {solid_id, LEFTi, true},
            {solid_id, RIGHTi, true},
            }, 5
        }, // Midle vertical single
        {24, (RuleMatch[]){
            {solid_id, ZEROi, false},
            {solid_id, UPi, false},
            {solid_id, DOWNi, true},
            {solid_id, LEFTi, true},
            {solid_id, RIGHTi, true},
            }, 5
        }, // Bottom single
        {34, (RuleMatch[]){
            {solid_id, ZEROi, false},
            {solid_id, DOWNi, true},
            {solid_id, UPi, true},
            {solid_id, LEFTi, true},
            {solid_id, RIGHTi, true},
            }, 5
        }, // Single block
        {31, (RuleMatch[]){
            {solid_id, ZEROi, false},
            {solid_id, RIGHTi, false},
            {solid_id, DOWNi, true},
            {solid_id, UPi, true},
            {solid_id, LEFTi, true},
            }, 5
        }, // Single left
        {32, (RuleMatch[]){
            {solid_id, ZEROi, false},
            {solid_id, LEFTi, false},
            {solid_id, RIGHTi, false},
            {solid_id, DOWNi, true},
            {solid_id, UPi, true},
            }, 5
        }, // Single horizontal middle
        {33, (RuleMatch[]){
            {solid_id, ZEROi, false},
            {solid_id, LEFTi, false},
            {solid_id, DOWNi, true},
            {solid_id, UPi, true},
            {solid_id, RIGHTi, true},
            }, 5
        }, // Single right
        {12, (RuleMatch[]){{solid_id, ZEROi, false},}, 1}, // Default to solid center
    };

static void Enter() {
    // same properties as main Tilemap
    group_tilemap = TilemapInit(tilemap.position, tilemap.size, tilemap.tile_size, group_buffer, GROUP_BUFFER_SIZE);
	// reset map to predictable state
	TilemapClear(&group_tilemap);

    // Init group tilemap
    TileID tile_id;
    vec2i tile_pos;
    for (int32_t y = 0; y < group_tilemap.size.y; y += 1) {
        for (int32_t x = 0; x < group_tilemap.size.x; x += 1) {
            tile_pos = (vec2i){x, y};
            tile_id = TilemapGetTile(&tilemap, tile_pos);
            if (tile_id == TILE_EMPTY) {
                // Group tilemap already zeroed out
                continue;
            }
            TilemapSetTile(&group_tilemap, tile_pos, solid_id);
        }
    }

    // Apply group rules to main Tilemap
    const uint32_t RULES_COUNT = sizeof(rules) / sizeof(AutotileRule); 
    AutotileRuleUpdateTilemap(&group_tilemap, &tilemap, rules, RULES_COUNT);
}

static void Draw(){
    bool skip_zero = true;
	DrawTilemap(&tilemap, &tileset, &tile_atlas, skip_zero, TileRandType_NONE, &tileset_texture);
}