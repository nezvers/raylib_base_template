#include "raylib.h"
#include "screen_state.h"
#define VIEWPORT_RECT_IMPLEMENTATION
#include "viewport_rect.h"

static ScreenState screen_state;

ScreenState *ScreenStateGet() {
    return &screen_state;
}

void ScreenStateSet(ScreenState *value) {
    screen_state = *value;
}

void ScreenStateReset() {
    screen_state.width = 1280;
    screen_state.height = 720;
    screen_state.game_width = 1280;
    screen_state.game_height = 720;
    screen_state.viewport_type = KEEP_HEIGHT;
}

void ScreenStateResize() {
    screen_state.width = GetScreenWidth();
    screen_state.height = GetScreenHeight();

    switch(screen_state.viewport_type){
        case KEEP_ASPECT:{
            KeepAspectCentered(screen_state.width, screen_state.height, screen_state.game_width, screen_state.game_height, &screen_state.source_rect, &screen_state.dest_rect);
            break;
        }
        case KEEP_HEIGHT:{
            KeepHeightCentered(screen_state.width, screen_state.height, screen_state.game_width, screen_state.game_height, &screen_state.source_rect, &screen_state.dest_rect);
            break;
        }
        case KEEP_WIDTH:{
            KeepWidthCentered(screen_state.width, screen_state.height, screen_state.game_width, screen_state.game_height, &screen_state.source_rect, &screen_state.dest_rect);
            break;
        }
    }

    // Render texture to draw, enables screen scaling
    // NOTE: If screen is scaled, mouse input should be scaled proportionally
    UnloadRenderTexture(screen_state.target);
    screen_state.target = LoadRenderTexture(screen_state.source_rect.width, -screen_state.source_rect.height);
    // Nearest Neighbour color interpolation
    SetTextureFilter(screen_state.target.texture, TEXTURE_FILTER_POINT);
}

Vector2 ScreenStateTargetSize() {
    return (Vector2){screen_state.source_rect.width, -screen_state.source_rect.height};
}

void ScreenStateCleanup() {
    UnloadRenderTexture(screen_state.target);
}

void ScreenStateDrawTarget(){
        DrawTexturePro(screen_state.target.texture, screen_state.source_rect, screen_state.dest_rect, (Vector2){ 0.0f, 0.0f }, 0.0f, WHITE);
}