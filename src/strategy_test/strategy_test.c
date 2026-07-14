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

    // Selection census (drives which panel shows).
    int workers = 0;
    int soldiers = 0;   // fighters: melee + ranged
    int templars = 0;
    for (int i = 0; i < STRAT_MAX_UNITS; i++)
    {
        Unit *u = &world->units[i];
        if (!u->active || u->faction != 0 || !u->selected) continue;
        if (u->kind == KIND_WORKER) workers++;
        else if (u->kind == KIND_TEMPLAR ||
                 u->kind == KIND_TEMPLAR_HEALER) templars++;
        else soldiers++;
    }

    // Reserve the whole strip up front; individual widgets draw inside it.
    world->guiBlock = (Rectangle){ vp.x, y - 6.0f, vp.width, h + 12.0f };

    if (world->buildMenuOpen)
    {
        float w = 110.0f*(float)s;
        for (int kind = 0; kind < BLD_COUNT; kind++)
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
        if (GuiButton((Rectangle){ x, y, 80.0f*(float)s, h }, "CLOSE"))
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

        if (b->trainKind >= 0)
        {
            // Progress bar for the current trainee.
            const UnitDef *ud = StrategyUnitDef((UnitKind)b->trainKind);
            float frac = b->trainProgress/ud->trainTime;
            GuiLabel((Rectangle){ x, y, w, h }, TextFormat("training %s...", ud->name));
            x += w + gap;
            DrawRectangle((int)x, (int)(y + h*0.35f),
                          (int)w, (int)(h*0.3f), Fade(DARKGRAY, 0.8f));
            DrawRectangle((int)x, (int)(y + h*0.35f),
                          (int)(w*frac), (int)(h*0.3f), GREEN);
            x += w + gap;
        }
        else if (b->trainCooldown > 0.0f && bd->trainableCount > 0)
        {
            GuiLabel((Rectangle){ x, y, w, h },
                     TextFormat("%s resting %.1fs", bd->name, b->trainCooldown));
            x += w + gap;
        }
        else if (bd->trainableCount > 0)
        {
            // One TRAIN button per kind this building can produce.
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
        else if (b->kind == BLD_FARM)
        {
            int hands = 0;
            for (int i = 0; i < STRAT_MAX_UNITS; i++)
            {
                Unit *u = &world->units[i];
                if (u->active && u->state == UNIT_FARM &&
                    u->targetBuilding == world->selectedBuilding) hands++;
            }
            GuiLabel((Rectangle){ x, y, w, h },
                     TextFormat("FARM - workers: %d (RMB workers here)", hands));
            x += w + gap;
        }
        else
        {
            GuiLabel((Rectangle){ x, y, w, h }, bd->name);
            x += w + gap;
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
    else if (workers + soldiers + templars > 0)
    {
        GuiLabel((Rectangle){ x, y, 220.0f*(float)s, h },
                 TextFormat("%d WORKER  %d ARMY  %d TEMPLAR",
                            workers, soldiers, templars));
        x += 220.0f*(float)s + gap;

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
    world->guiBlock = (Rectangle){ 0 };     // nothing owns the mouse by default

    if (!paused)
    {
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
