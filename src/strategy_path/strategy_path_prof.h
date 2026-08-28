// ============================================================================
//  strategy_path_prof.h  -  per-subsystem frame timing, header-only
//
//  WHY THIS EXISTS. Nothing in this codebase measured anything before now. The
//  movement overhaul is a performance project, and a performance project
//  without numbers is a guessing project - so this lands FIRST, before a single
//  line of pathfinding, and every later phase is judged against the baseline it
//  produces.
//
//  ALWAYS COMPILED, RUNTIME GATED. The obvious design is #ifdef SP_PROFILE, but
//  the instrumentation points live in strategy_world.c, which is shared by the
//  game and the path lab. A compile-time switch would mean either instrumenting
//  the shipping game unconditionally or building the world twice. So the bodies
//  are always here and cost one bool test when off. Ten Begin/End pairs is
//  twenty GetTime() calls per frame - against a 16.6 ms budget that is noise,
//  and it means the overlay can be toggled live with a key.
//
//  GetTime() IS THE CLOCK on purpose: raylib's own double-seconds timer works
//  identically on desktop and Web, where QueryPerformanceCounter and
//  clock_gettime do not.
//
//  SMOOTHED, NOT RAW. Raw per-frame milliseconds at 60 Hz are unreadable - the
//  numbers strobe. Everything reported is exponentially smoothed; the raw value
//  is kept only for the current frame's accumulation.
//
//  NESTING IS NOT SUPPORTED. Begin/End on the same slot must not overlap, and a
//  slot must not enclose itself. Slots are peers, not a call tree; two slots
//  that overlap in wall time will both be right and will sum to more than the
//  frame. Keep the instrumentation flat.
// ============================================================================

#ifndef STRATEGY_PATH_PROF_H
#define STRATEGY_PATH_PROF_H

#include "raylib.h"
#include <stdbool.h>

typedef enum {
    SP_PROF_NAV_HASH = 0,   // spatial hash rebuild
    SP_PROF_ASTAR,          // A* service: queue drain + searches
    SP_PROF_FLOW,           // flow field builds
    SP_PROF_STEER,          // path following / steering integration
    SP_PROF_SEPARATE,       // unit-vs-unit push
    SP_PROF_UNIT_UPDATE,    // the per-unit state machine
    SP_PROF_ANIM,           // entity anim update + retire
    SP_PROF_AI,             // StrategyAiTick
    SP_PROF_DRAW_UNITS,     // the unit draw loop alone
    SP_PROF_DRAW_WORLD,     // terrain, buildings, nodes, effects
    SP_PROF_COUNT
} SpProfSlot;

// -- Counters -----------------------------------------------------------------
// Times alone do not explain a slowdown. A spike in "paths failed" or a pinned
// request queue tells you WHY in a way a millisecond number never does, so the
// overlay carries these next to the timings.
typedef enum {
    SP_COUNT_UNITS_ACTIVE = 0,
    SP_COUNT_UNITS_SELECTED,
    SP_COUNT_PATH_REQUESTS,     // queue depth this frame
    SP_COUNT_PATH_ACTIVE,       // units currently following a path
    SP_COUNT_PATH_PENDING,      // units waiting on a search
    SP_COUNT_PATH_FAILED,       // searches that returned no route
    SP_COUNT_PATH_PARTIAL,      // paths truncated at SP_PATH_MAX
    SP_COUNT_FLOW_LIVE,         // flow fields resident
    SP_COUNT_FLOW_HIT,          // field cache hits this frame
    SP_COUNT_FLOW_MISS,         // field cache misses this frame
    SP_COUNT_ASTAR_NODES,       // nodes expanded this frame
    SP_COUNT_DRAWN_UNITS,       // units that survived culling
    SP_COUNT_UNITS_MOVING,      // units in a move state this frame
    SP_COUNT_UNITS_SETTLED,     // ...of those, ones that have parked. A pile
                                //   that has stopped spiralling reads as
                                //   settled == moving and STAYS there; one
                                //   still orbiting shows the number churning,
                                //   which is the whole acceptance test
    SP_COUNT_HASH_DROPPED,      // units the spatial hash could not hold - any
                                //   nonzero value means separation has holes
                                //   in it, which reads in-game as units walking
                                //   through each other at the back of a crowd
    SP_COUNT_COUNT
} SpProfCounter;

// ----------------------------------------------------------------------------
//  State. Header-only, so one translation unit must define
//  SP_PROF_IMPLEMENTATION before including this. strategy_world.c does.
// ----------------------------------------------------------------------------
#ifdef SP_PROF_IMPLEMENTATION
    #define SP_PROF_LINKAGE
#else
    #define SP_PROF_LINKAGE extern
#endif

SP_PROF_LINKAGE bool   spProfEnabled;
SP_PROF_LINKAGE double spProfStart[SP_PROF_COUNT];   // Begin timestamp
SP_PROF_LINKAGE double spProfAccum[SP_PROF_COUNT];   // this frame's total
SP_PROF_LINKAGE float  spProfMs[SP_PROF_COUNT];      // smoothed, reported
SP_PROF_LINKAGE int    spProfCount[SP_COUNT_COUNT];

#ifdef SP_PROF_IMPLEMENTATION
bool   spProfEnabled = false;
double spProfStart[SP_PROF_COUNT];
double spProfAccum[SP_PROF_COUNT];
float  spProfMs[SP_PROF_COUNT];
int    spProfCount[SP_COUNT_COUNT];
#endif

// ----------------------------------------------------------------------------
//  API. Everything is static inline: no .c file, no link order, and the whole
//  thing folds to nothing when disabled.
// ----------------------------------------------------------------------------
static inline void SpProfBegin(SpProfSlot slot)
{
    if (!spProfEnabled) return;
    spProfStart[slot] = GetTime();
}

static inline void SpProfEnd(SpProfSlot slot)
{
    if (!spProfEnabled) return;
    spProfAccum[slot] += GetTime() - spProfStart[slot];
}

// Roll this frame's accumulators into the smoothed history. Call once per
// frame, at the very end. The 0.9/0.1 blend settles in ~20 frames: fast enough
// to see a spawn spike, slow enough to read.
static inline void SpProfFrame(void)
{
    for (int i = 0; i < SP_PROF_COUNT; i++)
    {
        float sample = (float)(spProfAccum[i]*1000.0);
        spProfMs[i]  = spProfMs[i]*0.9f + sample*0.1f;
        spProfAccum[i] = 0.0;
    }
}

static inline float SpProfMs(SpProfSlot slot) { return spProfMs[slot]; }

static inline void SpProfSet(SpProfCounter c, int v) { spProfCount[c] = v; }
static inline void SpProfAdd(SpProfCounter c, int v) { spProfCount[c] += v; }
static inline int  SpProfGet(SpProfCounter c)        { return spProfCount[c]; }

// Counters that describe THIS frame (nodes expanded, units drawn) must be
// zeroed each frame; counters that describe current state (units active, fields
// live) must not. Only the per-frame ones are listed here.
//
// FLOW HIT/MISS ARE DELIBERATELY NOT RESET. They are incremented by a GROUP
// ORDER, which happens on the frame the player clicks and on no other - zeroing
// them per frame would leave the overlay reading `hit 0 miss 0` permanently
// except for the single frame of the click, which nobody can see. As session
// totals the ratio between them is the number that matters, and it is the
// direct measure of whether goal coarsening is earning its place.
static inline void SpProfResetFrameCounters(void)
{
    spProfCount[SP_COUNT_ASTAR_NODES] = 0;
    spProfCount[SP_COUNT_DRAWN_UNITS] = 0;
}

static inline const char *SpProfName(SpProfSlot slot)
{
    switch (slot)
    {
        case SP_PROF_NAV_HASH:    return "hash";
        case SP_PROF_ASTAR:       return "astar";
        case SP_PROF_FLOW:        return "flow";
        case SP_PROF_STEER:       return "steer";
        case SP_PROF_SEPARATE:    return "separate";
        case SP_PROF_UNIT_UPDATE: return "unitupd";
        case SP_PROF_ANIM:        return "anim";
        case SP_PROF_AI:          return "ai";
        case SP_PROF_DRAW_UNITS:  return "drawunits";
        case SP_PROF_DRAW_WORLD:  return "drawworld";
        default:                  return "?";
    }
}

// Sum of every slot. Not the frame time - vsync, the GPU and everything
// uninstrumented live outside it. The GAP between this and the real frame time
// is itself the useful number: it is what you have not measured yet.
static inline float SpProfTotalMs(void)
{
    float total = 0.0f;
    for (int i = 0; i < SP_PROF_COUNT; i++) total += spProfMs[i];
    return total;
}

#endif // STRATEGY_PATH_PROF_H
