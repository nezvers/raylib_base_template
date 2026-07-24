#include "raylib.h"

#if defined(PLATFORM_WEB)
    #include <emscripten/emscripten.h>      // Emscripten library
#endif

#include <stdio.h>                          // Required for: printf()

#include "screen_state/screen_state.h"
#include "settings_state/settings_state.h"
#include "app_state/app_state.h"
#include "audio_state/audio_state.h"
#include "anim/signal.h"
#include "anim/anim_stage.h"

#define TEMPORARY_ALLOCATOR_IMPLEMENTATION
#define TEMPORARY_ALLOCATOR_SIZE (1024 * 1024)
#include "temporary_allocator.h"


// Simple log system to avoid printf() calls if required
// NOTE: Avoiding those calls, also avoids const strings memory usage
#define SUPPORT_LOG_INFO
#if defined(SUPPORT_LOG_INFO)
    #define LOG(...) printf(__VA_ARGS__)
#else
    #define LOG(...)
#endif


static void UpdateDrawFrame(void);      // Update and Draw one frame


int main(void)
{
#if !defined(_DEBUG)
    SetTraceLogLevel(LOG_NONE);         // Disable raylib trace log messages
#endif
#if defined(PLATFORM_WEB)
    // FLAG_WINDOW_RESIZABLE breaks canvas presentation on Web (raylib 6.0):
    // the frame renders but never reaches the screen -> black canvas.
    SetConfigFlags(FLAG_VSYNC_HINT);
#else
    SetConfigFlags(FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE);
#endif
    SettingsReset();    // defaults for Settings + Screen + Audio -> SettingsGet()
    SettingsLoad();     // override defaults from settings.cfg (+ future screen load)
    SignalReset();      // clear the named-signal bus (anim system listeners)
    AnimStageReset();   // clear the animation playback slots (anim_stage.h)

    ScreenState *screen_state = ScreenStateGet();
    InitWindow(screen_state->width, screen_state->height, "raylib gamejam template");
    SetExitKey(KEY_NULL);   // disable ESC=quit; states own ESC (main menu: Options->Main, else quit)
    ScreenStateResize();

    InitAudioDevice();
    AudioStateLoad();   // load UI sounds once (needs audio device)

    SettingsApply();    // apply saved/default window mode + volume + letterbox resize
    
    // TODO: Load resources / Initialize variables at this point
    // Check app_state.h for "public" AppStates
    AppStateTransition(&app_state_main_menu);
    

#if defined(PLATFORM_WEB)
    emscripten_set_main_loop(UpdateDrawFrame, 60, 1);
#else
    SetTargetFPS(60);     // Set our game frames-per-second

    // Main game loop
    // Detect window close button
    // Exit on the window close button OR a state's quit request (see app_state.h).
    while (!WindowShouldClose() && !AppStateShouldQuit()) {
        UpdateDrawFrame();
    }
#endif

    if (SettingsGet()->persist) SettingsSave();   // remember options for next launch

    AppStateExit();
    ScreenStateCleanup();
    // TODO: Unload all loaded resources at this point
    AudioStateUnload();   // free UI sounds before tearing down the audio device
    CloseAudioDevice();

    CloseWindow();        // Close window and OpenGL context
    return 0;
}

// Update and draw frame
void UpdateDrawFrame(void)
{
    if (IsWindowResized()) {
        ScreenStateResize();
    }
    AppStateUpdate();

    // Draw
    ScreenState *screen_state = ScreenStateGet();
    BeginTextureMode(screen_state->target);
        ClearBackground(screen_state->clear_color);
        AppStateDraw();
        
    EndTextureMode();
    
    // Render to screen (main framebuffer)
    BeginDrawing();
        ClearBackground(BLACK); // TODO: screen_state
        ScreenStateDrawTarget();
        AppStateGui();

    EndDrawing();

    TempAllocReset();
}
