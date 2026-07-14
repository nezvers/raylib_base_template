#ifndef APP_STATE_H
#define APP_STATE_H


#include "raylib.h"
#include "stddef.h"
#include "stdbool.h"

typedef struct{
    void (*enter)();
    void (*exit)();
    void (*update)();
    void (*draw)();
    void (*gui)();
    const char *name;
} AppState;

void AppStateTransition(AppState* value);
void AppStateEnter();
void AppStateExit();
void AppStateUpdate();
void AppStateDraw();
void AppStateGui();

// Quit request: a state asks to exit by calling AppStateRequestQuit() instead of CloseWindow() directly. 
// Calling CloseWindow() mid-frame (from Gui()) destroys the GL context while the frame is still rendering -> segfault. 
// main.c polls AppStateShouldQuit() at the top of the loop and tears down once, cleanly.
void AppStateRequestQuit();
bool AppStateShouldQuit();

/* List of "public" app states */
extern AppState app_state_main_menu;    // learning demo: main menu
extern AppState app_state_transition;   // menu -> game transition animation
extern AppState app_state_platformer;   // original: launch platformer
extern AppState app_state_strategy;     // RTS test: units, resources, factions


#endif // APP_STATE_H