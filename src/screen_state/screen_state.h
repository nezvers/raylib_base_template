#ifndef SCREEN_STATE_H
#define SCREEN_STATE_H

typedef struct {
    int width;
    int height;
    RenderTexture2D target;
} ScreenState;

ScreenState *ScreenStateGet();
void ScreenStateSet(ScreenState *value);
void ScreenStateResize();
void ScreenStateCleanup();

#endif // SCREEN_STATE_H