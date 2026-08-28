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

// Distance from its own destination at which a unit stops riding the shared
// flow field and steers at its own slot instead.
//
// A field points every unit at ONE goal cell. A formation deliberately spreads
// them over many, so riding the field all the way in would funnel the whole
// group onto a single tile - rebuilding, at the last moment, exactly the pile
// the formation exists to prevent. Releasing on the approach costs nothing:
// the last few units of travel are open ground the group has already reached.
#define MOVE_FLOW_RELEASE     4.0f

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

    // Flow field this unit is riding, or SP_FIELD_NONE. Set by a group order
    // and never by an individual one: a field only pays for itself when many
    // units share the destination, and a lone unit is better served by the
    // exact route A* gives it than by a whole-grid approximation.
    SpFieldId   field;
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
    p->field       = SP_FIELD_NONE;
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
        s_paths[i].field   = SP_FIELD_NONE;
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

// Group cohesion, defined with the flow sweep that feeds it - a unit out in
// front of its group walks slower. 1.0 whenever it does not apply.
static float CohesionScale(SpFieldId field, int tx, int tz);

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

    // -- Flow field, if this unit is riding one ------------------------------
    // Checked BEFORE anything asks for a path, because the whole point of a
    // field is that a unit on one costs no search at all. Two array reads and
    // a step; that is what makes two thousand units to one place affordable.
    //
    // The field is dropped on the approach, not ridden to the last inch. A
    // field points at ONE goal cell, but every unit in a formation has its own
    // slot up to half a block away - so near the end the field would herd them
    // all onto the same tile, which is precisely the pile the formation exists
    // to prevent. The last few units of travel are steered directly at the
    // unit's own slot.
    if (p->field != SP_FIELD_NONE)
    {
        if (!SpFlowValid(p->field)) p->field = SP_FIELD_NONE;    // grid moved
        else if (dist > MOVE_FLOW_RELEASE)
        {
            int tx, tz;
            SpWorldToTile(g, u->pos.x, u->pos.z, &tx, &tz);
            float dx, dz;
            if (SpFlowDir(p->field, tx, tz, &dx, &dz))
            {
                Vector3 step = { u->pos.x + dx, 0.0f, u->pos.z + dz };
                // Cohesion is applied by scaling dt, the same trick the arrival
                // ramp uses: the unit still walks at its own speed toward the
                // same place, it just covers less ground this frame. Scaling
                // moveSpeed instead would leak into everything else that reads
                // it, animation rate included.
                StrategyMoveDirect(u, step, dt*CohesionScale(p->field, tx, tz));
                return;
            }
            // The field cannot route this tile - the unit is somewhere the goal
            // cannot be reached from. Fall through to A*, which will say so
            // properly rather than leaving it standing.
        }
    }

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

// Which flow field this unit is riding, or SP_FIELD_NONE. The lab's F overlay
// draws the field a SELECTED unit is on rather than all of them at once: with
// sixteen resident fields, drawing every arrow of every one is a solid mat that
// answers nothing, and the question being asked is always "what is THIS group
// following".
SpFieldId StrategyMoveFieldOf(int index)
{
    if (index < 0 || index >= STRAT_MAX_UNITS) return SP_FIELD_NONE;
    return s_paths[index].field;
}

// Where this unit was actually sent - its own formation slot, not the point the
// player clicked. The O overlay draws a line from each selected unit to this,
// which is the only way to see whether slot assignment is spatially coherent:
// crossed lines mean units are walking through the formation to reach a slot,
// and that is a sort-key bug, not a steering one.
bool StrategyMoveGoalOf(int index, Vector3 *out)
{
    if (index < 0 || index >= STRAT_MAX_UNITS) return false;
    const UnitPath *p = &s_paths[index];
    if (p->count == 0 && !p->pending && p->field == SP_FIELD_NONE) return false;
    *out = p->goal;
    return true;
}

// ============================================================================
//  Group orders and formations
//
//  SETTLING STOPS A PILE ORBITING. IT DOES NOT STOP IT BEING A PILE. Phase 2
//  measured both halves of that: 300 units sent to one point stop dead in 2.9
//  seconds and then move exactly zero - and 7,316 pairs end up standing inside
//  each other. The same steering code with FORMATION SLOTS instead of one
//  shared point gives 0 overlapping pairs, still with zero residual motion.
//  Nothing about the steering changed; only the targets did.
//
//  So the crowd is dispersed BY CONSTRUCTION rather than resolved by shoving:
//  a single click becomes an AREA, and each unit gets its own destination
//  spread across it. That is what this layer does, and it is the whole reason
//  0 overlaps at 300 units is achievable at all.
//
//  Three rules make it hold up, each of which is a bug if missed:
//    - RADIUS SCALES WITH COUNT. Twenty units get a tight knot, two thousand a
//      wide block. A fixed formation radius is exactly what makes big armies
//      pile.
//    - EVERY SLOT RESOLVES THROUGH SpNearestOpen. A slot in a lake is an
//      unreachable target and that unit orbits forever - the Phase 2 bug
//      reintroduced for a minority of units, which is far harder to notice.
//    - ASSIGNMENT IS SPATIALLY COHERENT. Units and slots are sorted on the same
//      axis and zipped, so nobody crosses the formation to reach their slot.
//      Assign naively and 200 units trade places on the spot.
// ============================================================================

#define FORMATION_SPACING     1.5f
#define FORMATION_MAX         512

// Above this many units sharing one destination, build a flow field instead of
// a path each. Runtime-tunable because the A/B - watch one field serve 2,000
// units, then raise the threshold past the group size and watch the queue pin -
// is the single most informative thing the lab can show.
static int s_flowThreshold = 12;

void StrategyMoveFlowThresholdSet(int n) { s_flowThreshold = (n < 1) ? 1 : n; }
int  StrategyMoveFlowThreshold(void)     { return s_flowThreshold; }

// Sort key scratch. Static, not stack: 512 entries twice over is not something
// to put on a frame's stack in a function reached from input handling.
typedef struct { float key; int index; } SortEntry;
static SortEntry s_unitOrder[FORMATION_MAX];
static SortEntry s_slotOrder[FORMATION_MAX];
static Vector3   s_slotPos[FORMATION_MAX];

// Insertion sort. n is capped at 512 and this runs on a click, not per frame -
// qsort's function-pointer indirection would cost more than it saves here, and
// insertion sort on nearly-sorted input (which a selected group usually is) is
// close to linear.
static void SortByKey(SortEntry *a, int n)
{
    for (int i = 1; i < n; i++)
    {
        SortEntry v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j].key > v.key) { a[j+1] = a[j]; j--; }
        a[j+1] = v;
    }
}

void StrategyOrderMoveGroup(const int *units, int count, Vector3 dest)
{
    StrategyWorld *world = StrategyWorldGet();
    const SpGrid *g = StrategyNavGrid();

    if (count <= 0) return;

    // A single unit has nothing to form up with, and giving it a "formation"
    // would only offset it away from where the player clicked.
    if (count == 1)
    {
        StrategyOrderMove(&world->units[units[0]], dest);
        return;
    }

    if (count > FORMATION_MAX) count = FORMATION_MAX;

    // -- Facing ---------------------------------------------------------------
    // The formation faces the way the group is travelling, so a block of units
    // arrives broadside-on rather than in a column. Centroid first.
    Vector3 centroid = { 0 };
    for (int k = 0; k < count; k++)
    {
        centroid.x += world->units[units[k]].pos.x;
        centroid.z += world->units[units[k]].pos.z;
    }
    centroid.x /= (float)count;
    centroid.z /= (float)count;

    float fx = dest.x - centroid.x, fz = dest.z - centroid.z;
    float flen = sqrtf(fx*fx + fz*fz);
    if (flen < 0.001f) { fx = 0.0f; fz = 1.0f; }       // already there: any facing
    else               { fx /= flen; fz /= flen; }
    float rx = -fz, rz = fx;                            // right-hand perpendicular

    // -- Slots ----------------------------------------------------------------
    // A square-ish grid centred on the destination, rotated to the facing.
    int cols = (int)ceilf(sqrtf((float)count));
    if (cols < 1) cols = 1;
    int rows = (count + cols - 1)/cols;

    int slotCount = 0;
    for (int r = 0; r < rows && slotCount < count; r++)
    {
        for (int c = 0; c < cols && slotCount < count; c++)
        {
            float offR = ((float)c - (float)(cols - 1)*0.5f)*FORMATION_SPACING;
            float offF = ((float)r - (float)(rows - 1)*0.5f)*FORMATION_SPACING;

            Vector3 p = {
                dest.x + rx*offR + fx*offF,
                0.0f,
                dest.z + rz*offR + fz*offF
            };

            // EVERY slot through SpNearestOpen. A slot inside a lake or a
            // building is an unreachable target, and the unit assigned to it
            // walks at a wall forever - the exact orbit-forever failure this
            // whole overhaul exists to remove, reintroduced quietly for one
            // unit in twenty.
            int tx, tz, ox, oz;
            SpWorldToTile(g, p.x, p.z, &tx, &tz);
            if (SpNearestOpen(g, tx, tz, 6, &ox, &oz)) p = SpCellToWorld(g, (SpCell)(oz*g->w + ox));

            s_slotPos[slotCount++] = p;
        }
    }
    if (slotCount == 0) return;

    // -- Assignment -----------------------------------------------------------
    // Project both units and slots onto the SAME axis and zip the sorted
    // orders. Units that start in front end up in front, so nobody walks
    // through the formation to reach their slot. Assign naively instead and a
    // 200-unit group visibly trades places on the spot before setting off.
    //
    // The axis is the formation's right-hand perpendicular with the forward
    // component folded in, so the ordering is stable for a group approaching
    // from any direction rather than only from the side.
    for (int k = 0; k < count; k++)
    {
        const Unit *u = &world->units[units[k]];
        s_unitOrder[k].key   = u->pos.x*rx + u->pos.z*rz + (u->pos.x*fx + u->pos.z*fz)*0.5f;
        s_unitOrder[k].index = k;
    }
    for (int k = 0; k < slotCount; k++)
    {
        s_slotOrder[k].key   = s_slotPos[k].x*rx + s_slotPos[k].z*rz
                             + (s_slotPos[k].x*fx + s_slotPos[k].z*fz)*0.5f;
        s_slotOrder[k].index = k;
    }
    SortByKey(s_unitOrder, count);
    SortByKey(s_slotOrder, slotCount);

    for (int k = 0; k < count; k++)
    {
        int unitSlot = s_unitOrder[k].index;
        int slot     = s_slotOrder[(k < slotCount) ? k : slotCount - 1].index;
        Unit *u = &world->units[units[unitSlot]];

        // Still one target per unit, and still through the SAME single-unit
        // order every other caller uses. UNIT_MOVE's handler never learns that
        // formations exist - which is what keeps this addition survivable.
        StrategyOrderMove(u, s_slotPos[slot]);
    }

    // -- Flow field -----------------------------------------------------------
    // Looked up FIRST and only built when the group earns one. A lone worker
    // sent where two hundred units are already headed costs nothing; three
    // units to three corners are three searches and rightly stay individual.
    int gx, gz;
    SpWorldToTile(g, dest.x, dest.z, &gx, &gz);
    int ox, oz;
    if (!SpNearestOpen(g, gx, gz, 8, &ox, &oz)) return;

    SpFieldId field = SpFlowFind(ox, oz);

    // Counted here and nowhere else: this is the only place the cache is
    // consulted, so hit/miss on the overlay means exactly "orders that reused a
    // field" against "orders that had to build one". A miss rate that stays
    // high under repeated orders to the same area means the 2x2 goal
    // coarsening is not doing its job.
    SpProfAdd(SP_COUNT_FLOW_HIT,  (field != SP_FIELD_NONE) ? 1 : 0);
    SpProfAdd(SP_COUNT_FLOW_MISS, (field == SP_FIELD_NONE) ? 1 : 0);

    if (field == SP_FIELD_NONE && count >= s_flowThreshold)
        field = SpFlowAcquire(ox, oz);

    for (int k = 0; k < count; k++) s_paths[units[k]].field = field;
    SpProfSet(SP_COUNT_FLOW_LIVE, SpFlowLiveCount());
}

// ----------------------------------------------------------------------------
//  Flow field refcount sweep
//
//  RECOUNTS, never adjusts. Every exit path a unit can take - dying, being
//  re-ordered, arriving, a world reset - would otherwise need its own
//  decrement, and missing exactly one leaks a field forever. Once all slots are
//  pinned by phantom users, SpFlowAcquire starts refusing and the whole system
//  quietly degrades to individual A* with nothing in the logs to say why.
//
//  Counting from the live roster instead is O(live), cannot leak by
//  construction, and is cheap enough to run every second.
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
//  Group cohesion
//
//  The cheap 80% of "keep the group together", and deliberately nothing more.
//  A unit well AHEAD of its group slows down; nobody speeds up, nobody waits,
//  and there is no group object, no leader, and no regrouping state. Formal
//  cohesion means real state and real bugs, and waiting for stragglers is what
//  makes an RTS feel unresponsive - the player ordered a move, not a parade.
//
//  Distance-to-goal comes free: it is the flow field's own integration cost,
//  already computed, one array read. That is the whole reason this is fifteen
//  lines and not a subsystem, and it is why cohesion only exists for units on a
//  field - a unit on an individual A* path has no cheap way to know where the
//  rest of its group is, and by definition is not part of a bulk move anyway.
// ----------------------------------------------------------------------------
#define COHESION_SLOW      0.70f    // speed for a unit out in front
#define COHESION_LEAD      1.30f    // ...applied past this fraction of median

// Median integration cost per field, refreshed by the 1 Hz sweep. Zero means
// "not enough of a group to be worth it", which the steering treats as off.
static uint16_t s_fieldMedian[SP_FLOW_FIELDS_MAX];

// Cohesion only means anything for an actual crowd. Below this many units on
// one field, a "median" is two samples and slowing anyone against it is noise.
#define COHESION_MIN_GROUP 8

void StrategyMoveFlowSweep(float now)
{
    int liveCount = 0;
    const int *live = StrategyActiveUnits(&liveCount);
    const SpGrid *g = StrategyNavGrid();

    // Sum and count per field, for a mean standing in for the median. A true
    // median needs the samples kept and sorted; the mean needs two counters and
    // answers the same question - "is this unit ahead of the pack" - closely
    // enough to scale one speed by 0.7.
    uint32_t sum[SP_FLOW_FIELDS_MAX]   = { 0 };
    int32_t  count[SP_FLOW_FIELDS_MAX] = { 0 };

    SpFlowSweepBegin();
    for (int k = 0; k < liveCount; k++)
    {
        int i = live[k];
        SpFieldId f = s_paths[i].field;
        if (f == SP_FIELD_NONE) continue;

        SpFlowSweepMark(f);

        if (f >= 0 && f < SP_FLOW_FIELDS_MAX)
        {
            const Unit *u = &StrategyWorldGet()->units[i];
            int tx, tz;
            SpWorldToTile(g, u->pos.x, u->pos.z, &tx, &tz);
            uint16_t c = SpFlowCost(f, tx, tz);
            if (c != SP_FLOW_UNREACHED) { sum[f] += c; count[f]++; }
        }
    }
    SpFlowSweepEnd(now);

    for (int f = 0; f < SP_FLOW_FIELDS_MAX; f++)
    {
        s_fieldMedian[f] = (count[f] >= COHESION_MIN_GROUP)
                         ? (uint16_t)(sum[f]/(uint32_t)count[f])
                         : 0;
    }

    SpProfSet(SP_COUNT_FLOW_LIVE, SpFlowLiveCount());
}

// Speed multiplier for a unit riding a field: 1.0 normally, COHESION_SLOW when
// it is meaningfully closer to the goal than its group's average. Note the
// direction - integration cost DECREASES toward the goal, so "ahead" is a cost
// BELOW the median, not above it.
static float CohesionScale(SpFieldId field, int tx, int tz)
{
    if (field < 0 || field >= SP_FLOW_FIELDS_MAX) return 1.0f;
    uint16_t median = s_fieldMedian[field];
    if (median == 0) return 1.0f;                   // group too small to matter

    uint16_t c = SpFlowCost(field, tx, tz);
    if (c == SP_FLOW_UNREACHED) return 1.0f;        // unroutable: leave it alone

    return ((float)c*COHESION_LEAD < (float)median) ? COHESION_SLOW : 1.0f;
}

int StrategyMoveFlowLive(void) { return SpFlowLiveCount(); }
