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

/* List of "public" app states */
extern AppState app_state_main_menu;    // learning demo: main menu
extern AppState app_state_platformer;   // original: launch platformer


#endif // APP_STATE_H