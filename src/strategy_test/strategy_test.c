// ============================================================================
//  strategy_test.c  -  RTS test app state (see strategy_world.c for the sim)
//
//  This file is deliberately the same shape as platformer_test.c: intro text
//  via SceneAnimPlayer, an in-state ESC pause menu with a dim layer and the
//  MAIN/OPTIONS pages, and a GP_UNFADE_BLACK entry fade drawn last.
//  The only strategy-specific extras are the build bar in Gui() and the
//  three-way ESC priority (cancel placement > options back > pause toggle).
// ============================================================================

#include "../app_state/app_state.h"
#include "../screen_state/screen_state.h"
#include "../settings_state/settings_state.h"
#include "../audio_state/audio_state.h"
#include "../scene_anim/scene_anim.h"
#include "strategy_world.h"
#include <math.h>

// raygui implementation is compiled ONCE in main_menu.c - plain include here.
#include "raygui.h"

// Forward declare functions
static void Enter();
static void Update();
static void Exit();
static void Draw();
static void Gui();
                             /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_state_strategy = {Enter, Exit, Update, Draw, Gui, "Strategy"};

// ============================================================================
//  INTRO: "STRATEGY" fades in over the fresh battlefield then disappears,
//  while GP_UNFADE_BLACK reveals the scene from black (platformer pattern).
// ============================================================================
static const AnimPhase introTextPhases[] = {
    { TP_FADE_IN,  0.30f, 1.10f, sineEaseOutf },   // fade up
    { TP_FADE_OUT, 2.00f, 2.80f, sineEaseInf  },   // then fade away
};
static const AnimPhase introGlobalPhases[] = {
    { GP_UNFADE_BLACK, 0.00f, 0.60f, sineEaseOutf },  // black -> battlefield
};
static AnimText introTexts[] = {
    { "STRATEGY", 0.15f, {0.5f, 0.42f}, RAYWHITE, introTextPhases, 2, NULL, 0 },
};
static const SceneAnim introAnim = {
    .texts = introTexts, .textCount = 1,
    .introGlobal = introGlobalPhases, .introGlobalCount = 1,
};
static SceneAnimPlayer introPlayer;

// ============================================================================
//  PAUSE MENU: same in-state flag + dim + slide-in/out texts as the
//  platformer (see the long comments there).
// ============================================================================
static const AnimPhase pauseTitleIntro[] = {
    { TP_SLIDE_IN, 0.00f, 0.50f, sineEaseOutf },
};
static const AnimPhase pauseSubIntro[] = {
    { TP_SLIDE_IN, 0.20f, 0.70f, sineEaseOutf },
};
static const AnimPhase pauseTitleOutro[] = {
    { TP_SLIDE_OUT, 0.10f, 0.55f, sineEaseInf },
};
static const AnimPhase pauseSubOutro[] = {
    { TP_SLIDE_OUT, 0.00f, 0.45f, sineEaseInf },
};
static AnimText pauseTexts[] = {
    { "PAUSE MENU",                  0.11f,  {0.5f, 0.08f}, RAYWHITE,
      pauseTitleIntro, 1, pauseTitleOutro, 1 },
    { "the armies hold their ground", 0.056f, {0.5f, 0.21f}, LIGHTGRAY,
      pauseSubIntro,   1, pauseSubOutro,   1 },
};
static const SceneAnim pauseAnim = {
    .texts = pauseTexts, .textCount = 2,
};
static SceneAnimPlayer pausePlayer;

typedef enum { PAUSE_PAGE_MAIN = 0, PAUSE_PAGE_OPTIONS } PausePage;
static PausePage pausePage = PAUSE_PAGE_MAIN;
static bool  paused   = false;
static float pauseDim = 0.0f;   // 0..1 dark-overlay strength (eased toward paused)

static void PauseOpen()
{
    paused    = true;
    pausePage = PAUSE_PAGE_MAIN;
    SceneAnimStart(&pausePlayer, &pauseAnim, ANIM_INTRO);
}

static void PauseClose()
{
    paused = false;
    SceneAnimStart(&pausePlayer, &pauseAnim, ANIM_OUTRO);   // texts slide back out
}

static void Enter()
{
    ScreenState *screen_state = ScreenStateGet();
    screen_state->clear_color = (Color){ 120, 160, 200, 255 };  // sky blue

    StrategyWorldInit();

    // Kick off the "STRATEGY" intro (appears, then fades out).
    SceneAnimStart(&introPlayer, &introAnim, ANIM_INTRO);

    // Fresh pause state (Enter is also the KEY_R restart).
    paused    = false;
    pausePage = PAUSE_PAGE_MAIN;
    pauseDim  = 0.0f;
    pausePlayer.spec = NULL;   // nothing to play/draw until the first ESC
}

static void Exit()
{
    // Everything is static fixed-size data - nothing to free.
}

static void Update()
{
    StrategyWorld *world = StrategyWorldGet();

    // ESC priority: cancel a building ghost first, then close the build
    // menu, then back out of the options page, then toggle the pause menu.
    if (IsKeyPressed(KEY_ESCAPE))
    {
        if (!paused && world->placing >= 0)
        {
            world->placing = -1;
        }
        else if (!paused && world->buildMenuOpen)
        {
            world->buildMenuOpen = false;
        }
        else if (paused && pausePage == PAUSE_PAGE_OPTIONS)
        {
            AudioPlayButton();
            pausePage = PAUSE_PAGE_MAIN;
        }
        else if (paused) PauseClose();
        else             PauseOpen();
    }

    if (!paused)
    {
        if (IsKeyPressed(KEY_R))
        {
            // Restart
            Enter();
        }
        StrategyWorldHandleInput();
        StrategyWorldUpdate(GetFrameTime());
        SceneAnimUpdate(&introPlayer, GetFrameTime());   // advance intro text (no-op once done)
    }

    // Pause texts animate on their own clock: intro while paused, outro over
    // live gameplay right after resume (no-op once finished / never started).
    if (pausePlayer.spec) SceneAnimUpdate(&pausePlayer, GetFrameTime());

    // Ease the dim overlay toward its target (1 when paused, 0 when playing).
    float dimTarget = paused ? 1.0f : 0.0f;
    pauseDim += (dimTarget - pauseDim) * fminf(1.0f, 10.0f*GetFrameTime());
}

static void Draw()
{
    Vector2 game_size = ScreenStateTargetSize();

    StrategyWorldDraw3D();          // Begin/EndMode3D + world + effects
    StrategyWorldDraw2DOverlay();   // drag rect, HP bars, resource HUD

    // "STRATEGY" intro text, drawn on top of the world (game space).
    SceneAnimDrawTexts(&introPlayer);

    // Pause overlay: dim the frozen game, then the animated pause texts.
    if (pauseDim > 0.01f)
        DrawRectangle(0, 0, (int)game_size.x, (int)game_size.y,
                      Fade(BLACK, 0.55f*pauseDim));
    if (pausePlayer.spec) SceneAnimDrawTexts(&pausePlayer);

    // Entry fade-in: GP_UNFADE_BLACK eases 0->1, so the remaining blackness is
    // 1-amount (missing row would read 1 = no overlay). Drawn last, over all.
    float black = 1.0f - SceneAnimGlobalAmount(&introPlayer, GP_UNFADE_BLACK);
    if (black > 0.001f)
        DrawRectangle(0, 0, (int)game_size.x, (int)game_size.y,
                      Fade(BLACK, black));
}

// ----------------------------------------------------------------------------
//  Command panel: ONE bottom strip whose content depends on what is selected
//  (derived fresh every frame - only buildMenuOpen is stored state):
//    build menu open   -> all building buttons with costs + CLOSE
//    building selected -> that building's actions (train/progress/info)
//    units selected    -> counts + STOP + GATHER NEAREST
//    nothing           -> a single BUILD button
//  Publishes its rectangle to world->guiBlock so the world ignores mouse
//  presses that land on the panel (no click-through selecting units).
// ----------------------------------------------------------------------------
// Readable info drawn on its own row ABOVE the button strip, with an opaque
// backing so it stays legible over the battlefield. `sub` (may be NULL) is a
// second, dimmer line below `text` - used for what a building is doing.
static void GuiInfoRow(Rectangle row, const char *text, const char *sub)
{
    DrawRectangleRec(row, Fade(BLACK, 0.7f));
    DrawRectangleLinesEx(row, 1.0f, Fade(RAYWHITE, 0.3f));
    int fs = (int)(row.height*(sub ? 0.34f : 0.6f));
    if (sub)
    {
        DrawText(text, (int)(row.x + 10.0f),
                 (int)(row.y + row.height*0.14f), fs, RAYWHITE);
        DrawText(sub, (int)(row.x + 10.0f),
                 (int)(row.y + row.height*0.54f), fs, Fade(RAYWHITE, 0.75f));
    }
    else
    {
        DrawText(text, (int)(row.x + 10.0f),
                 (int)(row.y + (row.height - (float)fs)*0.5f), fs, RAYWHITE);
    }
}

// ----------------------------------------------------------------------------
//  Population panel: a small left-middle column listing the player's unit
//  composition and a SELECT IDLE button that grabs the nearest idle worker and
//  pans the camera to it. Reserves world->guiBlock2 so clicks don't fall
//  through to the battlefield (see MouseOnGui).
// ----------------------------------------------------------------------------
static void GuiPopPanel(StrategyWorld *world)
{
    ScreenState *ss = ScreenStateGet();
    Rectangle vp = ss->dest_rect;
    Settings *settings = SettingsGet();

    const float scales[3] = { 1.0f, 2.0f, 3.0f };
    int s = (int)scales[settings->gui_scale_wish];

    int baseSize = GuiGetFont().baseSize;
    GuiSetStyle(DEFAULT, TEXT_SIZE, baseSize*s);
    GuiSetIconScale(s);

    int workers  = StrategyCountUnits(0, KIND_WORKER);
    int soldiers = StrategyCountUnits(0, KIND_SOLDIER) +
                   StrategyCountUnits(0, KIND_RANGED);
    int templars = StrategyCountUnits(0, KIND_TEMPLAR) +
                   StrategyCountUnits(0, KIND_TEMPLAR_HEALER);
    int idle     = StrategyCountIdleWorkers(0);

    // Panel geometry: one header row + four census rows + a button, stacked.
    float rowH = 22.0f*(float)s;
    float w    = 150.0f*(float)s;
    float btnH = 30.0f*(float)s;
    float pad  = 8.0f*(float)s;
    int   rows = 5;         // header + 4 census lines
    float panelH = pad*2.0f + (float)rows*rowH + 6.0f*(float)s + btnH;
    float x = vp.x + 16.0f;
    float y = vp.y + (vp.height - panelH)*0.5f;

    Rectangle panel = { x, y, w, panelH };
    world->guiBlock2 = panel;   // block world clicks under the panel

    DrawRectangleRec(panel, Fade(BLACK, 0.7f));
    DrawRectangleLinesEx(panel, 1.0f, Fade(RAYWHITE, 0.3f));

    int fs = (int)(rowH*0.62f);
    float tx = x + pad;
    float ty = y + pad;
    DrawText("POPULATION", (int)tx, (int)ty, fs, RAYWHITE);            ty += rowH;
    DrawText(TextFormat("WORKERS  %d", workers),  (int)tx, (int)ty, fs, RAYWHITE); ty += rowH;
    DrawText(TextFormat("ARMY     %d", soldiers), (int)tx, (int)ty, fs, RAYWHITE); ty += rowH;
    DrawText(TextFormat("TEMPLARS %d", templars), (int)tx, (int)ty, fs, RAYWHITE); ty += rowH;
    Color idleColor = (idle > 0) ? (Color){ 240, 210, 90, 255 } : Fade(RAYWHITE, 0.6f);
    DrawText(TextFormat("IDLE     %d", idle),     (int)tx, (int)ty, fs, idleColor); ty += rowH;

    ty += 6.0f*(float)s;
    if (GuiButton((Rectangle){ tx, ty, w - 2.0f*pad, btnH }, "SELECT IDLE"))
    {
        AudioPlayButton();
        StrategySelectNearestIdleWorker();
    }
}

static void GuiCommandPanel(StrategyWorld *world)
{
    ScreenState *ss = ScreenStateGet();
    Rectangle vp = ss->dest_rect;
    Settings *settings = SettingsGet();

    const float scales[3] = { 1.0f, 2.0f, 3.0f };
    int s = (int)scales[settings->gui_scale_wish];

    int baseSize = GuiGetFont().baseSize;
    GuiSetStyle(DEFAULT, TEXT_SIZE, baseSize*s);
    GuiSetIconScale(s);

    float h   = 32.0f*(float)s;
    float gap = 8.0f*(float)s;
    float x   = vp.x + 20.0f;
    float y   = vp.y + vp.height - h - 20.0f;

    // Info row sits one row above the buttons. A selected building shows two
    // lines (name+HP, then what it's doing), so the row is taller then.
    bool bldSelected = (world->selectedBuilding >= 0);
    float infoH = (bldSelected ? 42.0f : 26.0f)*(float)s;
    Rectangle infoRow = { vp.x + 20.0f, y - infoH - 6.0f,
                          vp.width - 40.0f, infoH };

    // Selection census (drives which panel shows).
    int workers = 0;
    int soldiers = 0;   // fighters: melee + ranged
    int templars = 0;
    int lastSel = -1;   // index of the single selected unit (for detailed info)
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world->units[i];
        if (!u->active || u->faction != 0 || !u->selected) continue;
        if (u->kind == KIND_WORKER) workers++;
        else if (u->kind == KIND_TEMPLAR ||
                 u->kind == KIND_TEMPLAR_HEALER) templars++;
        else soldiers++;
        lastSel = i;
    }
    int selCount = workers + soldiers + templars;

    // Reserve BOTH rows so world clicks under the info row are also blocked.
    world->guiBlock = (Rectangle){ vp.x, infoRow.y - 6.0f, vp.width,
                                   (y + h + 12.0f) - (infoRow.y - 6.0f) };

    // --- Info row content: single-unit stats, group census, or building. ---
    if (bldSelected)
    {
        Building *b = &world->buildings[world->selectedBuilding];
        const BuildingDef *bd = StrategyBuildingDef(b->kind);

        // Top line: name + HP. Bottom line: what the building is DOING (build
        // progress / training + queue / resting / farm hands / idle).
        const char *head = b->underConstruction
            ? TextFormat("%s (scaffold)   build %.0f%%", bd->name,
                         100.0f*b->buildProgress/bd->buildTime)
            : TextFormat("%s   HP %.0f/%.0f", bd->name, b->hp, b->maxHp);

        const char *sub;
        if (b->underConstruction)
        {
            sub = "RMB a worker onto it to build";
        }
        else if (b->trainKind >= 0)
        {
            const UnitDef *ud = StrategyUnitDef((UnitKind)b->trainKind);
            const char *base = TextFormat("training %s  %.0f%%", ud->name,
                                          100.0f*b->trainProgress/ud->trainTime);
            sub = (b->trainQueueCount > 0)
                ? TextFormat("%s   (+%d queued)", base, b->trainQueueCount)
                : base;
        }
        else if (b->trainCooldown > 0.0f && bd->trainableCount > 0)
        {
            sub = (b->trainQueueCount > 0)
                ? TextFormat("resting %.1fs   (+%d queued)", b->trainCooldown,
                             b->trainQueueCount)
                : TextFormat("resting %.1fs", b->trainCooldown);
        }
        else if (bd->tendNode >= 0)
        {
            int hands = 0;
            for (int i = 0; i < STRAT_MAX_UNITS; i++)
            {
                Unit *u = &world->units[i];
                if (u->active && u->state == UNIT_FARM &&
                    u->targetBuilding == world->selectedBuilding) hands++;
            }
            const char *what = (bd->tendNode == NODE_TREE) ? "planting wood" : "planting wheat";
            sub = TextFormat("%s - workers: %d  (RMB a worker here)", what, hands);
        }
        else sub = "idle";

        GuiInfoRow(infoRow, head, sub);
    }
    else if (selCount == 1)
    {
        Unit *u = &world->units[lastSel];
        const char *name = StrategyUnitDef(u->kind)->name;
        const char *base = TextFormat("%s   HP %.0f/%.0f   DMG %.0f",
                                      name, u->hp, u->maxHp, u->damage);
        // A worker mid-chain shows how many jobs are still queued behind it.
        const char *head = (u->jobQueueCount > 0)
            ? TextFormat("%s   (+%d queued)", base, u->jobQueueCount) : base;
        GuiInfoRow(infoRow, head, NULL);
    }
    else if (selCount > 1)
    {
        GuiInfoRow(infoRow, TextFormat("%d WORKER   %d ARMY   %d TEMPLAR",
                   workers, soldiers, templars), NULL);
    }

    if (world->buildMenuOpen)
    {
        // Paginate the building buttons so they always fit the screen width.
        static int buildPage = 0;
        float w = 110.0f*(float)s;
        float navW = 44.0f*(float)s;
        float closeW = 80.0f*(float)s;
        // Width available for building buttons after nav + close reserve.
        float avail = vp.width - 40.0f - (2.0f*(navW + gap)) - (closeW + gap);
        int perPage = (int)(avail/(w + gap));
        if (perPage < 1) perPage = 1;
        int pages = (BLD_COUNT + perPage - 1)/perPage;
        if (buildPage >= pages) buildPage = pages - 1;
        if (buildPage < 0) buildPage = 0;

        if (pages > 1)
        {
            if (GuiButton((Rectangle){ x, y, navW, h }, "<") && buildPage > 0)
            {
                AudioPlayButton();
                buildPage--;
            }
            x += navW + gap;
        }

        int first = buildPage*perPage;
        int last  = first + perPage;
        if (last > BLD_COUNT) last = BLD_COUNT;
        for (int kind = first; kind < last; kind++)
        {
            const BuildingDef *bd = StrategyBuildingDef((BuildingKind)kind);
            const char *text = TextFormat("%s %dw %ds", bd->name,
                                          bd->cost[RES_WOOD], bd->cost[RES_STONE]);
            if (GuiButton((Rectangle){ x, y, w, h }, text))
            {
                AudioPlayButton();
                world->placing = kind;
                world->buildMenuOpen = false;
            }
            x += w + gap;
        }

        if (pages > 1)
        {
            if (GuiButton((Rectangle){ x, y, navW, h }, ">") && buildPage < pages - 1)
            {
                AudioPlayButton();
                buildPage++;
            }
            x += navW + gap;
        }

        if (GuiButton((Rectangle){ x, y, closeW, h }, "CLOSE"))
        {
            AudioPlayButton();
            world->buildMenuOpen = false;
        }
    }
    else if (world->selectedBuilding >= 0)
    {
        Building *b = &world->buildings[world->selectedBuilding];
        const BuildingDef *bd = StrategyBuildingDef(b->kind);
        float w = 190.0f*(float)s;

        // Status text (scaffold / training / resting / farm) now lives in the
        // info row above; the button strip is actions only. TRAIN buttons stay
        // available while training so you can queue more (StrategyTrainStart
        // enqueues when busy). Scaffolds can't train.
        if (!b->underConstruction && bd->trainableCount > 0)
        {
            for (int t = 0; t < bd->trainableCount; t++)
            {
                const UnitDef *ud = StrategyUnitDef(bd->trainable[t]);
                const char *text = TextFormat("TRAIN %s %dw %df", ud->name,
                                              ud->cost[RES_WOOD], ud->cost[RES_FOOD]);
                if (GuiButton((Rectangle){ x, y, w, h }, text))
                {
                    AudioPlayButton();
                    StrategyTrainStart(world->selectedBuilding, bd->trainable[t]);
                }
                x += w + gap;
            }
        }

        // QUARRY: spend providence to conjure a fresh stone node nearby.
        if (!b->underConstruction && b->kind == BLD_QUARRY)
        {
            const char *stone = TextFormat("SPAWN STONE (%dprov)", STRAT_QUARRY_STONE_PROV);
            if (GuiButton((Rectangle){ x, y, 200.0f*(float)s, h }, stone))
            {
                AudioPlayButton();
                StrategyQuarrySpawnStone(world->selectedBuilding);
            }
            x += 200.0f*(float)s + gap;
        }

        // CANCEL: drop the last queued trainee (or the active one), refunded.
        if (b->trainKind >= 0 || b->trainQueueCount > 0)
        {
            if (GuiButton((Rectangle){ x, y, 90.0f*(float)s, h }, "CANCEL"))
            {
                AudioPlayButton();
                StrategyTrainCancel(world->selectedBuilding);
            }
            x += 90.0f*(float)s + gap;
        }

        // SELL: refund scales with the difficulty bonus (Easy sells best).
        float rate = bd->refundRate + world->mods[0].refundBonus;
        const char *sell = TextFormat("SELL +%dw +%ds",
                                      (int)floorf((float)bd->cost[RES_WOOD]*rate),
                                      (int)floorf((float)bd->cost[RES_STONE]*rate));
        if (GuiButton((Rectangle){ x, y, 140.0f*(float)s, h }, sell))
        {
            AudioPlayButton();
            StrategySellBuilding(world->selectedBuilding);
        }
    }
    else if (selCount > 0)
    {
        // Census now lives in the info row above; this row is just actions.
        if (GuiButton((Rectangle){ x, y, 80.0f*(float)s, h }, "STOP"))
        {
            AudioPlayButton();
            for (int i = 0; i < STRAT_MAX_UNITS; i++)
            {
                Unit *u = &world->units[i];
                if (!u->active || u->faction != 0 || !u->selected) continue;
                u->state          = UNIT_IDLE;
                u->targetUnit     = -1;
                u->targetNode     = -1;
                u->targetBuilding = -1;
            }
        }
        x += 80.0f*(float)s + gap;

        if (workers > 0 &&
            GuiButton((Rectangle){ x, y, 160.0f*(float)s, h }, "GATHER NEAREST"))
        {
            AudioPlayButton();
            for (int i = 0; i < STRAT_MAX_UNITS; i++)
            {
                Unit *u = &world->units[i];
                if (!u->active || u->faction != 0 || !u->selected) continue;
                if (u->kind != KIND_WORKER) continue;

                int node = StrategyNearestNodeOfKind(u->pos, -1, STRAT_RETARGET_RADIUS);
                if (node >= 0) StrategyOrderGather(u, node);
            }
        }
    }
    else
    {
        if (GuiButton((Rectangle){ x, y, 120.0f*(float)s, h }, "BUILD"))
        {
            AudioPlayButton();
            world->buildMenuOpen = true;
        }
    }
}

// ----------------------------------------------------------------------------
//  Gui: SCREEN SPACE. Build bar while playing; while paused, the same
//  anchored right-hand column and DESIRED vs EFFECTIVE scale logic as
//  main_menu.c / platformer_test.c (see the long comments there).
// ----------------------------------------------------------------------------
static void Gui()
{
    StrategyWorld *world = StrategyWorldGet();
    world->guiBlock  = (Rectangle){ 0 };    // nothing owns the mouse by default
    world->guiBlock2 = (Rectangle){ 0 };

    if (!paused)
    {
        GuiPopPanel(world);
        GuiCommandPanel(world);
        return;
    }

    ScreenState *ss = ScreenStateGet();
    Rectangle vp = ss->dest_rect;   // game region in REAL screen pixels

    Settings *settings = SettingsGet();
    const float scales[3] = { 1.0f, 2.0f, 3.0f };
    int desired = (int)scales[settings->gui_scale_wish];   // 1, 2 or 3

    const float game_top_margin = 120.0f;  // clears the pause title when contained
    const float screen_margin   = 20.0f;
    // Column height at s=1 (see main_menu.c for the calc rules):
    //   MAIN:    resume48 +options48 +quit36 = 132.
    //   OPTIONS: back48 +vollbl22 +slider32 +modelbl20 +modetog48 +persist32
    //            +difftog48 +difflbl32 +scalelbl20 +scaletog36 = 338.
    const float LAYOUT_UNITS = (pausePage == PAUSE_PAGE_MAIN) ? 132.0f : 338.0f;

    float contained_top = vp.y + game_top_margin;
    float fit_contained = vp.height - game_top_margin - screen_margin;
    float fit_expanded  = ss->height - 2.0f*screen_margin;
    int effective = desired;
    while (effective > 1 &&
           LAYOUT_UNITS * effective > fit_contained &&
           LAYOUT_UNITS * effective > fit_expanded) effective--;

    float col_h = LAYOUT_UNITS * effective;
    float col_top = (col_h <= fit_contained)
                        ? contained_top
                        : (ss->height - col_h) * 0.5f;

    float s = (float)effective;
    settings->gui_scale = s;

    int baseSize = GuiGetFont().baseSize;
    GuiSetStyle(DEFAULT, TEXT_SIZE, baseSize * effective);
    GuiSetIconScale(effective);

    float w = 220.0f * s;
    float h = 36.0f  * s;
    float gap = 12.0f * s;
    float rh = 20.0f * s;
    float x = vp.x + vp.width - w - 40.0f;
    float y = col_top;

    if (pausePage == PAUSE_PAGE_MAIN)
    {
        if (GuiButton((Rectangle){ x, y, w, h }, "RESUME"))
        {
            AudioPlayButton();
            PauseClose();
        }
        y += h + gap;

        if (GuiButton((Rectangle){ x, y, w, h }, "OPTIONS"))
        {
            AudioPlayButton();
            pausePage = PAUSE_PAGE_OPTIONS;
        }
        y += h + gap;

        if (GuiButton((Rectangle){ x, y, w, h }, "QUIT (-> main menu)"))
        {
            AudioPlayButton();
            AppStateTransition(&app_state_main_menu);
        }

        DrawText("Press ESC to resume", 20, 20, 20, RAYWHITE);
    }
    else // PAUSE_PAGE_OPTIONS - same widgets as main_menu's OPTIONS page
    {
        if (GuiButton((Rectangle){ x, y, w, h }, "< BACK"))
        {
            AudioPlayButton();
            pausePage = PAUSE_PAGE_MAIN;
        }
        y += h + gap;

        GuiLabel((Rectangle){ x, y, w, rh }, TextFormat("Volume: %.0f%%", settings->music_volume*100.0f));
        y += rh + 2.0f*s;

        float prevVol = settings->music_volume;
        GuiSlider((Rectangle){ x + 10.0f*s, y, w - 20.0f*s, rh }, "0", "100", &settings->music_volume, 0.0f, 1.0f);
        SettingsApplyVolume();
        if (settings->music_volume != prevVol) AudioPlayVolumePreview();
        y += rh + gap;

        GuiLabel((Rectangle){ x, y, w, rh }, "Window Mode:");
        y += rh;
        int prevMode = settings->window_mode;
        int mode = prevMode;
        GuiToggleGroup((Rectangle){ x, y, (w - gap*2.0f)/3.0f, h },
                       "Windowed;Fullscreen;Borderless", &mode);
        if (mode != prevMode) {
            AudioPlayButton();
            SettingsApplyWindowMode(mode);
        }
        y += h + gap;

        if (GuiCheckBox((Rectangle){ x, y, rh, rh }, "Persist settings on quit", &settings->persist))
            AudioPlayButton();
        y += rh + gap;

        int prevDiff = settings->difficulty;
        GuiToggleGroup((Rectangle){ x, y, (w - gap*2.0f)/3.0f, h }, "Easy;Normal;Hard", &settings->difficulty);
        if (settings->difficulty != prevDiff) AudioPlayButton();
        y += h + gap;

        GuiLabel((Rectangle){ x, y, w, rh }, TextFormat("Difficulty index: %i", settings->difficulty));
        y += rh + gap;

        const char *names[3] = { "Small", "Medium", "Large" };
        GuiLabel((Rectangle){ x, y, w, rh },
                 effective == desired ? "GUI Scale:"
                                      : TextFormat("GUI Scale: (%s fits)", names[effective - 1]));
        y += rh;
        int prevScaleWish = settings->gui_scale_wish;
        GuiToggleGroup((Rectangle){ x, y, (w - gap*2.0f)/3.0f, h }, "Small;Medium;Large", &settings->gui_scale_wish);
        if (settings->gui_scale_wish != prevScaleWish) AudioPlayButton();

        DrawText("Press ESC or click BACK to return", 20, 20, 20, RAYWHITE);
    }
}
