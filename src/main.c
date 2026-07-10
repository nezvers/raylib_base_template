#include "raylib.h"

#if defined(PLATFORM_WEB)
    #include <emscripten/emscripten.h>      // Emscripten library
#endif

#include <stdio.h>                          // Required for: printf()

#include "screen_state/screen_state.h"
#include "app_state/app_state.h"


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
    ScreenState *screen_state = ScreenStateGet();
    // TODO: place to load screen settings
    ScreenStateReset();
    SetConfigFlags(FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE | FLAG_BORDERLESS_WINDOWED_MODE);

    InitWindow(screen_state->width, screen_state->height, "raylib gamejam template");
    ScreenStateResize();
    InitAudioDevice();
    SetMasterVolume(100.0);
    
    // TODO: Load resources / Initialize variables at this point
    extern AppState app_state_platformer;
    AppStateTransition(&app_state_platformer);
    

#if defined(PLATFORM_WEB)
    emscripten_set_main_loop(UpdateDrawFrame, 60, 1);
#else
    SetTargetFPS(60);     // Set our game frames-per-second

    // Main game loop
    // Detect window close button
    while (!WindowShouldClose()) {
        UpdateDrawFrame();
    }
#endif

    ScreenStateCleanup();
    // TODO: Unload all loaded resources at this point

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
}
