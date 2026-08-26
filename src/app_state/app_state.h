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

// Is this state still the one running? AppStateTransition switches states
// SYNCHRONOUSLY, so a state that transitions from inside its own Gui() keeps
// executing the rest of that function - drawing its header, footer and tooltips
// on top of the frame the NEW state already painted, for one visible frame.
// Returning right after the transition is not enough when the call is nested a
// few draw functions deep. A state whose Gui() continues after a possible
// transition guards the remainder with AppStateIsCurrent(&app_state_mine).
bool AppStateIsCurrent(const AppState* value);
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
extern AppState app_state_platformer;   // original: launch platformer
extern AppState app_state_strategy;     // RTS test: units, resources, factions
extern AppState app_state_strategy_showcase;    // RTS asset gallery (menu -> here -> game)
extern AppState app_state_anim_editor_zen;  // animation editor (menu bar, zoomed viewport)
extern AppState app_state_shape_editor;     // pixel-shape editor (the global shape pool)
extern AppState app_state_strategy_forge;   // RTS asset forge (showcase -> here -> back)


#endif // APP_STATE_H