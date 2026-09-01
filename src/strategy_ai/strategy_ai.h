// ============================================================================
//  strategy_ai.h  -  faction behaviour: archetypes, brains, decisions
//
//  WHAT THIS MODULE IS. The part of an AI faction that DECIDES. It is a pure
//  function of (brain state, archetype profile, census) -> a list of intents.
//  It does not move units, spend resources, place buildings or draw anything;
//  the game turns intents into StrategyOrder* calls, exactly as the mouse does.
//
//  IT MUST NOT INCLUDE strategy_types.h. Same rule, and the same reason, as
//  src/strategy_path/: that header pulls in strategy_asset.h and from there the
//  whole anim system, which would drag a test binary into the renderer. So this
//  module talks in PLAIN INDICES - a unit kind is an int, not a UnitKind - the
//  way src/strategy_map/ stores a placement family plus a numeric kind. The
//  adapter in strategy_test/strategy_ai.c owns the translation.
//
//  WHY IT IS SPLIT OUT AT ALL. The behaviour it encodes is the interesting,
//  fiddly, balance-sensitive part of an RTS opponent, and while it lived inside
//  the game loop none of it could be tested: asserting "a rusher attacks before
//  a turtle does" needed a window, a world and ninety seconds of wall clock.
//  As a pure decision function it needs none of those, so tests/ai_tests.c can
//  assert the behaviour directly and this file can be tuned without fear.
//
//  raylib is NOT included. Nothing here needs a Vector3 - positions are the
//  adapter's problem, because "which building" is a decision and "where exactly"
//  is geometry.
// ============================================================================

#ifndef STRATEGY_AI_H
#define STRATEGY_AI_H

#include <stdbool.h>

// -- Dimensions ---------------------------------------------------------------
// These MIRROR the game's enums (UnitKind, BuildingKind, ResourceKind) without
// including them. The adapter static-asserts them equal to the real counts, so
// adding a unit kind to the game fails the build here rather than silently
// giving every archetype a weight of zero for it.
#define SAI_UNIT_KINDS      7   // == UNIT_KIND_COUNT
#define SAI_BLD_KINDS       8   // == BLD_COUNT
#define SAI_RES_KINDS       4   // == RES_COUNT

#define SAI_FACTIONS_MAX    9   // == STRAT_FACTIONS
#define SAI_BUILD_ORDER_MAX 8   // steps in an opening build order
#define SAI_INTENT_MAX      8   // intents one think tick may emit

// Unit kind indices, mirroring UnitKind. Named so the profile table reads as
// behaviour ("mass rangers") rather than as an array of unlabelled floats.
#define SAI_WORKER          0
#define SAI_SOLDIER         1
#define SAI_RANGED          2
#define SAI_TEMPLAR         3
#define SAI_HEALER          4

// Building kind indices, mirroring BuildingKind.
#define SAI_BLD_HOUSE       0
#define SAI_BLD_LOGGING     1
#define SAI_BLD_QUARRY      2
#define SAI_BLD_BARRACKS    3
#define SAI_BLD_FARM        4
#define SAI_BLD_TOWN_HALL   5
#define SAI_BLD_CHANTRY     6
#define SAI_BLD_FORESTRY    7

// Resource indices, mirroring ResourceKind.
#define SAI_RES_WOOD        0
#define SAI_RES_STONE       1
#define SAI_RES_FOOD        2
#define SAI_RES_PROVIDENCE  3

// ---------------------------------------------------------------------------
//  What an archetype attacks
// ---------------------------------------------------------------------------
typedef enum {
    SAI_TARGET_NEAREST = 0,     // the closest enemy building to home
    SAI_TARGET_PLAYER,          // always the human, wherever they are
    SAI_TARGET_WEAKEST,         // whichever faction is furthest behind
    SAI_TARGET_WORKERS,         // harass the economy, not the buildings
    SAI_TARGET_COUNT
} AiTargetPref;

// ---------------------------------------------------------------------------
//  The archetype: every dial that makes one faction play unlike another
//
//  THIS STRUCT IS THE FEATURE. Everything the old AI hardcoded - six workers,
//  a 1:1 melee/ranged split, a squad of four, one chantry - is a field here, so
//  "masses workers" and "masses rangers" are two rows of a table rather than
//  two branches of an if.
// ---------------------------------------------------------------------------
typedef struct {
    const char *name;

    // -- Composition ---------------------------------------------------------
    int   workerTarget;                 // workers it wants before it stops
    float kindWeight[SAI_UNIT_KINDS];   // relative army mix; 0 = never trains it
    int   armyCeiling;                  // fighters before it stops training

    // -- Aggression ----------------------------------------------------------
    float aggression;                   // 0..1, scales squad size and cadence
    int   attackSquad;                  // idle fighters needed to commit a wave
    float firstAttackTime;              // seconds before its first wave, ever
    float waveInterval;                 // seconds between waves
    float commitFraction;               // 0..1 of its army that goes

    // -- Build priorities ----------------------------------------------------
    int   buildOrder[SAI_BUILD_ORDER_MAX];  // building kinds, in order
    int   buildOrderCount;
    int   bldWant[SAI_BLD_KINDS];       // ceiling per kind

    // -- Target selection ----------------------------------------------------
    AiTargetPref targetPref;

    // -- Expansion / defence -------------------------------------------------
    float expandRadius;                 // how far from home it works and builds
    int   garrison;                     // fighters held back to defend

    // -- Stat mods, multiplied ON TOP of the difficulty row ------------------
    // Kept mild on purpose (0.9 .. 1.1). An archetype should be recognisable by
    // what it DOES; if it wins on stats the behaviour stops mattering.
    float hpMul, dmgMul, gatherMul, sightMul;

    // How often this brain thinks, as a multiple of the base AI period. A
    // frantic archetype reconsiders more often than a patient one.
    float thinkMul;
} AiProfile;

// ---------------------------------------------------------------------------
//  The census: what the game measured for one faction this tick
//
//  Filled by the adapter, never by this module. Everything the decision
//  functions are allowed to know lives in here, which is what keeps them pure.
// ---------------------------------------------------------------------------
typedef struct {
    int   unitCount[SAI_UNIT_KINDS];    // this faction's live units, by kind
    int   bldCount[SAI_BLD_KINDS];      // owned, including scaffolds
    int   bldReady[SAI_BLD_KINDS];      // finished only: these can train
    int   stock[SAI_RES_KINDS];
    int   popUsed, popCap;
    int   idleWorkers;
    int   idleFighters;
    int   scaffolds;                    // own buildings still unbuilt
    bool  hasEnemyTarget;               // any hostile building is reachable
    float elapsed;                      // seconds since the match began
} AiCensus;

// ---------------------------------------------------------------------------
//  Intents: what the brain wants done. The adapter maps these onto orders.
// ---------------------------------------------------------------------------
typedef enum {
    AI_INTENT_NONE = 0,
    AI_INTENT_TRAIN,        // train .kind at a finished .bldKind
    AI_INTENT_BUILD,        // place a .bldKind near home
    AI_INTENT_ATTACK_WAVE,  // send .squadSize fighters, chosen by .targetPref
    AI_INTENT_COUNT
} AiIntentKind;

typedef struct {
    AiIntentKind kind;
    int          unitKind;      // TRAIN
    int          bldKind;       // TRAIN (where), BUILD (what)
    int          squadSize;     // ATTACK_WAVE
    AiTargetPref targetPref;    // ATTACK_WAVE
} AiIntent;

// ---------------------------------------------------------------------------
//  The brain: the per-faction state the old AI never had
//
//  The previous implementation re-derived everything from a fresh census every
//  tick and remembered nothing, which is why it could not pace waves, could not
//  tell a first attack from a tenth, and could not hold anything back.
// ---------------------------------------------------------------------------
typedef struct {
    int   faction;
    float clock;            // seconds until this brain next thinks
    float period;           // seconds between its thinks
    float elapsed;          // seconds this brain has been alive
    float lastWaveAt;       // when it last committed a wave; < 0 = never
    int   wavesSent;
    int   buildStep;        // how far through the archetype's opening order
    bool  active;
} AiBrain;

// ---------------------------------------------------------------------------
//  API
// ---------------------------------------------------------------------------

// The archetype a faction plays. Assigned BY INDEX, so a given faction on a
// given map always plays the same way and a bug is reproducible. Faction 0 is
// the human player and returns a benign row that nothing reads.
const AiProfile *AiProfileFor(int faction);
const char      *AiProfileName(int faction);

// Set up one faction's brain. `basePeriod` is the game's AI tick in seconds;
// the archetype's thinkMul and a per-faction phase offset are applied here, the
// offset so eight brains never all think on the same frame.
void AiBrainInit(AiBrain *b, int faction, float basePeriod);

// Advance the think clock. Returns true when this brain is due, and only then
// should the caller build a census and call AiBrainDecide. Cheap by design: it
// runs for every faction every frame.
bool AiBrainTick(AiBrain *b, float dt);

// THE decision function. Pure: same state + same census => same intents, no
// clock reads, no randomness, no I/O. Returns how many intents were written.
int  AiBrainDecide(AiBrain *b, const AiProfile *p, const AiCensus *c,
                   AiIntent *out, int outMax);

// Which unit kind an archetype most wants next, given what it already has, or
// -1 if it wants none. Exposed because it is the heart of "masses rangers" and
// is worth testing directly.
int  AiPickUnitKind(const AiProfile *p, const AiCensus *c);

// Which building an archetype wants next, or -1. Follows buildOrder while it
// has steps left, then tops up toward bldWant.
int  AiPickBuilding(const AiProfile *p, const AiCensus *c, int buildStep);

#endif // STRATEGY_AI_H
