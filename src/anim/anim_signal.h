// ============================================================================
//  anim_signal.h  -  bridge that wires an AnimDoc's signals to the signal bus
//
//  Keeps anim.* and signal.* independent: neither knows about the other. This
//  bridge registers, for each AnimSignal in a doc, a listener that starts the
//  given AnimSignalPlayer on that signal. Then any code that calls
//  SignalEmit("<name>") runs the transition.
//
//  A signal eases from the scene's LIVE pose (see AnimSignalPlayerStart), so
//  firing needs to know what time the document is currently being shown at.
//  That is read through `docTime` at fire time - point it at whatever clock
//  drives your AnimDocDraw (the editor's playhead, a player's sample time).
//  It may be NULL, which is read as 0.
//
//  Ownership: the caller owns the AnimDoc, the AnimSignalPlayer and the
//  docTime float (all live as long as the registration). Call
//  AnimSignalUnregister before any of them dies.
// ============================================================================

#ifndef ANIM_SIGNAL_H
#define ANIM_SIGNAL_H

#include "anim.h"

// Register listeners for every signal in `doc`; firing one starts `player` on
// that signal, capturing the live pose sampled at *docTime.
void AnimSignalRegister(const AnimDoc *doc, AnimSignalPlayer *player,
                        const float *docTime);

// Remove the listeners previously registered for this (doc, player) pair.
void AnimSignalUnregister(const AnimDoc *doc, AnimSignalPlayer *player);

#endif // ANIM_SIGNAL_H
