#include "raylib.h"
#include "screen_state.h"

static ScreenState screen_state;

ScreenState *ScreenStateGet() {
    return &screen_state;
}

void ScreenStateSet(ScreenState *value) {
    screen_state = *value;
}

void ScreenStateResize() {
    screen_state.width = GetScreenWidth();
    screen_state.height = GetScreenHeight();

    // Render texture to draw, enables screen scaling
    // NOTE: If screen is scaled, mouse input should be scaled proportionally
    UnloadRenderTexture(screen_state.target);
    screen_state.target = LoadRenderTexture(screen_state.width, -screen_state.height);
    // Nearest Neighbour color interpolation
    SetTextureFilter(screen_state.target.texture, TEXTURE_FILTER_POINT);
}

void ScreenStateCleanup() {
    UnloadRenderTexture(screen_state.target);
}