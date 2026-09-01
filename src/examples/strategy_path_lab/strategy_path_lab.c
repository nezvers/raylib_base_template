// ============================================================================
//  strategy_path_lab.c  -  movement / pathfinding stress harness
//
//  WHY A SEPARATE STATE. strategy_test.c already carries the GAME_START intro,
//  a two-page pause menu, the build bar and a three-way ESC priority. Grafting
//  a dozen debug toggles and a profiler into it would double its complexity and
//  every key added would risk colliding with a gameplay binding. This is a
//  different SHELL around the same world - exactly what strategy_showcase is
//  for assets - so it binds whatever keys it likes, and it can be deleted
//  without touching a line of the game.
//
//  WHAT IT IS FOR. Measuring the MOVER. The authored render path saturates the
//  batcher around a thousand units, long before the simulation is in trouble,
//  so every measurement taken at AUTHORED is really a measurement of DrawUnit.
//  The L key exists to get the renderer out of the way; use it before believing
//  any number on this screen.
//
//  WHAT IT ALSO COMPARES. Three movement systems share this world, and K cycles
//  between them: LEGACY (the pre-overhaul lerp, no separation or formations),
//  SIMPLE (legacy plus index-order slots and a line-of-sight dodge) and CURRENT
//  (formations, A*, flow fields). Measuring them against each other is only
//  meaningful because it is the SAME build, the same map and the same profiler -
//  which is exactly what a key press between two runs gives you and two branches
//  do not.
//
//  Keys:  N spawn   M two armies   C clear   [ ] count   1-9 faction
//         K control scheme   L render LOD
//         T profiler   SPACE pause   . step   ESC back
// ============================================================================

#include "../../app_state/app_state.h"
#include "../../screen_state/screen_state.h"
#include "../../strategy_path/strategy_path_prof.h"
#include "../strategy_test/strategy_world.h"
#include "../strategy_test/strategy_models.h"   // StrategyFactionTint
#include <math.h>

#include "raygui.h"

static void Enter();
static void Update();
static void Exit();
static void Draw();
static void Gui();
                                  /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_state_strategy_path_lab = {Enter, Exit, Update, Draw, Gui, "PathLab"};

// The state to return to on ESC. Set by the opener before the transition, the
// same open-then-transition convention the forges use (StrategyForgeSetReturn).
static AppState *s_return = NULL;

// -- Spawn count ladder -------------------------------------------------------
// Decades rather than a free slider: stress results are only comparable if the
// counts are, and "about two thousand" is not a number you can repeat.
static const int SPAWN_STEPS[] = { 100, 250, 500, 1000, 2000, 5000, 10000 };
#define SPAWN_STEP_COUNT ((int)(sizeof(SPAWN_STEPS)/sizeof(SPAWN_STEPS[0])))
static int s_spawnStep = 3;     // 1000

// -- Spawn faction ------------------------------------------------------------
// Which faction N spawns for. Held so repeated presses can build an arbitrary
// multi-faction population, which the old fixed "player" / "player vs enemy"
// pair could not express.
//
// UNCLAMPED. This used to stop at STRAT_FACTIONS and grey out the rest, because
// the runtime played two factions and strategyFactionColor[] had two entries -
// a faction 2 unit was an out-of-bounds read on the first frame it was drawn.
// The runtime now plays up to nine and the palette has nine rows, so every slot
// the selector offers is real. Keys 1..9 map to factions 0..8.
#define LAB_FACTION_SLOTS STRAT_FACTIONS
static int s_faction = 0;

static bool  s_paused    = false;
static bool  s_stepOnce  = false;
static int   s_profView  = 1;   // 0 = off, 1 = summary, 2 = full
static float s_lastSpawnMs = 0.0f;
static int   s_lastSpawned = 0;

void StrategyPathLabSetReturn(AppState *state) { s_return = state; }

static void Enter()
{
    // The world is whatever map the opener selected. Built-in layout is fine
    // too - it just has no obstacles to path around.
    StrategyWorldInit();
    StrategyRenderLodSet(STRAT_LOD_AUTHORED);
    // Start every visit on the shipped system, so a run begins from a known
    // arm rather than from whatever the previous visit was left cycled to.
    StrategyControlSet(STRAT_CTRL_CURRENT);
    spProfEnabled = true;
    s_paused   = false;
    s_stepOnce = false;
}

static void Exit()
{
    // Leave nothing behind: the game shares this world and this renderer.
    StrategyRenderLodSet(STRAT_LOD_AUTHORED);
    // ...and this MOVER. A scheme left on LEGACY would silently degrade normal
    // play - units piling onto a click and never settling - and it would read
    // as a regression in the movement code rather than a toggle nobody reset.
    StrategyControlSet(STRAT_CTRL_CURRENT);
    spProfEnabled = false;
}

// Spawn at the camera focus, timed. The spawn cost itself is worth seeing -
// UnitSpawn does a linear scan for a free slot, which is O(n) per unit and
// therefore O(n^2) for a bulk spawn. That shows up here as a visible hitch.
static void SpawnBurst(void)
{
    StrategyWorld *w = StrategyWorldGet();
    Vector3 c = (Vector3){ w->camFocus.x, 0.0f, w->camFocus.y };
    float radius = sqrtf((float)SPAWN_STEPS[s_spawnStep])*0.45f;

    double t0 = GetTime();
    s_lastSpawned = StrategyDebugSpawnUnits(s_faction, KIND_SOLDIER, c, radius,
                                            SPAWN_STEPS[s_spawnStep]);
    s_lastSpawnMs = (float)((GetTime() - t0)*1000.0);
}

// Two opposing armies at opposite corners. This is the case that matters:
// a head-on collision is where separation, aggro scanning and repathing all
// peak at the same instant.
static void SpawnArmies(void)
{
    StrategyWorld *w = StrategyWorldGet();
    int half = SPAWN_STEPS[s_spawnStep]/2;
    float radius = sqrtf((float)half)*0.45f;
    float ox = w->groundHalfX*0.6f;
    float oz = w->groundHalfZ*0.6f;

    double t0 = GetTime();
    s_lastSpawned  = StrategyDebugSpawnUnits(0, KIND_SOLDIER,
                        (Vector3){ -ox, 0.0f, -oz }, radius, half);
    s_lastSpawned += StrategyDebugSpawnUnits(1, KIND_SOLDIER,
                        (Vector3){  ox, 0.0f,  oz }, radius, half);
    s_lastSpawnMs = (float)((GetTime() - t0)*1000.0);
}

static void Update()
{
    if (IsKeyPressed(KEY_ESCAPE))
    {
        AppStateTransition(s_return ? s_return : &app_state_main_menu);
        return;
    }

    // -- Toggles --------------------------------------------------------------
    if (IsKeyPressed(KEY_L))
    {
        StrategyRenderLod next = (StrategyRenderLod)((StrategyRenderLodGet() + 1) % STRAT_LOD_COUNT);
        StrategyRenderLodSet(next);
    }
    // THE A/B SWITCH. Cycles which movement system drives a walk-to-a-point,
    // over the same world, the same input path and the same profiler - which is
    // the only way the three sets of numbers are comparable at all.
    //
    // ORDERS ALREADY GIVEN ARE NOT RE-ISSUED. Units mid-march keep the
    // destination they were given; the new scheme takes effect from the next
    // order. So measure by cycling FIRST and ordering SECOND - flipping the key
    // mid-march tells you nothing, because the interesting half of each system
    // is what it does when the order is issued.
    if (IsKeyPressed(KEY_K))
    {
        StrategyControlScheme next =
            (StrategyControlScheme)((StrategyControlGet() + 1) % STRAT_CTRL_COUNT);
        StrategyControlSet(next);
    }
    if (IsKeyPressed(KEY_T))     s_profView = (s_profView + 1) % 3;
    if (IsKeyPressed(KEY_G))     StrategyDebugNavShow(!StrategyDebugNavShown());
    if (IsKeyPressed(KEY_P))     StrategyDebugPathShow(!StrategyDebugPathShown());
    // J, not F: F cycles the formation shape in StrategyWorldHandleInput, which
    // the lab calls, so binding the flow overlay there too would fire both.
    if (IsKeyPressed(KEY_J))     StrategyDebugFlowShow(!StrategyDebugFlowShown());
    if (IsKeyPressed(KEY_O))     StrategyDebugSlotShow(!StrategyDebugSlotShown());

    // Flow threshold, doubling and halving for the same reason the budget does:
    // the A/B this exists for is "one field for 2,000 units" against "2,000
    // individual searches", and crossing a group size of thousands one unit at
    // a time is not a control.
    //
    // THIS IS THE MOST INFORMATIVE KNOB IN THE LAB. Raise it past the size of
    // the selected group and the same order that cost one field build becomes
    // one A* search per unit - watch the queue pin and astar climb off zero.
    if (IsKeyPressed(KEY_COMMA))
    {
        int t = StrategyMoveFlowThreshold()/2;
        StrategyMoveFlowThresholdSet((t < 1) ? 1 : t);
    }
    if (IsKeyPressed(KEY_SLASH))
    {
        int t = StrategyMoveFlowThreshold()*2;
        StrategyMoveFlowThresholdSet((t > 16384) ? 16384 : t);
    }

    // A* budget, halving and doubling rather than stepping: the interesting
    // range spans three orders of magnitude (100 nodes makes paths visibly
    // late, 8000 is the default, 64000 never binds), and a linear step would
    // take fifty presses to cross it.
    if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD))
    {
        int b = StrategyDebugPathBudget()*2;
        StrategyDebugPathBudgetSet((b > 64000) ? 64000 : b);
    }
    if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT))
    {
        int b = StrategyDebugPathBudget()/2;
        StrategyDebugPathBudgetSet((b < 50) ? 50 : b);
    }
#ifndef NDEBUG
    // Roster audit. Costs a full pool scan per frame, so it is deliberately
    // manual: turn it on when units go missing or update twice, not by default.
    if (IsKeyPressed(KEY_R))
    {
        strategyRosterAudit = !strategyRosterAudit;
        TraceLog(LOG_INFO, "PATHLAB: roster audit %s",
                 strategyRosterAudit ? "ON" : "off");
    }
#endif
    if (IsKeyPressed(KEY_SPACE)) s_paused = !s_paused;
    if (IsKeyPressed(KEY_PERIOD)) s_stepOnce = true;

    // -- Population -----------------------------------------------------------
    if (IsKeyPressed(KEY_LEFT_BRACKET)  && s_spawnStep > 0) s_spawnStep--;
    if (IsKeyPressed(KEY_RIGHT_BRACKET) && s_spawnStep < SPAWN_STEP_COUNT - 1) s_spawnStep++;
    // Faction select, keys 1..9.
    for (int f = 0; f < LAB_FACTION_SLOTS; f++)
    {
        if (IsKeyPressed(KEY_ONE + f)) s_faction = f;
    }

    if (IsKeyPressed(KEY_N)) SpawnBurst();
    if (IsKeyPressed(KEY_M)) SpawnArmies();
    if (IsKeyPressed(KEY_C)) StrategyDebugClearUnits();

    // Camera, selection and orders all come from the game unchanged - the whole
    // point is to stress the real input path, not a stand-in.
    StrategyWorldHandleInput();

    if (!s_paused || s_stepOnce)
    {
        // While single-stepping, feed a fixed dt so a paused frame that took
        // 300 ms to render does not teleport everything on resume.
        StrategyWorldUpdate(s_stepOnce ? (1.0f/60.0f) : GetFrameTime());
        s_stepOnce = false;
    }

    SpProfFrame();
}

static void Draw()
{
    ClearBackground((Color){ 24, 26, 30, 255 });
    StrategyWorldDraw3D();
    StrategyWorldDraw2DOverlay();
}

// The overlay follows the main_menu.c debug readout: bottom-left, stacked
// upward, sized as a fraction of the canvas so it survives any resolution.
static void DrawProfiler(Vector2 gameSize)
{
    if (s_profView == 0) return;

    int   fs   = (int)fmaxf(10.0f, gameSize.y*0.020f);
    int   line = fs + (int)fmaxf(1.0f, gameSize.y*0.004f);
    float x    = gameSize.x*0.015f;
    float y    = gameSize.y*0.030f;

    Color dim  = (Color){ 150, 155, 165, 255 };
    Color hot  = (Color){ 255, 210, 120, 255 };
    Color good = (Color){ 140, 220, 150, 255 };

    float frameMs = GetFrameTime()*1000.0f;
    Color frameCol = (frameMs > 16.7f) ? hot : good;

    DrawText(TextFormat("units %d / %d      spawn %d in %.1f ms",
                        SpProfGet(SP_COUNT_UNITS_ACTIVE), STRAT_MAX_UNITS,
                        s_lastSpawned, s_lastSpawnMs),
             (int)x, (int)y, fs, RAYWHITE);
    y += line;

    DrawText(TextFormat("frame %.2f ms  (%d fps)   measured %.2f ms",
                        frameMs, GetFPS(), SpProfTotalMs()),
             (int)x, (int)y, fs, frameCol);
    y += line;

    // Scheme sits on the render row because both answer the same question -
    // how is this frame being produced - and because reading a measurement off
    // this HUD without knowing which arm produced it is worse than useless.
    // Highlighted whenever it is NOT the shipped system, so a number recorded
    // under a debug arm cannot be mistaken for a number about the game.
    StrategyControlScheme scheme = StrategyControlGet();
    DrawText(TextFormat("render %s      drawn %d      control %s",
                        StrategyRenderLodName(StrategyRenderLodGet()),
                        SpProfGet(SP_COUNT_DRAWN_UNITS),
                        StrategyControlName(scheme)),
             (int)x, (int)y, fs,
             (scheme == STRAT_CTRL_CURRENT) ? dim : hot);
    y += line;

    // The clustering acceptance test. Order a pile onto one point: `moving`
    // should fall to zero and STAY there. If it sits pinned at the group size,
    // the crowd is still orbiting - which is exactly what it did before the
    // arrival rewrite, and is invisible from a framerate number.
    int moving = SpProfGet(SP_COUNT_UNITS_MOVING);
    DrawText(TextFormat("moving %d   settling %d",
                        moving, SpProfGet(SP_COUNT_UNITS_SETTLED)),
             (int)x, (int)y, fs, (moving > 0) ? hot : good);
    y += line;

    // Nonzero means the spatial hash ran out of room and separation now has
    // holes in it - units at the back of a crowd walking through each other.
    // It is a capacity bug, not a tuning one, so it is called out loudly.
    int dropped = SpProfGet(SP_COUNT_HASH_DROPPED);
    if (dropped > 0)
    {
        DrawText(TextFormat("HASH OVERFLOW  %d units unhashed", dropped),
                 (int)x, (int)y, fs, (Color){ 255, 110, 110, 255 });
        y += line;
    }

    // Nav grid: the numbers behind the G overlay. Placing a building should
    // move `blocked` by its footprint and `skirt` by its perimeter, live - a
    // stamp that does not show up here never reached the grid.
    if (StrategyDebugNavShown())
    {
        int blocked = 0, skirt = 0;
        StrategyDebugNavStats(&blocked, &skirt);
        DrawText(TextFormat("nav  blocked %d   skirt %d", blocked, skirt),
                 (int)x, (int)y, fs, dim);
        y += line;
    }

    // Path service. Shown whether or not the P lines are on, because the
    // numbers answer a different question than the lines do: `pending` climbing
    // and staying up means the budget is binding, and `failed` spiking explains
    // a slowdown that a millisecond figure never would.
    //
    // NOT EXPECTED TO BE ZERO UNDER LEGACY / SIMPLE. The control scheme forks
    // only UNIT_MOVE - a walk to a clicked point. Workers heading for a tree or
    // a scaffold path under every scheme, deliberately: gathering is not what
    // these arms disagree about. The maps seed a handful of workers per faction,
    // so expect a small standing figure here even on LEGACY. It is the SPAWNED
    // soldiers, which never enter those states, that the A/B actually measures.
    {
        int queued = 0, active = 0, pending = 0, nodes = 0;
        StrategyDebugPathStats(&queued, &active, &pending, &nodes);
        int failed = SpProfGet(SP_COUNT_PATH_FAILED);
        DrawText(TextFormat("path  walking %d  waiting %d  queue %d  nodes %d/%d%s",
                            active, pending, queued, nodes,
                            StrategyDebugPathBudget(),
                            failed ? TextFormat("  FAILED %d", failed) : ""),
                 (int)x, (int)y, fs,
                 (failed > 0) ? (Color){ 255, 150, 110, 255 } : dim);
        y += line;
    }

    // Flow fields. `fields` is what the refcount sweep is judged by: leave a
    // long session running and it must come back DOWN as groups arrive. Pinned
    // at the cap means a missed release, and the symptom would otherwise be
    // silent - everything quietly degrading to individual A*.
    {
        int live = StrategyMoveFlowLive();
        bool pinned = (live >= SP_FLOW_FIELDS_MAX);
        DrawText(TextFormat("flow  fields %d/%d   threshold %d   hit %d  miss %d",
                            live, SP_FLOW_FIELDS_MAX,
                            StrategyMoveFlowThreshold(),
                            SpProfGet(SP_COUNT_FLOW_HIT),
                            SpProfGet(SP_COUNT_FLOW_MISS)),
                 (int)x, (int)y, fs, pinned ? hot : dim);
        y += line;
    }

    if (s_profView < 2) return;

    for (int i = 0; i < SP_PROF_COUNT; i++)
    {
        float ms = SpProfMs((SpProfSlot)i);
        if (ms < 0.001f) continue;      // never measured / not wired yet
        DrawText(TextFormat("  %-10s %6.2f ms", SpProfName((SpProfSlot)i), ms),
                 (int)x, (int)y, fs, (ms > 4.0f) ? hot : dim);
        y += line;
    }
}

static void Gui()
{
    Vector2 gameSize = ScreenStateTargetSize();

    DrawProfiler(gameSize);

    // -- Key legend, bottom-left ---------------------------------------------
    int fs   = (int)fmaxf(9.0f, gameSize.y*0.018f);
    int line = fs + (int)fmaxf(1.0f, gameSize.y*0.003f);
    float x  = gameSize.x*0.015f;

    const char *keys[] = {
        "N spawn    M two armies    C clear",
        "K control scheme    [ ] count    1-9 faction    L render LOD",
        "G nav grid    P paths    J flow    O slots",
        "- + astar budget    , / flow threshold",
        "F formation shape    V formation behaviour",
        "T profiler    SPACE pause    . step    ESC back",
    };
    int n = (int)(sizeof(keys)/sizeof(keys[0]));
    float y = gameSize.y - (float)(line*n) - gameSize.y*0.02f;
    for (int i = 0; i < n; i++)
    {
        DrawText(keys[i], (int)x, (int)(y + line*i), fs, (Color){ 120, 125, 135, 255 });
    }

    // -- Spawn count + pause state, top-right --------------------------------
    const char *label = TextFormat("SPAWN %d%s", SPAWN_STEPS[s_spawnStep],
                                   s_paused ? "    [PAUSED]" : "");
    int lw = MeasureText(label, fs);
    float rx = gameSize.x - gameSize.x*0.015f;
    DrawText(label, (int)(rx - lw), (int)(gameSize.y*0.030f), fs,
             s_paused ? (Color){ 255, 200, 120, 255 } : (Color){ 150, 155, 165, 255 });

    // -- Faction chips, right-aligned under the spawn count ------------------
    // Drawn right-to-left so the row stays anchored to the edge. Slots the
    // runtime cannot play are dimmed rather than hidden: an absent key is
    // indistinguishable from a broken one.
    float chip = (float)fs*1.6f;
    float cy   = gameSize.y*0.030f + (float)line;
    for (int f = LAB_FACTION_SLOTS - 1, slot = 0; f >= 0; f--, slot++)
    {
        float cx = rx - chip*(float)(slot + 1);
        Color col = StrategyFactionTint(f);

        if (f == s_faction) DrawRectangle((int)cx, (int)cy, (int)chip - 3, fs + 4, col);
        else                DrawRectangleLines((int)cx, (int)cy, (int)chip - 3, fs + 4, col);

        const char *num = TextFormat("%d", f + 1);
        DrawText(num, (int)(cx + (chip - 3 - (float)MeasureText(num, fs))*0.5f),
                 (int)(cy + 2), fs,
                 (f == s_faction) ? BLACK : col);
    }
}
