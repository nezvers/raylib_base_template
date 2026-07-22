// ============================================================================
//  main_menu.c  -  SHOWCASE ALL IN ONE
//
//  Two draw phases (see main.c -> UpdateDrawFrame):
//    Draw()  -> renders into the fixed 1280x720 game render-texture (scaled).
//               Use ScreenStateTargetSize() for its size. Put "world"/art here.
//    Gui()   -> renders to the REAL screen framebuffer, AFTER Draw().
//               Put raygui widgets / HUD here (real pixels, not scaled).
// ============================================================================

#include "raylib.h"
#include "../app_state/app_state.h"
#include "../screen_state/screen_state.h"
#include "../settings_state/settings_state.h"
#include "../audio_state/audio_state.h"
#include "../scene_anim/scene_anim.h"        // AnimText / AnimPhase / SceneAnim data model
#include "../transition/transition_state.h"  // TransitionStateStart (generic outro player)
#include "../anim_stage/anim_scene.h"        // declarative editor-authored anim table
#include "../signal/signal.h"                // SignalParams
#include <stddef.h>
#include <math.h>

// raygui lives in src/include/ (on the include path). 
// Its implementation must be compiled EXACTLY ONCE in the whole project - we do it here. 
// If you add this same #define to another .c you will get duplicate-symbol link errors.
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

// Forward declare the state functions (same pattern as placeholder_state.c).
static void Enter();
static void Exit();
static void Update();
static void Draw();
static void Gui();

                            /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_state_main_menu = {Enter, Exit, Update, Draw, Gui, "MainMenu"};

// --- widget values (persist across frames -> file-scope statics) --------
// Volume, difficulty AND the GUI-scale wish now all live in the Settings singleton
// (settings->music_volume / ->difficulty / ->gui_scale_wish), so they're global and
// persist on quit; the widgets below bind straight to them.
static float animTime = 0.0f;           // accumulates for the animation demo

// The menu shows one of two pages (contexts) at a time. MAIN has the top-level
// buttons; OPTIONS has BACK + all the settings widgets. The OPTIONS button switches
// to OPTIONS; BACK or ESC returns to MAIN (ESC handled in Update()).
typedef enum { MENU_PAGE_MAIN = 0, MENU_PAGE_OPTIONS } MenuPage;
static MenuPage menuPage = MENU_PAGE_MAIN;

// ============================================================================
//  SCENE ANIMATION DATA (see scene_anim.h) - the demonstration integration.
//
//  This block is the SINGLE source of truth for every animated text the menu
//  shows and for how the menu animates in (intro, played by this state) and
//  out (outro, played by the generic transition state). There is no second
//  DrawText for the title/sub-text anywhere - the same AnimText rows drive
//  the idle menu, the intro and the outro, so nothing can drift apart.
//
//  Phase rows: { kind, start s, end s, easing } - ANY order, found by kind.
//  A gap between rows = the old "hold" (nothing needs a row to stand still).
// ============================================================================

typedef struct {
    AnimPhase titleIntro[1];
    AnimPhase titleOutro[3];
    AnimPhase descrIntro[1];
    AnimPhase descrOutro[1];
    AnimText menuTexts[2];
    AnimPhase menuOutroGlobal[2];
    AnimPhase menuOutroShape[2];
    SceneAnim menuAnim;
    SceneAnimPlayer player;
} MainMenUAnimations_t;
static MainMenUAnimations_t menu_animations;

// The zoom boxes' clock lives HERE (in the scene) and the spec points at it,
// so the outro captures the boxes exactly where the menu just drew them.
static ZoomBoxes zoomBoxes = { .count = 3, .period = 5.0f };

// ============================================================================
//  EDITOR-AUTHORED ANIMATION (anim_scene.h) - the NEW model, running alongside
//  the scene_anim one above rather than replacing it.
//
//  These animations are not declared in C at all: they are authored in the ANIM
//  EDITOR, saved to anims/<name>.cfg, and listed HERE as a declarative table -
//  each row an instance with its own loop/delay/layer and the signals it answers
//  to. The whole set plays with one AnimScenePlay call (see Enter).
//
//    * "signal_test" is the looping overlay; the menu ends it by emitting its
//      authored TERMINAL signal MENU_END_SIGNAL, and AnimSceneEmitTerminal waits
//      for that transition to finish before the state changes.
//    * "zooming_box" appears THREE times, each started a little later (the
//      .delay column), so one file gives a staggered train of ripples. It
//      answers the "ripple" signal, which USES A POSITION PARAM (usesPos): the
//      menu emits it at the mouse on click, so the ripple appears where clicked.
//
//  How much of the menu shows THROUGH the overlay is decided in the editor, by
//  the document's global element (AP_G_BG_ALPHA 0 = fully transparent).
//
//  The row count must stay within ANIM_STAGE_SLOTS_MAX (each row holds a slot
//  for its whole life, delay included).
// ============================================================================
#define MENU_END_SIGNAL    "TV-out"    // terminal signal that ends the overlay
#define MENU_RIPPLE_SIGNAL "ripple"    // placed ripple; consumes params.pos

static const AnimStageEntry MENU_SCENE[] = {
    { .anim="signal_test", .loop=true, .delay=0.0f, .layer=10, .tag=0,
      .signals={ { MENU_END_SIGNAL, false } }, .signalCount=1 },
    { .anim="zooming_box", .loop=true, .delay=0.0f, .layer=9,  .tag=1,
      .signals={ { MENU_RIPPLE_SIGNAL, true } }, .signalCount=1 },
    { .anim="zooming_box", .loop=true, .delay=0.6f, .layer=9,  .tag=2,
      .signals={ { MENU_RIPPLE_SIGNAL, true } }, .signalCount=1 },
    { .anim="zooming_box", .loop=true, .delay=1.2f, .layer=9,  .tag=3,
      .signals={ { MENU_RIPPLE_SIGNAL, true } }, .signalCount=1 },
};

// The state owns one scene object (holds the live handles + terminal bookkeeping).
static AnimStageScene menuScene;

static void DrawMenuArt(float alpha, float time);   // defined below Draw()

// Plays the INTRO when the menu is entered; after it finishes, the same
// SceneAnimDrawTexts call keeps drawing the texts at their rest pose forever.

static void AnimationsInit() {
    // slides in from off-screen
    menu_animations.titleIntro[0] = (AnimPhase){ TP_SLIDE_IN, 0.00f, 0.60f, sineEaseOutf };

    // already centered -> no motion, but would center any rest pose
    menu_animations.titleOutro[0] = (AnimPhase){ TP_CENTER_X, 0.00f, 0.55f, sineEaseOutf };
    // drop to the vertical center
    menu_animations.titleOutro[1] = (AnimPhase){ TP_CENTER_Y, 0.10f, 0.75f, sineEaseInOutf };
    // then break apart and fall
    menu_animations.titleOutro[2] = (AnimPhase){ TP_CRUMBLE,  1.05f, 2.15f, cubicEaseInf };

    // NULL ease = linear
    menu_animations.descrIntro[0] = (AnimPhase){ TP_FADE_IN,  0.20f, 0.70f, NULL };
    // sub-text crumbles a beat later
    menu_animations.descrOutro[0] = (AnimPhase){ TP_CRUMBLE,  1.15f, 2.25f, cubicEaseInf };
    
    // Every DrawText the menu owns: text, size (fraction of game height), anchor
    // (x = text center, y = top edge; fractions of game size), color, phases.
    menu_animations.menuTexts[0] = (AnimText){ "MAIN MENU",                0.125f, {0.5f, 0.06f}, RAYWHITE, menu_animations.titleIntro, 1,  menu_animations.titleOutro, 3 };
    menu_animations.menuTexts[1] = (AnimText){ "place of all the buttons", 0.062f, {0.5f, 0.18f}, RAYWHITE, menu_animations.descrIntro, 1,  menu_animations.descrOutro, 1 };

    // Global (whole-screen) outro beats. GP_FADE_BLACK's end is the hand-off time.
    menu_animations.menuOutroGlobal[0] = (AnimPhase){ GP_ART_FADE,   0.15f, 1.10f, NULL };
    menu_animations.menuOutroGlobal[1] = (AnimPhase){ GP_FADE_BLACK, 1.90f, 2.55f, NULL };

    // Shape (zoom box) outro beats: keep zooming while the gaps shrink to zero
    // (they group up mid-zoom), then the grouped stack reverses inward together.
    menu_animations.menuOutroShape[0] = (AnimPhase){ SP_ZOOM_SETTLE_GAP1,  0.00f, 1.35f, sineEaseOutf };
    menu_animations.menuOutroShape[1] = (AnimPhase){ SP_ZOOM_REVERSE, 1.35f, 2.45f, cubicEaseInf };

    // The bundle handed to both players: our texts, global/shape outro tables,
    // the shared zoom boxes and the decor callback. (introGlobal is NULL: this
    // menu has no whole-screen intro beat; GP_UNFADE_BLACK would go there.)
    menu_animations.menuAnim = (SceneAnim){
        .texts = menu_animations.menuTexts,
        .textCount = 2,

        .introGlobal = NULL,
        .introGlobalCount = 0,

        .outroGlobal = menu_animations.menuOutroGlobal,
        .outroGlobalCount = 2,

        .outroShape = menu_animations.menuOutroShape,
        .outroShapeCount = 2,

        .boxes = &zoomBoxes,
        .drawArt = DrawMenuArt,
        };
}

// Where StartGameTransition is headed once the overlay has wound down. Held
// across frames because the hand-off is deferred (see below).
static AppState *pendingDestination = NULL;

// Hand off to the outro player + destination. This is the ORIGINAL exit path,
// now reached either immediately or from the overlay's done callback.
static void EnterGameTransition(void)
{
    AppState *destination = pendingDestination ? pendingDestination
                                               : &app_state_platformer;
    pendingDestination = NULL;
    TransitionStateStart(&menu_animations.menuAnim, destination);
    AppStateTransition(&app_state_transition);
}

// Fired by anim_scene once the overlay's terminal transition has finished (or
// immediately if nothing was armed). `user` is unused - the destination is in
// pendingDestination.
static void OnSceneDone(void *user)
{
    (void)user;
    EnterGameTransition();
}

// Leave the menu through the generic outro player (PLAY/STRATEGY and ENTER).
//
// If the authored scene is playing, the menu does NOT leave right away: it
// emits the overlay's TERMINAL signal across the scene and waits for that
// transition to play through to its end, handing off from OnSceneDone.
// AnimSceneEmitTerminal fires OnSceneDone immediately when nothing arms (no
// scene, or the signal isn't terminal), so this never hangs.
static void StartGameTransition(AppState *destination)
{
    pendingDestination = destination;

    if (AnimSceneAlive(&menuScene))
        AnimSceneEmitTerminal(&menuScene, MENU_END_SIGNAL, NULL, OnSceneDone, NULL);
    else
        EnterGameTransition();
}

// ----------------------------------------------------------------------------
//  Enter: called once when the state becomes active (via AppStateTransition).
//  Good place to set the background color, load resources, reset variables.
// ----------------------------------------------------------------------------
static void Enter()
{
    ScreenState *screenState = ScreenStateGet();
    screenState->clear_color = (Color){ 25, 30, 40, 255 };  // dark blue-gray
    animTime = 0.0f;
    menuPage = MENU_PAGE_MAIN;   // always open on the main page

    // Kick off the intro: texts slide/fade IN to their rest pose. When it is
    // done the same player simply keeps drawing the rest pose (see Draw()).
    AnimationsInit();
    SceneAnimStart(&menu_animations.player, &menu_animations.menuAnim, ANIM_INTRO);

    // The entire editor-authored integration in one call: the looping overlay
    // plus the staggered ripple train, from the MENU_SCENE table. A missing or
    // unreadable .cfg is harmless - that row's handle is ANIM_HANDLE_NONE and
    // every scene call simply skips it, so the menu still works without the file.
    AnimScenePlay(&menuScene, MENU_SCENE, sizeof(MENU_SCENE)/sizeof(MENU_SCENE[0]));

    // No GUI-scale seeding needed: the wish lives in settings->gui_scale_wish, loaded
    // at startup and bound directly to the toggle below.
}

// ----------------------------------------------------------------------------
//  Exit: called once when leaving this state (before the next Enter).
//  Free anything you loaded in Enter here.
// ----------------------------------------------------------------------------
static void Exit()
{
    // Nothing else loaded in this demo - but no stage slot may outlive the
    // state that started it, or it would keep drawing over the next one.
    AnimSceneStop(&menuScene);
}

// ----------------------------------------------------------------------------
//  Update: pure logic, no drawing. Runs every frame before Draw/Gui.
// ----------------------------------------------------------------------------
static void Update()
{
    // GetFrameTime() = seconds since last frame. Multiply movement by it so
    // speed is frame-rate independent. (GetTime() = seconds since start.)
    float dt = GetFrameTime();
    animTime += dt;

    SceneAnimUpdate(&menu_animations.player, dt);   // advance the intro (no-op once done)
    ZoomBoxesUpdate(&zoomBoxes, dt);     // the boxes' shared loop clock
    AnimStageUpdate(dt);                 // the editor-authored overlay(s)

    // Keyboard input example: ENTER also starts the game.
    if (IsKeyPressed(KEY_ENTER))
    {
        StartGameTransition(&app_state_platformer);
    }

    // Position-parameter demo: a left click spawns a ripple WHERE it clicked.
    // The same "ripple" signal fires across every scene row that declares it
    // (all three zooming_box copies), each re-anchored to this canvas fraction -
    // so the authored in-place zoom instead plays at the mouse. The GUI column
    // owns its own clicks; this only reacts to clicks over the open game area.
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        Vector2 game = ScreenStateTargetSize();
        Vector2 px   = Screen2Target(GetMousePosition());
        SignalParams p = { .pos = { px.x / game.x, px.y / game.y }, .hasPos = true };
        AnimSceneEmit(&menuScene, MENU_RIPPLE_SIGNAL, &p);
    }

    // ESC: on the OPTIONS page it returns to MAIN; on MAIN it quits the app.
    // main.c disabled raylib's default ESC=quit (SetExitKey(KEY_NULL)) so we can own
    // it here - otherwise WindowShouldClose() would latch ESC and exit before Update().
    if (IsKeyPressed(KEY_ESCAPE))
    {
        if (menuPage == MENU_PAGE_OPTIONS)
        {
            AudioPlayButton();
            menuPage = MENU_PAGE_MAIN;
        }
        else
        {
            AppStateRequestQuit();
        }
    }
}

// ----------------------------------------------------------------------------
//  Draw: GAME SPACE (canvas = ScreenStateTargetSize(), scaled to the window).
//  Resolution-independent: all sizes/positions are fractions of game_size,
//  so the pixelart holds at any render resolution. Raylib primitives here.
// ----------------------------------------------------------------------------
static void Draw()
{
    Vector2 game_size = ScreenStateTargetSize();   // {width, height} of game space

    // -- Zoom boxes: staggered ambient loop off the shared clock --------------
    ZoomBoxesDrawLoop(&zoomBoxes);

    // -- Texts: title + sub-text from the menuTexts table ---------------------
    // During the intro they animate in; afterwards this draws the rest pose.
    SceneAnimDrawTexts(&menu_animations.player);

    // -- Mouse input in GAME space.
    Vector2 pos_mouse = Screen2Target(GetMousePosition());
    DrawCircleLines((int)pos_mouse.x, (int)pos_mouse.y, game_size.y*0.017f, LIME);

    // -- Scene decor (bar, lines, bobbing circle) -----------------------------
    // Shared with the outro: the transition state calls this same function
    // with a fading alpha, so the art can never drift from what the menu drew.
    DrawMenuArt(1.0f, 0.0f);

    Vector2 screen_size = ScreenStateSize();

    // -- Debug readout (bottom-left), stacked upward ------------------------
    int   dbgSize = (int)fmaxf(1.0f, game_size.y*0.052f);
    int   dbgPad  = (int)fmaxf(1.0f, game_size.y*0.005f);
    float dbgX    = game_size.x*0.02f;
    DrawText(TextFormat("size(screen): %.0f, %.0f; size(game): %.0f, %.0f", screen_size.x, screen_size.y, game_size.x, game_size.y), (int)dbgX, (int)game_size.y - (3*(dbgPad+dbgSize)), dbgSize, GRAY);
    DrawText(TextFormat("mouse(screen): %.0f, %.0f", pos_mouse.x, pos_mouse.y), (int)dbgX, (int)game_size.y - (2*(dbgSize+dbgPad)), dbgSize, GRAY);
    DrawText(TextFormat("GetTime(): %.1fs   FPS: %i", GetTime(), GetFPS()), (int)dbgX, (int)game_size.y - (dbgSize+dbgPad), dbgSize, GRAY);

    // -- Editor-authored animations: LAST in Draw() ---------------------------
    // Last, so they composite over everything the menu drew above (showing it
    // through wherever the authored global background alpha allows), and still
    // inside Draw(), so main.c's Gui() pass keeps the raygui column on top and
    // clickable. Nothing to draw when no animation is playing.
    AnimStageDraw();
}

// ----------------------------------------------------------------------------
//  DrawMenuArt: the menu's non-text, non-box decor. ONE definition used by
//  both the live menu (alpha=1) and the outro player (alpha=1-GP_ART_FADE),
//  so the transition never has to copy these draws.
//  `time` is ADDED to the menu's own clock: the menu passes 0, the outro
//  passes its play clock - animTime froze when the menu exited, so the sum
//  keeps the bobbing motion continuous across the hand-off.
// ----------------------------------------------------------------------------
static void DrawMenuArt(float alpha, float time)
{
    Vector2 game_size = ScreenStateTargetSize();
    float cx = game_size.x*0.5f;
    float cy = game_size.y*0.5f;
    float t  = animTime + time;

    // -- Rectangles: the header underline bar + the boxes' tiny seed outline --
    float bar_w = game_size.x*0.31f;
    int   bar_h = (int)fmaxf(1.0f, game_size.y*0.006f);
    DrawRectangle((int)(cx - bar_w*0.5f), (int)(game_size.y*0.24f), (int)bar_w, bar_h,
                  Fade(SKYBLUE, alpha));

    float db_w = game_size.x * 0.05f;
    float db_h = game_size.y * 0.05f;
    DrawRectangleLines((int)(cx - (int)(db_w/2)), (int)(cy - (int)(db_h/2)),
                       (int)db_w, (int)db_h, Fade(DARKGRAY, alpha));

    // -- Lines ----------------------------------------------------------------
    DrawLine(0, 0, (int)game_size.x, (int)game_size.y, Fade((Color){ 60, 70, 90, 255 }, alpha));
    DrawLine((int)game_size.x, 0, 0, (int)game_size.y, Fade((Color){ 60, 70, 90, 255 }, alpha));

    // -- Animation: a circle bobbing up/down using sinf + accumulated time ----
    float bob = sinf(t*2.0f)*(game_size.y*0.056f);   // proportional bob
    float bob_off = game_size.y*0.17f;               // below center
    float bob_r   = game_size.y*0.042f;
    DrawCircle((int)cx, (int)(cy + bob_off + bob), bob_r, Fade(ORANGE, alpha));
    DrawCircleLines((int)cx, (int)(cy + bob_off + bob), bob_r, Fade(RAYWHITE, alpha));
}

// ----------------------------------------------------------------------------
//  Gui: SCREEN SPACE (real window pixels). raygui widgets / HUD here.
//  raygui functions return true / write into a pointer; you react to that.
// ----------------------------------------------------------------------------
static void Gui()
{
    // Layout anchored to the real window size (not game space).
    ScreenState *ss = ScreenStateGet();
    Rectangle vp = ss->dest_rect;   // game region in REAL screen pixels
    
    // -- GUI scale: DESIRED vs EFFECTIVE -------------------------------------
    // settings->gui_scale_wish is what the user WANTS: 0/1/2 (persisted).
    // The desired scale is a whole number (1/2/3) so the bitmap font stays crisp
    // raygui's default font is a 10px atlas that only renders sharply at whole multiples of baseSize (10, 20, 30...); fractional sizes blur.
    Settings *settings = SettingsGet();
    const float scales[3] = { 1.0f, 2.0f, 3.0f };
    int desired = (int)scales[settings->gui_scale_wish];   // 1, 2 or 3

    // The column is a fixed stack, so its total height is LINEAR in the scale:
    // total(s) = LAYOUT_UNITS * s. 
    // We pick the largest whole scale (<= desired) that fits (see below) so the last widget is always reachable. 
    // If the user picked Large but only Medium fits now, we render Medium; a taller window (fullscreen) re-runs this every frame and restores Large automatically.
    const float game_top_margin = 120.0f;  // clears the game-space title when contained
    const float screen_margin   = 20.0f;    // margin when expanded to the full window
    // Height of the active page's column at s=1: the sum of every "y +=" advance below
    // (for that page) plus the final row's own height. Each page fits/scales on its own.
    // If you ADD/REMOVE a widget on a page, update its manual calc of gui height here:
    //   MAIN:    label48 +play48 +strategy48 +options48 +quit36 = 228.
    //   OPTIONS: back48 +vollbl22 +slider32 +modelbl20 +modetog48 +persist32
    //            +difftog48 +difflbl32 +scalelbl20 +scaletog36 = 338.
    const float LAYOUT_UNITS = (menuPage == MENU_PAGE_MAIN) ? 228.0f : 338.0f;

    // Prefer to CONTAIN the column in the game region, anchored below the title.
    // If it's too tall for that, EXPAND to use the whole window height (anchored near the screen top). 
    // Only shrink the scale when it won't fit even that, so the last widget is always on-screen and clickable.
    float contained_top = vp.y + game_top_margin;
    float fit_contained = vp.height - game_top_margin - screen_margin;
    float fit_expanded  = ss->height - 2.0f*screen_margin;
    int effective = desired;
    while (effective > 1 &&
           LAYOUT_UNITS * effective > fit_contained &&
           LAYOUT_UNITS * effective > fit_expanded) effective--;

    // Pick the anchor for the chosen scale: stay below the title when it fits the game region; otherwise vertically CENTER it in the full window.
    float col_h = LAYOUT_UNITS * effective;
    float col_top = (col_h <= fit_contained)
                        ? contained_top
                        : (ss->height - col_h) * 0.5f;

    float s = (float)effective;
    settings->gui_scale = s;   // publish the ACTUAL rendered scale to the singleton

    int baseSize = GuiGetFont().baseSize;                  // 10 for the default font
    GuiSetStyle(DEFAULT, TEXT_SIZE, baseSize * effective); // crisp glyphs at every preset
    GuiSetIconScale(effective);                            // icons scale by the same step

    float w = 220.0f * s;                 // widget sizes grow with the scale so
    float h = 36.0f  * s;                 // the boxes match the bigger font
    float gap = 12.0f * s;
    float rh = 20.0f * s;                  // short-row height (labels/slider/checkbox)
    float x = vp.x + vp.width - w - 40.0f; // anchor by the SCALED width, 40px margin
    float y = col_top;                     // top of the column (computed above)


    // ========================================================================
    //  The menu has two PAGES (contexts). Same anchored column, different body.
    //  MAIN = top-level buttons; OPTIONS = BACK + all settings widgets. Clicking
    //  OPTIONS switches to the OPTIONS page; BACK / ESC returns to MAIN.
    // ========================================================================
    if (menuPage == MENU_PAGE_MAIN)
    {
        // -- LEFT-side tool button ------------------------------------------
        // Mirrors the right column's anchor (vp.x + vp.width - w - 40) to the
        // left edge. It is NOT part of the auto-scaled right column, so it does
        // not count toward LAYOUT_UNITS - it stands alone on the left.
        float xLeft = vp.x + 40.0f;
        if (GuiButton((Rectangle){ xLeft, y, w, h }, "ANIM EDITOR"))
        {
            AudioPlayButton();
            AppStateTransition(&app_state_anim_editor);
        }

        // -- Label: static text, no interaction ------------------------------
        GuiLabel((Rectangle){ x, y, w, h }, "--- raygui widgets ---");
        y += h + gap;

        // -- Button: returns non-zero (true) on click ------------------------
        if (GuiButton((Rectangle){ x, y, w, h }, "PLAY (-> platformer)"))
        {
            AudioPlayButton();
            // Arm the generic outro player with OUR spec + destination, then
            // enter it. Our Exit() runs, then the transition's Enter().
            StartGameTransition(&app_state_platformer);
        }
        y += h + gap;

        if (GuiButton((Rectangle){ x, y, w, h }, "STRATEGY (-> RTS test)"))
        {
            AudioPlayButton();
            StartGameTransition(&app_state_strategy);
        }
        y += h + gap;

        // -- OPTIONS: switch this state to its OPTIONS page (no state change) -
        if (GuiButton((Rectangle){ x, y, w, h }, "OPTIONS"))
        {
            AudioPlayButton();
            menuPage = MENU_PAGE_OPTIONS;
        }
        y += h + gap;

        if (GuiButton((Rectangle){ x, y, w, h }, "QUIT"))
        {
            AudioPlayButton();
            // Ask to exit. Do NOT call CloseWindow() here: we're mid-frame inside Gui()
            // and that would destroy the GL context under us (segfault).
            // main.c sees the request at the top of the next loop and shuts down cleanly.
            AppStateRequestQuit();
        }

        // -- Hint (drawn with plain raylib text, also screen space) ----------
        DrawText("Press ENTER or click PLAY to start", 20, 20, 20, RAYWHITE);
    }
    else // MENU_PAGE_OPTIONS
    {
        // -- BACK: return to the MAIN page (same as ESC, see Update) ----------
        if (GuiButton((Rectangle){ x, y, w, h }, "< BACK"))
        {
            AudioPlayButton();
            menuPage = MENU_PAGE_MAIN;
        }
        y += h + gap;

        // -- Slider: writes a float into &music_volume between min and max ----
        GuiLabel((Rectangle){ x, y, w, rh }, TextFormat("Volume: %.0f%%", settings->music_volume*100.0f));
        y += rh + 2.0f*s;

        float prevVol = settings->music_volume;   // snapshot to detect a drag this frame
        GuiSlider((Rectangle){ x + 10.0f*s, y, w - 20.0f*s, rh }, "0", "100", &settings->music_volume, 0.0f, 1.0f);
        // Slider writes straight into the singleton; push it to the audio engine every frame so the change is immediately audible.
        // Play the "huh" preview when the level actually changed (already at the new volume, so the user hears it).
        SettingsApplyVolume();
        if (settings->music_volume != prevVol) AudioPlayVolumePreview();
        y += rh + gap;

        // -- Window mode: 3-way selector, applied to the real window on change -
        // Bound directly to the singleton (window state is global, unlike gui_scale's wish-vs-effective dance).
        // SettingsApplyWindowMode reconfigures the window and rebuilds the letterbox only when the selection actually changes.
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

        // -- CheckBox: save settings to disk on quit (writes bool at &settings->persist)
        if (GuiCheckBox((Rectangle){ x, y, rh, rh }, "Persist settings on quit", &settings->persist))
            AudioPlayButton();
        y += rh + gap;

        // -- ToggleGroup: row of mutually-exclusive buttons; writes selected index into &activeTab. ";" separates the labels horizontally, "\n" seperates vertically.
        int prevDiff = settings->difficulty;
        GuiToggleGroup((Rectangle){ x, y, (w - gap*2.0f)/3.0f, h }, "Easy;Normal;Hard", &settings->difficulty);
        if (settings->difficulty != prevDiff) AudioPlayButton();
        y += h + gap;

        GuiLabel((Rectangle){ x, y, w, rh }, TextFormat("Difficulty index: %i", settings->difficulty));
        y += rh + gap;

        // -- GUI Scale: the toggle records the user's WISH in settings->gui_scale_wish.
        // The top of Gui() reads it, clamps to what fits, and publishes the actual scale to settings->gui_scale.
        // If the pick doesn't fit, show which size is actually rendering so the mismatch (e.g. picked Large, showing Medium) is visible rather than silent.
        const char *names[3] = { "Small", "Medium", "Large" };
        GuiLabel((Rectangle){ x, y, w, rh },
                 effective == desired ? "GUI Scale:"
                                      : TextFormat("GUI Scale: (%s fits)", names[effective - 1]));
        y += rh;
        int prevScaleWish = settings->gui_scale_wish;
        GuiToggleGroup((Rectangle){ x, y, (w - gap*2.0f)/3.0f, h }, "Small;Medium;Large", &settings->gui_scale_wish);
        if (settings->gui_scale_wish != prevScaleWish) AudioPlayButton();

        // -- Hint: how to leave this page ------------------------------------
        DrawText("Press ESC or click BACK to return", 20, 20, 20, RAYWHITE);
    }
}
