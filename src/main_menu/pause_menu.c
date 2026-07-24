#include "pause_menu.h"
#include "../anim/anim_scene.h"        // declarative editor-authored anim table
#include "../anim/anim_stage.h"        // AnimStageUpdate / AnimStageDraw pool pump
#include <stddef.h>                    // NULL

// The overlay is one editor-authored animation instance. `loop=true`: the stage
// auto-deactivates a NON-looping doc the moment it runs out (see anim_stage.c),
// so a non-loop overlay would vanish a couple seconds into the pause. Looping
// keeps it held (PAUSE_MENU.cfg is authored to settle/loop cleanly, like
// MAIN_MENU.cfg). It answers the authored TERMINAL signal "exit", which plays
// the slide-out and ends the instance - the close transition.
#define PAUSE_EXIT_SIGNAL "exit"

static const AnimStageEntry PAUSE_SCENE[] = {
    { .anim="PAUSE_MENU", .loop=true, .delay=0.0f, .layer=20, .tag=0, .seq=0,
      .signals={ { PAUSE_EXIT_SIGNAL, false } }, .signalCount=1 },
};

static AnimStageScene pauseScene;

void PauseMenuOpen(void)
{
    // Stop any leftover instance first so a rapid close->open can't stack two.
    AnimSceneStop(&pauseScene);
    AnimScenePlay(&pauseScene, PAUSE_SCENE,
                  sizeof(PAUSE_SCENE)/sizeof(PAUSE_SCENE[0]));
}

void PauseMenuBeginClose(void (*onClosed)(void *user), void *user)
{
    // Terminal emit: plays the "exit" outro across the overlay and calls onClosed
    // once it has wound down (or immediately if nothing is armed/playing).
    AnimSceneEmitTerminal(&pauseScene, PAUSE_EXIT_SIGNAL, NULL, onClosed, user);
}

void PauseMenuUpdate(float dt)
{
    AnimStageUpdate(dt);
}

void PauseMenuDraw(void)
{
    AnimStageDraw();
}

bool PauseMenuAlive(void)
{
    return AnimSceneAlive(&pauseScene);
}
