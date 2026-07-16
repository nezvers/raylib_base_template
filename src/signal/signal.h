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

#include <stdbool.h>

#define SIGNAL_NAME_MAX  32
#define SIGNAL_MAX       32   // total registered listeners across all names

// A listener: called with the emitted name and its own user pointer.
typedef void (*SignalHandler)(const char *name, void *user);

// Reset lifecycle (call once at startup, like SettingsReset). Clears listeners.
void SignalReset(void);

// Register `fn` to run whenever SignalEmit(name) is called. Same (name,fn,user)
// triple is de-duplicated. Silently ignored if the table is full.
void SignalListen(const char *name, SignalHandler fn, void *user);

// Remove a specific listener (by the exact triple). Safe if not present.
void SignalStopListening(const char *name, SignalHandler fn, void *user);

// Fire now: run every listener registered for `name`, in registration order.
void SignalEmit(const char *name);

// Diagnostics (used by tests / editor): how many listeners match a name.
int  SignalListenerCount(const char *name);

#endif // SIGNAL_H
