// ============================================================================
//  anim_editor.h  -  app-state that authors AnimDoc animations (anim.*)
//
//  Reached from the LEFT-side "ANIM EDITOR" button in the main menu. Draws a
//  live preview of the working document in game space (Draw) and a raygui tool
//  UI in screen space (Gui): element list, inspector, signals, timeline
//  scrubber, and New/Load/Save/Undo/Redo. Saves to a hand-editable .cfg via
//  anim_io. See anim.h for the data model.
// ============================================================================

#ifndef ANIM_EDITOR_H
#define ANIM_EDITOR_H

#include "../app_state/app_state.h"

// The app state (registered in app_state.h as app_state_anim_editor too).
extern AppState app_state_anim_editor;

#endif // ANIM_EDITOR_H
