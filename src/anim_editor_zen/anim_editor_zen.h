#ifndef ANIM_EDITOR_ZEN_H
#define ANIM_EDITOR_ZEN_H

// ============================================================================
//  anim_editor_zen.h  -  public face of the Zen animation editor
//
//  A ground-up rework of the anim editor UI (menu bar, hotkey navigation,
//  draggable modals, zoomed-out viewport). Lives NEXT TO the classic editor
//  (app_state_anim_editor) and shares only the src/anim/* runtime with it;
//  all editor-side logic is deliberately duplicated so the classic editor can
//  be deleted later without untangling.
//
//  The app state (registered in app_state.h as app_state_anim_editor_zen too).
// ============================================================================

#include "../app_state/app_state.h"

extern AppState app_state_anim_editor_zen;

#endif // ANIM_EDITOR_ZEN_H
