// ============================================================================
//  strategy_move.c  -  path following: the bridge between routes and units
//
//  THE MODULE LINE. src/strategy_path/ computes WHERE TO GO - it takes a cost
//  grid and two tiles and returns cell indices, and it has never heard of a
//  Unit. This file decides WHO GOES THERE AND HOW: which units get a path at
//  all, when to ask for a new one, which waypoint to steer at, and when to give
//  up and walk straight. Everything here needs `Unit`, which is exactly why it
//  cannot live in the headless module.
//
//  No header, no state of its own beyond the path pool; it fetches
//  StrategyWorldGet() and threads StrategyWorld* through statics, following
//  strategy_ai.c. Public entry points are declared in strategy_world.h.
//
//  WHY PATHS LIVE IN A SIDE ARRAY. Twenty-four waypoints inlined into Unit
//  would grow a 264-byte struct by half, and Unit is walked three times a frame
//  by loops - separation, drawing, HP bars - that have no interest in paths.
//  s_paths[] is indexed by the same unit index and touched only by units that
//  are actually walking somewhere.
//
//  NOT EVERY MOVE IS A PATH, AND THIS IS THE IMPORTANT PART. Of the sixteen
//  places the old code called MoveToward, only nine ask for a route. The rule:
//
//      static destination, possibly far   -> MoveTo (path)
//      target that MOVES, or a shuffle of
//      a couple of units                  -> MoveDirect (steering, as before)
//
//  So walking to a tree, a building or a clicked point is pathed; chasing an
//  enemy, shadowing an ally, kiting backwards and stepping to a plant spot are
//  not. Routing the chase cases through A* would mean a search per unit per
//  frame against a target that has already moved by the time the path lands -
//  maximum cost for a stale answer. Routing the two-unit shuffles through it
//  would mean ten thousand searches for units adjusting their footing.
// ============================================================================

#include "strategy_world.h"
#include "strategy_entity_anim.h"       // StrategyEntityFace: heading on each step
#include "../../strategy_path/strategy_path.h"
#include "../../strategy_path/strategy_path_prof.h"

#include "raymath.h"
#include <math.h>

// ----------------------------------------------------------------------------
//  Tuning
// ----------------------------------------------------------------------------
// How close to a waypoint counts as reaching it. Generous on purpose: a
// waypoint is a hint about which way to go, not a place to stand. Tight
// tolerances make units in a crowd stop dead at each corner while separation
// nudges them off it, which reads as stuttering.
#define MOVE_WAYPOINT_REACH   (2.0f*STRAT_UNIT_RADIUS)

// Below this distance a path is not worth asking for - the unit is nearly
// there and steering straight is both cheaper and smoother. Also the guard
// that stops a settled unit re-requesting on every tiny re-order.
#define MOVE_PATH_MIN_DIST    2.5f

// A unit that has made less than this much progress toward its current
// waypoint for MOVE_BLOCKED_TIME seconds asks for a new path. This is the only
// repath trigger besides a new order and a nav change: repathing on a TIMER
// would mean, at ten thousand units, a guaranteed and permanent budget
// overrun no matter how the budget is tuned.
#define MOVE_BLOCKED_EPS      0.05f
#define MOVE_BLOCKED_TIME     0.75f

// Global limiter on REFRESHES per frame - a unit that already has a route
// asking for a better one. First paths are never limited; see StrategyMoveTo.
//
// THE LIMIT ALONE IS NOT FAIR. Units are updated in active-list order, so a
// naive budget is always spent by whoever comes first, and a wedged unit late
// in the list waits forever while the same early ones refresh every frame. So
// each unit also carries a phase: it may only attempt a refresh on frames where
// (frame + phase) lands on its slot. The two together give every unit the same
// share of a budget it can never monopolise.
#define MOVE_REPATH_PER_FRAME 32
#define MOVE_REPATH_PERIOD    8     // frames between a given unit's attempts

typedef struct {
    SpCell      cell[SP_PATH_MAX];
    uint8_t     count;
    uint8_t     cursor;             // next waypoint to steer at
    bool        pending;            // a search is in flight for this unit
    SpRequestId request;
    uint32_t    navVersion;         // grid version the path was built against
    Vector3     goal;               // what was actually asked for
    float       blockedTime;
    float       lastDist;           // distance to the current waypoint last tick
} UnitPath;

static UnitPath s_paths[STRAT_MAX_UNITS];
static int      s_repathBudget;
static uint32_t s_moveFrame;

// ----------------------------------------------------------------------------
//  Helpers
// ----------------------------------------------------------------------------
static float MoveDistXZ(Vector3 a, Vector3 b)
{
    float dx = a.x - b.x, dz = a.z - b.z;
    return sqrtf(dx*dx + dz*dz);
}

static void PathClear(int index)
{
    UnitPath *p = &s_paths[index];
    if (p->pending) SpServiceCancel(p->request);
    p->count       = 0;
    p->cursor      = 0;
    p->pending     = false;
    p->request     = SP_REQUEST_NONE;
    p->blockedTime = 0.0f;
}

void StrategyMoveInit(void)
{
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        s_paths[i].count   = 0;
        s_paths[i].cursor  = 0;
        s_paths[i].pending = false;
        s_paths[i].request = SP_REQUEST_NONE;
        s_paths[i].blockedTime = 0.0f;
    }
    s_moveFrame = 0;
}

void StrategyMoveForget(int index)
{
    if (index < 0 || index >= STRAT_MAX_UNITS) return;
    PathClear(index);
}

// ----------------------------------------------------------------------------
//  Steering primitives
// ----------------------------------------------------------------------------
// Plain lerp toward a point - what every call site did before this file
// existed, and still the right answer for a moving target or a short shuffle.
// Kept here rather than in strategy_world.c so that all movement, pathed or
// not, goes through one place.
void StrategyMoveDirect(Unit *u, Vector3 dest, float dt)
{
    Vector3 delta = Vector3Subtract(dest, u->pos);
    delta.y = 0.0f;
    float dist = Vector3Length(delta);
    if (dist < 0.001f) return;

    float step = u->moveSpeed*dt;
    if (step > dist) step = dist;
    u->pos = Vector3Add(u->pos, Vector3Scale(delta, step/dist));

    StrategyEntityFace(u, delta);
}

// ----------------------------------------------------------------------------
//  Requesting
// ----------------------------------------------------------------------------
static void RequestPath(int index, Unit *u, Vector3 goal)
{
    const SpGrid *g = StrategyNavGrid();
    UnitPath *p = &s_paths[index];

    int sx, sz, gx, gz;
    SpWorldToTile(g, u->pos.x, u->pos.z, &sx, &sz);
    SpWorldToTile(g, goal.x,  goal.z,  &gx, &gz);

    p->goal        = goal;
    p->navVersion  = StrategyNavVersion();
    p->blockedTime = 0.0f;

    SpRequestId id = SpServiceRequest(index, sx, sz, gx, gz);
    if (id == SP_REQUEST_NONE)
    {
        // Queue full, or no open tile within the ring cap. The unit keeps its
        // old path if it has one and otherwise steers straight - degraded, not
        // broken, and counted so the overlay can show it.
        p->pending = false;
        return;
    }
    p->request = id;
    p->pending = true;
}

// Does this unit need a new route to `goal`?
static bool NeedsPath(int index, const Unit *u, Vector3 goal, float dist)
{
    const UnitPath *p = &s_paths[index];

    if (dist < MOVE_PATH_MIN_DIST) return false;    // close enough to walk at it
    if (p->pending)                                  // one search at a time
    {
        // ...unless the destination moved meaningfully since we asked. A unit
        // re-ordered mid-search must not arrive at where it was first sent.
        return (MoveDistXZ(p->goal, goal) > MOVE_WAYPOINT_REACH);
    }
    if (p->count == 0)                        return true;   // nothing to follow
    if (p->navVersion != StrategyNavVersion()) return true;  // a wall appeared
    if (MoveDistXZ(p->goal, goal) > MOVE_WAYPOINT_REACH) return true;
    if (p->blockedTime > MOVE_BLOCKED_TIME)   return true;   // wedged
    return false;
}

// ----------------------------------------------------------------------------
//  The public mover
//
//  Steer `u` toward `dest`, using a path when the trip is long enough to be
//  worth one. Returns the point actually being steered at, which is the next
//  waypoint rather than the final goal - callers that want to know whether the
//  unit has ARRIVED still test against `dest` themselves, exactly as they did
//  when this was a straight lerp.
// ----------------------------------------------------------------------------
void StrategyMoveTo(Unit *u, int index, Vector3 dest, float dt)
{
    UnitPath *p = &s_paths[index];
    const SpGrid *g = StrategyNavGrid();
    float dist = MoveDistXZ(u->pos, dest);

    if (NeedsPath(index, u, dest, dist))
    {
        // The repath limiter applies to REFRESHES, not to first paths: a unit
        // with no route at all is the case where waiting is most visible, and
        // there are only ever as many of those as there were orders this frame.
        bool refresh = (p->count > 0);
        bool myTurn  = (((s_moveFrame + (uint32_t)index) % MOVE_REPATH_PERIOD) == 0);
        if (!refresh || (myTurn && s_repathBudget > 0))
        {
            if (refresh) s_repathBudget--;
            RequestPath(index, u, dest);
        }
    }

    // No usable route: walk at the destination. This is the old behaviour, and
    // it is deliberately the fallback for EVERY failure - queue full, search
    // still running, no route exists, destination too close to bother. A unit
    // in this game never stands still because pathfinding is busy.
    if (p->count == 0 || p->cursor >= p->count)
    {
        StrategyMoveDirect(u, dest, dt);
        return;
    }

    Vector3 wp = SpCellToWorld(g, p->cell[p->cursor]);
    float wpDist = MoveDistXZ(u->pos, wp);

    // Consume every waypoint we are already close enough to. A crowd can shove
    // a unit past two corners in one frame, and advancing only one per frame
    // would make it walk back to a waypoint it has already passed.
    while ((wpDist < MOVE_WAYPOINT_REACH) && (p->cursor < p->count))
    {
        p->cursor++;
        p->blockedTime = 0.0f;
        if (p->cursor >= p->count) break;
        wp = SpCellToWorld(g, p->cell[p->cursor]);
        wpDist = MoveDistXZ(u->pos, wp);
    }

    if (p->cursor >= p->count)
    {
        // Path spent. The last waypoint is the goal TILE CENTRE, which is up to
        // half a tile from the goal itself, so the final approach is always
        // direct - that half tile is also what stops a unit settling on the
        // wrong side of a tile boundary from whatever it was walking to.
        StrategyMoveDirect(u, dest, dt);
        return;
    }

    // Progress check for the wedge detector. Compared against the WAYPOINT, not
    // the goal: a unit walking a legitimate detour is moving away from its goal
    // for a while, and treating that as blocked would repath it in a loop.
    if ((p->lastDist - wpDist) < MOVE_BLOCKED_EPS) p->blockedTime += dt;
    else                                           p->blockedTime  = 0.0f;
    p->lastDist = wpDist;

    StrategyMoveDirect(u, wp, dt);
}

// ----------------------------------------------------------------------------
//  Frame hooks
// ----------------------------------------------------------------------------
void StrategyMoveBeginFrame(void)
{
    s_repathBudget = MOVE_REPATH_PER_FRAME;
    s_moveFrame++;
}

// Drain finished searches into the path pool. Called once a frame, after
// SpServiceUpdate and before the unit state machine, so a path that landed this
// frame is walked this frame.
void StrategyMoveCollect(void)
{
    StrategyWorld *world = StrategyWorldGet();
    SpCell cells[SP_PATH_MAX];
    int32_t owner;
    SpPathStatus status;
    int count = 0;
    int failed = 0, partial = 0;

    while (SpServicePoll(&owner, &status, cells, (int)SP_PATH_MAX, &count))
    {
        if (owner < 0 || owner >= STRAT_MAX_UNITS) continue;
        UnitPath *p = &s_paths[owner];
        p->pending = false;
        p->request = SP_REQUEST_NONE;

        // The unit may have died, or been re-ordered, while the search ran.
        // Dropping the result is correct in both cases: a dead unit has no use
        // for it, and a re-ordered one already has a newer request in flight.
        if (!world->units[owner].active) { p->count = 0; continue; }

        if (status != SP_PATH_FOUND)
        {
            // No route. Leave count at 0 so the unit steers straight - which,
            // for a goal that is genuinely walled off, means it walks up to the
            // wall and stops there. That is the honest outcome, and the overlay
            // counts it so a spike is visible rather than mysterious.
            p->count = 0;
            failed++;
            continue;
        }

        p->count  = (uint8_t)count;
        p->cursor = 0;
        p->blockedTime = 0.0f;
        p->lastDist = 1e9f;
        for (int i = 0; i < count; i++) p->cell[i] = cells[i];
        if (count == (int)SP_PATH_MAX) partial++;
    }

    SpProfAdd(SP_COUNT_PATH_FAILED, failed);
    SpProfAdd(SP_COUNT_PATH_PARTIAL, partial);
}

// Overlay census. Separate from the collect pass because it counts what is
// TRUE now, not what changed this frame.
void StrategyMoveStats(void)
{
    int liveCount = 0;
    const int *live = StrategyActiveUnits(&liveCount);
    int active = 0, pending = 0;
    for (int k = 0; k < liveCount; k++)
    {
        const UnitPath *p = &s_paths[live[k]];
        if (p->pending)                          pending++;
        if (p->count > 0 && p->cursor < p->count) active++;
    }
    SpProfSet(SP_COUNT_PATH_ACTIVE, active);
    SpProfSet(SP_COUNT_PATH_PENDING, pending);
    SpProfSet(SP_COUNT_PATH_REQUESTS, SpServiceGetStats()->queued);
    SpProfSet(SP_COUNT_ASTAR_NODES, SpServiceGetStats()->expanded);
}

// ----------------------------------------------------------------------------
//  Debug read-out for the path lab's P overlay.
// ----------------------------------------------------------------------------
int StrategyMovePathOf(int index, Vector3 *out, int maxOut)
{
    if (index < 0 || index >= STRAT_MAX_UNITS) return 0;
    const UnitPath *p = &s_paths[index];
    const SpGrid *g = StrategyNavGrid();

    int n = 0;
    for (int i = p->cursor; i < p->count && n < maxOut; i++)
        out[n++] = SpCellToWorld(g, p->cell[i]);
    return n;
}
