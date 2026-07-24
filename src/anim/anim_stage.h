// ============================================================================
//  anim_stage.h  -  runtime playback of editor-authored animations in a scene
//
//  The anim editor (anim_editor.c) authors AnimDocs and saves them one per file
//  under anims/. This module is what PLAYS them in an actual app state: it owns
//  a fixed set of SLOTS, each holding a loaded document, its AnimPlayer, its
//  AnimSignalPlayer and a draw LAYER, and it drives them all from two calls a
//  state makes every frame:
//
//      Update() : AnimStageUpdate(GetFrameTime());
//      Draw()   : AnimStageDraw();          // LAST - after the scene's own art
//      Exit()   : AnimStageStopAll();
//
//  DRAW SPACE. AnimStageDraw draws in GAME space (the render texture), so it
//  belongs in a state's Draw(), never its Gui(). That puts an animation over
//  everything the scene drew and UNDER the raygui widgets (which main.c draws
//  afterwards, straight to the framebuffer) - so the GUI always stays visible
//  and clickable. Authored fractions then also land exactly where the editor
//  previewed them, since the editor previews in the same space.
//
//  TRANSPARENCY. Nothing here composites specially: a document is simply drawn
//  on top of whatever the scene already rendered. What shows through is decided
//  by the AUTHORED global element - AP_G_BG_ALPHA 0 draws no background fill at
//  all (see DrawGlobalBackground in anim.c), so the scene below is fully
//  visible, and AP_G_FADE is the deliberate full-screen cover drawn last.
//
//  LAYERING. Several animations can play at once; slots are drawn in ascending
//  `layer`, ties broken by slot index (so an earlier AnimStagePlay wins). A
//  higher layer draws IN FRONT.
//
//  SIGNALS. Every slot registers its document's signals on the global bus
//  (signal.h) when it starts, so SignalEmit("<name>") reaches EVERY playing
//  instance that declares that name - name signals uniquely to scope them. A
//  signal marked `terminal` (AnimSignal.terminal, authored in the editor) ends
//  the instance when its player has run its full length: a looping animation
//  winds down through the authored transition instead of being cut off. Both
//  one-shot and looping playback can run signals.
//
//  Fixed capacity, no heap, singleton house style (AnimStageReset alongside
//  SignalReset in main.c).
// ============================================================================

#ifndef ANIM_STAGE_H
#define ANIM_STAGE_H

#include <stdbool.h>
#include "signal.h"   // SignalParams (per-emit position parameter)

// How many animations can play at once. Slots are a fixed pool: each holds a
// whole AnimDoc BY VALUE, so this is the module's entire memory footprint.
#define ANIM_STAGE_SLOTS_MAX 8

// Handle to one playing animation. Stable while that instance lives; -1 is the
// invalid/never-started value. A handle of a finished instance is safely inert
// (AnimStageAlive false, Stop a no-op) and is never reused by a later Play.
typedef int AnimHandle;

#define ANIM_HANDLE_NONE (-1)

// Clear every slot without firing callbacks. Call once at startup, next to
// SignalReset(). Also unregisters any listeners the slots had.
void AnimStageReset(void);

// Load anims/<name>.cfg and start playing it on a free slot.
//   loop  false -> plays the intro then ONE loop body, then stops (onDone fires)
//         true  -> plays the intro once, then repeats the loop body forever
//                  (the intro/outro trim is AnimPlayer's, see anim.h)
//   layer draw order, ascending; higher draws in front.
// Returns ANIM_HANDLE_NONE if every slot is busy or the file could not be read.
AnimHandle AnimStagePlay(const char *name, bool loop, int layer);

// As AnimStagePlay, but the instance waits `delay` seconds before it starts.
// While waiting it is ALIVE (the handle is valid, its signals are registered)
// but draws nothing and does not advance - a true stagger, so the SAME document
// can be played several times at different offsets without duplicating the file.
// It occupies its slot and counts toward AnimStageActiveCount from the moment it
// is played, so a delayed instance still costs one of the ANIM_STAGE_SLOTS_MAX.
// delay <= 0 is identical to AnimStagePlay.
AnimHandle AnimStagePlayEx(const char *name, bool loop, int layer, float delay);

// As AnimStagePlayEx, plus this instance's SEQUENCE NUMBER - the integer a
// signal's sequence offset multiplies by seqMult (see AnimSignal.usesSeq in
// anim.h). Playing the same document three times with seq 0/1/2 is what lets one
// authored transition fan its copies apart in size or position. AnimStagePlayEx
// is this with seq = 0.
AnimHandle AnimStagePlaySeq(const char *name, bool loop, int layer, float delay,
                            int seq);

// Stop an instance NOW (an abrupt cut-off, as opposed to letting a terminal
// signal end it). Fires its done callback. Unknown/finished handles are ignored.
void AnimStageStop(AnimHandle h);

// Stop every instance. A state's Exit() should call this so no slot outlives it.
void AnimStageStopAll(void);

// Is this instance still playing?
bool AnimStageAlive(AnimHandle h);

// Call `fn(user)` once, when this instance stops - whether it ran out, was
// ended by a terminal signal, or was stopped explicitly. Ignored for a handle
// that is not alive (there would be no stop left to report).
void AnimStageSetDoneCallback(AnimHandle h, void (*fn)(void *user), void *user);

// Fire signal `name` on ONE instance only, bypassing the global bus. Use when
// several instances share a signal name and only this one should react.
// `params` (may be NULL for none) carries the emit's position parameter so the
// transition can be placed per-instance (see SignalParams in signal.h).
void AnimStageEmit(AnimHandle h, const char *name, const SignalParams *params);

// Is this instance currently running a TERMINAL signal - i.e. is it already on
// its way out, and will it report done on its own? Use this before waiting on
// a done callback after emitting an ending signal: if the emit did not actually
// arm a shutdown (wrong name, or the signal is not marked terminal) nothing
// would ever end the animation and the waiting code would hang.
bool AnimStageEndsOnCurrentSignal(AnimHandle h);

// Advance every playing instance. Call once per frame from a state's Update().
void AnimStageUpdate(float dt);

// Draw every playing instance, in ascending layer order. Call from a state's
// Draw(), AFTER the scene's own art (see the DRAW SPACE note above).
void AnimStageDraw(void);

// How many instances are currently playing (diagnostics / tests).
int AnimStageActiveCount(void);

// The order AnimStageDraw would visit: writes up to `max` SLOT indices into
// `out`, ascending (layer, slot), and returns how many. Exposed so the layering
// can be asserted in a headless test - pair it with AnimStageSlotOf to compare
// against handles rather than hard-coding how a handle is packed.
int AnimStageDrawOrder(int *out, int max);

// The slot index a handle occupies, or -1 if it is not alive.
int AnimStageSlotOf(AnimHandle h);

#endif // ANIM_STAGE_H
