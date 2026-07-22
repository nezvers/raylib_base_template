// ============================================================================
//  anim_scene.c  -  the declarative scene layer (see anim_scene.h)
//
//  Every row maps to one anim_stage slot; this module is bookkeeping over the
//  handles anim_stage hands back. It adds NO playback of its own - AnimScenePlay
//  is a loop of AnimStagePlayEx, AnimSceneEmit is a loop of AnimStageEmit.
// ============================================================================

#include "anim_scene.h"
#include "raylib.h"     // TextIsEqual
#include <stddef.h>

// True if entry `e` lists a signal called `name`.
static bool EntryHasSignal(const AnimStageEntry *e, const char *name)
{
    for (int i = 0; i < e->signalCount && i < ANIM_SCENE_SIG_MAX; i++)
        if (e->signals[i].name && TextIsEqual(e->signals[i].name, name))
            return true;
    return false;
}

void AnimScenePlay(AnimStageScene *sc, const AnimStageEntry *entries, int count)
{
    if (!sc) return;
    *sc = (AnimStageScene){0};
    sc->entries = entries;
    sc->count   = (count < ANIM_SCENE_ENTRIES_MAX) ? count : ANIM_SCENE_ENTRIES_MAX;

    for (int i = 0; i < ANIM_SCENE_ENTRIES_MAX; i++) sc->handles[i] = ANIM_HANDLE_NONE;
    for (int i = 0; i < sc->count; i++)
    {
        const AnimStageEntry *e = &entries[i];
        sc->handles[i] = AnimStagePlayEx(e->anim, e->loop, e->layer, e->delay);
    }
}

void AnimSceneStop(AnimStageScene *sc)
{
    if (!sc) return;
    for (int i = 0; i < sc->count; i++)
    {
        AnimStageStop(sc->handles[i]);
        sc->handles[i] = ANIM_HANDLE_NONE;
    }
    sc->count = 0;
    sc->pendingTerminals = 0;
    sc->onSceneDone = NULL;
    sc->doneUser = NULL;
}

bool AnimSceneAlive(const AnimStageScene *sc)
{
    if (!sc) return false;
    for (int i = 0; i < sc->count; i++)
        if (AnimStageAlive(sc->handles[i])) return true;
    return false;
}

void AnimSceneEmit(AnimStageScene *sc, const char *name,
                   const SignalParams *params)
{
    if (!sc || !name) return;
    for (int i = 0; i < sc->count; i++)
        if (EntryHasSignal(&sc->entries[i], name))
            AnimStageEmit(sc->handles[i], name, params);
}

void AnimSceneEmitTag(AnimStageScene *sc, int tag, const char *name,
                      const SignalParams *params)
{
    if (!sc || !name) return;
    for (int i = 0; i < sc->count; i++)
        if (sc->entries[i].tag == tag)
        {
            AnimStageEmit(sc->handles[i], name, params);
            return;
        }
}

// One armed instance finished its terminal transition: count it down, and fire
// the scene's onDone when the last one lands. `user` is the scene itself.
static void OnOneTerminalDone(void *user)
{
    AnimStageScene *sc = (AnimStageScene *)user;
    if (!sc || sc->pendingTerminals <= 0) return;
    if (--sc->pendingTerminals == 0)
    {
        void (*fn)(void *) = sc->onSceneDone;
        void *u = sc->doneUser;
        sc->onSceneDone = NULL;      // cleared first: fn may start a new scene
        sc->doneUser    = NULL;
        if (fn) fn(u);
    }
}

void AnimSceneEmitTerminal(AnimStageScene *sc, const char *name,
                           const SignalParams *params,
                           void (*onDone)(void *user), void *user)
{
    if (!sc || !name) { if (onDone) onDone(user); return; }

    sc->onSceneDone      = onDone;
    sc->doneUser         = user;
    sc->pendingTerminals = 0;

    for (int i = 0; i < sc->count; i++)
    {
        if (!EntryHasSignal(&sc->entries[i], name)) continue;
        AnimHandle h = sc->handles[i];
        AnimStageEmit(h, name, params);
        // Only wait on instances the emit actually armed for shutdown - a
        // non-terminal signal (or an absent name) ends nothing, so counting it
        // would hang the transition forever.
        if (AnimStageAlive(h) && AnimStageEndsOnCurrentSignal(h))
        {
            AnimStageSetDoneCallback(h, OnOneTerminalDone, sc);
            sc->pendingTerminals++;
        }
    }

    if (sc->pendingTerminals == 0)
    {
        sc->onSceneDone = NULL;
        sc->doneUser    = NULL;
        if (onDone) onDone(user);    // nothing to wait for
    }
}
