#include "raylib.h"
#include "screen_state.h"
#define VIEWPORT_RECT_IMPLEMENTATION
#include "viewport_rect.h"

static ScreenState state;

ScreenState *ScreenStateGet() {
    return &state;
}

void ScreenStateSet(ScreenState *value) {
    state = *value;
}

void ScreenStateReset() {
    state.width = 1280;
    state.height = 720;
    state.game_width = 1280;
    state.game_height = 720;
    state.viewport_type = KEEP_HEIGHT;
    state.clear_color = RAYWHITE;
}

void ScreenStateResize() {
    state.width = GetScreenWidth();
    state.height = GetScreenHeight();

    switch(state.viewport_type){
        case KEEP_ASPECT:{
            KeepAspectCentered(state.width, state.height, state.game_width, state.game_height, &state.source_rect, &state.dest_rect);
            break;
        }
        case KEEP_HEIGHT:{
            KeepHeightCentered(state.width, state.height, state.game_width, state.game_height, &state.source_rect, &state.dest_rect);
            break;
        }
        case KEEP_WIDTH:{
            KeepWidthCentered(state.width, state.height, state.game_width, state.game_height, &state.source_rect, &state.dest_rect);
            break;
        }
    }

    // Render texture to draw, enables screen scaling
    // NOTE: If screen is scaled, mouse input should be scaled proportionally
    UnloadRenderTexture(state.target);
    state.target = LoadRenderTexture(state.source_rect.width, -state.source_rect.height);
    // Nearest Neighbour color interpolation
    SetTextureFilter(state.target.texture, TEXTURE_FILTER_POINT);
}

Vector2 ScreenStateTargetSize() {
    return (Vector2){state.source_rect.width, -state.source_rect.height};
}

void ScreenStateCleanup() {
    UnloadRenderTexture(state.target);
}

void ScreenStateDrawTarget(){
        DrawTexturePro(state.target.texture, state.source_rect, state.dest_rect, (Vector2){ 0.0f, 0.0f }, 0.0f, WHITE);
}

Vector2 ScreenStateMouseGame(){
    Vector2 pos_mouse = GetMousePosition();
    return (Vector2){
        (pos_mouse.x - state.dest_rect.x) / state.dest_rect.width  * state.source_rect.width,
        (pos_mouse.y - state.dest_rect.y) / state.dest_rect.height * (-state.source_rect.height)
    };
}

Vector2 ScreenStatePosToGame(Vector2 _pos_screen){
    return (Vector2){
        (_pos_screen.x - state.dest_rect.x) / state.dest_rect.width  * state.source_rect.width,
        (_pos_screen.y - state.dest_rect.y) / state.dest_rect.height * (-state.source_rect.height)
    };
}
