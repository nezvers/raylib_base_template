#ifndef SHAPE_EDITOR_H
#define SHAPE_EDITOR_H

// ============================================================================
//  shape_editor.h  -  pixel-shape authoring state
//
//  Draws the 2-bit shapes the zen editor's SHAPE_CUSTOM elements reference
//  (anim_shape_pool.h). Reachable from the main menu and from the zen editor's
//  shape row. It edits the GLOBAL pool, so anything saved here is immediately
//  visible to every document.
//
//  A shape carries NO COLOUR. Cells are empty / fill / outline, and the element
//  that draws them supplies the two colours from its own animatable tracks -
//  which is exactly what makes a custom shape as recolourable as a rectangle.
//  The editor shows this as a banner rather than a convention to remember.
// ============================================================================

#include "../app_state/app_state.h"

// Opens `name` when the state is next entered; "" (or an unknown name) starts on
// the first pool shape, or on the new-shape prompt when the pool is empty. Call
// before AppStateTransition.
void ShapeEditorOpen(const char *name);

extern AppState app_state_shape_editor;

#endif
