#include "raylib.h"
#include "../app_state/app_state.h"
#include "../screen_state/screen_state.h"
#include <stddef.h> // used for NULL

#define SPRITE_IMPLEMENTATION
#define SPRITE_RAYLIB_IMPLEMENTATION
#include "sprite_raylib.h"


// Forward declare functions
static void Enter();
static void Update();
static void Exit();
static void Draw();
static void Gui();
                               /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_sprite_example = {Enter, Exit, Update, Draw, Gui, "Sprite Example"};


enum PLAYER_STATE {
    PlayerStateIdle,
    PlayerStateWalk,
    PlayerStateJumpUp,
    PlayerStateJumpDown,
};

const vec2 SPRITE_SIZE = {16, 16};
vec2 tex_pos[] = {{0.f,0.f}, {16.f,0.f}, {32.f,0.f}, {48.f,0.f}, {64.f,0.f}, {80.f,0.f}, {96.f,0.f}, {112.f,0.f}};
Frames anim_idle = {.data = &tex_pos[0], .count = 2, .size = SPRITE_SIZE};
Frames anim_walk = {.data = &tex_pos[2], .count = 6, .size = SPRITE_SIZE};
Frames anim_up =   {.data = &tex_pos[5], .count = 1, .size = SPRITE_SIZE};
Frames anim_down = {.data = &tex_pos[4], .count = 1, .size = SPRITE_SIZE};
Frames *player_anim_list[] = {&anim_idle, &anim_walk, &anim_up, &anim_down};

const AnimationSet player_animations = {
    .frames = player_anim_list,
    .count = 4, 
    .animation_index = PlayerStateIdle, 
    .image_index = 0, 
    .frame_rate = 12, 
    .time = 0,
};


Texture2D player_texture;
SpriteRaylib player_sprite = (SpriteRaylib){
    .sprite = (Sprite){
        .animation_set = player_animations,
        .position = {10, 60},
        .origin = {8, 16},
        .offset = {0, 0},
        .scale = {1, 1},
        .rotation = 0,
    },
    .texture = &player_texture,
    .tint = WHITE
};

static void Enter(){
    ChangeAnimation(&player_sprite.sprite.animation_set, PlayerStateWalk);
    // RESOURCES_PATH is a macro definition from compiler
    player_texture = LoadTexture(RESOURCES_PATH"/textures/player_sheet.png");
}

static void Exit(){
    UnloadTexture(player_texture);
}

static void Update(){
    UpdateSprite(&player_sprite.sprite, GetFrameTime());
}

static void Draw(){
    ScreenState *screen_state = ScreenStateGet();
    Vector2 target_size = ScreenStateTargetSize();

    Camera2D camera = { {0,0}, {0,0}, 0, 4};
    BeginMode2D(camera);

    // Draw animation directly
    Vector2 frame_pos = (Vector2){10,10};
    rectf frame_rect = GetAnimationFrame(&player_sprite.sprite.animation_set);
    DrawTextureRec(player_texture, *(Rectangle*)&frame_rect, frame_pos, RAYWHITE);

    // Draw Sprite with visual controls, packed within SpriteRaylib
    DrawSpriteRaylib(&player_sprite);

    EndMode2D();
}

static void Gui() {
    /* from Odin
    slider_rect:rl.Rectangle = {screen_size.x - 110, 10, 100, 25}
    rl.GuiSlider(slider_rect, "offset.x", "", &player_sprite.offset.x, -16, 16)
    slider_rect.y += 30
    rl.GuiSlider(slider_rect, "offset.y", "", &player_sprite.offset.y, -16, 16)
    slider_rect.y += 30
    rl.GuiSlider(slider_rect, "origin.x", "", &player_sprite.origin.x, -16, 16)
    slider_rect.y += 30
    rl.GuiSlider(slider_rect, "origin.y", "", &player_sprite.origin.y, -16, 16)
    slider_rect.y += 30
    rl.GuiSlider(slider_rect, "scale.x", "", &player_sprite.scale.x, -1, 1)
    slider_rect.y += 30
    rl.GuiSlider(slider_rect, "scale.y", "", &player_sprite.scale.y, -1, 1)
    slider_rect.y += 30
    rl.GuiSlider(slider_rect, "rotate", "", &player_sprite.rotation, -180, 180)
    */
}