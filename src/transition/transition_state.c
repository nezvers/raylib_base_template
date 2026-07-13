// ============================================================================
//  transition_state.c  -  GENERIC scene-exit (outro) player
//
//  This state owns NO animation data. The leaving scene arms it with its own
//  SceneAnim spec + the destination state (TransitionStateStart), then enters
//  it; we play the spec's OUTRO tables through a SceneAnimPlayer and hand off
//  when the last phase ends. See scene_anim.h for the data model and
//  main_menu.c for the demonstration spec.
//
//  State-machine note: AppStateTransition runs exit()->enter() synchronously
//  and there is NO deferred next-state queue. So this state owns the outro
//  clock (the leaving scene is already gone) and calls AppStateTransition
//  itself from Update() once the outro completes.
//
//  Draw phases (see main.c): Draw() = game render-texture (scaled to window);
//  Gui() = real screen pixels on top. Everything happens in Draw() (game
//  space) so it lines up with where the scene drew its texts/art, and the
//  GP_FADE_BLACK rect covers it all.
// ============================================================================

#include "raylib.h"
#include "transition_state.h"
#include "../screen_state/screen_state.h"
#include <stddef.h>

// Armed by TransitionStateStart BEFORE entering this state.
static const SceneAnim *spec      = NULL;
static AppState        *nextState = NULL;

static SceneAnimPlayer player;

// Forward declares (same pattern as the other states).
static void Enter();
static void Exit();
static void Update();
static void Draw();
static void Gui();

                            /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_state_transition = {Enter, Exit, Update, Draw, Gui, "Transition"};

void TransitionStateStart(const SceneAnim *outro, AppState *next)
{
    spec      = outro;
    nextState = next;
}

// ----------------------------------------------------------------------------
static void Enter()
{
    if (spec) SceneAnimStart(&player, spec, ANIM_OUTRO);
}

static void Exit()
{
    spec      = NULL;   // one-shot: the next use must arm Start again
    nextState = NULL;
}

static void Update()
{
    // Not armed (entered directly without TransitionStateStart): fall through.
    if (!spec)
    {
        AppStateTransition(nextState ? nextState : &app_state_main_menu);
        return;
    }

    SceneAnimUpdate(&player, GetFrameTime());

    if (SceneAnimDone(&player))
        AppStateTransition(nextState);   // hand off to the destination state
}

// ----------------------------------------------------------------------------
//  Draw: GAME SPACE. Scene art (fading) -> zoom boxes -> texts -> black.
// ----------------------------------------------------------------------------
static void Draw()
{
    if (!spec) return;
    Vector2 size = ScreenStateTargetSize();

    // Scene decor fades by GP_ART_FADE. Passing the play clock keeps the
    // scene's idle motion (e.g. the bobbing circle) running - the scene's
    // drawArt adds it to its own frozen clock, so nothing snaps.
    float artAlpha = 1.0f - SceneAnimGlobalAmount(&player, GP_ART_FADE);
    if (spec->drawArt && artAlpha > 0.0f) spec->drawArt(artAlpha, player.t);

    // Zoom boxes: converge from their captured live phases (SP_ZOOM_SETTLE
    // overlays them; SP_ZOOM_SETTLE_GAP1 keeps a gap between them), then
    // reverse inward together (SP_ZOOM_REVERSE).
    if (spec->boxes)
    {
        float settle  = SceneAnimShapeAmount(&player, SP_ZOOM_SETTLE);
        float gap1    = SceneAnimShapeAmount(&player, SP_ZOOM_SETTLE_GAP1);
        float reverse = SceneAnimShapeAmount(&player, SP_ZOOM_REVERSE);
        float alpha   = 0.35f + 0.65f*artAlpha;   // boxes outlive the art a bit
        ZoomBoxesDrawOutro(spec->boxes, settle, gap1, reverse, alpha);
    }

    // Every scene text at its animated outro pose (slide/center/crumble/fade).
    SceneAnimDrawTexts(&player);

    // Fade to black; full exactly when the outro ends (hand-off in Update).
    float black = SceneAnimGlobalAmount(&player, GP_FADE_BLACK);
    if (black > 0.0f)
        DrawRectangle(0, 0, (int)size.x, (int)size.y, Fade(BLACK, black));
}

// Nothing in screen space.
static void Gui() { }
