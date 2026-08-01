#ifndef ANIM_EDITOR_ZEN_H
#define ANIM_EDITOR_ZEN_H

// ============================================================================
//  anim_editor_zen.h  -  public face of the Zen animation editor
//
//  THE anim editor (menu bar, hotkey navigation, draggable modals, zoomed-out
//  viewport). It owns all editor-side logic and depends only on the src/anim/*
//  runtime, which it shares with in-game playback (anim_stage).
//
//  The app state (registered in app_state.h as app_state_anim_editor_zen).
// ============================================================================

#include "../app_state/app_state.h"

extern AppState app_state_anim_editor_zen;

#endif // ANIM_EDITOR_ZEN_H
