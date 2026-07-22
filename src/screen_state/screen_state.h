#ifndef SCREEN_STATE_H
#define SCREEN_STATE_H

#include "raylib.h"

typedef struct {
    int width;
    int height;
    int game_width;
    int game_height;
    int viewport_type;
    Rectangle source_rect;
    Rectangle dest_rect;
    RenderTexture2D target;
    float resize_ratio;
    Color clear_color;
    Shader *shader_target;
} ScreenState;

enum ViewportType {KEEP_ASPECT, KEEP_HEIGHT, KEEP_WIDTH};

ScreenState *ScreenStateGet();
void ScreenStateSet(ScreenState *value);
void ScreenStateShader(Shader *shader);
void ScreenStateReset();
void ScreenStateLoad();     // override defaults from disk (no-op today; part of Reset->Load->Apply lifecycle)
void ScreenStateResize();
void ScreenStateDrawTarget();
void ScreenStateCleanup();


Vector2 ScreenStateSize();
Vector2 ScreenStateTargetSize();
Vector2 Screen2Target(Vector2 pos);

#endif // SCREEN_STATE_H