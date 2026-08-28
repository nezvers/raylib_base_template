// ============================================================================
//  strategy_path_service.c  -  request queue, frame budget, result delivery
//
//  ONE SEARCHER, MANY ASKERS. Everything here exists to make that arrangement
//  behave under load. The design points worth knowing before editing:
//
//  THE QUEUE IS A RING OF SLOTS, NOT A LIST. Slots are reused, and a slot's
//  identity is (index, sequence) packed into the SpRequestId handed out. That
//  pair is what makes SpServiceCancel safe to call on a stale id: the sequence
//  will not match, so the cancel is a no-op instead of killing whichever
//  unrelated request now owns that slot. Cancelling a dead request is the
//  common case - a unit dies, the caller cancels, the path finished two frames
//  ago - so it has to be free and it has to be right.
//
//  RESULTS ARE BUFFERED, NOT PUSHED. The service cannot call back into game
//  code: it does not know what a Unit is, and that is the module boundary this
//  file exists to keep. So a completed search parks its waypoints and the game
//  drains them with SpServicePoll at a time of its choosing.
//
//  DEDUP IS BY (START CELL, GOAL CELL) AND IS DELIBERATELY MODEST. Two units on
//  the same tile with the same destination get one search. In a real crowd
//  units are spread over many tiles, so the hit rate is low - which is exactly
//  why flow fields are a separate phase and not an optimisation of this one.
//  What dedup does here is stop pathological cases (a stack of units on one
//  tile, repeated identical orders in one frame) from multiplying work.
// ============================================================================

#include "strategy_path.h"

#include <string.h>

#define SP_GOAL_RING        8       // how far SpNearestOpen searches for a legal goal
#define SP_BUDGET_DEFAULT   8000

typedef enum {
    SLOT_FREE = 0,
    SLOT_WAITING,       // queued, not started
    SLOT_RUNNING,       // the searcher is mid-search on this one
    SLOT_DONE           // result parked, waiting for a poll
} SlotState;

typedef struct {
    SlotState    state;
    int32_t      owner;
    uint32_t     sequence;      // bumped on every reuse; half of the request id
    int32_t      sx, sz, gx, gz;
    SpCell       startCell, goalCell;
    uint32_t     order;         // arrival ticket: serves oldest first

    SpPathStatus status;
    SpCell       cells[SP_PATH_MAX];
    int32_t      count;
} Slot;

static Slot          s_slots[SP_REQUESTS_MAX];
static const SpGrid *s_grid;
static SpAStar       s_astar;           // the single searcher
static int32_t       s_running = -1;    // slot index the searcher is on, or -1
static uint32_t      s_nextOrder = 1;
static int           s_budget = SP_BUDGET_DEFAULT;
static SpServiceStats s_stats;

// Scratch for one search's output. SP_PATH_MAX per slot is what the caller
// keeps; the searcher may produce more before truncation, and that is fine -
// SpAStarStep truncates for us.
static SpCell s_out[SP_PATH_MAX];

// ----------------------------------------------------------------------------
//  Request ids
//
//  Packed (sequence << 16) | index. The sequence is what makes a stale id
//  detectable, so it must be compared on every use - never trust the index
//  alone, or a cancel from a dead unit silently cancels a live one.
// ----------------------------------------------------------------------------
static SpRequestId MakeId(int32_t index, uint32_t sequence)
{
    return (SpRequestId)(((sequence & 0x7FFFu) << 16) | ((uint32_t)index & 0xFFFFu));
}

static Slot *SlotFromId(SpRequestId id)
{
    if (id < 0) return NULL;
    int32_t  index = (int32_t)((uint32_t)id & 0xFFFFu);
    uint32_t seq   = ((uint32_t)id >> 16) & 0x7FFFu;
    if (index < 0 || index >= (int32_t)SP_REQUESTS_MAX) return NULL;
    Slot *s = &s_slots[index];
    if (s->state == SLOT_FREE) return NULL;
    if ((s->sequence & 0x7FFFu) != seq) return NULL;
    return s;
}

// ----------------------------------------------------------------------------
//  Lifecycle
// ----------------------------------------------------------------------------
void SpServiceReset(const SpGrid *g)
{
    memset(s_slots, 0, sizeof(s_slots));
    s_grid      = g;
    s_running   = -1;
    s_nextOrder = 1;
    memset(&s_stats, 0, sizeof(s_stats));
    // The searcher's generation deliberately survives - it is a monotonic
    // counter with its own wraparound handling, and resetting it would make two
    // searches either side of a world load share a generation.
    s_astar.active = false;
}

void SpServiceSetBudget(int nodesPerFrame)
{
    if (nodesPerFrame < 1) nodesPerFrame = 1;
    s_budget = nodesPerFrame;
}

int SpServiceBudget(void) { return s_budget; }

const SpServiceStats *SpServiceGetStats(void) { return &s_stats; }

void SpServiceResetStats(void)
{
    int32_t queued = s_stats.queued;        // a live count, not a tally
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.queued = queued;
}

Vector3 SpCellToWorld(const SpGrid *g, SpCell c)
{
    int x = (int)(c % (SpCell)g->w);
    int z = (int)(c / (SpCell)g->w);
    return SpTileToWorld(g, x, z);
}

// ----------------------------------------------------------------------------
//  Requesting
// ----------------------------------------------------------------------------
static void SlotRelease(Slot *s)
{
    if (s->state == SLOT_FREE) return;
    if (s->state != SLOT_DONE) s_stats.queued--;
    // Cancelling the request the searcher is MID-SEARCH on has to abandon the
    // search too, or the next SpServiceUpdate resumes into a slot that has been
    // reused by someone else and delivers them a stranger's path.
    if ((s_running >= 0) && (&s_slots[s_running] == s))
    {
        s_running      = -1;
        s_astar.active = false;
    }
    s->state = SLOT_FREE;
}

// A unit only ever wants its most recent order, so a new request supersedes any
// outstanding one from the same owner. Without this a unit that is re-ordered
// every frame (a player dragging a waypoint) accumulates requests until the
// queue fills, and the queue-full rejection then hits some innocent unit.
static void CancelOwner(int32_t owner)
{
    for (int32_t i = 0; i < (int32_t)SP_REQUESTS_MAX; i++)
        if (s_slots[i].state != SLOT_FREE && s_slots[i].owner == owner)
            SlotRelease(&s_slots[i]);
}

SpRequestId SpServiceRequest(int32_t owner, int sx, int sz, int gx, int gz)
{
    if (s_grid == NULL) return SP_REQUEST_NONE;

    // Both ends resolve to open ground here rather than at every call site.
    // A goal inside a lake or a building is the single most common way to
    // reintroduce "unit walks at an unreachable point forever".
    int rsx, rsz, rgx, rgz;
    if (!SpNearestOpen(s_grid, sx, sz, SP_GOAL_RING, &rsx, &rsz) ||
        !SpNearestOpen(s_grid, gx, gz, SP_GOAL_RING, &rgx, &rgz))
    {
        s_stats.failed++;
        return SP_REQUEST_NONE;
    }

    CancelOwner(owner);

    int32_t free = -1;
    for (int32_t i = 0; i < (int32_t)SP_REQUESTS_MAX; i++)
        if (s_slots[i].state == SLOT_FREE) { free = i; break; }
    if (free < 0) { s_stats.rejected++; return SP_REQUEST_NONE; }

    Slot *s = &s_slots[free];
    s->sequence++;
    s->state     = SLOT_WAITING;
    s->owner     = owner;
    s->sx = rsx; s->sz = rsz;
    s->gx = rgx; s->gz = rgz;
    s->startCell = (SpCell)(rsz*s_grid->w + rsx);
    s->goalCell  = (SpCell)(rgz*s_grid->w + rgx);
    s->order     = s_nextOrder++;
    s->count     = 0;
    s->status    = SP_PATH_BUSY;
    s_stats.queued++;

    return MakeId(free, s->sequence);
}

void SpServiceCancel(SpRequestId id)
{
    Slot *s = SlotFromId(id);
    if (s != NULL) SlotRelease(s);
}

// ----------------------------------------------------------------------------
//  Serving
// ----------------------------------------------------------------------------
// Any other WAITING request with the same start and goal wants the identical
// answer. Hand them the result rather than searching again.
static void ShareResult(const Slot *done)
{
    for (int32_t i = 0; i < (int32_t)SP_REQUESTS_MAX; i++)
    {
        Slot *s = &s_slots[i];
        if (s == done || s->state != SLOT_WAITING) continue;
        if (s->startCell != done->startCell || s->goalCell != done->goalCell) continue;

        memcpy(s->cells, done->cells, sizeof(SpCell)*(size_t)done->count);
        s->count  = done->count;
        s->status = done->status;
        s->state  = SLOT_DONE;
        s_stats.queued--;
        s_stats.shared++;
        if (s->status == SP_PATH_FAILED) s_stats.failed++;
        else                             s_stats.served++;
    }
}

static int32_t PickNext(void)
{
    int32_t best = -1;
    uint32_t bestOrder = 0xFFFFFFFFu;
    for (int32_t i = 0; i < (int32_t)SP_REQUESTS_MAX; i++)
        if (s_slots[i].state == SLOT_WAITING && s_slots[i].order < bestOrder)
        {
            bestOrder = s_slots[i].order;
            best = i;
        }
    return best;
}

void SpServiceUpdate(void)
{
    if (s_grid == NULL) return;

    int budget = s_budget;
    s_stats.expanded = 0;

    while (budget > 0)
    {
        if (s_running < 0)
        {
            s_running = PickNext();
            if (s_running < 0) break;               // nothing left to do
            Slot *s = &s_slots[s_running];
            SpAStarBegin(&s_astar, s_grid, s->sx, s->sz, s->gx, s->gz);
        }

        Slot *s = &s_slots[s_running];
        int before = s_astar.expanded;
        int outCount = 0;
        SpPathStatus st = SpAStarStep(&s_astar, budget, s_out,
                                      (int)SP_PATH_MAX, &outCount);
        int used = s_astar.expanded - before;
        if (used < 1) used = 1;                     // always make progress
        budget -= used;
        s_stats.expanded += used;

        if (st == SP_PATH_BUSY)
        {
            s_stats.slicedFrames++;
            return;                                 // resume next frame, same slot
        }

        memcpy(s->cells, s_out, sizeof(SpCell)*(size_t)outCount);
        s->count  = outCount;
        s->status = st;
        s->state  = SLOT_DONE;
        s_stats.queued--;
        if (st == SP_PATH_FAILED) s_stats.failed++;
        else                      s_stats.served++;

        int32_t finished = s_running;
        s_running = -1;
        ShareResult(&s_slots[finished]);
    }
}

bool SpServicePoll(int32_t *outOwner, SpPathStatus *outStatus,
                   SpCell *out, int maxOut, int *outCount)
{
    for (int32_t i = 0; i < (int32_t)SP_REQUESTS_MAX; i++)
    {
        Slot *s = &s_slots[i];
        if (s->state != SLOT_DONE) continue;

        int n = (s->count < maxOut) ? (int)s->count : maxOut;
        memcpy(out, s->cells, sizeof(SpCell)*(size_t)n);
        *outCount  = n;
        *outOwner  = s->owner;
        *outStatus = s->status;
        s->state   = SLOT_FREE;
        return true;
    }
    return false;
}
