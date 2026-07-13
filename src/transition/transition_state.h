// ============================================================================
//  transition_state.h  -  generic scene-exit (outro) player app-state
//
//  Usage from any state that wants an animated exit:
//      TransitionStateStart(&myOutroSpec, &app_state_next);
//      AppStateTransition(&app_state_transition);
//  The transition state plays the spec's OUTRO (texts, global beats, zoom
//  boxes) and hands off to `next` when the last phase ends. If Start was
//  never armed it falls straight through to `next` (or the main menu).
// ============================================================================

#ifndef TRANSITION_STATE_H
#define TRANSITION_STATE_H

#include "../scene_anim/scene_anim.h"
#include "../app_state/app_state.h"

void TransitionStateStart(const SceneAnim *outro, AppState *next);

#endif // TRANSITION_STATE_H
