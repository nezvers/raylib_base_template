// ============================================================================
//  ai_tests.c  -  headless checks for faction behaviour (src/strategy_ai/)
//
//  WHAT THIS SUITE IS FOR. Before the AI was a module, every claim about it
//  ("the rusher attacks earlier", "the marksman actually builds archers") could
//  only be checked by launching the game, picking a map and watching for a few
//  minutes - so in practice none of them were checked, and the behaviour drifted
//  whenever a constant moved. AiBrainDecide is pure, so all of it is assertable
//  here in milliseconds.
//
//  These are BEHAVIOUR tests, not balance tests. They assert the shape of each
//  archetype - that a turtle holds its garrison, that a swarm commits nearly
//  everything, that nobody trains forever - and deliberately not the exact
//  numbers, which are meant to be tuned in strategy_ai_profile.c without a test
//  failing every time.
//
//  No raylib, no window, no world: src/strategy_ai/ includes neither the game
//  types nor raylib, which is exactly what makes this file possible.
// ============================================================================

#include "../src/strategy_ai/strategy_ai.h"

#include <stdio.h>
#include <string.h>

static int s_checks = 0, s_fails = 0;

#define CHECK(cond) do { \
    s_checks++; \
    if (!(cond)) { s_fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// A faction that is up and running: enough of everything that the brain is
// never blocked on economy, so a test can isolate the dial it cares about.
static AiCensus HealthyCensus(void)
{
    AiCensus c = (AiCensus){ 0 };
    c.popUsed  = 10;
    c.popCap   = 60;
    c.stock[SAI_RES_WOOD]  = 500;
    c.stock[SAI_RES_STONE] = 500;
    c.stock[SAI_RES_FOOD]  = 500;
    c.hasEnemyTarget = true;
    for (int k = 0; k < SAI_BLD_KINDS; k++) { c.bldCount[k] = 3; c.bldReady[k] = 3; }
    return c;
}

// ---------------------------------------------------------------------------
//  Profiles
// ---------------------------------------------------------------------------
static void TestProfilesDistinct(void)
{
    // Every faction index resolves, including out-of-range ones.
    for (int f = 0; f < SAI_FACTIONS_MAX; f++)
    {
        const AiProfile *p = AiProfileFor(f);
        CHECK(p != NULL);
        CHECK(p->name != NULL);
    }
    CHECK(AiProfileFor(-1)  != NULL);       // total, never NULL
    CHECK(AiProfileFor(999) != NULL);

    // The AI rows must not be accidental copies of one another - the entire
    // point is that faction 1 and faction 5 play differently.
    for (int a = 1; a < SAI_FACTIONS_MAX; a++)
    {
        for (int b = a + 1; b < SAI_FACTIONS_MAX; b++)
        {
            const AiProfile *pa = AiProfileFor(a), *pb = AiProfileFor(b);
            CHECK(strcmp(pa->name, pb->name) != 0);
            bool same = (pa->workerTarget == pb->workerTarget) &&
                        (pa->attackSquad  == pb->attackSquad)  &&
                        (pa->firstAttackTime == pb->firstAttackTime) &&
                        (pa->targetPref   == pb->targetPref);
            CHECK(!same);
        }
    }

    // Faction 0 is the human: it must never want to do anything.
    const AiProfile *player = AiProfileFor(0);
    CHECK(player->workerTarget == 0);
    CHECK(player->armyCeiling == 0);
}

static void TestProfileSanity(void)
{
    for (int f = 1; f < SAI_FACTIONS_MAX; f++)
    {
        const AiProfile *p = AiProfileFor(f);

        CHECK(p->workerTarget > 0);
        CHECK(p->armyCeiling > 0);
        CHECK(p->attackSquad > 0);
        CHECK(p->waveInterval > 0.0f);
        CHECK(p->thinkMul > 0.0f);
        CHECK(p->expandRadius > 0.0f);
        CHECK(p->garrison >= 0);

        CHECK((p->aggression >= 0.0f) && (p->aggression <= 1.0f));
        CHECK((p->commitFraction > 0.0f) && (p->commitFraction <= 1.0f));

        // Stat mods stay mild: an archetype must be recognisable by behaviour,
        // not by being handed better units. Difficulty owns "how strong".
        CHECK((p->hpMul     >= 0.85f) && (p->hpMul     <= 1.15f));
        CHECK((p->dmgMul    >= 0.85f) && (p->dmgMul    <= 1.15f));
        CHECK((p->gatherMul >= 0.85f) && (p->gatherMul <= 1.15f));
        CHECK((p->sightMul  >= 0.85f) && (p->sightMul  <= 1.15f));

        // It must want SOME army, or it can never contest anything.
        float total = 0.0f;
        for (int k = 0; k < SAI_UNIT_KINDS; k++) total += p->kindWeight[k];
        CHECK(total > 0.0f);

        // Never weights the neutral animals - they are not trainable.
        CHECK(p->kindWeight[5] == 0.0f);
        CHECK(p->kindWeight[6] == 0.0f);

        // A build order must be walkable and within its own ceilings.
        CHECK((p->buildOrderCount >= 0) && (p->buildOrderCount <= SAI_BUILD_ORDER_MAX));
        for (int s = 0; s < p->buildOrderCount; s++)
        {
            int k = p->buildOrder[s];
            CHECK((k >= 0) && (k < SAI_BLD_KINDS));
            CHECK(p->bldWant[k] > 0);   // listed but capped at 0 = never buildable
        }

        // Every row needs houses, or it pop-caps and stops playing.
        CHECK(p->bldWant[SAI_BLD_HOUSE] > 0);
    }
}

// ---------------------------------------------------------------------------
//  Composition: the "masses X" claim, tested directly
// ---------------------------------------------------------------------------

// Run the picker in a loop, accumulating what it asks for. This is the closest
// thing to "play the faction for a while" the pure function allows.
static void SimulateTraining(int faction, int rounds, int *countsOut)
{
    const AiProfile *p = AiProfileFor(faction);
    AiCensus c = HealthyCensus();
    for (int k = 0; k < SAI_UNIT_KINDS; k++) c.unitCount[k] = 0;

    for (int i = 0; i < rounds; i++)
    {
        int kind = AiPickUnitKind(p, &c);
        if (kind < 0) break;
        c.unitCount[kind]++;
        c.popUsed++;
        if (c.popUsed >= c.popCap) c.popCap += 20;   // assume houses keep up
    }
    for (int k = 0; k < SAI_UNIT_KINDS; k++) countsOut[k] = c.unitCount[k];
}

static void TestCompositionDiffers(void)
{
    int agg[SAI_UNIT_KINDS], mark[SAI_UNIT_KINDS], zeal[SAI_UNIT_KINDS];
    int ind[SAI_UNIT_KINDS], swarm[SAI_UNIT_KINDS];

    SimulateTraining(1, 200, agg);    // AGGRESSOR    - soldiers
    SimulateTraining(2, 200, ind);    // INDUSTRIALIST- workers
    SimulateTraining(3, 200, mark);   // MARKSMAN     - rangers
    SimulateTraining(4, 200, zeal);   // ZEALOT       - templars
    SimulateTraining(8, 200, swarm);  // SWARM        - soldiers, cheaply

    // Each row masses what its name says.
    CHECK(agg[SAI_SOLDIER] > agg[SAI_RANGED]);
    CHECK(mark[SAI_RANGED] > mark[SAI_SOLDIER]);
    CHECK((zeal[SAI_TEMPLAR] + zeal[SAI_HEALER]) > 0);
    CHECK(swarm[SAI_SOLDIER] > swarm[SAI_RANGED]);

    // The economic row really does out-work the rush row, which is the single
    // most visible difference between them on the field.
    CHECK(ind[SAI_WORKER] > agg[SAI_WORKER]);

    // And the ranged/melee emphasis is genuinely opposite, not just noise.
    CHECK(mark[SAI_RANGED] > agg[SAI_RANGED]);
    CHECK(agg[SAI_SOLDIER] > mark[SAI_SOLDIER]);

    // A row with zero weight for a kind must never train it at all.
    CHECK(mark[SAI_TEMPLAR] == 0);
    CHECK(agg[SAI_TEMPLAR] == 0);
    CHECK(agg[SAI_HEALER] == 0);
}

static void TestWorkersComeFirst(void)
{
    // Below the worker target, every row builds economy before army - even the
    // rusher. A faction that opens on soldiers never gets a second building.
    for (int f = 1; f < SAI_FACTIONS_MAX; f++)
    {
        AiCensus c = HealthyCensus();
        c.unitCount[SAI_WORKER] = 0;
        CHECK(AiPickUnitKind(AiProfileFor(f), &c) == SAI_WORKER);
    }
}

static void TestTrainingTerminates(void)
{
    // armyCeiling and workerTarget must both actually bind. Without this a row
    // trains until the pop cap forever, and every archetype ends up identical.
    for (int f = 1; f < SAI_FACTIONS_MAX; f++)
    {
        const AiProfile *p = AiProfileFor(f);
        int counts[SAI_UNIT_KINDS];
        SimulateTraining(f, 5000, counts);

        CHECK(counts[SAI_WORKER] >= p->workerTarget);

        int fighters = counts[SAI_SOLDIER] + counts[SAI_RANGED];
        CHECK(fighters <= p->armyCeiling + 1);      // +1: the ceiling test is
                                                    //   applied before the add
    }
}

static void TestPopCapBlocksTraining(void)
{
    AiCensus c = HealthyCensus();
    const AiProfile *p = AiProfileFor(1);
    c.unitCount[SAI_WORKER] = p->workerTarget;      // economy already satisfied
    c.popUsed = c.popCap;                           // ...but no room
    CHECK(AiPickUnitKind(p, &c) == -1);

    // Workers are still the exception: pop pressure is answered with a house
    // (the BUILD intent), not by abandoning the economy.
    c.unitCount[SAI_WORKER] = 0;
    CHECK(AiPickUnitKind(p, &c) == SAI_WORKER);
}

// ---------------------------------------------------------------------------
//  Build priorities
// ---------------------------------------------------------------------------
static void TestBuildOrderFollowed(void)
{
    // With nothing built, a row asks for the first step of its own opening.
    for (int f = 1; f < SAI_FACTIONS_MAX; f++)
    {
        const AiProfile *p = AiProfileFor(f);
        if (p->buildOrderCount == 0) continue;

        AiCensus c = HealthyCensus();
        for (int k = 0; k < SAI_BLD_KINDS; k++) { c.bldCount[k] = 0; c.bldReady[k] = 0; }
        c.popUsed = 0; c.popCap = 20;       // no pop pressure to override it

        CHECK(AiPickBuilding(p, &c, 0) == p->buildOrder[0]);
    }
}

static void TestBuildRespectsCeiling(void)
{
    // Everything already at its ceiling: it must ask for nothing rather than
    // building a tenth barracks forever.
    for (int f = 1; f < SAI_FACTIONS_MAX; f++)
    {
        const AiProfile *p = AiProfileFor(f);
        AiCensus c = HealthyCensus();
        for (int k = 0; k < SAI_BLD_KINDS; k++)
        {
            c.bldCount[k] = p->bldWant[k];
            c.bldReady[k] = p->bldWant[k];
        }
        c.popUsed = 0; c.popCap = 100;
        CHECK(AiPickBuilding(p, &c, 0) == -1);
    }
}

static void TestOneScaffoldAtATime(void)
{
    // A scaffold in flight suppresses new placements, so the base does not end
    // up as six half-built shells with the workers split between them.
    AiCensus c = HealthyCensus();
    for (int k = 0; k < SAI_BLD_KINDS; k++) { c.bldCount[k] = 0; c.bldReady[k] = 0; }
    c.popUsed = 0; c.popCap = 20;
    c.scaffolds = 1;
    CHECK(AiPickBuilding(AiProfileFor(1), &c, 0) == -1);

    c.scaffolds = 0;
    CHECK(AiPickBuilding(AiProfileFor(1), &c, 0) >= 0);
}

static void TestPopPressureBuildsHouse(void)
{
    // Pop-capped overrides the opening order: nothing else matters while
    // training is blocked.
    AiCensus c = HealthyCensus();
    for (int k = 0; k < SAI_BLD_KINDS; k++) { c.bldCount[k] = 0; c.bldReady[k] = 0; }
    c.popUsed = 12; c.popCap = 12;
    CHECK(AiPickBuilding(AiProfileFor(2), &c, 0) == SAI_BLD_HOUSE);
}

// ---------------------------------------------------------------------------
//  Aggression
// ---------------------------------------------------------------------------
static void TestNoAttackBeforeFirstAttackTime(void)
{
    for (int f = 1; f < SAI_FACTIONS_MAX; f++)
    {
        const AiProfile *p = AiProfileFor(f);
        AiBrain b; AiBrainInit(&b, f, 1.0f);

        AiCensus c = HealthyCensus();
        c.unitCount[SAI_WORKER] = p->workerTarget;
        c.idleFighters = p->attackSquad + p->garrison + 50;   // plenty
        c.elapsed = p->firstAttackTime*0.5f;                  // ...but too early

        AiIntent out[SAI_INTENT_MAX];
        int n = AiBrainDecide(&b, p, &c, out, SAI_INTENT_MAX);
        for (int i = 0; i < n; i++) CHECK(out[i].kind != AI_INTENT_ATTACK_WAVE);
    }
}

static void TestAttacksOnceReady(void)
{
    for (int f = 1; f < SAI_FACTIONS_MAX; f++)
    {
        const AiProfile *p = AiProfileFor(f);
        AiBrain b; AiBrainInit(&b, f, 1.0f);

        AiCensus c = HealthyCensus();
        c.unitCount[SAI_WORKER] = p->workerTarget;
        c.idleFighters = p->attackSquad + p->garrison + 50;
        c.elapsed = p->firstAttackTime + 1.0f;

        AiIntent out[SAI_INTENT_MAX];
        int n = AiBrainDecide(&b, p, &c, out, SAI_INTENT_MAX);

        bool attacked = false;
        for (int i = 0; i < n; i++)
        {
            if (out[i].kind != AI_INTENT_ATTACK_WAVE) continue;
            attacked = true;
            CHECK(out[i].squadSize >= p->attackSquad);
            CHECK(out[i].targetPref == p->targetPref);
        }
        CHECK(attacked);
        CHECK(b.wavesSent == 1);
    }
}

static void TestWaveIntervalRespected(void)
{
    const AiProfile *p = AiProfileFor(1);
    AiBrain b; AiBrainInit(&b, 1, 1.0f);

    AiCensus c = HealthyCensus();
    c.unitCount[SAI_WORKER] = p->workerTarget;
    c.idleFighters = 100;
    c.elapsed = p->firstAttackTime + 1.0f;

    AiIntent out[SAI_INTENT_MAX];
    AiBrainDecide(&b, p, &c, out, SAI_INTENT_MAX);
    CHECK(b.wavesSent == 1);

    // Immediately after: still cooling down.
    c.elapsed += p->waveInterval*0.5f;
    AiBrainDecide(&b, p, &c, out, SAI_INTENT_MAX);
    CHECK(b.wavesSent == 1);

    // Past the interval: allowed again.
    c.elapsed += p->waveInterval;
    AiBrainDecide(&b, p, &c, out, SAI_INTENT_MAX);
    CHECK(b.wavesSent == 2);
}

static void TestGarrisonHeldBack(void)
{
    // THE turtle test. With exactly attackSquad fighters idle, a row that keeps
    // a garrison must NOT move - those fighters are the garrison. A row with no
    // garrison at the same count must.
    const AiProfile *turtle = AiProfileFor(5);
    CHECK(turtle->garrison > 0);

    AiBrain b; AiBrainInit(&b, 5, 1.0f);
    AiCensus c = HealthyCensus();
    c.unitCount[SAI_WORKER] = turtle->workerTarget;
    c.elapsed = turtle->firstAttackTime + 1.0f;
    c.idleFighters = turtle->attackSquad;       // enough, but all of it is home guard

    AiIntent out[SAI_INTENT_MAX];
    int n = AiBrainDecide(&b, turtle, &c, out, SAI_INTENT_MAX);
    for (int i = 0; i < n; i++) CHECK(out[i].kind != AI_INTENT_ATTACK_WAVE);

    // Give it the garrison on top and it commits.
    c.idleFighters = turtle->attackSquad + turtle->garrison;
    n = AiBrainDecide(&b, turtle, &c, out, SAI_INTENT_MAX);
    bool attacked = false;
    for (int i = 0; i < n; i++) if (out[i].kind == AI_INTENT_ATTACK_WAVE) attacked = true;
    CHECK(attacked);
}

static void TestCommitFractionOrdering(void)
{
    // Given identical armies, the all-in rows commit more than the cautious
    // ones. This is the dial that makes a swarm feel unlike a turtle.
    const int ARMY = 200;
    int sizes[SAI_FACTIONS_MAX];

    for (int f = 1; f < SAI_FACTIONS_MAX; f++)
    {
        const AiProfile *p = AiProfileFor(f);
        AiBrain b; AiBrainInit(&b, f, 1.0f);

        AiCensus c = HealthyCensus();
        c.unitCount[SAI_WORKER] = p->workerTarget;
        c.idleFighters = ARMY;
        c.elapsed = p->firstAttackTime + 1.0f;

        AiIntent out[SAI_INTENT_MAX];
        int n = AiBrainDecide(&b, p, &c, out, SAI_INTENT_MAX);
        sizes[f] = 0;
        for (int i = 0; i < n; i++)
            if (out[i].kind == AI_INTENT_ATTACK_WAVE) sizes[f] = out[i].squadSize;
        CHECK(sizes[f] > 0);
        CHECK(sizes[f] <= ARMY);
    }

    CHECK(sizes[8] > sizes[5]);     // SWARM commits harder than TURTLE
    CHECK(sizes[1] > sizes[6]);     // AGGRESSOR commits harder than RAIDER
}

static void TestNoTargetNoAttack(void)
{
    // Nothing to hit: the brain must not emit a wave that the adapter would
    // then have to throw away.
    const AiProfile *p = AiProfileFor(1);
    AiBrain b; AiBrainInit(&b, 1, 1.0f);

    AiCensus c = HealthyCensus();
    c.unitCount[SAI_WORKER] = p->workerTarget;
    c.idleFighters = 100;
    c.elapsed = p->firstAttackTime + 1.0f;
    c.hasEnemyTarget = false;

    AiIntent out[SAI_INTENT_MAX];
    int n = AiBrainDecide(&b, p, &c, out, SAI_INTENT_MAX);
    for (int i = 0; i < n; i++) CHECK(out[i].kind != AI_INTENT_ATTACK_WAVE);
    CHECK(b.wavesSent == 0);
}

static void TestTargetPrefsVary(void)
{
    // The rows must not all attack the same way, or "target selection" is not
    // a feature. At least three of the four preferences are in play.
    bool seen[SAI_TARGET_COUNT] = { false };
    for (int f = 1; f < SAI_FACTIONS_MAX; f++)
    {
        AiTargetPref t = AiProfileFor(f)->targetPref;
        CHECK((t >= 0) && (t < SAI_TARGET_COUNT));
        seen[t] = true;
    }
    int distinct = 0;
    for (int t = 0; t < SAI_TARGET_COUNT; t++) if (seen[t]) distinct++;
    CHECK(distinct >= 3);

    CHECK(AiProfileFor(6)->targetPref == SAI_TARGET_WORKERS);   // RAIDER harasses
    CHECK(AiProfileFor(7)->targetPref == SAI_TARGET_WEAKEST);   // OPPORTUNIST picks
}

static void TestAggressionOrdering(void)
{
    // The headline claim of the whole feature: these factions do not all attack
    // at the same time. Asserted as an ORDERING, not as absolute seconds, so
    // the numbers stay tunable.
    CHECK(AiProfileFor(1)->firstAttackTime < AiProfileFor(2)->firstAttackTime);
    CHECK(AiProfileFor(6)->firstAttackTime < AiProfileFor(5)->firstAttackTime);
    CHECK(AiProfileFor(1)->aggression      > AiProfileFor(5)->aggression);
    CHECK(AiProfileFor(6)->waveInterval    < AiProfileFor(2)->waveInterval);

    // The raider sends small groups; the industrialist sends an army.
    CHECK(AiProfileFor(6)->attackSquad < AiProfileFor(2)->attackSquad);
}

// ---------------------------------------------------------------------------
//  Brain lifecycle
// ---------------------------------------------------------------------------
static void TestBrainClockAndPhase(void)
{
    // Brains must not think in lockstep: eight censuses on one frame is a
    // periodic spike that grows with every faction added.
    AiBrain b[SAI_FACTIONS_MAX];
    for (int f = 1; f < SAI_FACTIONS_MAX; f++) AiBrainInit(&b[f], f, 1.0f);

    bool allSame = true;
    for (int f = 2; f < SAI_FACTIONS_MAX; f++)
        if (b[f].clock != b[1].clock) allSame = false;
    CHECK(!allSame);

    // The clock fires, and only once per period.
    AiBrain one; AiBrainInit(&one, 1, 1.0f);
    one.clock = 0.5f;
    CHECK(!AiBrainTick(&one, 0.2f));
    CHECK(!AiBrainTick(&one, 0.2f));
    CHECK(AiBrainTick(&one, 0.2f));         // crossed zero
    CHECK(!AiBrainTick(&one, 0.01f));       // and reset, so not again

    // thinkMul actually reaches the period.
    for (int f = 1; f < SAI_FACTIONS_MAX; f++)
    {
        AiBrain t; AiBrainInit(&t, f, 1.0f);
        CHECK(t.period > 0.0f);
        CHECK(t.active);
        CHECK(t.lastWaveAt < 0.0f);         // has never attacked
        CHECK(t.wavesSent == 0);
    }
}

static void TestBrainSurvivesLongFrame(void)
{
    // A long stall must produce ONE think, not a backlog the brain then runs
    // back-to-back on the following frames.
    AiBrain b; AiBrainInit(&b, 1, 1.0f);
    CHECK(AiBrainTick(&b, 60.0f));
    CHECK(!AiBrainTick(&b, 0.01f));
}

// ---------------------------------------------------------------------------
//  Degenerate inputs
// ---------------------------------------------------------------------------
static void TestEmptyCensusIsSafe(void)
{
    // A faction that owns nothing at all: the brain must not divide by zero,
    // must not ask to train from buildings it lacks, and must not attack.
    for (int f = 1; f < SAI_FACTIONS_MAX; f++)
    {
        AiBrain b; AiBrainInit(&b, f, 1.0f);
        AiCensus c = (AiCensus){ 0 };       // nothing owned, no pop, no stock

        AiIntent out[SAI_INTENT_MAX];
        int n = AiBrainDecide(&b, AiProfileFor(f), &c, out, SAI_INTENT_MAX);
        CHECK((n >= 0) && (n <= SAI_INTENT_MAX));
        for (int i = 0; i < n; i++)
        {
            CHECK(out[i].kind != AI_INTENT_ATTACK_WAVE);
            if (out[i].kind == AI_INTENT_TRAIN)
            {
                // Never from a building it does not have finished.
                CHECK(c.bldReady[out[i].bldKind] > 0);
            }
        }
    }
}

static void TestNullAndBoundsSafe(void)
{
    AiBrain b; AiBrainInit(&b, 1, 1.0f);
    AiCensus c = HealthyCensus();
    AiIntent out[SAI_INTENT_MAX];

    CHECK(AiBrainDecide(NULL, AiProfileFor(1), &c, out, SAI_INTENT_MAX) == 0);
    CHECK(AiBrainDecide(&b, NULL, &c, out, SAI_INTENT_MAX) == 0);
    CHECK(AiBrainDecide(&b, AiProfileFor(1), NULL, out, SAI_INTENT_MAX) == 0);
    CHECK(AiBrainDecide(&b, AiProfileFor(1), &c, NULL, SAI_INTENT_MAX) == 0);
    CHECK(AiBrainDecide(&b, AiProfileFor(1), &c, out, 0) == 0);

    // A one-slot buffer must be respected, not overrun.
    AiIntent one;
    int n = AiBrainDecide(&b, AiProfileFor(1), &c, &one, 1);
    CHECK((n >= 0) && (n <= 1));
}

static void TestDecideIsPure(void)
{
    // Same brain state + same census => same intents. This is the property the
    // whole module is built around; if it ever stops holding, every test above
    // becomes meaningless.
    const AiProfile *p = AiProfileFor(3);
    AiCensus c = HealthyCensus();
    c.unitCount[SAI_WORKER] = p->workerTarget;
    c.idleFighters = 30;
    c.elapsed = p->firstAttackTime + 1.0f;

    AiBrain b1, b2;
    AiBrainInit(&b1, 3, 1.0f);
    AiBrainInit(&b2, 3, 1.0f);

    AiIntent o1[SAI_INTENT_MAX], o2[SAI_INTENT_MAX];
    int n1 = AiBrainDecide(&b1, p, &c, o1, SAI_INTENT_MAX);
    int n2 = AiBrainDecide(&b2, p, &c, o2, SAI_INTENT_MAX);

    CHECK(n1 == n2);
    for (int i = 0; i < n1 && i < n2; i++)
    {
        CHECK(o1[i].kind      == o2[i].kind);
        CHECK(o1[i].unitKind  == o2[i].unitKind);
        CHECK(o1[i].bldKind   == o2[i].bldKind);
        CHECK(o1[i].squadSize == o2[i].squadSize);
    }
}

static void TestIntentsAreWellFormed(void)
{
    // Whatever a row emits, the adapter must be able to act on it: indices in
    // range, squad sizes positive.
    for (int f = 1; f < SAI_FACTIONS_MAX; f++)
    {
        const AiProfile *p = AiProfileFor(f);
        AiBrain b; AiBrainInit(&b, f, 1.0f);

        AiCensus c = HealthyCensus();
        c.unitCount[SAI_WORKER] = p->workerTarget;
        c.idleFighters = p->attackSquad + p->garrison + 20;
        c.elapsed = p->firstAttackTime + 1.0f;

        AiIntent out[SAI_INTENT_MAX];
        int n = AiBrainDecide(&b, p, &c, out, SAI_INTENT_MAX);
        for (int i = 0; i < n; i++)
        {
            CHECK(out[i].kind > AI_INTENT_NONE && out[i].kind < AI_INTENT_COUNT);
            switch (out[i].kind)
            {
                case AI_INTENT_TRAIN:
                    CHECK((out[i].unitKind >= 0) && (out[i].unitKind < SAI_UNIT_KINDS));
                    CHECK((out[i].bldKind  >= 0) && (out[i].bldKind  < SAI_BLD_KINDS));
                    break;
                case AI_INTENT_BUILD:
                    CHECK((out[i].bldKind >= 0) && (out[i].bldKind < SAI_BLD_KINDS));
                    break;
                case AI_INTENT_ATTACK_WAVE:
                    CHECK(out[i].squadSize > 0);
                    CHECK((out[i].targetPref >= 0) && (out[i].targetPref < SAI_TARGET_COUNT));
                    break;
                default: break;
            }
        }
    }
}

int main(void)
{
    TestProfilesDistinct();
    TestProfileSanity();

    TestCompositionDiffers();
    TestWorkersComeFirst();
    TestTrainingTerminates();
    TestPopCapBlocksTraining();

    TestBuildOrderFollowed();
    TestBuildRespectsCeiling();
    TestOneScaffoldAtATime();
    TestPopPressureBuildsHouse();

    TestNoAttackBeforeFirstAttackTime();
    TestAttacksOnceReady();
    TestWaveIntervalRespected();
    TestGarrisonHeldBack();
    TestCommitFractionOrdering();
    TestNoTargetNoAttack();
    TestTargetPrefsVary();
    TestAggressionOrdering();

    TestBrainClockAndPhase();
    TestBrainSurvivesLongFrame();

    TestEmptyCensusIsSafe();
    TestNullAndBoundsSafe();
    TestDecideIsPure();
    TestIntentsAreWellFormed();

    printf("ai_tests: %d checks, %d failed\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
