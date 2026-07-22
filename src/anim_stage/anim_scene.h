// ============================================================================
//  anim_scene.h  -  a state's editor-authored animations, declared as DATA
//
//  Where anim_stage.h is the low-level playback POOL (imperative: Play this,
//  Stop that, Emit here), this is the DECLARATIVE layer a game state actually
//  uses. A state lists every authored animation it shows in ONE table - each
//  row next to the next, with its own loop/delay/layer and the signals it
//  answers to - and plays the whole thing with a single call:
//
//      static const AnimStageEntry MY_SCENE[] = {
//          { .anim="overlay", .loop=true, .delay=0.0f, .layer=10, .tag=0,
//            .signals={ {"end", false} }, .signalCount=1 },
//          { .anim="ripple",  .loop=true, .delay=0.0f, .layer=9,  .tag=1,
//            .signals={ {"spawn", true} }, .signalCount=1 },   // uses pos param
//          { .anim="ripple",  .loop=true, .delay=0.6f, .layer=9,  .tag=2,
//            .signals={ {"spawn", true} }, .signalCount=1 },   // same anim, later
//      };
//      static AnimStageScene myScene;
//
//      Enter():  AnimScenePlay(&myScene, MY_SCENE, 3);
//      Exit():   AnimSceneStop(&myScene);
//      // Update()/Draw() still pump the POOL: AnimStageUpdate/AnimStageDraw.
//
//  SAME ANIM, MANY TIMES. A row is a whole instance; listing "ripple" three
//  times with different delays gives a staggered train from one .cfg - the
//  stagger lives HERE, at the integration point, not in the file.
//
//  SIGNALS + PARAMS. Each row lists the signals it responds to right next to
//  it, so which animation reacts to what - and which ones consume a position
//  parameter (usesPos) - is visible at a glance. AnimSceneEmit(name, params)
//  fires a signal across EVERY row that declares it (all matching animations
//  trigger), each placed by the same params. AnimSceneEmitTag targets one row.
//
//  TERMINAL TRANSITIONS. AnimSceneEmitTerminal fires an ending signal across
//  the matching rows and calls onDone once every armed instance has wound down
//  through its authored transition - the standardized replacement for the
//  hand-rolled "emit, wait, then change state" dance a state used to write.
//
//  This is a THIN layer: every row maps to one anim_stage slot via
//  AnimStagePlayEx, and emits go through AnimStageEmit. Fixed-capacity, no heap,
//  same house style as anim_stage.
// ============================================================================

#ifndef ANIM_SCENE_H
#define ANIM_SCENE_H

#include <stdbool.h>
#include "anim_stage.h"          // AnimHandle, ANIM_STAGE_SLOTS_MAX
#include "../signal/signal.h"    // SignalParams

// Signals one entry can respond to, listed inline in the table.
#define ANIM_SCENE_SIG_MAX      4
// A scene can hold at most one instance per stage slot.
#define ANIM_SCENE_ENTRIES_MAX  ANIM_STAGE_SLOTS_MAX

// One row of a scene table = one animation instance. Filled as a `static const`
// array by a state: all fields are values or STRING LITERALS (MSVC-clean at
// file scope - only address-of-static is banned, not literals).
typedef struct {
    const char *anim;        // anims/<anim>.cfg to play
    bool  loop;              // play once (false) vs loop forever (true)
    float delay;             // seconds to wait before starting (0 = at load)
    int   layer;             // draw order, ascending; higher draws in front
    int   tag;               // caller-chosen stable id to address this row later
                             // (the SAME anim may appear several times, each with
                             //  its own tag). Not required to be unique, but
                             //  AnimSceneEmitTag reaches only the first match.

    // Signals this row answers to, listed HERE so the integration point shows
    // which animation reacts to what, and which consume a position parameter.
    struct {
        const char *name;    // signal name (must match one authored in the .cfg)
        bool  usesPos;       // documents that this signal consumes params.pos
    } signals[ANIM_SCENE_SIG_MAX];
    int signalCount;
} AnimStageEntry;

// A state owns ONE of these (a plain static). Holds the live handle per row and
// the terminal-transition bookkeeping. Value type, no heap.
typedef struct {
    const AnimStageEntry *entries;
    int         count;
    AnimHandle  handles[ANIM_SCENE_ENTRIES_MAX];

    // AnimSceneEmitTerminal bookkeeping: onDone fires once `pendingTerminals`
    // armed instances have all wound down.
    void      (*onSceneDone)(void *user);
    void       *doneUser;
    int         pendingTerminals;
} AnimStageScene;

// Play every row: each becomes one anim_stage instance (via AnimStagePlayEx).
// Rows past ANIM_SCENE_ENTRIES_MAX, or that fail to load, get ANIM_HANDLE_NONE
// and are simply inert. A state calls this once in Enter().
void AnimScenePlay(AnimStageScene *sc, const AnimStageEntry *entries, int count);

// Stop every instance the scene started. A state calls this in Exit() so no
// stage slot outlives it. Safe to call twice.
void AnimSceneStop(AnimStageScene *sc);

// Is any instance in the scene still playing?
bool AnimSceneAlive(const AnimStageScene *sc);

// Fire `name` across ALL rows that declare it, each placed by `params` (may be
// NULL). Per-instance so a position param lands on exactly those slots. This is
// the "every matching animation triggers" path.
void AnimSceneEmit(AnimStageScene *sc, const char *name,
                   const SignalParams *params);

// Fire `name` on the ONE row whose `tag` matches (first match), with `params`.
void AnimSceneEmitTag(AnimStageScene *sc, int tag, const char *name,
                      const SignalParams *params);

// Fire a TERMINAL signal `name` across matching rows and call `onDone(user)`
// once every armed instance has finished its ending transition. If nothing arms
// (no row names it, or the signal is not terminal) `onDone` fires immediately,
// so a caller can always wait on it without hanging. Replaces the hand-rolled
// pendingDestination/done-callback dance.
void AnimSceneEmitTerminal(AnimStageScene *sc, const char *name,
                           const SignalParams *params,
                           void (*onDone)(void *user), void *user);

#endif // ANIM_SCENE_H
