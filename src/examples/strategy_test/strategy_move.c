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

// FLOOR on the distance from its own destination at which a unit stops riding
// the shared flow field and steers at its own slot instead. The real radius
// scales with the formation's own half-extent (see FormReleaseRadius); this is
// what a unit on a field with no formation gets, and the minimum for one with.
//
// A field points every unit at ONE goal cell. A formation deliberately spreads
// them over many, so riding the field all the way in would funnel the whole
// group onto a single tile - rebuilding, at the last moment, exactly the pile
// the formation exists to prevent. Releasing on the approach costs nothing:
// the last few units of travel are open ground the group has already reached.
//
// AS A CONSTANT IT WAS THE FUNNEL BUG. A block's width grows with its unit
// count while this does not, so at a thousand units 98% of the slots sat
// outside it and the field governed the entire march - which is what "they all
// funnel through a single path line" actually was.
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

    // -- Formation side state -------------------------------------------------
    // Here rather than in Unit for the same reason the waypoints are: Unit is
    // walked three times a frame by loops - separation, drawing, HP bars - that
    // have no interest in any of it, and the struct is already 248 bytes.
    int16_t     formGroupSlot;  // resolved index into the group table, or -1.
                                //   CACHED because StrategyMoveTo runs per unit
                                //   per frame and the table lookup is a linear
                                //   scan of 64 entries - 640k integer compares a
                                //   frame at ten thousand units, for an answer
                                //   that changes only when an order does.
    bool        formReleased;   // has left the shared field for its own slot
    bool        formSlotClear;  // line of sight to the slot, tested ONCE at
                                //   release: the common case is open ground the
                                //   group just crossed, and a unit that really is
                                //   blocked still gets a path via blockedTime
    bool        formChoked;     // in terrain too narrow for the block: rides the
                                //   field through and re-forms on the far side
    float       formChokeTime;  // dwell accumulator for the verdict above
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
    p->formGroupSlot = -1;
    p->formReleased  = false;
    p->formSlotClear = false;
    p->formChoked    = false;
    p->formChokeTime = 0.0f;
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
        s_paths[i].formGroupSlot = -1;
        s_paths[i].formReleased  = false;
        s_paths[i].formSlotClear = false;
        s_paths[i].formChoked    = false;
        s_paths[i].formChokeTime = 0.0f;
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

// Form-up, defined with the per-frame pass that feeds it - a unit ahead of its
// formation slows exponentially until the block closes up, once per order.
static float FormUpScale(int index, const Unit *u);

// Formation release and chokepoint tests, defined with the group table they
// read. Both answer questions about the BLOCK, which is why neither can be a
// property of the unit in front of you.
static float FormReleaseRadius(int index);
static bool  FormChokedHere(int index, const Unit *u, Vector3 dest, float dt);

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

    // Form-up scales dt for EVERY branch below - field, path, or straight line -
    // because a unit out in front should hang back regardless of how it happens
    // to be routed. Applied here once rather than at each steering call, which
    // is also what stops it being forgotten when a branch is added.
    dt *= FormUpScale(index, u);

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
        // THE RELEASE RADIUS SCALES WITH THE BLOCK, and this is the fix that
        // matters most at scale. It used to be a flat 4.0, which is fine for a
        // formation a few units wide and catastrophic past that: at a thousand
        // units the block is 46 across, so 98% of the slots sit OUTSIDE the
        // release and the field - which points every unit at ONE goal cell -
        // governs the whole march. That is the "funnels through a single path
        // line" report, and it is not a steering bug at all.
        float release = FormReleaseRadius(index);

        // Terrain narrower than the block gets to break it. A funnelling unit
        // keeps riding the field THROUGH the gap regardless of how close it is,
        // and picks its slot up again on the far side - so a formation is
        // something the ground can take away and give back, rather than a thing
        // that wedges a thousand units against a two-tile bridge.
        bool choked = FormChokedHere(index, u, dest, dt);

        // Choked units go back on the field even after release: `formReleased`
        // is cleared by the chokepoint exit test, so a block re-forms past a gap
        // rather than threading it one unit at a time forever.
        if (choked) p->formReleased = false;

        if (!SpFlowValid(p->field)) p->field = SP_FIELD_NONE;    // grid moved
        else if (choked || (!p->formReleased && dist > release))
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
        else if (!p->formReleased)
        {
            // FIRST FRAME OFF THE FIELD. Test line of sight to the slot ONCE and
            // cache it, because this is the moment the whole change can go
            // wrong: after release a unit is up to ~35 world units from its slot
            // at n=1000, which is well past MOVE_PATH_MIN_DIST, so NeedsPath
            // fires - and first paths deliberately bypass the repath limiter.
            // Releasing a thousand-unit block would then dump a thousand
            // unbudgeted A* requests into one frame, which is a worse bug than
            // the funnel it replaces.
            //
            // The common case needs no search at all: the field has already
            // carried the group to the destination area and the last leg is open
            // ground the group just walked across. A unit that genuinely IS
            // blocked still gets a path, via blockedTime.
            int sx, sz, ux, uz;
            SpWorldToTile(g, u->pos.x, u->pos.z, &ux, &uz);
            SpWorldToTile(g, dest.x, dest.z, &sx, &sz);
            p->formSlotClear = SpLosClear(g, ux, uz, sx, sz);
            p->formReleased  = true;

            // THE FIELD IS KEPT, NOT DROPPED, and that is deliberate. Release is
            // not a one-way door: a block that has reached its slots can still
            // meet ground too narrow to hold them - a re-order, a break-off and
            // a rejoin, or simply a destination on the far side of a bridge.
            // Dropping the field here would leave nothing to funnel on, so the
            // chokepoint rule below could never fire again for this unit. The
            // handoff is gated by `formReleased` alone; the sweep reclaims the
            // field when the last member stops referencing it.
        }
    }

    // Released with a clear line to its slot: walk at it. No path, by design -
    // see the release branch above.
    if (p->formReleased && p->formSlotClear && p->count == 0 && !p->pending)
    {
        StrategyMoveDirect(u, dest, dt);
        return;
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

// FORMATION_SPACING lives in strategy_types.h, derived from STRAT_UNIT_RADIUS.
// It was defined here as a bare 1.5 and that is half of how D3 happened: the
// form-up tolerance sat in this file too, the two were never compared, and one
// ended up smaller than the other. A static assert now pins the relationship,
// but only because both numbers are finally in one place to be compared.
#define FORMATION_MAX         512

// Player's current shape and break-off rule, set by the hotkeys. Globals rather
// than per-group state because they are a PLAYER SETTING: the next order uses
// whatever is selected now, and a group already marching keeps the rule it was
// given. Storing them per-group would mean re-ordering a group to change its
// shape, which is not what a hotkey implies.
static FormationShape    s_formShape    = FORM_GRID;
static FormationBehavior s_formBehavior = FORM_BEHAVIOR_SKIRMISH;

// Monotonic group id. Every group order mints a fresh one so units from an
// earlier order can never be mistaken for members of this one - the ids are
// only ever compared for equality, never indexed, so wrapping is harmless.
static int s_formNextGroup = 1;

void StrategyFormationShapeSet(FormationShape s)
{
    if (s >= 0 && s < FORM_COUNT) s_formShape = s;
}
FormationShape StrategyFormationShape(void) { return s_formShape; }

void StrategyFormationBehaviorSet(FormationBehavior b)
{
    if (b >= 0 && b < FORM_BEHAVIOR_COUNT) s_formBehavior = b;
}
FormationBehavior StrategyFormationBehavior(void) { return s_formBehavior; }

const char *StrategyFormationShapeName(FormationShape s)
{
    switch (s)
    {
        case FORM_GRID:       return "GRID";
        case FORM_LINE:       return "LINE";
        case FORM_COLUMN:     return "COLUMN";
        case FORM_TWO_COLUMN: return "TWO COLUMN";
        case FORM_WEDGE:      return "WEDGE";
        case FORM_FREEFORM:   return "FREEFORM";
        default:              return "?";
    }
}

const char *StrategyFormationBehaviorName(FormationBehavior b)
{
    switch (b)
    {
        case FORM_BEHAVIOR_SKIRMISH: return "SKIRMISH";
        case FORM_BEHAVIOR_ENGAGE:   return "ENGAGE";
        case FORM_BEHAVIOR_HOLD:     return "HOLD";
        default:                     return "?";
    }
}

// The layout math itself lives in src/strategy_path/strategy_path_form.c, as
// pure geometry with no idea what a Unit is - which is what makes it testable.
// path_tests asserts spacing, extent, uniqueness and the width caps for every
// shape at every size that matters; before the extraction none of that could be
// checked at all, and the bug that cost the most (a form-up tolerance tighter
// than the slot pitch, so the brake could never release) is exactly the kind a
// test like that catches on the first run.
//
// THE TWO ENUMERATIONS MUST AGREE, and nothing but this makes them. The module
// takes the shape as a plain int because it must not include strategy_types.h;
// reorder either list and the build stops here rather than the formations
// quietly turning into each other at runtime.
_Static_assert((int)FORM_GRID       == SP_FORM_GRID,       "formation shape enums drifted");
_Static_assert((int)FORM_LINE       == SP_FORM_LINE,       "formation shape enums drifted");
_Static_assert((int)FORM_COLUMN     == SP_FORM_COLUMN,     "formation shape enums drifted");
_Static_assert((int)FORM_TWO_COLUMN == SP_FORM_TWO_COLUMN, "formation shape enums drifted");
_Static_assert((int)FORM_WEDGE      == SP_FORM_WEDGE,      "formation shape enums drifted");
_Static_assert((int)FORM_FREEFORM   == SP_FORM_FREEFORM,   "formation shape enums drifted");
_Static_assert((int)FORM_COUNT      == SP_FORM_SHAPE_COUNT,"formation shape enums drifted");

// THE TWO INVARIANTS D3 VIOLATED, pinned so they cannot drift apart again.
// Both were true statements about the old code, and both were false: the shipped
// form-up tolerance was 1.4 against a 1.5 slot pitch, so a group was asked to
// pack tighter than its own slots allowed and the brake could never release.
// A comment saying so would have been just as wrong; a static assert stops the
// build instead.
_Static_assert(FORMUP_TIGHT > FORMATION_SPACING,
               "form-up tolerance is tighter than the slot pitch: the latch can never fire");
_Static_assert(FORM_HOLD_DEADBAND > STRAT_SEP_RADIUS,
               "hold deadband is inside the separation radius: the two forces will oscillate");

// The extent caps are a balance question, not a geometry one, so they stay in
// strategy_types.h and are handed to the module per call.
static const SpFormCaps s_formCaps = {
    FORM_LINE_MAX_WIDTH,
    FORM_LINE_RANKS,
    FORM_COLUMN_MAX_DEPTH,
    FORM_COLUMN_FILES,
    FORM_TWO_COLUMN_LANE,
};

// FREEFORM is an AREA order, not a block: the player asking for a loose scatter
// is asking for the opposite of what the form-up brake and the slot pull do. One
// predicate, so the two opt-outs can never disagree.
static bool FormShapeIsLoose(FormationShape s) { return (s == FORM_FREEFORM); }

// Above this many units sharing one destination, build a flow field instead of
// a path each. Runtime-tunable because the A/B - watch one field serve 2,000
// units, then raise the threshold past the group size and watch the queue pin -
// is the single most informative thing the lab can show.
static int s_flowThreshold = 12;

void StrategyMoveFlowThresholdSet(int n) { s_flowThreshold = (n < 1) ? 1 : n; }
int  StrategyMoveFlowThreshold(void)     { return s_flowThreshold; }

// Assignment scratch. Static, not stack: several 512-entry arrays is not
// something to put on a frame's stack in a function reached from input handling.
// The sort entry type and the sort itself come from the geometry module, so the
// assignment and its caller cannot disagree about either.
typedef SpFormSortEntry SortEntry;
static SortEntry   s_unitOrder[FORMATION_MAX];
static SortEntry   s_slotOrder[FORMATION_MAX];
static Vector3     s_slotPos[FORMATION_MAX];
static SpFormPoint s_assignUnits[FORMATION_MAX];
static SpFormPoint s_assignSlots[FORMATION_MAX];
static int         s_assignSlot[FORMATION_MAX];
static int         s_assignPrev[FORMATION_MAX];
static unsigned char s_assignTaken[FORMATION_MAX];


// ============================================================================
//  The live formation group table
//
//  WHY THIS EXISTS AT ALL. The first cut of formations had no runtime identity
//  for a group - just three parallel arrays holding a worst-case distance. That
//  single gap is what made four separate bugs unfixable in place: the flow
//  release could not scale with a block's extent because nothing knew the
//  extent; the form-up tolerance could not scale with the spacing because
//  nothing owned the shape; the table overflowed silently at 32 groups; and the
//  chokepoint rule had nothing to ask how wide the block needed to be.
//
//  One record per live group answers all four. 64 entries at ~56 bytes is 3.5 KB
//  of static memory - small enough that the alternative was never worth the
//  argument.
//
//  SIXTY-FOUR, NOT THIRTY-TWO. A ten-thousand-unit army now chunks into twenty
//  echelons from a SINGLE click, so 32 is one large order plus one small one
//  away from overflow - and overflow used to mean a group that never latched
//  and therefore crawled forever.
// ============================================================================
#define FORM_GROUPS_MAX  64

typedef struct {
    int     id;             // group id, or 0 for a free slot
    int     members;        // live units still carrying this id
    int     inPlace;        // ...of which this many are within FORMUP_TIGHT
    float   worst;          // furthest any member is from its slot
    float   halfExtent;     // centre to furthest slot; what release scales with
    float   needWidth;      // frontage the block wants, capped; chokepoint test
    float   formTime;       // seconds spent forming up, for the hard cap
    bool    everFormed;     // THE LATCH. Authoritative here, mirrored on Unit.
    bool    holding;        // arrived: members hold their slots
    bool    loose;          // FREEFORM: no brake, no hold, still shares a field
    Vector3 dest;
} FormGroup;

static FormGroup s_formGroups[FORM_GROUPS_MAX];
static int       s_formGroupCount;

static int FormGroupSlot(int groupId)
{
    if (groupId < 0) return -1;
    for (int i = 0; i < s_formGroupCount; i++)
        if (s_formGroups[i].id == groupId) return i;
    return -1;
}

// Claim a table slot for a new group. On overflow, evict the group with the
// FEWEST live members rather than refusing.
//
// FAIL OPEN, and this is the whole point of the change. The old code did
// `continue` on a full table, which left the group with no record - and a group
// with no record could never latch its form-up, so it crawled at the minimum
// scale for the rest of the march. A group marching at full speed in a slightly
// ragged shape is a cosmetic loss; one that crawls forever is the bug that was
// actually reported. The smallest group is evicted because form-up matters
// least to a skeleton crew.
static int FormGroupClaim(int groupId)
{
    int slot = FormGroupSlot(groupId);
    if (slot >= 0) return slot;

    if (s_formGroupCount < FORM_GROUPS_MAX) slot = s_formGroupCount++;
    else
    {
        slot = 0;
        for (int i = 1; i < s_formGroupCount; i++)
            if (s_formGroups[i].members < s_formGroups[slot].members) slot = i;
    }

    s_formGroups[slot] = (FormGroup){ 0 };
    s_formGroups[slot].id = groupId;
    return slot;
}

// Read-only accessors for the world layer: the hold force and the HUD both need
// to know what a group is doing without reaching into the table.
bool StrategyFormationGroupHolding(int groupId)
{
    int slot = FormGroupSlot(groupId);
    return (slot >= 0) && s_formGroups[slot].holding;
}

bool StrategyFormationGroupLoose(int groupId)
{
    int slot = FormGroupSlot(groupId);
    return (slot >= 0) && s_formGroups[slot].loose;
}

// Distance from its own slot at which a unit stops riding the shared field.
//
// DERIVED FROM THE BLOCK'S OWN FOOTPRINT, not a constant. The field delivers the
// group to the destination area - which is the whole performance win and is
// kept - and hands off exactly when the block reaches the ground it is going to
// stand on. The 1.25 multiplier releases slightly OUTSIDE that: release exactly
// at the block edge and the rear ranks are still funnelling while the front is
// already forming, which reads as the block being extruded through a hole.
static float FormReleaseRadius(int index)
{
    int slot = s_paths[index].formGroupSlot;
    if (slot < 0) return MOVE_FLOW_RELEASE;         // no formation: the old rule

    float r = (s_formGroups[slot].halfExtent + FORM_RELEASE_MARGIN)*FORM_RELEASE_SCALE;
    return (r < MOVE_FLOW_RELEASE) ? MOVE_FLOW_RELEASE : r;
}

// Is this unit in ground too narrow for its block to hold shape through?
//
// A SUSPEND, NOT A BREAK-OFF. The unit rejoins its slot past the constriction
// and the group's form-up latch is left alone, so a block does not re-enter the
// form-up crawl every time it threads a gap. Break-off stays the combat concept.
//
// HYSTERESIS IS MANDATORY. Two widths and a dwell time, matching what
// STRAT_ARRIVE_RESUME does against exactly this failure: with a single threshold
// a unit at the mouth of a gap flips verdict every frame, and the block visibly
// shivers between formed and funnelling.
//
// AMORTISED ON A STRIDE, like the aggro scan. Two dozen grid reads per unit per
// frame is not affordable at ten thousand units, and corridor width changes on
// the scale of a unit walking a few tiles - not per frame.
static bool FormChokedHere(int index, const Unit *u, Vector3 dest, float dt)
{
    UnitPath *p = &s_paths[index];
    int slot = p->formGroupSlot;
    if (slot < 0) return false;

    const FormGroup *fg = &s_formGroups[slot];
    if (fg->loose) return false;            // a scatter has no shape to protect

    p->formChokeTime += dt;
    if (((s_moveFrame + (uint32_t)index) % FORM_CHOKE_STRIDE) != 0)
        return p->formChoked;               // between probes: last verdict stands
    if (p->formChokeTime < FORM_CHOKE_DWELL)
        return p->formChoked;               // and it must hold for the dwell

    const SpGrid *g = StrategyNavGrid();
    int tx, tz;
    SpWorldToTile(g, u->pos.x, u->pos.z, &tx, &tz);

    float dx = dest.x - u->pos.x, dz = dest.z - u->pos.z;
    int width = SpFormCorridorWidth(g, tx, tz, dx, dz, FORM_CHOKE_PROBE_MAX);

    // Tiles are one world unit, so the needed frontage compares directly. The
    // enter/exit pair is what stops the flicker; the needWidth cap set at order
    // time is what stops every large block reading as permanently choked.
    float need = fg->needWidth;
    int enterAt = (int)((need < FORM_CHOKE_ENTER) ? need : FORM_CHOKE_ENTER);
    int exitAt  = (int)((need < FORM_CHOKE_EXIT)  ? need : FORM_CHOKE_EXIT);

    bool was = p->formChoked;
    if (!was && width <= enterAt)     { p->formChoked = true;  p->formChokeTime = 0.0f; }
    else if (was && width >= exitAt)  { p->formChoked = false; p->formChokeTime = 0.0f;
                                        p->formReleased = false; }   // re-form past the gap
    return p->formChoked;
}

// ============================================================================
//  Order preview
//
//  A fading ring the size of the block, plus one marker per slot, showing where
//  a move order will actually put the group before it gets there.
//
//  NOT THROUGH THE EFFECT POOL, deliberately. That pool holds 96 entries shared
//  with combat - a 512-slot order would evict every hit spark and muzzle flash
//  on screen, and then still drop most of its own markers. The preview is one
//  array owned here, where the slots already exist, and it costs one float of
//  bookkeeping per frame.
//
//  SLOTS ARE COPIED, NOT RECOMPUTED FOR DRAWING. They have already been through
//  SpNearestOpen by the time they land here, so the preview shows the ground the
//  units are really going to stand on - including a slot nudged out of a lake.
//  Recomputing the layout at draw time would quietly show the pre-resolution
//  positions and disagree with where the units actually end up.
// ============================================================================
#define PREVIEW_LIFE  3.30f     // seconds. Far longer than FX_RING's 0.6 because
                                //   this is READ, not just noticed - the player
                                //   is checking whether the block fits, and that
                                //   is a look-and-compare, not a glance.

static Vector3 s_previewSlot[FORMATION_MAX];
static int     s_previewCount;
static Vector3 s_previewCentre;
static float   s_previewRadius;
static float   s_previewLife;
static Vector3 s_previewFace;   // formation heading, for the direction arrow
static bool    s_previewHeld;   // frozen while the player is dragging a facing

// Called once per ORDER, after the slots are resolved. A multi-echelon order
// calls it per chunk; the last chunk wins, which is the one the player is most
// likely to be looking at and keeps this to a single fixed array.
static void PreviewCapture(const Vector3 *slots, int count, Vector3 dest,
                           float radius, float faceX, float faceZ)
{
    if (count > FORMATION_MAX) count = FORMATION_MAX;
    for (int i = 0; i < count; i++) s_previewSlot[i] = slots[i];
    s_previewCount  = count;
    s_previewCentre = dest;
    s_previewRadius = radius;
    s_previewFace   = (Vector3){ faceX, 0.0f, faceZ };
    s_previewLife   = PREVIEW_LIFE;
    s_previewHeld   = false;
}

// Build the preview for an order that has NOT been given yet - the live picture
// under a drag. Same layout code the real order uses, so what the player aims is
// exactly what they get; the only thing missing is SpNearestOpen resolution per
// slot, which is skipped because this runs every frame of the drag and the
// player is aiming a direction, not inspecting individual tiles.
//
// `held` keeps the preview alive while the button is down: the normal decay is a
// fade after the fact, and a drag needs the markers to simply stay put.
void StrategyMovePreviewAim(const int *units, int count, Vector3 dest,
                            float faceX, float faceZ)
{
    if (count <= 0) return;
    if (count > FORMATION_MAX) count = FORMATION_MAX;

    float fx = faceX, fz = faceZ;
    float l = sqrtf(fx*fx + fz*fz);
    if (l < 0.001f) { fx = 0.0f; fz = 1.0f; }
    else            { fx /= l; fz /= l; }
    float rx = -fz, rz = fx;

    float bias = SpFormForwardBias((int)s_formShape, count, FORMATION_SPACING,
                                   &s_formCaps);

    for (int i = 0; i < count; i++)
    {
        float offR, offF;
        SpFormSlotLocal((int)s_formShape, i, count, FORMATION_SPACING,
                        &s_formCaps, &offR, &offF);
        offF -= bias;
        s_previewSlot[i] = (Vector3){ dest.x + rx*offR + fx*offF, 0.0f,
                                      dest.z + rz*offR + fz*offF };
    }

    s_previewCount  = count;
    s_previewCentre = dest;
    s_previewRadius = SpFormHalfExtent((int)s_formShape, count, FORMATION_SPACING,
                                       &s_formCaps);
    s_previewFace   = (Vector3){ fx, 0.0f, fz };
    s_previewLife   = PREVIEW_LIFE;
    s_previewHeld   = true;

    (void)units;
}

// Let the held preview start fading. Called on release, so the markers persist
// for the whole drag and then decay normally once the order is given.
void StrategyMovePreviewRelease(void) { s_previewHeld = false; }

void StrategyMovePreviewUpdate(float dt)
{
    if (s_previewHeld) return;      // a drag is in progress: hold the picture
    if (s_previewLife > 0.0f)
    {
        s_previewLife -= dt;
        if (s_previewLife < 0.0f) s_previewLife = 0.0f;
    }
}

// Fraction of life remaining, 1 -> 0, and the geometry to draw it with. Returns
// false when nothing is live, so the caller draws nothing rather than a ring at
// zero alpha.
bool StrategyMovePreview(Vector3 *outCentre, float *outRadius, float *outFade,
                         const Vector3 **outSlots, int *outCount, Vector3 *outFace)
{
    if (s_previewLife <= 0.0f) return false;
    *outCentre = s_previewCentre;
    *outRadius = s_previewRadius;
    *outFade   = s_previewLife/PREVIEW_LIFE;
    *outSlots  = s_previewSlot;
    *outCount  = s_previewCount;
    *outFace   = s_previewFace;
    return true;
}

// ----------------------------------------------------------------------------
//  Ordering one echelon
//
//  Extracted from StrategyOrderMoveGroup so a large order can call it once per
//  chunk. Everything here is what the single-block version always did: lay out
//  slots, resolve each through SpNearestOpen, sort both ends of the assignment
//  on the same axis, and zip.
//
//  `backOff` slides this echelon's whole slot block backward along -forward, so
//  chunks stack behind one another instead of landing on top of each other.
// ----------------------------------------------------------------------------
static void FormationChunkOrder(const int *units, int count, Vector3 dest,
                                float fx, float fz, float rx, float rz,
                                float backOff, int totalCount, SpFieldId field)
{
    StrategyWorld *world = StrategyWorldGet();
    const SpGrid *g = StrategyNavGrid();

    if (count <= 0) return;

    // -- Slots ----------------------------------------------------------------
    // Shape-driven, rotated to the facing. SpFormSlotLocal emits formation-local
    // offsets and the rotation is applied identically for every shape, so adding
    // a shape never touches this loop.
    // Re-anchor the shape on its own centroid. Without this only GRID lands on
    // the point the player clicked - LINE, COLUMN and WEDGE all grow backward
    // from it, so their mass sits behind the click and, on a SHORT move, the
    // rear ranks are sent to ground behind where they already stand and walk
    // backwards to get there. Computed once per chunk, not per slot.
    float bias = SpFormForwardBias((int)s_formShape, count, FORMATION_SPACING,
                                   &s_formCaps);

    int slotCount = 0;
    for (int i = 0; i < count; i++)
    {
        float offR, offF;
        SpFormSlotLocal((int)s_formShape, i, count, FORMATION_SPACING,
                        &s_formCaps, &offR, &offF);
        offF -= bias;
        offF -= backOff;

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
        //
        // THE RING CAP MUST SPAN A REAL OBSTACLE. It was 6, which cannot escape
        // anything wider than twelve tiles - so a LINE reaching its corner into
        // a lake got slots left INSIDE it, and those units wedged against the
        // shore and did the orbit-forever dance. A wide shape is exactly the
        // case that puts a slot deep inside terrain, because it is the shape
        // that reaches furthest from the point the player clicked.
        int tx, tz, ox, oz;
        SpWorldToTile(g, p.x, p.z, &tx, &tz);
        if (SpNearestOpen(g, tx, tz, FORM_SLOT_RESOLVE_RING, &ox, &oz))
        {
            p = SpCellToWorld(g, (SpCell)(oz*g->w + ox));
        }
        else
        {
            // AND WHEN IT STILL FAILS, FALL BACK TO THE DESTINATION - never keep
            // the blocked position. The old code did exactly that on failure,
            // which is how an unreachable slot survived the very check written to
            // remove it. The clicked point has itself been resolved by the
            // caller, so it is somewhere a unit can stand; several units sharing
            // it is a pile, which separation handles, and a pile at the right
            // place beats a unit grinding on a cliff face forever.
            p = dest;
        }

        s_slotPos[slotCount++] = p;
    }
    if (slotCount == 0) return;

    // -- Assignment -----------------------------------------------------------
    // NEAREST-SLOT, not a 1-D zip. The zip this replaced sorted units and slots
    // onto one axis and paired them off, which is coherent along that axis and
    // arbitrary across it. The cost lands exactly where the player looks: a
    // group already standing on its destination, re-ordered, walked 30-47%
    // further than it needed to and individual units crossed the whole block to
    // reach a slot beside the one they were standing on. Line and column suffer
    // worst, because their slots are spread along the axis the zip sorts by.
    //
    // The units array is indexed 0..count-1 here and the slot each one gets is
    // written to s_assignSlot; both scratch arrays belong to this file.
    for (int k = 0; k < count; k++)
    {
        const Unit *u = &world->units[units[k]];
        s_assignUnits[k].x = u->pos.x;
        s_assignUnits[k].z = u->pos.z;
    }
    for (int k = 0; k < slotCount; k++)
    {
        s_assignSlots[k].x = s_slotPos[k].x;
        s_assignSlots[k].z = s_slotPos[k].z;
    }

    // Fewer slots than units cannot happen - one slot is laid out per unit - but
    // if it ever did, the extra units would read past the slot array. Pair only
    // what exists and let the tail fall back to the last slot, as before.
    int pairs = (count < slotCount) ? count : slotCount;

    // -- Does this group already have an identity to keep? --------------------
    // Only if EVERY unit in it was last assigned under the same shape and the
    // same member count. Either changing means the slot indices describe a
    // different layout, and keeping them would scatter the block into the shape
    // of its predecessor.
    //
    // Note this deliberately does not require the same group id: a re-order
    // mints a fresh one every time, so keying on it would mean the memory never
    // applies, which is the bug rather than the fix.
    bool keepIdentity = true;
    for (int k = 0; k < pairs; k++)
    {
        const Unit *u = &world->units[units[k]];
        if (u->formSlotIndex < 0 ||
            u->formSlotShape != (int)s_formShape ||
            u->formSlotOf    != pairs)
        {
            keepIdentity = false;
            break;
        }
    }

    if (keepIdentity)
    {
        for (int k = 0; k < pairs; k++)
            s_assignPrev[k] = world->units[units[k]].formSlotIndex;

        // Read BEFORE StrategyOrderMove runs below - that call funnels through
        // MoveArriveReset, which clears the remembered slot precisely so a unit
        // given a different job cannot re-claim one. The stamp is redone
        // immediately after the order, the same dance the rest of the formation
        // state already does.

        // The block is the same shape and size as last time: everyone keeps the
        // slot they hold and it simply translates and turns. This is what stops
        // an ordinary click - which swings the facing a few degrees - from
        // reshuffling the formation around its own centre.
        SpFormAssignStable(s_assignUnits, s_assignSlots, pairs, s_assignPrev,
                           s_assignSlot, s_assignTaken);
    }
    else
    {
        // A new formation, or one whose shape or size changed. There is nothing
        // to preserve, so pair from scratch for the shortest walk.
        SpFormAssign(s_assignUnits, s_assignSlots, pairs, s_assignSlot,
                     s_unitOrder, s_slotOrder);
    }

    // One id per ECHELON, not per order. Form-up measures a worst-case distance
    // across a group, so two echelons sharing an id would make each wait for the
    // other's stragglers - the rear chunk holding the front one at a crawl is
    // exactly the "1000 units got stuck" report.
    int groupId = s_formNextGroup++;
    bool loose  = FormShapeIsLoose(s_formShape);

    int slot = FormGroupClaim(groupId);
    s_formGroups[slot].dest       = dest;
    s_formGroups[slot].loose      = loose;
    s_formGroups[slot].everFormed = loose;   // a loose scatter never forms up
    s_formGroups[slot].halfExtent =
        SpFormHalfExtent((int)s_formShape, count, FORMATION_SPACING, &s_formCaps);

    // Frontage the block asks the terrain for, CAPPED. Uncapped, a 512-unit
    // block wants ~34 tiles and almost no corridor is that wide, so every large
    // group would read as permanently choked and the funnel would become the
    // default again - reinstating the very bug the chokepoint rule bounds.
    float need = s_formGroups[slot].halfExtent*2.0f;
    s_formGroups[slot].needWidth = (need > FORM_CHOKE_NEED_MAX) ? FORM_CHOKE_NEED_MAX : need;

    for (int k = 0; k < count; k++)
    {
        int slotIdx = (k < pairs) ? s_assignSlot[k] : (slotCount - 1);
        int index   = units[k];
        Unit *u = &world->units[index];

        // Still one target per unit, and still through the SAME single-unit
        // order every other caller uses. UNIT_MOVE's handler never learns that
        // formations exist - which is what keeps this addition survivable.
        StrategyOrderMove(u, s_slotPos[slotIdx]);

        // Stamped AFTER the order: StrategyOrderMove clears formation state (a
        // single-unit order means "leave your formation"), so setting it first
        // would be wiped by the very call that starts the march. The field is
        // assigned after this too, for the same reason - MoveArriveReset drops
        // it, which is what step 7 relies on for individual re-orders.
        u->formGroup      = groupId;
        u->formSlot       = s_slotPos[slotIdx];

        // Remember WHICH slot, under which shape and size, so the next order can
        // keep it rather than re-deriving the whole pairing.
        u->formSlotIndex  = slotIdx;
        u->formSlotShape  = (int)s_formShape;
        u->formSlotOf     = pairs;
        u->formForming    = !loose;
        u->formEverFormed = loose;
        u->formBrokeOff   = false;

        UnitPath *p = &s_paths[index];
        p->field         = field;
        p->formGroupSlot = (int16_t)slot;
        p->formReleased  = false;
        p->formSlotClear = false;
        p->formChoked    = false;
        p->formChokeTime = 0.0f;
    }

    // The ring is sized to the block, not to a constant: the whole point is to
    // show how much ground this order takes. The half-extent already accounts
    // for the shape, so a LINE previews wide and a COLUMN previews deep.
    PreviewCapture(s_slotPos, slotCount, dest, s_formGroups[slot].halfExtent, fx, fz);

    (void)totalCount;
}

// The real entry point. `hasFacing` lets the caller pin the formation's heading -
// the drag-to-orient order does, an ordinary click and every AI order do not.
static void OrderMoveGroupFaced(const int *units, int count, Vector3 dest,
                                bool hasFacing, float faceX, float faceZ)
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

    if (count > STRAT_MAX_UNITS) count = STRAT_MAX_UNITS;

    // -- LEGACY: no group order at all ----------------------------------------
    // Every unit gets the SAME point, which is what the pre-overhaul game did
    // and is the behaviour the formation layer was written to replace. 500 units
    // ordered onto one spot are being asked to stand where about six fit, so
    // they queue and shove indefinitely. Reproduced exactly, because a baseline
    // that quietly did something smarter would understate what formations buy.
    if (StrategyControlGet() == STRAT_CTRL_LEGACY)
    {
        for (int k = 0; k < count; k++)
            StrategyOrderMove(&world->units[units[k]], dest);
        return;
    }

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

    // An EXPLICIT facing wins when the player dragged one out; otherwise the
    // formation faces the way the group is travelling, which is the right guess
    // for a plain click and the only one available for an AI order.
    float fx, fz;
    if (hasFacing)
    {
        fx = faceX; fz = faceZ;
        float l = sqrtf(fx*fx + fz*fz);
        if (l < 0.001f) { fx = 0.0f; fz = 1.0f; }
        else            { fx /= l; fz /= l; }
    }
    else
    {
        fx = dest.x - centroid.x; fz = dest.z - centroid.z;
        float flen = sqrtf(fx*fx + fz*fz);
        if (flen < 0.001f) { fx = 0.0f; fz = 1.0f; }   // already there: any facing
        else               { fx /= flen; fz /= flen; }
    }
    float rx = -fz, rz = fx;                            // right-hand perpendicular

    // -- SIMPLE: naive slots, index order -------------------------------------
    // The cheap approximation of a formation, and the arm this whole experiment
    // is really about. It keeps the SHAPE - SpFormSlotLocal is the same headless
    // geometry the current system uses - and throws away everything expensive
    // built on top of it:
    //
    //   no SpFormAssignStable  - assignment is unit k -> slot k, so a re-order
    //                            reshuffles the block instead of translating it.
    //                            Visible as churn when you re-click; that is the
    //                            defect stable assignment was added to fix, and
    //                            here it is, on purpose, to show the difference.
    //   no chunking            - one block however large, so a 5000-unit order
    //                            lays out one absurdly wide formation. Also on
    //                            purpose: it shows what the echelon logic is for.
    //   no flow field, no A*   - MoveLegacy steers with SimpleDodge instead.
    //   no form-up pacing      - StrategyMoveFormUpdate early-returns below.
    //
    // What it KEEPS is SpNearestOpen on every slot, because a slot in a lake is
    // not a formation defect but an unreachable order, and a unit sent to one
    // walks at it forever. Every goal goes through that check under every arm.
    if (StrategyControlGet() == STRAT_CTRL_SIMPLE)
    {
        float bias = SpFormForwardBias((int)s_formShape, count, FORMATION_SPACING,
                                       &s_formCaps);

        for (int k = 0; k < count; k++)
        {
            float offR, offF;
            SpFormSlotLocal((int)s_formShape, k, count, FORMATION_SPACING,
                            &s_formCaps, &offR, &offF);

            // Same re-anchoring the current path uses: without it the block
            // lands with its FRONT rank on the clicked point and every rear
            // unit walks backward away from it on a short move.
            offF -= bias;

            Vector3 slot = {
                dest.x + rx*offR + fx*offF,
                dest.y,
                dest.z + rz*offR + fz*offF,
            };

            // Pull the slot out of anything impassable. Ring cap 16, matching
            // the current path - 6 could not escape a building footprint, which
            // is what left units standing in walls.
            int tx, tz, ox, oz;
            SpWorldToTile(g, slot.x, slot.z, &tx, &tz);
            if (SpNearestOpen(g, tx, tz, 16, &ox, &oz))
            {
                Vector3 open = SpTileToWorld(g, ox, oz);
                slot.x = open.x;
                slot.z = open.z;
            }

            StrategyOrderMove(&world->units[units[k]], slot);
        }
        return;
    }

    // -- Flow field -----------------------------------------------------------
    // Resolved ONCE for the whole order, before any chunk is laid out, and
    // handed to each. All echelons walk to the same place, so they share one
    // field - and the threshold is tested against the TOTAL count, not a
    // chunk's, or a 600-unit order would fail it on its 88-unit tail and leave
    // that echelon to find its own way with individual searches.
    SpFieldId field = SP_FIELD_NONE;
    int gx, gz, gox, goz;
    SpWorldToTile(g, dest.x, dest.z, &gx, &gz);
    if (SpNearestOpen(g, gx, gz, 8, &gox, &goz))
    {
        field = SpFlowFind(gox, goz);

        // Counted here and nowhere else: this is the only place the cache is
        // consulted, so hit/miss on the overlay means exactly "orders that
        // reused a field" against "orders that had to build one". A miss rate
        // that stays high under repeated orders to the same area means the 2x2
        // goal coarsening is not doing its job.
        SpProfAdd(SP_COUNT_FLOW_HIT,  (field != SP_FIELD_NONE) ? 1 : 0);
        SpProfAdd(SP_COUNT_FLOW_MISS, (field == SP_FIELD_NONE) ? 1 : 0);

        if (field == SP_FIELD_NONE && count >= s_flowThreshold)
            field = SpFlowAcquire(gox, goz);
    }

    // -- Chunking -------------------------------------------------------------
    // Anything past one block's worth becomes several echelons, each with its
    // own id and its own slot block, stacked progressively backward.
    //
    // NOT just an array-bounds fix. The old code silently truncated at 512, so
    // in a 1000-unit order the last 488 units kept whatever they were doing -
    // which is the "some settle in a distant corner" report, exactly.
    //
    // AND CHUNKING IS CHEAPER THAN ONE BIG BLOCK. The assignment sorts with an
    // insertion sort, so it is O(n^2): 50 million comparisons at ten thousand
    // units, a multi-second hitch on a single right-click. Twenty sorted blocks
    // of 512 is 2.6 million - nineteen times less - and one 150-unit-wide
    // formation would not fit on most maps anyway.
    if (count > FORMATION_MAX)
    {
        // CHUNK MEMBERSHIP MUST BE SPATIAL, and this is the subtle part. The
        // caller fills its buffer in SELECTION order, which is roster order and
        // therefore arbitrary. Chunk that naively and every echelon holds units
        // from both ends of the map, so the rear block marches straight through
        // the front one.
        //
        // Bucketing on the same forward-axis key the assignment sort already
        // uses costs one O(n) pass and fixes it. Approximate is fine: exact
        // ordering inside a chunk is still done by the insertion sort.
        static SortEntry s_chunkOrder[STRAT_MAX_UNITS];
        static int       s_chunkBuf[STRAT_MAX_UNITS];

        for (int k = 0; k < count; k++)
        {
            const Unit *u = &world->units[units[k]];
            s_chunkOrder[k].key   = -(u->pos.x*fx + u->pos.z*fz);   // nearest the
            s_chunkOrder[k].index = units[k];                        //   destination first
        }

        // Counting sort into coarse buckets, then read out in bucket order. A
        // full sort of ten thousand entries is the cost this whole branch
        // exists to avoid, so the bucketing must stay linear.
        #define FORM_CHUNK_BUCKETS 64
        float lo = s_chunkOrder[0].key, hi = lo;
        for (int k = 1; k < count; k++)
        {
            if (s_chunkOrder[k].key < lo) lo = s_chunkOrder[k].key;
            if (s_chunkOrder[k].key > hi) hi = s_chunkOrder[k].key;
        }
        float span = hi - lo;
        if (span < 0.001f) span = 0.001f;

        int histo[FORM_CHUNK_BUCKETS + 1] = { 0 };
        for (int k = 0; k < count; k++)
        {
            int b = (int)((s_chunkOrder[k].key - lo)/span*(FORM_CHUNK_BUCKETS - 1));
            if (b < 0) b = 0;
            if (b >= FORM_CHUNK_BUCKETS) b = FORM_CHUNK_BUCKETS - 1;
            histo[b + 1]++;
        }
        for (int b = 0; b < FORM_CHUNK_BUCKETS; b++) histo[b + 1] += histo[b];
        for (int k = 0; k < count; k++)
        {
            int b = (int)((s_chunkOrder[k].key - lo)/span*(FORM_CHUNK_BUCKETS - 1));
            if (b < 0) b = 0;
            if (b >= FORM_CHUNK_BUCKETS) b = FORM_CHUNK_BUCKETS - 1;
            s_chunkBuf[histo[b]++] = s_chunkOrder[k].index;
        }
        #undef FORM_CHUNK_BUCKETS

        int chunks = (count + FORMATION_MAX - 1)/FORMATION_MAX;
        float backOff = 0.0f;
        for (int c = 0; c < chunks; c++)
        {
            int base = c*FORMATION_MAX;
            int n    = count - base;
            if (n > FORMATION_MAX) n = FORMATION_MAX;

            FormationChunkOrder(&s_chunkBuf[base], n, dest, fx, fz, rx, rz,
                                backOff, count, field);

            // Stack the next echelon behind this one, plus a rank of clearance
            // so the two blocks do not interleave at their seam.
            backOff += SpFormHalfExtent((int)s_formShape, n, FORMATION_SPACING,
                                        &s_formCaps)*2.0f + FORMATION_SPACING*2.0f;
        }
    }
    else FormationChunkOrder(units, count, dest, fx, fz, rx, rz, 0.0f, count, field);

    SpProfSet(SP_COUNT_FLOW_LIVE, SpFlowLiveCount());
}

void StrategyOrderMoveGroup(const int *units, int count, Vector3 dest)
{
    OrderMoveGroupFaced(units, count, dest, false, 0.0f, 0.0f);
}

// Same order, with the formation's heading chosen by the player rather than
// inferred from the direction of travel.
void StrategyOrderMoveGroupFacing(const int *units, int count, Vector3 dest,
                                  float faceX, float faceZ)
{
    OrderMoveGroupFaced(units, count, dest, true, faceX, faceZ);
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
    // Neither non-CURRENT arm ever acquires a field, so there is nothing to
    // recount and the O(live) sweep is pure cost. The lab's `flow fields` row
    // reading 0/N under those arms is how a missed gate would show up.
    if (StrategyControlGet() != STRAT_CTRL_CURRENT) return;

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

// ----------------------------------------------------------------------------
//  Form-up
//
//  A scattered group that sets off immediately arrives scattered, because the
//  units that started nearest simply get there first. The fix is NOT to stop
//  and assemble - that reads as unresponsive, and the player ordered a move,
//  not a parade. Instead the units out in FRONT slow down until the block has
//  closed up, and then it is over.
//
//  TWO PROPERTIES MATTER, AND BOTH ARE EASY TO GET WRONG:
//
//  1. THE SLOWDOWN IS EXPONENTIAL IN HOW FAR AHEAD A UNIT IS, not a flat
//     multiplier. A flat scale either barely helps the badly-scattered case or
//     visibly hobbles the nearly-formed one; scaling by the ratio makes a unit
//     far out in front crawl while one slightly ahead barely notices.
//
//  2. IT LATCHES OFF, ONCE, PER ORDER. `formEverFormed` is the whole reason
//     this is usable: without it every straggler that falls behind later - one
//     unit walking round a rock - re-triggers form-up and the entire army
//     crawls for the rest of the march. Form up once at the start; after that
//     the group marches at full speed and stragglers stay stragglers.
// ----------------------------------------------------------------------------
// The tuning lives in strategy_types.h, derived from FORMATION_SPACING and
// STRAT_UNIT_RADIUS rather than typed as bare numbers. That is not tidiness: the
// bug this whole section exists to fix was a tolerance (1.4) SMALLER than the
// slot pitch (1.5), so a group was asked to pack tighter than its own slots
// allowed, the latch could never fire, and the brake stayed down for the rest of
// the march. Tie the two numbers together and that cannot be written again.

// Recompute each live group's state from the roster. Runs once per frame before
// the movement pass, so every unit in a group compares against the SAME
// snapshot - computed inside the unit update it would depend on iteration order,
// and units early in the roster would pace themselves against a group that had
// already half-moved.
//
// RECOUNTED, never incrementally maintained, for the same reason the flow
// refcounts are: every exit a unit can take - dying, re-ordering, arriving,
// breaking off - would otherwise need its own decrement, and missing exactly one
// leaves a phantom member holding a group short of its latch forever.
void StrategyMoveFormUpdate(float dt)
{
    // LEGACY and SIMPLE never stamp formGroup, so every group record would be
    // empty and every pass over them a no-op with extra steps. Returning here
    // also keeps form-up pacing and the slot hold-pull out of those arms, which
    // is what makes them the cheap systems they are advertised as.
    if (StrategyControlGet() != STRAT_CTRL_CURRENT) return;

    StrategyWorld *world = StrategyWorldGet();
    int liveCount = 0;
    const int *live = StrategyActiveUnits(&liveCount);

    // Zero the per-frame tallies but KEEP the records. The latch, the timer and
    // the extents are what the table is for; clearing the whole table each frame
    // is what the parallel arrays used to do, and it is why nothing could
    // remember anything for longer than a frame.
    for (int i = 0; i < s_formGroupCount; i++)
    {
        s_formGroups[i].members = 0;
        s_formGroups[i].inPlace = 0;
        s_formGroups[i].worst   = 0.0f;
    }

    for (int k = 0; k < liveCount; k++)
    {
        int index = live[k];
        Unit *u = &world->units[index];
        if (u->formGroup < 0) { s_paths[index].formGroupSlot = -1; continue; }

        int slot = FormGroupSlot(u->formGroup);

        // Cached on the path side array so StrategyMoveTo does not repeat the
        // linear scan per unit per frame - 640k integer compares a frame at ten
        // thousand units, for an answer that only changes when an order does.
        // This pass already walks every unit and already does the lookup.
        s_paths[index].formGroupSlot = (int16_t)slot;
        if (slot < 0) continue;

        FormGroup *fg = &s_formGroups[slot];
        fg->members++;

        float dx = u->formSlot.x - u->pos.x;
        float dz = u->formSlot.z - u->pos.z;
        float d  = sqrtf(dx*dx + dz*dz);

        if (d > fg->worst) fg->worst = d;
        if (d <= FORMUP_TIGHT) fg->inPlace++;
    }

    // Second pass over the GROUPS, not the units: the latch is a group decision
    // and a unit that flipped it mid-scan would stop contributing to its own
    // group's worst distance.
    for (int i = 0; i < s_formGroupCount; i++)
    {
        FormGroup *fg = &s_formGroups[i];
        if (fg->members == 0) continue;

        if (!fg->everFormed)
        {
            fg->formTime += dt;

            // A FRACTION of the members, not the worst one. The worst-unit rule
            // makes the latch hostage to the single unluckiest member, and at a
            // thousand units there is always exactly one stuck behind a rock -
            // which is precisely how the reported 1000-unit group ended up
            // crawling until an unrelated enemy spawn released the brake.
            bool packed = ((float)fg->inPlace >= (float)fg->members*FORMUP_FRACTION);

            // THE BACKSTOP, and the reason this class of bug does not recur. Any
            // latch predicated on crowd geometry can be defeated by geometry; a
            // brake that can be held down indefinitely is exactly the failure
            // being removed. With the cap, "crawls forever" stops being a state
            // the system can reach at all.
            bool timedOut = (fg->formTime >= FORMUP_MAX_TIME);

            if (packed || timedOut) fg->everFormed = true;
        }

        // Arrived: the block holds its slots so the formation stays visible.
        // Measured on the same in-place fraction the latch uses, so a group that
        // formed up is a group that can hold.
        fg->holding = !fg->loose && fg->everFormed &&
                      ((float)fg->inPlace >= (float)fg->members*FORMUP_FRACTION);
    }

    // Mirror the latch onto the units. `formEverFormed` must stay on Unit even
    // though the group record is authoritative: MoveArriveReset reads it to drop
    // a unit out of its formation on any individual re-order, and deleting the
    // field would silently break every single-unit order in the game.
    for (int k = 0; k < liveCount; k++)
    {
        Unit *u = &world->units[live[k]];
        if (u->formGroup < 0) continue;
        int slot = s_paths[live[k]].formGroupSlot;
        if (slot < 0) continue;

        if (s_formGroups[slot].everFormed)
        {
            u->formEverFormed = true;
            u->formForming    = false;
        }
    }

    // Retire records nobody carries any more, so a long session cannot fill the
    // table with dead groups and start evicting live ones.
    //
    // COMPACTION MUST REPAIR THE CACHES IT MOVES. Swapping the last record down
    // leaves every unit that cached the old index pointing at whatever landed
    // there - an index still IN RANGE, so the `slot < 0` guard every reader has
    // does not catch it, and the unit silently paces itself against a different
    // group's extent and latch. Waiting for the next frame's refresh is not good
    // enough either: a group order arriving between now and then stamps fresh
    // caches alongside the stale ones. So the moved record's members are
    // re-pointed here, while the id needed to find them is still known.
    for (int i = 0; i < s_formGroupCount; )
    {
        if (s_formGroups[i].members != 0) { i++; continue; }

        int moved = --s_formGroupCount;
        s_formGroups[i] = s_formGroups[moved];

        if (moved != i)
        {
            int movedId = s_formGroups[i].id;
            for (int k = 0; k < liveCount; k++)
            {
                int index = live[k];
                if (world->units[index].formGroup == movedId)
                    s_paths[index].formGroupSlot = (int16_t)i;
            }
        }
    }
}

// Release every member of a group from its formation. Used by FORM_BEHAVIOR_
// ENGAGE, where one unit making contact commits the whole block - feeding units
// in one at a time is how a formation loses a fight it should win.
void StrategyFormationBreak(int groupId)
{
    if (groupId < 0) return;

    StrategyWorld *world = StrategyWorldGet();
    int liveCount = 0;
    const int *live = StrategyActiveUnits(&liveCount);

    for (int k = 0; k < liveCount; k++)
    {
        Unit *u = &world->units[live[k]];
        if (u->formGroup == groupId) u->formBrokeOff = true;
    }

    int slot = FormGroupSlot(groupId);
    if (slot >= 0) s_formGroups[slot].holding = false;
}

// Speed multiplier while forming up: 1.0 for the unit furthest from its slot,
// falling off exponentially for the ones already close. Returns 1.0 the moment
// the group has formed once, forever after.
static float FormUpScale(int index, const Unit *u)
{
    if (u->formGroup < 0 || u->formEverFormed || u->formBrokeOff) return 1.0f;

    int slot = s_paths[index].formGroupSlot;
    if (slot < 0) return 1.0f;

    const FormGroup *fg = &s_formGroups[slot];
    if (fg->loose) return 1.0f;                 // a scatter has nothing to close up

    float worst = fg->worst;
    if (worst <= FORMUP_TIGHT) return 1.0f;     // group is already together

    float dx = u->formSlot.x - u->pos.x;
    float dz = u->formSlot.z - u->pos.z;
    float d  = sqrtf(dx*dx + dz*dz);

    // Ratio of "how far I still have to go" to "how far the worst unit has to
    // go". The unit setting the pace gets 1.0; everyone nearer is scaled down.
    float ratio = d/worst;
    if (ratio > 1.0f) ratio = 1.0f;

    float scale = powf(ratio, FORMUP_EXP);
    return (scale < FORMUP_MIN_SCALE) ? FORMUP_MIN_SCALE : scale;
}

// Drop the flow field a unit is riding, WITHOUT touching its path.
//
// A field is a GROUP answer: the moment a unit is given its own destination the
// field is steering it at somebody else's, and since the field is only released
// on the approach it does so for the entire march. Its A* route, by contrast, is
// still worth keeping - NeedsPath decides when that has gone stale.
void StrategyMoveDropField(int index)
{
    if (index < 0 || index >= STRAT_MAX_UNITS) return;
    s_paths[index].field         = SP_FIELD_NONE;
    s_paths[index].formGroupSlot = -1;
    s_paths[index].formReleased  = false;
    s_paths[index].formChoked    = false;
    s_paths[index].formChokeTime = 0.0f;
}

// The restoring pull that makes a formation strict, applied to an ARRIVED unit
// that is still a member of a holding group. Returns the displacement to add to
// the unit's position this frame; zero whenever holding does not apply.
//
// THREE RULES, ALL LOAD-BEARING. Get any one wrong and this reintroduces the
// clustering spiral that Phase 2 removed:
//
//   1. The deadband is WIDER than the separation radius. Separation pushes out
//      to STRAT_SEP_RADIUS; a pull engaging inside that would oppose it head-on
//      and the pair would oscillate forever.
//   2. The pull is slow and capped. A shoved unit rocketing back to its slot
//      reads worse than one drifting off it.
//   3. It moves `pos`, not `vel`. That bypasses the separation integrator
//      entirely, so its damping term cannot amplify the correction into a
//      bounce.
//
// And the thing that is NOT here: UNIT_IDLE is deliberately still absent from
// UnitPushes. A holding unit receives push without applying it, so two
// neighbours both drifting home never shove each other - the existing
// apply/receive asymmetry does the work that a fourth rule would otherwise need.
Vector3 StrategyFormationHoldPull(int index, const Unit *u, float dt)
{
    Vector3 zero = { 0 };
    if (index < 0 || index >= STRAT_MAX_UNITS) return zero;
    if (u->formGroup < 0 || u->formBrokeOff)   return zero;

    int slot = s_paths[index].formGroupSlot;
    if (slot < 0) return zero;

    const FormGroup *fg = &s_formGroups[slot];
    if (!fg->holding || fg->loose) return zero;

    float dx = u->formSlot.x - u->pos.x;
    float dz = u->formSlot.z - u->pos.z;
    float d  = sqrtf(dx*dx + dz*dz);
    if (d <= FORM_HOLD_DEADBAND || d < 0.001f) return zero;

    float step = u->moveSpeed*FORM_HOLD_SPEED*dt;
    float room = d - FORM_HOLD_DEADBAND;        // never pull past the deadband,
    if (step > room) step = room;               //   or the unit oscillates on it

    Vector3 out = { dx/d*step, 0.0f, dz/d*step };
    return out;
}
