// ============================================================================
//  strategy_ai_profile.c  -  THE archetype table: how each faction plays
//
//  One row per faction index. This is the balance file for BEHAVIOUR, the way
//  strategy_defs.c is the balance file for stats, and it is the only place a
//  faction's character is described.
//
//  READING A ROW. kindWeight is a MIX, not a count: the brain trains whichever
//  kind is furthest below its share of the total, so { soldier 3, ranged 1 }
//  means three melee per archer however big the army gets. A weight of 0 means
//  "never trains this", which is how an archetype declines templars entirely.
//
//  TUNING RULE. Behaviour dials first, stat mods last. Stat mods stay inside
//  0.9..1.1 because an archetype that wins on numbers is not an archetype - the
//  player should be able to tell a rusher from a turtle by watching, not by
//  losing. Difficulty already owns "how strong"; these rows own "how it plays".
// ============================================================================

#include "strategy_ai.h"

// Faction 0 is the human. The row exists so AiProfileFor is total and never
// returns NULL; nothing reads it, because the adapter never runs a brain for
// faction 0. Deliberately inert rather than plausible: if it ever DID get used
// the resulting do-nothing faction is an obvious bug, not a subtle one.
static const AiProfile PROFILES[SAI_FACTIONS_MAX] = {
    [0] = {
        .name = "PLAYER",
        .workerTarget = 0, .armyCeiling = 0,
        .aggression = 0.0f, .attackSquad = 999999,
        .firstAttackTime = 1.0e9f, .waveInterval = 1.0e9f, .commitFraction = 0.0f,
        .buildOrderCount = 0,
        .targetPref = SAI_TARGET_NEAREST,
        .expandRadius = 40.0f, .garrison = 0,
        .hpMul = 1.0f, .dmgMul = 1.0f, .gatherMul = 1.0f, .sightMul = 1.0f,
        .thinkMul = 1.0f,
    },

    // Straight at you, early, with whatever is standing. Fewest workers of any
    // row - it is spending that economy on bodies instead - and the shortest
    // fuse. Loses badly if the rush is held; that is the trade it makes.
    [1] = {
        .name = "AGGRESSOR",
        .workerTarget = 6,
        .kindWeight = { [SAI_SOLDIER] = 3.0f, [SAI_RANGED] = 1.0f },
        .armyCeiling = 40,
        .aggression = 0.95f, .attackSquad = 4,
        .firstAttackTime = 45.0f, .waveInterval = 25.0f, .commitFraction = 0.9f,
        .buildOrder = { SAI_BLD_HOUSE, SAI_BLD_BARRACKS, SAI_BLD_HOUSE, SAI_BLD_BARRACKS },
        .buildOrderCount = 4,
        .bldWant = { [SAI_BLD_HOUSE] = 5, [SAI_BLD_BARRACKS] = 2, [SAI_BLD_FARM] = 1 },
        .targetPref = SAI_TARGET_NEAREST,
        .expandRadius = 34.0f, .garrison = 0,
        .hpMul = 1.0f, .dmgMul = 1.05f, .gatherMul = 1.0f, .sightMul = 1.0f,
        .thinkMul = 0.8f,
    },

    // Economy first and for a long time: triple the worker count of the rusher,
    // farms and forestry so it never runs dry, and it does not attack at all
    // until it can send something overwhelming. The late-game problem.
    [2] = {
        .name = "INDUSTRIALIST",
        .workerTarget = 20,
        .kindWeight = { [SAI_SOLDIER] = 2.0f, [SAI_RANGED] = 2.0f, [SAI_TEMPLAR] = 1.0f },
        .armyCeiling = 80,
        .aggression = 0.35f, .attackSquad = 16,
        .firstAttackTime = 240.0f, .waveInterval = 90.0f, .commitFraction = 0.8f,
        .buildOrder = { SAI_BLD_HOUSE, SAI_BLD_FARM, SAI_BLD_LOGGING, SAI_BLD_HOUSE,
                        SAI_BLD_FORESTRY, SAI_BLD_BARRACKS, SAI_BLD_QUARRY, SAI_BLD_HOUSE },
        .buildOrderCount = 8,
        .bldWant = { [SAI_BLD_HOUSE] = 8, [SAI_BLD_BARRACKS] = 2, [SAI_BLD_FARM] = 2,
                     [SAI_BLD_LOGGING] = 2, [SAI_BLD_QUARRY] = 1, [SAI_BLD_FORESTRY] = 1 },
        .targetPref = SAI_TARGET_WEAKEST,
        .expandRadius = 46.0f, .garrison = 4,
        .hpMul = 1.0f, .dmgMul = 1.0f, .gatherMul = 0.9f, .sightMul = 1.0f,
        .thinkMul = 1.2f,
    },

    // Almost pure ranged, and two barracks to produce them. Kiting is already in
    // the unit (preferredRange), so this row just supplies bodies that use it.
    [3] = {
        .name = "MARKSMAN",
        .workerTarget = 12,
        .kindWeight = { [SAI_RANGED] = 5.0f, [SAI_SOLDIER] = 1.0f },
        .armyCeiling = 55,
        .aggression = 0.6f, .attackSquad = 10,
        .firstAttackTime = 120.0f, .waveInterval = 50.0f, .commitFraction = 0.7f,
        .buildOrder = { SAI_BLD_HOUSE, SAI_BLD_BARRACKS, SAI_BLD_FARM,
                        SAI_BLD_HOUSE, SAI_BLD_BARRACKS },
        .buildOrderCount = 5,
        .bldWant = { [SAI_BLD_HOUSE] = 6, [SAI_BLD_BARRACKS] = 2, [SAI_BLD_FARM] = 1,
                     [SAI_BLD_LOGGING] = 1 },
        .targetPref = SAI_TARGET_NEAREST,
        .expandRadius = 40.0f, .garrison = 3,
        .hpMul = 0.95f, .dmgMul = 1.05f, .gatherMul = 1.0f, .sightMul = 1.1f,
        .thinkMul = 1.0f,
    },

    // The providence faction: chantry early, both templar kinds, healers keeping
    // a modest army alive far longer than its size suggests.
    [4] = {
        .name = "ZEALOT",
        .workerTarget = 14,
        .kindWeight = { [SAI_TEMPLAR] = 2.0f, [SAI_HEALER] = 2.0f,
                        [SAI_SOLDIER] = 3.0f, [SAI_RANGED] = 1.0f },
        .armyCeiling = 50,
        .aggression = 0.5f, .attackSquad = 10,
        .firstAttackTime = 150.0f, .waveInterval = 60.0f, .commitFraction = 0.75f,
        .buildOrder = { SAI_BLD_HOUSE, SAI_BLD_CHANTRY, SAI_BLD_FARM,
                        SAI_BLD_BARRACKS, SAI_BLD_HOUSE },
        .buildOrderCount = 5,
        .bldWant = { [SAI_BLD_HOUSE] = 6, [SAI_BLD_CHANTRY] = 2, [SAI_BLD_BARRACKS] = 1,
                     [SAI_BLD_FARM] = 2 },
        .targetPref = SAI_TARGET_NEAREST,
        .expandRadius = 38.0f, .garrison = 4,
        .hpMul = 1.05f, .dmgMul = 0.95f, .gatherMul = 1.0f, .sightMul = 1.0f,
        .thinkMul = 1.1f,
    },

    // Builds up and sits on it. The largest garrison of any row, the smallest
    // working radius, and a wave that only leaves when it is already winning.
    [5] = {
        .name = "TURTLE",
        .workerTarget = 16,
        .kindWeight = { [SAI_SOLDIER] = 3.0f, [SAI_RANGED] = 2.0f, [SAI_HEALER] = 1.0f },
        .armyCeiling = 60,
        .aggression = 0.2f, .attackSquad = 20,
        .firstAttackTime = 300.0f, .waveInterval = 120.0f, .commitFraction = 0.5f,
        .buildOrder = { SAI_BLD_HOUSE, SAI_BLD_FARM, SAI_BLD_BARRACKS,
                        SAI_BLD_QUARRY, SAI_BLD_HOUSE, SAI_BLD_CHANTRY },
        .buildOrderCount = 6,
        .bldWant = { [SAI_BLD_HOUSE] = 7, [SAI_BLD_BARRACKS] = 2, [SAI_BLD_FARM] = 2,
                     [SAI_BLD_QUARRY] = 1, [SAI_BLD_CHANTRY] = 1 },
        .targetPref = SAI_TARGET_NEAREST,
        .expandRadius = 26.0f, .garrison = 12,
        .hpMul = 1.1f, .dmgMul = 0.95f, .gatherMul = 1.0f, .sightMul = 0.95f,
        .thinkMul = 1.3f,
    },

    // Never commits. Small groups, constantly, aimed at workers rather than
    // buildings - it wins by making the victim's economy never quite recover.
    [6] = {
        .name = "RAIDER",
        .workerTarget = 10,
        .kindWeight = { [SAI_RANGED] = 3.0f, [SAI_SOLDIER] = 2.0f },
        .armyCeiling = 45,
        .aggression = 0.85f, .attackSquad = 3,
        .firstAttackTime = 60.0f, .waveInterval = 20.0f, .commitFraction = 0.35f,
        .buildOrder = { SAI_BLD_HOUSE, SAI_BLD_BARRACKS, SAI_BLD_FARM, SAI_BLD_HOUSE },
        .buildOrderCount = 4,
        .bldWant = { [SAI_BLD_HOUSE] = 5, [SAI_BLD_BARRACKS] = 2, [SAI_BLD_FARM] = 1 },
        .targetPref = SAI_TARGET_WORKERS,
        .expandRadius = 44.0f, .garrison = 2,
        .hpMul = 0.95f, .dmgMul = 1.0f, .gatherMul = 1.0f, .sightMul = 1.1f,
        .thinkMul = 0.7f,
    },

    // Patient and balanced, but always hits whoever is already losing - the row
    // that punishes a player for winning a fight expensively somewhere else.
    [7] = {
        .name = "OPPORTUNIST",
        .workerTarget = 14,
        .kindWeight = { [SAI_SOLDIER] = 2.0f, [SAI_RANGED] = 2.0f, [SAI_HEALER] = 1.0f },
        .armyCeiling = 60,
        .aggression = 0.55f, .attackSquad = 8,
        .firstAttackTime = 110.0f, .waveInterval = 45.0f, .commitFraction = 0.65f,
        .buildOrder = { SAI_BLD_HOUSE, SAI_BLD_FARM, SAI_BLD_BARRACKS,
                        SAI_BLD_HOUSE, SAI_BLD_LOGGING },
        .buildOrderCount = 5,
        .bldWant = { [SAI_BLD_HOUSE] = 6, [SAI_BLD_BARRACKS] = 2, [SAI_BLD_FARM] = 2,
                     [SAI_BLD_LOGGING] = 1 },
        .targetPref = SAI_TARGET_WEAKEST,
        .expandRadius = 42.0f, .garrison = 5,
        .hpMul = 1.0f, .dmgMul = 1.0f, .gatherMul = 0.95f, .sightMul = 1.05f,
        .thinkMul = 1.0f,
    },

    // Quantity. Only the two cheapest units, the highest ceiling, and it throws
    // essentially everything every time. Individually worthless, collectively
    // the reason the pop cap exists.
    [8] = {
        .name = "SWARM",
        .workerTarget = 18,
        .kindWeight = { [SAI_SOLDIER] = 4.0f, [SAI_WORKER] = 1.0f },
        .armyCeiling = 100,
        .aggression = 0.8f, .attackSquad = 14,
        .firstAttackTime = 90.0f, .waveInterval = 35.0f, .commitFraction = 0.95f,
        .buildOrder = { SAI_BLD_HOUSE, SAI_BLD_HOUSE, SAI_BLD_BARRACKS,
                        SAI_BLD_FARM, SAI_BLD_HOUSE, SAI_BLD_BARRACKS },
        .buildOrderCount = 6,
        .bldWant = { [SAI_BLD_HOUSE] = 10, [SAI_BLD_BARRACKS] = 3, [SAI_BLD_FARM] = 2 },
        .targetPref = SAI_TARGET_NEAREST,
        .expandRadius = 40.0f, .garrison = 0,
        .hpMul = 0.9f, .dmgMul = 0.95f, .gatherMul = 0.95f, .sightMul = 1.0f,
        .thinkMul = 0.9f,
    },
};

const AiProfile *AiProfileFor(int faction)
{
    if ((faction < 0) || (faction >= SAI_FACTIONS_MAX)) return &PROFILES[0];
    return &PROFILES[faction];
}

const char *AiProfileName(int faction)
{
    return AiProfileFor(faction)->name;
}
