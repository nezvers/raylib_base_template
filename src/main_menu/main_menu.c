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
#include <stddef.h>
#include <math.h>

// raygui lives in src/include/ (on the include path). Its implementation must
// be compiled EXACTLY ONCE in the whole project - we do it here. If you add
// this same #define to another .c you will get duplicate-symbol link errors.
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

// The platformer state already exists (platformer_test.c). We reach it by
// extern so the "Play" button can transition into it - this is how you move
// between states from inside a state.
extern AppState app_state_platformer;

// --- Demo widget values (persist across frames -> file-scope statics) --------
static float musicVolume = 0.5f;        // driven by a GuiSlider
static bool fullscreenChecked = false;  // driven by a GuiCheckBox
static int activeTab = 0;               // driven by a GuiToggleGroup
static float animTime = 0.0f;           // accumulates for the animation demo

// ----------------------------------------------------------------------------
//  Enter: called once when the state becomes active (via AppStateTransition).
//  Good place to set the background color, load resources, reset variables.
// ----------------------------------------------------------------------------
static void Enter()
{
    ScreenState *screenState = ScreenStateGet();
    screenState->clear_color = (Color){ 25, 30, 40, 255 };  // dark blue-gray
    animTime = 0.0f;
}

// ----------------------------------------------------------------------------
//  Exit: called once when leaving this state (before the next Enter).
//  Free anything you loaded in Enter here.
// ----------------------------------------------------------------------------
static void Exit()
{
    // Nothing loaded in this demo, so nothing to free.
}

// ----------------------------------------------------------------------------
//  Update: pure logic, no drawing. Runs every frame before Draw/Gui.
// ----------------------------------------------------------------------------
static void Update()
{
    // GetFrameTime() = seconds since last frame. Multiply movement by it so
    // speed is frame-rate independent. (GetTime() = seconds since start.)
    animTime += GetFrameTime();

    // Keyboard input example: ENTER also starts the game.
    if (IsKeyPressed(KEY_ENTER))
    {
        AppStateTransition(&app_state_platformer);
    }
}

// ----------------------------------------------------------------------------
//  Draw: GAME SPACE (1280x720, scaled to the window). Raylib primitives here.
// ----------------------------------------------------------------------------
static void Draw()
{
    Vector2 size = ScreenStateTargetSize();   // {width, height} of game space
    float cx = size.x*0.5f;                   // horizontal center
    float cy = size.y*0.5f;                   // vertical center

    // -- Header Text: DrawText(text, x, y, fontSize, color) -------------------------
    const char *title = "MAIN MENU";
    int titleSize = 60;
    int titleWidth = MeasureText(title, titleSize);   // width in pixels
    DrawText(title, (int)(cx - titleWidth*0.5f), 80, titleSize, RAYWHITE);

    // -- Sub-Text: DrawText(text, x, y, fontSize, color) -------------------------
    const char *descr = "place of all the buttons";
    int descrSize = 20;
    int descrWidth = MeasureText(descr, descrSize);   // width in pixels
    DrawText(descr, (int)(cx - descrWidth*0.5f), 135, descrSize, RAYWHITE);

    // -- Rectangles ----------------------------------------------------------
    DrawRectangle((int)(cx - 200.0f), 170, 400, 4, SKYBLUE);            // filled
    DrawRectangleLines((int)(cx - 210.0f), 160, 420, 320, DARKGRAY);   // outline

    // -- Lines ---------------------------------------------------------------
    DrawLine(0, 0, (int)size.x, (int)size.y, (Color){ 60, 70, 90, 255 });
    DrawLine((int)size.x, 0, 0, (int)size.y, (Color){ 60, 70, 90, 255 });

    // -- Animation: a circle bobbing up/down using sinf + accumulated time ---
    float bob = sinf(animTime*2.0f)*40.0f;    // -40..+40 pixels
    DrawCircle((int)cx, (int)(cy + 120.0f + bob), 30.0f, ORANGE);
    DrawCircleLines((int)cx, (int)(cy + 120.0f + bob), 30.0f, RAYWHITE);

    // -- Mouse input in GAME space.
    Vector2 pos_mouse = ScreenStateMouseGame();
    DrawCircleLines((int)pos_mouse.x, (int)pos_mouse.y, 12.0f, LIME);
    DrawText(TextFormat("mouse(screen): %.0f, %.0f", pos_mouse.x, pos_mouse.y),
             20, (int)size.y - 60, 20, GRAY);

    // -- FPS + timing readout ------------------------------------------------
    DrawText(TextFormat("GetTime(): %.1fs   FPS: %i", GetTime(), GetFPS()),
             20, (int)size.y - 30, 20, GRAY);
}

// ----------------------------------------------------------------------------
//  Gui: SCREEN SPACE (real window pixels). raygui widgets / HUD here.
//  raygui functions return true / write into a pointer; you react to that.
// ----------------------------------------------------------------------------
static void Gui()
{
    // Layout anchored to the real window size (not game space).
    float screenW = (float)GetScreenWidth();
    float x = screenW - 260.0f;   // right-hand column
    float y = 120.0f;
    float w = 220.0f;
    float h = 36.0f;
    float gap = 12.0f;

    // -- Label: static text, no interaction ----------------------------------
    GuiLabel((Rectangle){ x, y, w, h }, "--- raygui widgets ---");
    y += h + gap;

    // -- Button: returns non-zero (true) on click ----------------------------
    if (GuiButton((Rectangle){ x, y, w, h }, "PLAY (-> platformer)"))
    {
        // Transition to another state. Exit() of this state runs, then the
        // platformer's Enter(). This is the core of switching screens.
        AppStateTransition(&app_state_platformer);
    }
    y += h + gap;

    if (GuiButton((Rectangle){ x, y, w, h }, "OPTIONS (no-op)"))
    {
        // Placeholder: a real menu would transition to an options state.
    }
    y += h + gap;

    if (GuiButton((Rectangle){ x, y, w, h }, "QUIT"))
    {
        // Requesting exit: closes the window; the main loop's WindowShouldClose()
        // then returns true and the program shuts down cleanly.
        CloseWindow();
    }
    y += h + gap*2.0f;

    // -- Slider: writes a float into &musicVolume between min and max ---------
    GuiLabel((Rectangle){ x, y, w, 20.0f },
             TextFormat("Volume: %.0f%%", musicVolume*100.0f));
    y += 22.0f;
    GuiSlider((Rectangle){ x + 10.0f, y, w - 20.0f, 20.0f },
              "0", "100", &musicVolume, 0.0f, 1.0f);
    y += 20.0f + gap;

    // -- CheckBox: toggles the bool at &fullscreenChecked --------------------
    GuiCheckBox((Rectangle){ x, y, 24.0f, 24.0f }, "Fullscreen (demo flag)",
                &fullscreenChecked);
    y += 24.0f + gap;

    // -- ToggleGroup: row of mutually-exclusive buttons; writes selected index into &activeTab. ";" separates the labels horizontally, "\n" seperates vertically.
    GuiToggleGroup((Rectangle){ x, y, (w - gap*2.0f)/3.0f, h },
                   "Easy;Normal;Hard", &activeTab);
    y += h + gap;
    GuiLabel((Rectangle){ x, y, w, 20.0f },
             TextFormat("Difficulty index: %i", activeTab));

    // -- Hint (drawn with plain raylib text, also screen space) --------------
    DrawText("Press ENTER or click PLAY to start", 20, 20, 20, RAYWHITE);
}
