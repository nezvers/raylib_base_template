#ifndef PAUSE_MENU_H
#define PAUSE_MENU_H

#include <stdbool.h>

// ----------------------------------------------------------------------------
//  Shared pause-menu overlay.
//
//  The animated PAUSE title/subtitle is authored in the ANIM EDITOR and saved to
//  anims/PAUSE_MENU.cfg - exactly the way main_menu.c plays MAIN_MENU.cfg. This
//  module wraps that one editor-authored animation behind a tiny Open/Close API
//  so any game state (platformer, strategy, ...) shows the SAME pause overlay
//  without hand-coding an animation. It replaces the deprecated SceneAnim pause
//  players those states used to carry.
//
//  A state still owns everything ELSE about its pause: the `paused` flag, the
//  dim overlay, and the GUI buttons (RESUME / OPTIONS / QUIT). This module is
//  ONLY the animated text overlay. Typical wiring:
//
//      Update(): PauseMenuUpdate(GetFrameTime());   // always; no-op when idle
//      Draw()  : ...dim overlay...; PauseMenuDraw(); // after the dim, over the game
//      on ESC-open : PauseMenuOpen();
//      on ESC-close: PauseMenuBeginClose(NULL, NULL);  // plays the exit outro
//
//  The .cfg's authored TERMINAL signal "exit" drives the close outro: emitting
//  it plays the slide-out and then removes the overlay on its own.
// ----------------------------------------------------------------------------

// Start (or restart) the pause overlay: plays anims/PAUSE_MENU.cfg, looping so
// it holds on screen for as long as the game is paused.
void PauseMenuOpen(void);

// Begin closing: fire the authored terminal "exit" signal so the overlay plays
// its slide-out and then stops itself. `onClosed(user)` fires once the outro has
// finished (or immediately if nothing is playing); pass NULL if not needed.
void PauseMenuBeginClose(void (*onClosed)(void *user), void *user);

// Hard-stop the overlay immediately (no outro), releasing its stage slot. Call
// from a state's Exit() so the overlay never leaks into the next state's stage
// pool - e.g. QUIT straight from the pause menu, with the overlay still up.
void PauseMenuStop(void);

// Pump the underlying stage pool. Call once per frame from the state's Update;
// harmless (no-op) when the overlay is idle.
void PauseMenuUpdate(float dt);

// Draw the overlay (in GAME space). Call from the state's Draw AFTER its own art
// and dim layer; no-op when idle.
void PauseMenuDraw(void);

// Is the overlay still on screen (opening, held, or winding down its outro)?
bool PauseMenuAlive(void);

#endif // PAUSE_MENU_H
