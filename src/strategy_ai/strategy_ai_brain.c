// ============================================================================
//  strategy_ai_brain.c  -  per-faction state and the decision function
//
//  PURE. AiBrainDecide reads its arguments and writes intents. It does not call
//  the clock, does not call rand, does not touch the world and does not know
//  what a Unit is. Same brain + same census => same intents, every time, which
//  is the whole reason the behaviour in here is testable at all.
//
//  The one piece of mutable state it does touch is the brain's own memory -
//  wave timings and build progress - which is exactly the thing the previous
//  AI lacked. Without it there is no difference between a first attack and a
//  tenth, so waves cannot be paced and nothing can be held in reserve.
// ============================================================================

#include "strategy_ai.h"

#include <stddef.h>    // NULL, and nothing else: this module stays dependency-free

// Fighters: what an attack wave is made of, and what armyCeiling counts.
// Workers are excluded even for SWARM, which trains them as filler - a faction
// that marched its entire economy off to die would stop being a faction.
static bool AiKindIsFighter(int kind)
{
    return (kind == SAI_SOLDIER) || (kind == SAI_RANGED);
}

// ---------------------------------------------------------------------------
//  Composition
//
//  Trains whichever kind is furthest BELOW its share of the archetype's mix.
//  Ratios, not counts: { soldier 3, ranged 1 } holds three-to-one at any army
//  size, so a row stays recognisable from its first wave to its last.
// ---------------------------------------------------------------------------
int AiPickUnitKind(const AiProfile *p, const AiCensus *c)
{
    // Workers first, always. An archetype starved of workers never reaches its
    // own build order, and every row's identity is downstream of its economy.
    if (c->unitCount[SAI_WORKER] < p->workerTarget) return SAI_WORKER;

    // Pop is the hard gate: training that cannot finish just parks resources in
    // a queue. The BUILD intent adds houses; this only declines to make it worse.
    if (c->popUsed >= c->popCap) return -1;

    int fighters = 0;
    for (int k = 0; k < SAI_UNIT_KINDS; k++)
        if (AiKindIsFighter(k)) fighters += c->unitCount[k];
    if (fighters >= p->armyCeiling) return -1;

    float total = 0.0f;
    for (int k = 0; k < SAI_UNIT_KINDS; k++) total += p->kindWeight[k];
    if (total <= 0.0f) return -1;       // a row that wants no army at all

    // How many of each kind the mix implies, against how many exist. The kind
    // with the largest shortfall wins. Measured over the units this row
    // actually wants, so a zero-weight kind never drags the denominator.
    int   have = 0;
    for (int k = 0; k < SAI_UNIT_KINDS; k++)
        if (p->kindWeight[k] > 0.0f) have += c->unitCount[k];

    int   best = -1;
    float bestGap = 0.0f;
    for (int k = 0; k < SAI_UNIT_KINDS; k++)
    {
        if (p->kindWeight[k] <= 0.0f) continue;
        float want = (p->kindWeight[k]/total)*(float)(have + 1);
        float gap  = want - (float)c->unitCount[k];
        if (gap > bestGap) { bestGap = gap; best = k; }
    }
    return best;
}

// ---------------------------------------------------------------------------
//  Build priorities
//
//  The opening order runs first, in sequence; after it is exhausted the row
//  tops up toward bldWant. Both are capped by bldWant, so an opening that lists
//  a kind more often than the ceiling allows cannot overshoot.
// ---------------------------------------------------------------------------
int AiPickBuilding(const AiProfile *p, const AiCensus *c, int buildStep)
{
    // Pop pressure overrides the plan. Being pop-capped stalls training
    // outright, and a faction that will not unblock itself is not playing.
    if ((c->popUsed + 1 > c->popCap) && (c->bldCount[SAI_BLD_HOUSE] < p->bldWant[SAI_BLD_HOUSE]))
        return SAI_BLD_HOUSE;

    // One scaffold at a time. Queuing several at once spreads the workers thin
    // and leaves the whole base half-built, which is worse than any order.
    if (c->scaffolds > 0) return -1;

    for (int s = buildStep; s < p->buildOrderCount; s++)
    {
        int kind = p->buildOrder[s];
        if ((kind < 0) || (kind >= SAI_BLD_KINDS)) continue;
        if (c->bldCount[kind] < p->bldWant[kind]) return kind;
    }

    for (int k = 0; k < SAI_BLD_KINDS; k++)
        if (c->bldCount[k] < p->bldWant[k]) return k;

    return -1;
}

// ---------------------------------------------------------------------------
//  Aggression
// ---------------------------------------------------------------------------
// Everything that gates a wave, in one place. The garrison is subtracted from
// what is available BEFORE the squad test, so a turtle holding twelve at home
// genuinely needs twenty-plus fighters to move, rather than emptying its base
// the moment it reaches the threshold.
static int AiWaveSize(const AiProfile *p, const AiCensus *c, const AiBrain *b)
{
    if (!c->hasEnemyTarget)                    return 0;
    if (c->elapsed < p->firstAttackTime)       return 0;
    if ((b->lastWaveAt >= 0.0f) &&
        (c->elapsed - b->lastWaveAt < p->waveInterval)) return 0;

    int available = c->idleFighters - p->garrison;
    if (available < p->attackSquad) return 0;

    // aggression widens the commitment rather than lowering the bar: the
    // threshold is what the archetype waits for, this is how much it then
    // sends. A raider clears its low bar often and still commits a third.
    float frac = p->commitFraction*(0.6f + 0.4f*p->aggression);
    int   size = (int)((float)available*frac);
    if (size < p->attackSquad) size = p->attackSquad;
    if (size > available)      size = available;
    return size;
}

// ---------------------------------------------------------------------------
//  Brain lifecycle
// ---------------------------------------------------------------------------
void AiBrainInit(AiBrain *b, int faction, float basePeriod)
{
    const AiProfile *p = AiProfileFor(faction);

    *b = (AiBrain){ 0 };
    b->faction    = faction;
    b->period     = basePeriod*p->thinkMul;
    if (b->period < 0.05f) b->period = 0.05f;   // never a per-frame brain
    b->lastWaveAt = -1.0f;                      // has never attacked
    b->buildStep  = 0;
    b->active     = true;

    // PHASE OFFSET. Without it eight brains initialised on the same frame stay
    // in lockstep forever and every AI census in the game lands on one frame in
    // sixty - a periodic spike that gets worse with each faction added. Spread
    // the first think across the period instead; they never realign.
    b->clock = b->period*((float)faction/(float)SAI_FACTIONS_MAX);
}

bool AiBrainTick(AiBrain *b, float dt)
{
    if (!b->active) return false;

    b->elapsed += dt;
    b->clock   -= dt;
    if (b->clock > 0.0f) return false;

    // Set, not accumulated: a brain that was starved by a long frame should
    // think once and move on, not owe several catch-up thinks it will run
    // back-to-back on the next frames.
    b->clock = b->period;
    return true;
}

// ---------------------------------------------------------------------------
//  The decision
// ---------------------------------------------------------------------------
int AiBrainDecide(AiBrain *b, const AiProfile *p, const AiCensus *c,
                  AiIntent *out, int outMax)
{
    if ((b == NULL) || (p == NULL) || (c == NULL) || (out == NULL) || (outMax <= 0))
        return 0;

    int n = 0;

    // 1. BUILD. First because being pop-capped or short a barracks blocks
    //    everything below it, and a tick spent unblocking is never wasted.
    if (n < outMax)
    {
        int want = AiPickBuilding(p, c, b->buildStep);
        if (want >= 0)
        {
            out[n++] = (AiIntent){ .kind = AI_INTENT_BUILD, .bldKind = want };

            // Advance past every opening step already satisfied, so a step that
            // was met some other way (an authored starting building, say) does
            // not stall the order behind it forever.
            while ((b->buildStep < p->buildOrderCount) &&
                   (c->bldCount[p->buildOrder[b->buildStep]] >=
                    p->bldWant[p->buildOrder[b->buildStep]]))
                b->buildStep++;
            if ((b->buildStep < p->buildOrderCount) &&
                (p->buildOrder[b->buildStep] == want))
                b->buildStep++;
        }
    }

    // 2. TRAIN. The composition rule, plus where that kind comes from.
    if (n < outMax)
    {
        int kind = AiPickUnitKind(p, c);
        if (kind >= 0)
        {
            int from = -1;
            switch (kind)
            {
                case SAI_WORKER:  from = SAI_BLD_TOWN_HALL; break;
                case SAI_SOLDIER:
                case SAI_RANGED:  from = SAI_BLD_BARRACKS;  break;
                case SAI_TEMPLAR:
                case SAI_HEALER:  from = SAI_BLD_CHANTRY;   break;
                default:          from = -1;                break;
            }
            // bldReady, not bldCount: a scaffold cannot train, and asking it to
            // would burn this tick's train intent on a building that will
            // refuse it.
            if ((from >= 0) && (c->bldReady[from] > 0))
                out[n++] = (AiIntent){ .kind = AI_INTENT_TRAIN,
                                       .unitKind = kind, .bldKind = from };
        }
    }

    // 3. ATTACK. Last: an archetype commits what it has after it has decided
    //    what to add, so a wave never leaves in place of a house it needed.
    if (n < outMax)
    {
        int size = AiWaveSize(p, c, b);
        if (size > 0)
        {
            out[n++] = (AiIntent){ .kind = AI_INTENT_ATTACK_WAVE,
                                   .squadSize = size, .targetPref = p->targetPref };
            b->lastWaveAt = c->elapsed;
            b->wavesSent++;
        }
    }

    return n;
}
