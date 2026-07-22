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

void ScreenStateShader(Shader *shader) {
    state.shader_target = shader;
}

void ScreenStateReset() {
    state.width = 1280;
    state.height = 720;
    state.game_width = 1920/6;
    state.game_height = 1080/6;
    state.viewport_type = KEEP_HEIGHT;
    state.clear_color = RAYWHITE;
}

void ScreenStateLoad() {
    // No persisted screen state yet; defaults from ScreenStateReset stand.
}

void ScreenStateResize() {
    state.width = GetScreenWidth();
    state.height = GetScreenHeight();

    switch(state.viewport_type){
        case KEEP_ASPECT:{
            state.resize_ratio = KeepAspectCentered(state.width, state.height, state.game_width, state.game_height, &state.source_rect, &state.dest_rect);
            break;
        }
        case KEEP_HEIGHT:{
            state.resize_ratio = KeepHeightCentered(state.width, state.height, state.game_width, state.game_height, &state.source_rect, &state.dest_rect);
            break;
        }
        case KEEP_WIDTH:{
            state.resize_ratio = KeepWidthCentered(state.width, state.height, state.game_width, state.game_height, &state.source_rect, &state.dest_rect);
            break;
        }
    }

    // Render texture to draw, enables screen scaling
    // NOTE: If screen is scaled, mouse input should be scaled proportionally
    UnloadRenderTexture(state.target);
    state.target = LoadRenderTexture(state.source_rect.width, -state.source_rect.height);
    // Nearest Neighbour color interpolation
    SetTextureFilter(state.target.texture, TEXTURE_FILTER_POINT);

    // in case game size has changed
    SetWindowMinSize(state.game_width, state.game_height);
}

Vector2 ScreenStateTargetSize() {
    return (Vector2){state.source_rect.width, -state.source_rect.height};
}

Vector2 ScreenStateSize() {
    return (Vector2){state.width, state.height};
}

void ScreenStateCleanup() {
    UnloadRenderTexture(state.target);
}

void ScreenStateDrawTarget(){
    if (state.shader_target) {
        BeginShaderMode(*state.shader_target);
    }
    DrawTexturePro(state.target.texture, state.source_rect, state.dest_rect, (Vector2){ 0.0f, 0.0f }, 0.0f, WHITE);
    if (state.shader_target) {
        EndShaderMode();
    }
}

Vector2 Screen2Target(Vector2 pos) {
    Vector2 relative_pos = {pos.x - state.dest_rect.x, pos.y - state.dest_rect.y};
    Vector2 scaled_position = {relative_pos.x / state.resize_ratio, relative_pos.y / state.resize_ratio};
    return scaled_position;
}