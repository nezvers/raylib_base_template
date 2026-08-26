// ============================================================================
//  strategy_entity_anim.h  -  turning game facts into an SgaStateSet
//
//  This is the bridge, and it is the only file that has to know BOTH the game's
//  UnitState and the asset module's SgaState. strategy_asset/ stays headless and
//  strategy_types.h stays ignorant of animation; the translation lives here.
//
//  TWO KINDS OF STATE, and the distinction drives everything below:
//
//    CONTINUOUS states describe what a unit IS doing and are derived fresh from
//    u->state every frame. They need no memory - stop walking and MOVING simply
//    stops being derived. IDLE is always on, at the bottom of the ladder, so it
//    fills in every part no louder state claimed.
//
//    EVENT states describe what just HAPPENED to a unit - damaged, healed, died.
//    Nothing in the world model records these; they are moments, not conditions,
//    and the moment is gone by the next frame. So they are fired as ONE-SHOTS
//    from the places where the thing actually occurs, and they run themselves
//    out on a countdown.
//
//  Each state clocks SEPARATELY. One shared clock would force MOVING and IDLE
//  to share a period, so the shorter of the two would jump mid-cycle every time
//  the longer one wrapped. Six floats is a cheap price for both looping cleanly.
// ============================================================================

#ifndef STRATEGY_ENTITY_ANIM_H
#define STRATEGY_ENTITY_ANIM_H

#include "strategy_types.h"
#include "../../strategy_asset/strategy_asset.h"

// The per-unit animation memory itself lives on Unit (strategy_types.h) as the
// `anim` block, so it travels with the unit slot and is cleared by the same
// memset that spawns one. Its fields:
//
//    yaw / yawTarget  facing in degrees, and where it is turning to
//    hasYaw           false until the unit has moved once (then yaw snaps)
//    walking          set by MoveToward, cleared at the end of the anim update
//    clock[state]     free-running seconds, wrapped against the state duration
//    oneShot[state]   non-zero while an EVENT animation is still playing
// Degrees per second the facing turns. Fast enough to look responsive, slow
// enough that a unit re-targeting mid-walk sweeps rather than snaps.
#define STRAT_YAW_RATE  540.0f

// Record where a unit is heading. Called from MoveToward, which already has the
// direction - deriving it anywhere else would mean recomputing it.
void StrategyEntityFace(Unit *u, Vector3 dir);

// Fire an event animation. Safe to call when the unit has no authored asset or
// the asset has no keys for that state: it just does not play. `state` is an
// SgaState (DAMAGED / HEALED / ...).
void StrategyEntityAnimEvent(Unit *u, int state);

// Advance every clock. Called once per unit per frame, before drawing.
void StrategyEntityAnimUpdate(Unit *u, float dt);

// Drops event flags whose animation has run out. Called once per unit per frame
// with that unit's resolved asset; a unit with no asset retires them at once.
void StrategyEntityAnimRetire(Unit *u, const SgaAsset *a);

// Fill `out` with every state the unit is currently in. The asset is needed for
// the loop durations, and may be NULL (then only IDLE is reported at time 0).
void StrategyEntityAnimSet(const Unit *u, const SgaAsset *a, SgaStateSet *out);

// The continuous SgaState a UnitState maps to. Exposed for the tests, which
// check that every one of the game's states has an answer.
int  StrategyEntityContinuousState(const Unit *u);

#endif // STRATEGY_ENTITY_ANIM_H
