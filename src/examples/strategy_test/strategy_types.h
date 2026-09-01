// ============================================================================
//  strategy_types.h  -  data model for the strategy (RTS) test state
//
//  Everything lives in fixed-size arrays inside one StrategyWorld singleton
//  (no malloc). A slot is "alive" when its .active flag is set; freeing is
//  just clearing the flag. Positions are Vector3 on the y = 0 ground plane.
//
//  Per-kind numbers (HP, damage, ranges, costs, ...) live in the def tables
//  in strategy_defs.c - NOT here. Stats are resolved ONCE at spawn:
//      base UnitDef  x  faction difficulty mods  x  training-building buffs
//  and stored on the Unit instance, so combat/gather code never looks up
//  tables and difficulty never touches already-living units.
// ============================================================================

#ifndef STRATEGY_TYPES_H
#define STRATEGY_TYPES_H

#include "raylib.h"
#include <stdbool.h>

// For SGA_STATE_COUNT only. The asset module is headless and pulls in nothing
// from the game, so the dependency runs one way and does not cycle. Sizing the
// clock arrays off the real enum is what keeps them from silently disagreeing
// with it if a state is ever added.
#include "../../strategy_asset/strategy_asset.h"

// -- Capacities --------------------------------------------------------------
// STRAT_MAX_UNITS is TWO-TIER, the same scheme as SGM_*/SGA_* (see CMakeLists):
// the value here is the WEB tier, and desktop gets a raised one injected as a
// compile definition. Web stays small as a PERFORMANCE guard, not a memory one
// - 10k units is only 2.4 MB, which the 128 MB heap swallows, but a browser
// cannot draw 10k procedural units at any framerate.
//
// Sizing note for anything added to Unit: the struct is 248 bytes and the
// update loop walks it three times a frame. At the desktop cap that is 2.4 MB
// per pass, so a field only earns its place if the SIM needs it. Per-unit data
// used by one subsystem belongs in a side array indexed by slot instead.
#ifndef STRAT_MAX_UNITS
#define STRAT_MAX_UNITS      256    // Web tier; desktop raised via CMake
#endif
#define STRAT_MAX_BUILDINGS  24
#define STRAT_MAX_NODES      48
#define STRAT_MAX_CORPSES    16     // visual-only death animations in flight
#define UNIT_MAX_JOB_QUEUE   8      // build/repair/gather jobs one worker Shift-queues
#define STRAT_FACTIONS       2      // 0 = player (blue), 1 = enemy (red)
#define FACTION_NEUTRAL      2      // animals: no stockpile, no color entry -
                                    //   always guard before indexing by faction

// -- Tuning (world-level; per-kind numbers live in strategy_defs.c) ----------
#define STRAT_GROUND_HALF    25.0f  // ground spans [-HALF, +HALF] on x and z
#define STRAT_UNIT_RADIUS    0.35f
#define STRAT_CARRY_MAX      5      // resource units carried before returning
#define STRAT_AI_PERIOD      1.0f   // seconds between enemy "think" ticks (Hard)
#define STRAT_RETARGET_RADIUS 10.0f // depleted node: search for the next one here
#define STRAT_ANIMAL_COUNT   6      // weak neutral critters spawned at init
#define STRAT_ANIMAL_STRONG_COUNT 3 // strong neutral beasts spawned at init
#define STRAT_AI_ATTACK_SQUAD 4     // idle enemy soldiers needed for an attack wave

// -- Separation and arrival ---------------------------------------------------
// EVERY DISTANCE HERE IS DERIVED FROM STRAT_UNIT_RADIUS, never typed as a bare
// number. The original arrival test was a literal 0.15 against a 0.35 radius,
// which meant a crowded unit could not physically satisfy it - the two numbers
// had drifted apart because nothing tied them together. Deriving them makes
// that class of mistake impossible: change the radius and the thresholds follow.
#define STRAT_SEP_RADIUS     (2.0f*STRAT_UNIT_RADIUS)   // pair push distance
#define STRAT_SEP_STRENGTH   6.0f    // push accel, world units/s^2 at full overlap
#define STRAT_SEP_DAMP       8.0f    // velocity decay/s; higher = stops sooner
#define STRAT_SEP_DEADBAND   0.02f   // push below this counts as zero (anti-shimmer)
#define STRAT_SEP_MAX_NEIGHBORS 8    // pushers considered; beyond this the sum
                                     //   barely turns, and the cap makes a
                                     //   death-ball's cost flat instead of spiky

#define STRAT_ARRIVE_SLOW    (3.0f*STRAT_UNIT_RADIUS)   // begin ramping speed down
#define STRAT_ARRIVE_STOP    (1.2f*STRAT_UNIT_RADIUS)   // close enough: settle
// Hysteresis: a settled unit only re-seeks once it is pushed WELL past the
// slowing band, not just outside the stop radius. Set this near STOP and a unit
// on the edge of a crowd flips between settled and seeking every few frames,
// which looks exactly like the spiral this is meant to remove.
#define STRAT_ARRIVE_RESUME  (2.0f*STRAT_ARRIVE_SLOW)   // shoved this far: re-seek
// Seconds of no progress before a unit gives up and settles. 0.5, not the 1.25
// this once claimed: the constant was declared and then never used - MoveArrive
// carried a literal - so the two had been disagreeing silently. The literal is
// the value that was actually shipped and tested, so it is the one kept.
#define STRAT_ARRIVE_STALL   0.5f
// The stall rule only applies once a unit is CLOSE. Ungated it fires on the
// rear of any large column - those units are blocked for seconds while the
// front resolves, and settling them strands a 500-unit order at its start.
#define STRAT_ARRIVE_GIVEUP  (8.0f*STRAT_UNIT_RADIUS)   // stall test applies inside this
#define STRAT_AGGRO_STRIDE   15      // 1 unit in N runs its sight scan per frame
                                     //   (~0.25s worst-case reaction at 60 Hz)
#define STRAT_SETTLE_CROWD   3       // settled neighbours that justify stopping
                                     //   short - without this a chokepoint
                                     //   grinds forever

// Neutral animal reactions to being hit.
#define STRAT_FLEE_PACK_RADIUS 5.0f // weak animals this close flee together
#define STRAT_FLEE_DIST        7.0f // how far away from the attacker they run

// Templar blessing / healing.
#define STRAT_BLESS_PERIOD   5.0f   // seconds between blessings near a target
#define STRAT_BLESS_TIME     0.6f   // pause while the sparkles play
#define STRAT_HEAL_AMOUNT    8.0f   // hp restored by one healer blessing
#define STRAT_HEAL_COST      1     // providence consumed by one healing

// Construction / repair (worker-driven).
#define STRAT_BUILD_RANGE    1.8f   // worker must be this close to build/repair
#define STRAT_REPAIR_RATE    12.0f  // building hp restored per second per worker

// Node-tending buildings (farm -> wheat, forestry -> wood): an assigned worker
// walks to a free spot nearby and plants a node. Once TEND_MAX nodes stand
// near the building the worker instead harvests the nearest one to depletion,
// then resumes planting. Amounts come from the building def (tendAmount).
#define STRAT_TEND_RANGE   6.0f   // how far from the building a worker plants
#define STRAT_TEND_SPACING 1.4f   // min gap to any node/building at a plant spot
#define STRAT_TEND_PERIOD  1.5f   // seconds to plant one node once in position
#define STRAT_TEND_MAX     8       // planted nodes near the building before gathering
#define STRAT_TEND_EQUIP_TIME 0.5f // dwell at the building to grab a hat + sapling

// Auto-gather: a worker that finishes / is right-clicked onto a gathering
// building looks this far from the building for a node of the right kind.
#define STRAT_AUTO_GATHER_RANGE 15.0f

// Quarry: spend providence to conjure a fresh stone node beside it.
#define STRAT_QUARRY_STONE_PROV   2     // providence spent per spawn
#define STRAT_QUARRY_STONE_AMOUNT 100   // stone in the spawned node
#define STRAT_QUARRY_STONE_SPREAD 3.0f  // jitter radius around the quarry

typedef enum {
    RES_WOOD = 0,
    RES_STONE,
    RES_FOOD,
    RES_PROVIDENCE,     // global currency generated by templar blessings;
                        //   never carried or dropped off (no node yields it)
    RES_COUNT
} ResourceKind;

typedef enum {
    NODE_TREE = 0,      // yields RES_WOOD
    NODE_ROCK,          // yields RES_STONE
    NODE_WHEAT,         // yields RES_FOOD
    NODE_CORPSE,        // yields RES_FOOD (left behind by hunted animals)
    NODE_KIND_COUNT,    // sentinel: iterate every node kind (asset showcase)
} NodeKind;

typedef enum {
    BLD_HOUSE = 0,      // raises the pop cap, accepts nothing
    BLD_LOGGING,        // wood dropoff
    BLD_QUARRY,         // stone dropoff
    BLD_BARRACKS,       // trains melee + ranged soldiers
    BLD_FARM,           // workers assigned to it generate food; food dropoff
    BLD_TOWN_HALL,      // CRITICAL initial building: trains workers, accepts
                        //   wood + stone + food
    BLD_CHANTRY,        // trains templars (providence economy)
    BLD_FORESTRY,       // auto-plants fresh tree nodes nearby over time
    BLD_COUNT
} BuildingKind;

typedef enum {
    KIND_WORKER = 0,        // gathers/farms/hunts, weak in a fight
    KIND_SOLDIER,           // melee fighter, cannot gather
    KIND_RANGED,            // bow soldier: kites at near-max shooting distance
    KIND_TEMPLAR,           // non-combat: follows own units, blesses gatherers,
                            //   each blessing generates RES_PROVIDENCE
    KIND_TEMPLAR_HEALER,    // follows wounded own units, blessing heals them
                            //   and CONSUMES providence
    KIND_ANIMAL_WEAK,       // neutral critter: when one is hit the pack flees
    KIND_ANIMAL_STRONG,     // neutral beast: when hit the pack fights back
    UNIT_KIND_COUNT
} UnitKind;

typedef enum {
    UNIT_IDLE = 0,
    UNIT_MOVE,          // walking to .target
    UNIT_GATHER,        // walking to / working .targetNode
    UNIT_RETURN,        // carrying resources to the nearest ACCEPTING building
    UNIT_ATTACK,        // chasing / hitting .targetUnit OR .targetBuilding
    UNIT_FARM,          // working .targetBuilding (a farm), food straight to stockpile
    UNIT_FLEE,          // animal running away from an attacker (to .target)
    UNIT_FOLLOW,        // templar shadowing .targetUnit
    UNIT_BLESS,         // templar performing the sparkly blessing on .targetUnit
    UNIT_BUILD,         // worker raising a scaffold (.targetBuilding) to full
    UNIT_REPAIR,        // worker restoring a damaged building (.targetBuilding)
} UnitState;

// How far through a move a unit is. This is NOT a UnitState - a unit is still
// UNIT_MOVE while it arrives, and the state machine in UnitUpdate does not
// branch on it. It exists to stop crowds orbiting their own destination.
//
// THE BUG IT FIXES. Arrival used to be a single test: within 0.15 units of the
// target, become UNIT_IDLE. But STRAT_UNIT_RADIUS is 0.35, so in any crowd the
// push from neighbours GUARANTEES a unit is never that close - it stays
// UNIT_MOVE forever, driving inward while separation drives it out, and the
// whole pile rotates. Every unit in a stuck blob is a permanent engine.
//
// The fix is a progression with hysteresis rather than one threshold, plus one
// asymmetry: a SETTLED unit stops APPLYING push while still RECEIVING it. Two
// settled neighbours therefore stop shoving each other, which is what actually
// terminates the loop - without it a finished pile still breathes.
typedef enum {
    ARRIVE_SEEKING = 0, // full speed toward target
    ARRIVE_SLOWING,     // inside the approach band; speed ramps down
    ARRIVE_SETTLED,     // parked: no drive, no push applied, still pushable
} ArrivalPhase;

// Formation shape for a group move. The player picks one; it applies to every
// group order until changed. SHAPE AND BREAK-OFF BEHAVIOUR ARE SEPARATE AXES -
// any shape can be marched with any of the three behaviours, because "what the
// block looks like" and "who peels off when shot at" are unrelated decisions.
//
// Every shape lays out on the SAME two axes: `offR` across the formation's
// right-hand perpendicular, `offF` along its forward. Only the offsets differ,
// which is what keeps the whole set one switch in one function.
typedef enum {
    FORM_GRID = 0,      // square block; the historical default
    FORM_LINE,          // wide and shallow - the anti-spearhead shape
    FORM_COLUMN,        // narrow and deep, for marching through gaps
    FORM_TWO_COLUMN,    // two parallel files with a lane between them
    FORM_WEDGE,         // V, point toward the destination
    FORM_FREEFORM,      // loose scatter over a disc; an AREA order, not a block
    FORM_COUNT,
} FormationShape;

// What a formation does when hostiles turn up mid-march. Orthogonal to shape.
typedef enum {
    FORM_BEHAVIOR_SKIRMISH = 0, // only units with an enemy in their own range peel
                                //   off; the rest hold shape and keep marching
    FORM_BEHAVIOR_ENGAGE,       // first contact breaks the whole formation
    FORM_BEHAVIOR_HOLD,         // nobody peels; the march is the order
    FORM_BEHAVIOR_COUNT,
} FormationBehavior;

// An attack order further than this from the group's centroid is a MARCH and
// gets a formation; anything nearer is "hit that now" and stays a direct order,
// because forming up first would be a visible delay in the one moment the
// player least wants one.
#define STRAT_FORM_MARCH_DIST 18.0f

#define FORM_LINE_RANKS      2      // nominal depth of FORM_LINE
#define FORM_COLUMN_FILES    2      // nominal width of FORM_COLUMN
#define FORM_TWO_COLUMN_LANE 3.0f   // gap between the two files, world units

// EXTENT CAPS. Without these a shape scales linearly with unit count and walks
// straight off the map: two ranks of 100 units is 73 world units of frontage,
// most of the long march, and the outer slots land outside the grid where
// SpNearestOpen's ring cap cannot recover them - those units are stranded.
// Past the cap a line gains ranks and a column gains files, which is what real
// formations do when they run out of frontage or road.
#define FORM_LINE_MAX_WIDTH  40.0f  // frontage before FORM_LINE adds ranks
#define FORM_COLUMN_MAX_DEPTH 40.0f // depth before a column adds files

// -- Formation geometry and pacing -------------------------------------------
// DERIVED FROM STRAT_UNIT_RADIUS, for the same reason every arrival distance is:
// the one formation bug that cost the most was a tolerance smaller than the slot
// pitch, so units were asked to pack tighter than their own slots allowed and
// the form-up brake could never release. Tie the numbers together and that
// class of mistake stops being expressible.
#define FORMATION_SPACING    (4.286f*STRAT_UNIT_RADIUS)  // slot pitch (= 1.5 at r=0.35)

// Slot distance that counts as "in place" for the form-up latch. MUST exceed
// FORMATION_SPACING: a unit standing exactly on its own slot still has
// neighbours one pitch away, and separation legitimately displaces it.
#define FORMUP_TIGHT         (1.5f*FORMATION_SPACING)

// Fraction of a group that must be in place before form-up releases. Not the
// worst unit: at a thousand units there is always exactly one stuck behind a
// rock, and letting it hold the brake down is how the whole army crawls.
#define FORMUP_FRACTION      0.85f

// Hard ceiling on form-up, in seconds. THE BACKSTOP. Any latch predicated on
// crowd geometry can be defeated by geometry; this is what makes "crawls
// forever" structurally impossible rather than merely unlikely.
#define FORMUP_MAX_TIME      6.0f

// Slowest a forming unit walks. 0.35 rather than 0.15 because a 6.7x slowdown
// reads as broken even when it is working - and it matches the arrival ramp's
// own floor, which is the other place in this game a unit is deliberately slow.
#define FORMUP_MIN_SCALE     0.35f
#define FORMUP_EXP           2.0f   // how sharply being far ahead is punished

// Extra margin past the block's own half-extent at which a unit stops riding
// the shared flow field. The multiplier releases the rear ranks slightly BEFORE
// the front reaches its slots - release exactly at the block edge and the rear
// is still funnelling while the front is forming, which reads as the block
// being extruded through a hole.
#define FORM_RELEASE_MARGIN  2.0f
#define FORM_RELEASE_SCALE   1.25f

// -- Chokepoints --------------------------------------------------------------
// Terrain narrower than the block gets to break it: the units funnel through and
// re-form on the far side. Two widths, not one, and a dwell time - a single
// threshold flickers every frame at the mouth of a gap, the same failure
// STRAT_ARRIVE_RESUME exists to prevent.
#define FORM_CHOKE_ENTER     6      // tiles of open width: narrower than this funnels
#define FORM_CHOKE_EXIT      10     // ...and it takes this much to re-form
#define FORM_CHOKE_DWELL     0.6f   // seconds a verdict must hold before it flips
#define FORM_CHOKE_PROBE_MAX 12     // tiles counted per side; 24 wide is open ground
#define FORM_CHOKE_STRIDE    11     // 1 unit in N probes per frame (prime: spreads load)

// THE WIDTH A BLOCK ASKS FOR IS CAPPED. A 512-unit block wants ~34 tiles of
// frontage and almost no corridor is that wide, so without this every large
// group reads as permanently choked and the funnel becomes the default again -
// reinstating the bug the chokepoint rule exists to bound. The question is "is
// this a GAP", not "can the whole block stand abreast".
#define FORM_CHOKE_NEED_MAX  16.0f

// Rings searched outward when a formation slot lands on impassable ground.
// SIZED TO CLEAR A REAL OBSTACLE, not to be cheap: at 6 it could not escape
// anything wider than twelve tiles, so a LINE reaching a corner into a lake kept
// slots INSIDE it and those units wedged on the shore. This runs once per slot
// per order, on a click - a wider ring costs nothing that matters.
#define FORM_SLOT_RESOLVE_RING  16

// -- Slot holding -------------------------------------------------------------
// A unit in a formation is pulled back toward its slot, so the block holds shape
// while moving and stays visible once arrived.
//
// THE DEADBAND MUST EXCEED THE SEPARATION RADIUS. Separation pushes out to
// STRAT_SEP_RADIUS; a pull that engaged inside that would oppose it directly and
// the pair would oscillate forever. 2x is the distance saying how far a
// neighbour may legitimately displace you.
#define FORM_HOLD_DEADBAND   (2.0f*STRAT_SEP_RADIUS)
#define FORM_HOLD_SPEED      0.35f  // fraction of moveSpeed; a unit rocketing
                                    //   home reads worse than one drifting

// A queued worker job (Shift-RMB chain). One building index serves all kinds;
// gather resolves its resource node at dispatch time. See WorkerStartNextJob.
typedef enum {
    WJOB_BUILD = 0,     // finish a scaffold
    WJOB_REPAIR,        // repair a damaged building
    WJOB_GATHER,        // auto-assign to gather for a building (tend or dropoff)
} WorkerJobKind;

typedef struct {
    WorkerJobKind kind;
    int           building;         // buildings[] index this job targets
} WorkerJob;

typedef struct {
    bool         active;
    int          faction;
    UnitKind     kind;
    Vector3      pos;
    Vector3      target;            // move destination (UNIT_MOVE / UNIT_FLEE)
    UnitState    state;

    // Separation velocity, in world units/second, y always 0. Accumulated by
    // the push pass and integrated ONCE at the end of the frame.
    //
    // WHY IT IS STORED. Separation used to teleport positions inside the pair
    // loop, so a unit's displacement depended on how many neighbors happened to
    // be visited after it - and with a spatial hash that order changes every
    // frame. Summing into a velocity makes the result order-independent, and
    // keeping it between frames is what lets it be DAMPED: an undamped push
    // that is recomputed from scratch each frame has no memory, so a crowd
    // oscillates instead of coming to rest.
    Vector3      vel;

    // Arrival progress for UNIT_MOVE / UNIT_FLEE. See ArrivalPhase.
    ArrivalPhase arrival;
    float        stallTimer;        // seconds of near-zero progress while moving
    float        lastProgressDist;  // distance to target at the last stall check
    // Last building this unit deposited into, or -1. A CACHE, not state: it is
    // revalidated before every use and re-searched when stale, so losing it
    // costs one scan and never changes behaviour. It exists because UNIT_RETURN
    // otherwise re-scans every building every frame for every carrying worker.
    int          dropoffCache;

    int          crowd;             // settled neighbours seen by the last push
                                    //   pass; lets a unit boxed in by finished
                                    //   units settle where it stands instead of
                                    //   grinding at a chokepoint forever

    // -- Formation march ------------------------------------------------------
    // Set by StrategyOrderMoveGroup, cleared by any single-unit order. A unit
    // with formGroup < 0 is not in a formation and every rule below is skipped.
    int          formGroup;         // group id this unit marches with, -1 = none
    Vector3      formSlot;          // its assigned slot, in world space

    // WHICH slot of the shape this unit holds, and the shape and member count it
    // was assigned under. Together these are the formation's IDENTITY: while all
    // three still match, a re-order keeps every unit in the position it already
    // occupies and the block simply translates and turns as one.
    //
    // Without them a re-order re-derives the pairing from scratch, and because
    // facing is recomputed from centroid-to-destination every time, a 15-degree
    // swing - which an ordinary click produces easily - reshuffled 33 units in
    // 36. That is the "bunch that shuffle completely around" report.
    //
    // -1 means no remembered slot, which is what a fresh order or a changed
    // shape/count leaves behind.
    int          formSlotIndex;
    int          formSlotShape;     // FormationShape it was assigned under
    int          formSlotOf;        // member count it was assigned under
    bool         formForming;       // still closing up: speed is being scaled down
    bool         formEverFormed;    // THE ONCE-ONLY LATCH. Form-up slows the units
                                    //   out in front until the block closes up, and
                                    //   then must never do it again - otherwise every
                                    //   straggler re-triggers it and the whole army
                                    //   crawls for the rest of the march.
    bool         formBrokeOff;      // peeled off to fight; rejoins on its next order
    float        hp, maxHp;

    // Stats resolved at spawn (def x difficulty x building buffs); see header.
    float        damage;
    float        attackRange;
    float        attackPeriod;      // seconds between hits
    float        preferredRange;    // >0: ranged unit's kiting stand-off distance
    float        moveSpeed;
    float        sightRange;        // auto-aggro scan radius
    float        gatherTime;        // seconds per chop/mine tick
    float        farmPeriod;        // seconds per food unit while farming

    float        attackCooldown;    // seconds until the next hit is allowed
    int          targetUnit;        // units[] index: UNIT_ATTACK victim, or the
                                    //   templar's FOLLOW/BLESS target (-1 = none)
    int          targetNode;        // nodes[] index while UNIT_GATHER (-1 = none)
    int          targetBuilding;    // buildings[] index while attacking one or
                                    //   farming (-1 = none); never set with targetUnit
    int          carryAmount;       // 0..STRAT_CARRY_MAX
    ResourceKind carryKind;
    float        gatherTimer;       // accumulates toward gatherTime / farmPeriod,
                                    //   and paces the templar bless cycle
    bool         selected;          // player faction only
    int          controlGroup;      // 0 = none, 1..3 = ctrl+digit group

    // Node-tending: UNIT_FARM planting is a round-trip. tendEquipped is true
    // once the worker has walked back to the building for a hat + sapling and
    // is carrying them out to the plant spot; cleared after planting/harvest.
    bool         tendEquipped;

    // Job queue: Shift-RMB build/repair/gather targets onto one worker to chain
    // them. The active job is the current state + targetBuilding; these follow.
    WorkerJob    jobQueue[UNIT_MAX_JOB_QUEUE];
    int          jobQueueCount;     // 0..UNIT_MAX_JOB_QUEUE

    // Presentation only. Nothing in here may ever decide a gameplay outcome -
    // it is derived FROM the sim each frame, never the other way round.
    // See strategy_entity_anim.h for what each field means.
    struct {
        float yaw, yawTarget;
        bool  hasYaw, walking;
        float clock[SGA_STATE_COUNT];
        float oneShot[SGA_STATE_COUNT];
    } anim;
} Unit;

typedef struct {
    bool         active;
    BuildingKind kind;
    int          faction;
    Vector3      pos;
    float        hp, maxHp;
    int          trainKind;         // UnitKind in production, -1 = idle
    float        trainProgress;     // seconds spent on the current trainee
    float        trainCooldown;     // anti-spam: seconds until training may restart

    // Production queue: kinds waiting behind the current trainee (paid for on
    // enqueue). The active trainee is trainKind; these are the ones after it.
#define BLD_MAX_QUEUE 8
    UnitKind     trainQueue[BLD_MAX_QUEUE];
    int          trainQueueCount;   // 0..BLD_MAX_QUEUE

    // Construction: a freshly placed building starts as a scaffold that a
    // worker must build up before it functions (no train / dropoff / popcap).
    bool         underConstruction;
    float        buildProgress;     // seconds accrued by workers, toward buildTime

    // Rally: trained units walk here on completion instead of going idle.
    Vector3      rally;
    bool         hasRally;

    // Timed buffs (reserved: filled by future upgrades / templar blessings
    // on buildings; property indices to be defined with the upgrade system).
#define BLD_MAX_ACTIVE_BUFFS 4
    struct { int property; float mul; float remaining; } buffs[BLD_MAX_ACTIVE_BUFFS];
} Building;

typedef struct {
    bool     active;
    NodeKind kind;
    Vector3  pos;
    int      remaining;             // resource units left; 0 -> despawn
} ResourceNode;

// Per-faction difficulty multipliers, resolved once at init from the
// settings difficulty. The player's row is always identity; FACTION_NEUTRAL
// gets an identity row too so UnitSpawn can index by faction unguarded.
typedef struct {
    float hpMul, dmgMul;        // unit maxHP / attack damage
    float gatherMul;            // gather + farm time (bigger = slower)
    float sightMul;             // auto-aggro scan radius
    float aiPeriodMul;          // stretches the faction brain tick
    float refundBonus;          // added to building sell refund rate
} FactionMods;

// A unit that has died and is still finishing its DIE animation. PURELY
// visual: it is not selectable, not targetable, not counted for population or
// game-over, and occupies no unit slot. That is the whole point - death frees
// the slot instantly, exactly as it always has, so no combat, AI or selection
// code had to learn about dying units in order for a death animation to exist.
typedef struct {
    bool     active;
    UnitKind kind;
    int      faction;
    Vector3  pos;
    float    yaw;
    float    t;             // seconds into DIE; retired past the duration
} UnitCorpse;

typedef struct {
    Unit         units[STRAT_MAX_UNITS];
    Building     buildings[STRAT_MAX_BUILDINGS];
    ResourceNode nodes[STRAT_MAX_NODES];
    UnitCorpse   corpses[STRAT_MAX_CORPSES];
    int          stockpile[STRAT_FACTIONS][RES_COUNT];

    FactionMods  mods[STRAT_FACTIONS + 1];  // [FACTION_NEUTRAL] = identity
    float        aiPeriod;                  // STRAT_AI_PERIOD * enemy aiPeriodMul

    // -- Battlefield extent, in world units from the origin ------------------
    // The ground used to be a fixed STRAT_GROUND_HALF square, and that constant
    // was read directly at the camera clamp, the placement margin, the ground
    // plane and the gridlines. An authored map sets its own extent, so those
    // four sites read THESE instead and the constant is now only the default
    // for the built-in layout. Half-extents (not full width) because every one
    // of those sites wants the distance from the origin to the edge.
    //
    // Set by StrategyWorldInit BEFORE anything spawns - PlacementValid and the
    // camera clamp are both live during init.
    float        groundHalfX;
    float        groundHalfZ;

    // The authored map this world was built from, or NULL for the built-in
    // layout. BORROWED from the map catalog, which outlives the world; held so
    // the passability grid can be consulted at placement time without copying
    // 256 KB into the world struct. Never freed here.
    const struct SgmMap *map;

    // Camera: fixed-pitch RTS view. The camera is DERIVED every frame from
    // focus + zoom, so panning/zooming only touch these two fields.
    Camera3D camera;
    Vector2  camFocus;              // ground point the camera looks at (x, z)
    float    camZoom;               // scales the fixed offset (clamped)

    // Input / UI state
    bool      pressInWorld;         // LMB press began in the world (not on GUI);
                                    //   drag/release selection only acts on these
    bool      dragging;             // LMB held and moved past the drag threshold
    Vector2   dragStart;            // game-canvas pixels (Screen2Target space)
    int       placing;              // BuildingKind ghost being placed, -1 = none
    int       selectedBuilding;     // buildings[] index (player), -1 = none;
                                    //   mutually exclusive with unit selection
    bool      buildMenuOpen;        // command panel currently shows the build list
    int       gameOver;             // -1 = playing, else the WINNING faction index
    float     aiTimer;              // countdown to the next enemy think tick
    Rectangle guiBlock;             // REAL-screen px area where the GUI owns the
                                    //   mouse (command panel); world clicks ignore it
    Rectangle guiBlock2;            // second reserved area (left pop panel)
} StrategyWorld;

#endif // STRATEGY_TYPES_H
