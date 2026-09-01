// ============================================================================
//  strategy_ai.c  -  the ADAPTER between the game and the faction brains
//
//  WHAT LIVES HERE. Measuring (census), translating (intent -> order) and the
//  geometry a decision needs but must not contain: where "near home" is, which
//  building is "nearest", which node is worth walking to. Nothing in this file
//  decides what a faction WANTS - that is src/strategy_ai/, which is headless
//  and tested. If a behaviour question can be answered without a Vector3, it
//  does not belong in this file.
//
//  It still issues orders through the same StrategyOrder* functions the mouse
//  uses, so every faction - human or not - shares identical mechanics. Combat
//  is not here either: auto-aggro in strategy_world.c already covers "attack
//  what you can see".
//
//  ONE BRAIN PER FACTION, on its own staggered clock. The previous version was
//  a single brain hardcoded to faction 1 in seven places, with the world's one
//  aiTimer for a clock - so a second AI faction was not a configuration change,
//  it was a rewrite. This is that rewrite.
// ============================================================================

#include "strategy_world.h"
#include "strategy_defs.h"
#include "../../strategy_ai/strategy_ai.h"
#include "raymath.h"
#include <math.h>

// The headless module mirrors the game's enums as plain ints so it can stay
// free of strategy_types.h. These are what keep the two in step: add a unit
// kind to the game and this file stops compiling, which is the entire point -
// silently giving every archetype a zero weight for the new kind would be a
// balance bug nobody would find for weeks.
_Static_assert(SAI_UNIT_KINDS == UNIT_KIND_COUNT, "AI unit kind count drifted from UnitKind");
_Static_assert(SAI_BLD_KINDS  == BLD_COUNT,       "AI building kind count drifted from BuildingKind");
_Static_assert(SAI_RES_KINDS  == RES_COUNT,       "AI resource count drifted from ResourceKind");
_Static_assert(SAI_FACTIONS_MAX == STRAT_FACTIONS, "AI faction ceiling drifted from STRAT_FACTIONS");

// Index agreement, not just count agreement: the profile table names kinds by
// number, so a reordered enum would silently turn every archetype into a
// different one.
_Static_assert(SAI_WORKER  == KIND_WORKER,  "SAI_WORKER  != KIND_WORKER");
_Static_assert(SAI_SOLDIER == KIND_SOLDIER, "SAI_SOLDIER != KIND_SOLDIER");
_Static_assert(SAI_RANGED  == KIND_RANGED,  "SAI_RANGED  != KIND_RANGED");
_Static_assert(SAI_TEMPLAR == KIND_TEMPLAR, "SAI_TEMPLAR != KIND_TEMPLAR");
_Static_assert(SAI_HEALER  == KIND_TEMPLAR_HEALER, "SAI_HEALER != KIND_TEMPLAR_HEALER");
_Static_assert(SAI_BLD_HOUSE     == BLD_HOUSE,     "SAI_BLD_HOUSE mismatch");
_Static_assert(SAI_BLD_BARRACKS  == BLD_BARRACKS,  "SAI_BLD_BARRACKS mismatch");
_Static_assert(SAI_BLD_TOWN_HALL == BLD_TOWN_HALL, "SAI_BLD_TOWN_HALL mismatch");
_Static_assert(SAI_BLD_CHANTRY   == BLD_CHANTRY,   "SAI_BLD_CHANTRY mismatch");
_Static_assert(SAI_BLD_FARM      == BLD_FARM,      "SAI_BLD_FARM mismatch");
_Static_assert(SAI_RES_WOOD == RES_WOOD && SAI_RES_FOOD == RES_FOOD, "resource index mismatch");

// One brain per faction. File statics rather than world fields: this is AI
// working memory, not world state, and it is reset explicitly on init.
static AiBrain s_brain[STRAT_FACTIONS];
static bool    s_brainsReady;

// Scratch for assembling an attack wave. Sized to the unit cap because a SWARM
// at full population may genuinely commit that many, and a stack array of this
// size is not safe on the Web build's 1 MB stack.
static int s_waveBuf[STRAT_MAX_UNITS];

static float AiDistXZ(Vector3 a, Vector3 b)
{
    float dx = a.x - b.x;
    float dz = a.z - b.z;
    return sqrtf(dx*dx + dz*dz);
}

// ---------------------------------------------------------------------------
//  Geometry the brain is not allowed to know about
// ---------------------------------------------------------------------------

// A faction's "home": its first standing building. The spawn order guarantees
// this is the town hall - SgmValidate enforces town-hall-first per faction and
// the map forge refuses to save a map that breaks it (see SpawnFromMap). That
// contract used to matter for one faction; it now matters for nine.
static Vector3 AiHome(const StrategyWorld *world, int faction)
{
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        const Building *b = &world->buildings[i];
        if (b->active && b->faction == faction) return b->pos;
    }
    return (Vector3){ 0.0f, 0.0f, 0.0f };
}

static int NearestUnitOfKind(const StrategyWorld *world, Vector3 pos,
                             int faction, UnitKind kind)
{
    int best = -1;
    float bestDist = 1000000.0f;
    int liveCount = 0;
    const int *live = StrategyActiveUnits(&liveCount);
    for (int k = 0; k < liveCount; k++)
    {
        int i = live[k];
        const Unit *u = &world->units[i];
        if (u->faction != faction || u->kind != kind) continue;

        float d = AiDistXZ(u->pos, pos);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

// How far along a faction is, for SAI_TARGET_WEAKEST. Buildings dominate on
// purpose: an army can be rebuilt in a minute, a base cannot, so "weakest"
// should mean "closest to elimination" rather than "lost a skirmish".
static int FactionStrength(const StrategyWorld *world, int faction)
{
    int score = 0;
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        const Building *b = &world->buildings[i];
        if (b->active && b->faction == faction) score += 10;
    }
    int liveCount = 0;
    const int *live = StrategyActiveUnits(&liveCount);
    for (int k = 0; k < liveCount; k++)
        if (world->units[live[k]].faction == faction) score += 1;
    return score;
}

// Pick the building a wave marches on, honouring the archetype's preference.
// Returns a buildings[] index, or -1 when nothing is worth attacking.
static int PickTargetBuilding(const StrategyWorld *world, int faction,
                              AiTargetPref pref, Vector3 from)
{
    // WEAKEST resolves to a victim faction first, then to its nearest building.
    int victim = -1;
    if (pref == SAI_TARGET_WEAKEST)
    {
        int worst = 1000000;
        for (int f = 0; f < world->factionCount; f++)
        {
            if (f == faction || world->defeated[f]) continue;
            int s = FactionStrength(world, f);
            if (s > 0 && s < worst) { worst = s; victim = f; }
        }
    }
    else if (pref == SAI_TARGET_PLAYER)
    {
        victim = 0;
    }

    int best = -1;
    float bestDist = 1000000.0f;
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        const Building *b = &world->buildings[i];
        if (!b->active || b->faction == faction) continue;
        if (b->faction == FACTION_NEUTRAL) continue;
        if (victim >= 0 && b->faction != victim) continue;

        float d = AiDistXZ(b->pos, from);
        if (d < bestDist) { bestDist = d; best = i; }
    }

    // Preferred victim already has nothing standing: fall back to anyone, so a
    // faction never idles at full army because its favourite target is gone.
    if (best < 0 && victim >= 0)
        return PickTargetBuilding(world, faction, SAI_TARGET_NEAREST, from);

    return best;
}

// The RAIDER's target: an enemy worker, which is a UNIT order rather than a
// building one. Falls back to a building when there is no worker in reach.
static int PickTargetWorker(const StrategyWorld *world, int faction, Vector3 from)
{
    int best = -1;
    float bestDist = 1000000.0f;
    int liveCount = 0;
    const int *live = StrategyActiveUnits(&liveCount);
    for (int k = 0; k < liveCount; k++)
    {
        int i = live[k];
        const Unit *u = &world->units[i];
        if (!u->active || u->kind != KIND_WORKER) continue;
        if (u->faction == faction || u->faction == FACTION_NEUTRAL) continue;

        float d = AiDistXZ(u->pos, from);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

// ---------------------------------------------------------------------------
//  Census: everything the brain is allowed to know, measured once per think
// ---------------------------------------------------------------------------
static void CensusGather(const StrategyWorld *world, int faction,
                         const AiBrain *brain, AiCensus *c)
{
    *c = (AiCensus){ 0 };

    int liveCount = 0;
    const int *live = StrategyActiveUnits(&liveCount);
    for (int k = 0; k < liveCount; k++)
    {
        const Unit *u = &world->units[live[k]];
        if (u->faction != faction) continue;

        c->unitCount[u->kind]++;
        if (u->state != UNIT_IDLE) continue;

        if (u->kind == KIND_WORKER)                              c->idleWorkers++;
        else if (u->kind == KIND_SOLDIER || u->kind == KIND_RANGED) c->idleFighters++;
    }

    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        const Building *b = &world->buildings[i];
        if (!b->active) continue;
        if (b->faction == faction)
        {
            c->bldCount[b->kind]++;
            if (b->underConstruction) c->scaffolds++;
            else                      c->bldReady[b->kind]++;
        }
        else if (b->faction != FACTION_NEUTRAL)
        {
            c->hasEnemyTarget = true;
        }
    }

    for (int r = 0; r < RES_COUNT; r++) c->stock[r] = world->stockpile[faction][r];

    c->popUsed = StrategyPopUsed(faction);
    c->popCap  = StrategyPopCap(faction);
    c->elapsed = brain->elapsed;
}

// ---------------------------------------------------------------------------
//  Intent -> orders
// ---------------------------------------------------------------------------

// Train: find an idle building of the right kind. StrategyTrainStart still
// validates cost, pop and cooldown, so a refusal here costs nothing and simply
// means the faction tries again next tick.
static void ApplyTrain(StrategyWorld *world, int faction, const AiIntent *in)
{
    for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
    {
        Building *b = &world->buildings[i];
        if (!b->active || b->faction != faction) continue;
        if (b->underConstruction || b->kind != in->bldKind) continue;
        if (b->trainKind >= 0) continue;        // already busy

        if (StrategyTrainStart(i, (UnitKind)in->unitKind)) return;
    }
}

// Build: scatter a few candidate spots around home and let StrategyTryBuild
// reject the blocked and unaffordable ones. The archetype's expandRadius sets
// how far out it is willing to sprawl, which is what makes a turtle's base
// compact and an industrialist's spread.
static void ApplyBuild(StrategyWorld *world, int faction,
                       const AiProfile *p, const AiIntent *in)
{
    Vector3 home = AiHome(world, faction);
    int spread = (int)(p->expandRadius*0.25f*100.0f);
    if (spread < 300) spread = 300;

    for (int attempt = 0; attempt < 6; attempt++)
    {
        Vector3 pos = home;
        pos.x += (float)GetRandomValue(-spread, spread)*0.01f;
        pos.z += (float)GetRandomValue(-spread, spread)*0.01f;
        pos.x  = roundf(pos.x);
        pos.z  = roundf(pos.z);
        if (StrategyTryBuild(faction, (BuildingKind)in->bldKind, pos)) return;
    }
}

// Attack: gather idle fighters nearest the target and send them AS A GROUP.
//
// The group order is the point. The old AI issued a separate attack order per
// unit, so a "wave" left home as a loose trickle and arrived piecemeal - each
// unit pathing alone and dying alone. Routing through StrategyOrderMoveGroup
// gives AI armies the same formation march the player's get.
static void ApplyAttack(StrategyWorld *world, int faction,
                        const AiProfile *p, const AiIntent *in)
{
    Vector3 home = AiHome(world, faction);

    int targetBld  = -1;
    int targetUnit = -1;
    Vector3 dest;

    if (in->targetPref == SAI_TARGET_WORKERS)
    {
        targetUnit = PickTargetWorker(world, faction, home);
        if (targetUnit >= 0) dest = world->units[targetUnit].pos;
    }
    if (targetUnit < 0)
    {
        targetBld = PickTargetBuilding(world, faction, in->targetPref, home);
        if (targetBld < 0) return;
        dest = world->buildings[targetBld].pos;
    }

    // Collect the idle fighters closest to the objective, capped at the squad
    // size the brain asked for. Nearest-first so the garrison that stays behind
    // is the part of the army already sitting at the back of the base.
    int n = 0;
    int liveCount = 0;
    const int *live = StrategyActiveUnits(&liveCount);
    for (int k = 0; k < liveCount && n < STRAT_MAX_UNITS; k++)
    {
        int i = live[k];
        const Unit *u = &world->units[i];
        if (u->faction != faction || u->state != UNIT_IDLE) continue;
        if (u->kind != KIND_SOLDIER && u->kind != KIND_RANGED) continue;
        s_waveBuf[n++] = i;
    }
    if (n <= 0) return;

    int size = in->squadSize;
    if (size > n) size = n;

    // Insertion-sort the first `size` slots by distance to the objective. A
    // partial selection, not a full sort: the tail is the garrison and its
    // order does not matter.
    for (int a = 0; a < size; a++)
    {
        int   bestAt = a;
        float bestD  = AiDistXZ(world->units[s_waveBuf[a]].pos, dest);
        for (int b = a + 1; b < n; b++)
        {
            float d = AiDistXZ(world->units[s_waveBuf[b]].pos, dest);
            if (d < bestD) { bestD = d; bestAt = b; }
        }
        int tmp = s_waveBuf[a]; s_waveBuf[a] = s_waveBuf[bestAt]; s_waveBuf[bestAt] = tmp;
    }

    // March as a formation, then hand each unit its actual target. The move
    // order sets up the group; the attack order is what makes them commit on
    // arrival, and auto-aggro handles anything they meet on the way.
    StrategyOrderMoveGroup(s_waveBuf, size, dest);
    for (int k = 0; k < size; k++)
    {
        Unit *u = &world->units[s_waveBuf[k]];
        if (targetUnit >= 0) StrategyOrderAttack(u, targetUnit);
        else                 StrategyOrderAttackBuilding(u, targetBld);
    }
    (void)p;
}

// ---------------------------------------------------------------------------
//  Workers: the economy loop, per faction
//
//  Not a brain decision - it is the same rule for everyone (finish what is
//  half-built, then feed yourself, then gather whatever is nearest), varying
//  only by how far the archetype ranges. Encoding that as intents would mean
//  one per idle worker per tick for no behavioural gain.
// ---------------------------------------------------------------------------
static void WorkersTick(StrategyWorld *world, int faction, const AiProfile *p)
{
    Vector3 home  = AiHome(world, faction);
    float   range = p->expandRadius;

    int liveCount = 0;
    const int *live = StrategyActiveUnits(&liveCount);
    for (int k = 0; k < liveCount; k++)
    {
        Unit *u = &world->units[live[k]];
        if (u->faction != faction) continue;
        if (u->kind != KIND_WORKER || u->state != UNIT_IDLE) continue;

        // Unfinished buildings first. AI buildings used to spawn complete - a
        // free head start that was tolerable against one enemy and is not
        // against eight - so now every faction raises its own scaffolds, and a
        // faction that would not build stalls at its starting pop cap forever.
        {
            int scaffold = -1;
            for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
            {
                const Building *b = &world->buildings[i];
                if (b->active && b->faction == faction && b->underConstruction)
                {
                    scaffold = i;
                    break;
                }
            }
            if (scaffold >= 0) { StrategyOrderBuild(u, scaffold); continue; }
        }

        // Food-poor: corpses and wheat first, then hunting. Food gates every
        // unit in the game, so a starving faction stops existing.
        if (world->stockpile[faction][RES_FOOD] < 6)
        {
            int corpse = StrategyNearestNodeOfKind(u->pos, NODE_CORPSE, range);
            if (corpse >= 0) { StrategyOrderGather(u, corpse); continue; }

            int wheat = StrategyNearestNodeOfKind(u->pos, NODE_WHEAT, range);
            if (wheat >= 0) { StrategyOrderGather(u, wheat); continue; }

            if (GetRandomValue(0, 99) < 40)
            {
                int animal = NearestUnitOfKind(world, u->pos, FACTION_NEUTRAL,
                                               KIND_ANIMAL_WEAK);
                if (animal >= 0) { StrategyOrderAttack(u, animal); continue; }
            }
        }

        int node = StrategyNearestNodeOfKind(u->pos, -1, range);
        if (node >= 0)
        {
            StrategyOrderGather(u, node);
        }
        else
        {
            Vector3 dest = home;
            dest.x += (float)GetRandomValue(-600, 600)*0.01f;
            dest.z += (float)GetRandomValue(-600, 600)*0.01f;
            StrategyOrderMove(u, dest);
        }
    }
}

// Idle animals amble to a random nearby spot now and then. Neutral only, and
// unchanged - it is not a faction behaviour and has no brain.
static void AnimalsTick(StrategyWorld *world)
{
    int liveCount = 0;
    const int *live = StrategyActiveUnits(&liveCount);
    for (int k = 0; k < liveCount; k++)
    {
        Unit *u = &world->units[live[k]];
        if (u->faction != FACTION_NEUTRAL) continue;
        if (u->state != UNIT_IDLE || GetRandomValue(0, 99) >= 30) continue;

        Vector3 dest = u->pos;
        dest.x += (float)GetRandomValue(-400, 400)*0.01f;
        dest.z += (float)GetRandomValue(-400, 400)*0.01f;
        // The world's authored extent, not the fixed STRAT_GROUND_HALF: on a
        // large authored map that constant would pen the animals into the
        // middle of the field.
        dest.x = Clamp(dest.x, -world->groundHalfX + 1.0f, world->groundHalfX - 1.0f);
        dest.z = Clamp(dest.z, -world->groundHalfZ + 1.0f, world->groundHalfZ - 1.0f);
        StrategyOrderMove(u, dest);
    }
}

// ---------------------------------------------------------------------------
//  Entry points
// ---------------------------------------------------------------------------
void StrategyAiReset(void)
{
    const StrategyWorld *world = StrategyWorldGet();
    for (int f = 0; f < STRAT_FACTIONS; f++)
    {
        AiBrainInit(&s_brain[f], f, world->aiPeriod > 0.0f ? world->aiPeriod
                                                           : STRAT_AI_PERIOD);
        // Faction 0 is the human and 0's profile is inert, but disarming the
        // brain outright is what guarantees the player is never issued orders.
        if (f == 0) s_brain[f].active = false;
    }
    s_brainsReady = true;
}

const char *StrategyAiArchetype(int faction)
{
    return AiProfileName(faction);
}

// Called every frame from StrategyWorldUpdate. The per-faction clocks live in
// the brains, so this is no longer gated by the world's single aiTimer - each
// faction thinks on its own staggered schedule and eight of them never land on
// the same frame.
void StrategyAiTick(float dt)
{
    StrategyWorld *world = StrategyWorldGet();
    if (!s_brainsReady) StrategyAiReset();

    AnimalsTick(world);

    for (int f = 1; f < world->factionCount; f++)
    {
        if (world->defeated[f]) continue;
        if (!AiBrainTick(&s_brain[f], dt)) continue;

        const AiProfile *p = AiProfileFor(f);

        AiCensus c;
        CensusGather(world, f, &s_brain[f], &c);

        AiIntent intents[SAI_INTENT_MAX];
        int n = AiBrainDecide(&s_brain[f], p, &c, intents, SAI_INTENT_MAX);
        for (int i = 0; i < n; i++)
        {
            switch (intents[i].kind)
            {
                case AI_INTENT_TRAIN:       ApplyTrain(world, f, &intents[i]);      break;
                case AI_INTENT_BUILD:       ApplyBuild(world, f, p, &intents[i]);   break;
                case AI_INTENT_ATTACK_WAVE: ApplyAttack(world, f, p, &intents[i]);  break;
                default: break;
            }
        }

        WorkersTick(world, f, p);
    }
}
