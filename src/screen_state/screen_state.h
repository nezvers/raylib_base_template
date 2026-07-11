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
    Color clear_color;
} ScreenState;

enum ViewportType {KEEP_ASPECT, KEEP_HEIGHT, KEEP_WIDTH};

ScreenState *ScreenStateGet();
void ScreenStateSet(ScreenState *value);
void ScreenStateReset();
void ScreenStateResize();
void ScreenStateDrawTarget();
void ScreenStateCleanup();


Vector2 ScreenStateTargetSize();

// need to solve screen state pos to game state pos
Vector2 ScreenStateMouseGame();
Vector2 ScreenStatePosToGame(Vector2);

#endif // SCREEN_STATE_H