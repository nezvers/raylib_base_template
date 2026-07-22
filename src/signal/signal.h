// ============================================================================
//  signal.h  -  tiny named-signal dispatch (emit a name -> run listeners)
//
//  A decoupling primitive in the spirit of the project's existing single-slot
//  callbacks (LevelSetRestartCallback) and the box2d per-frame event drain -
//  generalized to MANY listeners keyed by a string NAME. Fixed capacity, no
//  heap, singleton house style (SignalReset like the other *Reset lifecycles).
//
//  Intended first use: the new anim system. A loaded AnimDoc registers one
//  listener per AnimSignal.name (see AnimSignalRegister in anim_signal.*);
//  gameplay/UI code just calls SignalEmit("enter") / SignalEmit("button_play")
//  and any listening animation plays. Nothing in scene_anim/transition_state
//  changes - signals drive only the new model.
//
//  Not a deferred queue: SignalEmit runs matching handlers synchronously, then
//  returns (same timing model as the box2d sensor-callback drain).
// ============================================================================

#ifndef SIGNAL_H
#define SIGNAL_H

#include "raylib.h"      // Vector2 (for SignalParams)
#include <stdbool.h>

#define SIGNAL_NAME_MAX  32
#define SIGNAL_MAX       32   // total registered listeners across all names

// Per-EMIT parameters carried alongside a signal name. The firing site fills
// these; a listener reads them. A zeroed SignalParams (hasPos false) means "no
// params" and is byte-identical to the old parameter-less behaviour.
//
// `pos` is an ABSOLUTE screen location in canvas fractions (same space as an
// element's authored pos). Its use: an emit can place a transition WHERE it
// fired - e.g. blink/zoom at the mouse - overriding the animation's authored
// position for the signal's position targets. When hasPos is false those
// targets keep their authored position.
typedef struct {
    Vector2 pos;
    bool    hasPos;
} SignalParams;

// A listener: called with the emitted name, its own user pointer, and the
// emit's params (NULL when the caller passed none - read as "no params").
typedef void (*SignalHandler)(const char *name, void *user,
                              const SignalParams *params);

// Reset lifecycle (call once at startup, like SettingsReset). Clears listeners.
void SignalReset(void);

// Register `fn` to run whenever SignalEmit(name) is called. Same (name,fn,user)
// triple is de-duplicated. Silently ignored if the table is full.
void SignalListen(const char *name, SignalHandler fn, void *user);

// Remove a specific listener (by the exact triple). Safe if not present.
void SignalStopListening(const char *name, SignalHandler fn, void *user);

// Fire now: run every listener registered for `name`, in registration order,
// passing `params` (may be NULL for none) to each.
void SignalEmit(const char *name, const SignalParams *params);

// Diagnostics (used by tests / editor): how many listeners match a name.
int  SignalListenerCount(const char *name);

#endif // SIGNAL_H
