// ============================================================================
//  strategy_entity_anim.c  -  see strategy_entity_anim.h
// ============================================================================

#include "strategy_entity_anim.h"
#include "raymath.h"
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
//  Facing
// ---------------------------------------------------------------------------
void StrategyEntityFace(Unit *u, Vector3 dir)
{
    if (u == NULL) return;

    dir.y = 0.0f;
    if (Vector3Length(dir) < 0.0001f) return;

    // atan2(x, z) - not the usual (z, x) - because the models are authored
    // facing +Z, and rlRotatef turns around Y. Getting this pair backwards is
    // the classic way to end up with units walking sideways.
    u->anim.yawTarget = atan2f(dir.x, dir.z)*RAD2DEG;
    u->anim.walking   = true;

    if (!u->anim.hasYaw)        // spawned facing nowhere: snap, do not sweep
    {
        u->anim.yaw    = u->anim.yawTarget;
        u->anim.hasYaw = true;
    }
}

// Shortest way round from a to b, in degrees. Without this a unit turning from
// 170 to -170 takes the 340-degree route and visibly spins the wrong way.
static float AngleDelta(float a, float b)
{
    float d = fmodf(b - a + 540.0f, 360.0f) - 180.0f;
    return d;
}

// ---------------------------------------------------------------------------
//  State mapping
// ---------------------------------------------------------------------------
int StrategyEntityContinuousState(const Unit *u)
{
    if (u == NULL) return SGA_STATE_IDLE;

    switch (u->state)
    {
        // Plainly walking somewhere.
        case UNIT_MOVE:
        case UNIT_FLEE:
        case UNIT_RETURN:
        case UNIT_FOLLOW:
            return SGA_STATE_MOVING;

        // Work states are a walk THEN a task, and the unit stays in the same
        // UnitState for both halves. So the walking flag - set by MoveToward
        // itself - is what tells the two apart, rather than a distance test
        // duplicated out of the sim and liable to drift from it.
        case UNIT_GATHER:
        case UNIT_BUILD:
        case UNIT_REPAIR:
        case UNIT_FARM:
            return u->anim.walking ? SGA_STATE_MOVING : SGA_STATE_IDLE;

        // Chasing reads as moving; in range it reads as attacking. A kiting
        // ranged unit is genuinely doing both, and StrategyEntityAnimSet adds
        // MOVING alongside this - the ladder then gives contested parts to
        // ATTACKING, which is the intent.
        case UNIT_ATTACK:
            return SGA_STATE_ATTACKING;

        case UNIT_IDLE:
        case UNIT_BLESS:
        default:
            return SGA_STATE_IDLE;
    }
}

// ---------------------------------------------------------------------------
//  Events
// ---------------------------------------------------------------------------
void StrategyEntityAnimEvent(Unit *u, int state)
{
    if (u == NULL) return;
    if ((state < 0) || (state >= SGA_STATE_COUNT)) return;

    // Restart from zero rather than adding time: being hit twice should replay
    // the flinch, not run it at double length.
    //
    // The flag only says "an event is running" - it is not a countdown, because
    // the length belongs to the ASSET and this function cannot see one (the
    // fire sites are deep in combat code that has no business resolving a
    // binding). StrategyEntityAnimSet retires it against the real duration.
    u->anim.clock[state]   = 0.0f;
    u->anim.oneShot[state] = 1.0f;
}

// ---------------------------------------------------------------------------
//  Per-frame advance
// ---------------------------------------------------------------------------
void StrategyEntityAnimUpdate(Unit *u, float dt)
{
    if (u == NULL) return;

    // Turn toward the heading recorded during the move pass.
    if (u->anim.hasYaw)
    {
        float d = AngleDelta(u->anim.yaw, u->anim.yawTarget);
        float step = STRAT_YAW_RATE*dt;
        if (fabsf(d) <= step) u->anim.yaw = u->anim.yawTarget;
        else                  u->anim.yaw += (d > 0.0f) ? step : -step;

        u->anim.yaw = fmodf(u->anim.yaw + 360.0f, 360.0f);
    }

    for (int s = 0; s < SGA_STATE_COUNT; s++) u->anim.clock[s] += dt;

    // Cleared here, at the END of the frame's animation work, so it reflects
    // the move pass that just ran. MoveToward sets it again next frame.
    u->anim.walking = false;
}

// ---------------------------------------------------------------------------
//  Building the set
// ---------------------------------------------------------------------------
static float LoopedClock(const SgaAsset *a, int state, float clock)
{
    float dur = (a != NULL) ? a->duration[state] : 0.0f;
    if (dur <= 0.0f) return 0.0f;       // a still state holds its first pose

    float t = fmodf(clock, dur);
    return (t < 0.0f) ? 0.0f : t;
}

// Drops event flags whose animation has finished. Separate from AnimSet because
// that one takes a const Unit - the set is built during drawing, and drawing has
// no business mutating the world.
void StrategyEntityAnimRetire(Unit *u, const SgaAsset *a)
{
    if (u == NULL) return;

    for (int s = 0; s < SGA_STATE_COUNT; s++)
    {
        if (u->anim.oneShot[s] <= 0.0f) continue;

        // No asset, or nothing authored for this state, means there is nothing
        // to wait for - retire immediately rather than leaving a flag set
        // forever on a unit whose binding was cleared mid-flinch.
        float dur = (a != NULL) ? a->duration[s] : 0.0f;
        if ((dur <= 0.0f) || (u->anim.clock[s] >= dur)) u->anim.oneShot[s] = 0.0f;
    }
}

void StrategyEntityAnimSet(const Unit *u, const SgaAsset *a, SgaStateSet *out)
{
    if (out == NULL) return;
    StrategyAssetStateSetInit(out);
    if (u == NULL) return;

    // IDLE is ALWAYS on, and always at the bottom of the ladder. That single
    // rule is what makes a bobbing head survive walking, swinging and being
    // hit: those states claim the parts they animate, and IDLE quietly keeps
    // every part they did not.
    StrategyAssetStateSetAdd(out, SGA_STATE_IDLE,
                             LoopedClock(a, SGA_STATE_IDLE,
                                         u->anim.clock[SGA_STATE_IDLE]));

    int cont = StrategyEntityContinuousState(u);
    if (cont != SGA_STATE_IDLE)
        StrategyAssetStateSetAdd(out, cont,
                                 LoopedClock(a, cont, u->anim.clock[cont]));

    // A unit that is attacking AND still closing or kiting is legitimately in
    // both states; the per-part ladder sorts out anything they both animate.
    if ((cont == SGA_STATE_ATTACKING) && u->anim.walking)
        StrategyAssetStateSetAdd(out, SGA_STATE_MOVING,
                                 LoopedClock(a, SGA_STATE_MOVING,
                                             u->anim.clock[SGA_STATE_MOVING]));

    // One-shots play ONCE and are not wrapped - a flinch that looped would read
    // as a stutter. The clock is clamped to the state's duration so the pose
    // holds on the final key until the countdown expires.
    for (int s = 0; s < SGA_STATE_COUNT; s++)
    {
        if (u->anim.oneShot[s] <= 0.0f) continue;

        float dur = (a != NULL) ? a->duration[s] : 0.0f;
        if (dur <= 0.0f) continue;      // nothing authored: nothing to play
        if (u->anim.clock[s] >= dur) continue;   // finished; retired below

        StrategyAssetStateSetAdd(out, s, u->anim.clock[s]);
    }
}
