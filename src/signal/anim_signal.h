// ============================================================================
//  anim_signal.h  -  bridge that wires an AnimDoc's signals to the signal bus
//
//  Keeps anim.* and signal.* independent: neither knows about the other. This
//  bridge registers, for each AnimSignal in a doc, a listener that starts the
//  given AnimPlayer in the signal's direction/section. Then any code that calls
//  SignalEmit("<name>") plays the animation.
//
//  Ownership: the caller owns both the AnimDoc and the AnimPlayer (both live as
//  long as the registration). Call AnimSignalUnregister before either dies.
// ============================================================================

#ifndef ANIM_SIGNAL_H
#define ANIM_SIGNAL_H

#include "../anim/anim.h"

// Register listeners for every signal in `doc`; firing one starts `player`.
void AnimSignalRegister(const AnimDoc *doc, AnimPlayer *player);

// Remove the listeners previously registered for this (doc, player) pair.
void AnimSignalUnregister(const AnimDoc *doc, AnimPlayer *player);

#endif // ANIM_SIGNAL_H
