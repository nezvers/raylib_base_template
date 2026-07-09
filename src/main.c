/*******************************************************************************************
*
*   raylib gamejam template
*
*   Code licensed under an unmodified zlib/libpng license, which is an OSI-certified,
*   BSD-like license that allows static linking with closed source software
*
*   Copyright (c) 2022-2026 Ramon Santamaria (@raysan5)
*
********************************************************************************************/

#include "raylib.h"

#if defined(PLATFORM_WEB)
    #include <emscripten/emscripten.h>      // Emscripten library
#endif

#include <stdio.h>                          // Required for: printf()
#include <stdlib.h>                         // Required for: 
#include <string.h>                         // Required for:
#include "box2d/box2d.h"

#include "screen_state/screen_state.h"

//----------------------------------------------------------------------------------
// Defines and Macros
//----------------------------------------------------------------------------------
// Simple log system to avoid printf() calls if required
// NOTE: Avoiding those calls, also avoids const strings memory usage
#define SUPPORT_LOG_INFO
#if defined(SUPPORT_LOG_INFO)
    #define LOG(...) printf(__VA_ARGS__)
#else
    #define LOG(...)
#endif

//----------------------------------------------------------------------------------
// Types and Structures Definition
//----------------------------------------------------------------------------------
typedef enum { 
    SCREEN_LOGO = 0, 
    SCREEN_TITLE, 
    SCREEN_GAMEPLAY, 
    SCREEN_ENDING
} GameScreen;

// TODO: Define your custom data types here

//----------------------------------------------------------------------------------
// Global Variables Definition (local to this module)
//----------------------------------------------------------------------------------


static int screenWidth = 720;
static int screenHeight = 720;

static RenderTexture2D target = { 0 };  // Render texture to render our game
static int frameCounter = 0;

// TODO: Define global variables here, recommended to make them static

//----------------------------------------------------------------------------------
// Module Functions Declaration
//----------------------------------------------------------------------------------
static void UpdateDrawFrame(void);      // Update and Draw one frame

//------------------------------------------------------------------------------------
// Program main entry point
//------------------------------------------------------------------------------------
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
    

#if defined(PLATFORM_WEB)
    emscripten_set_main_loop(UpdateDrawFrame, 60, 1);
#else
    SetTargetFPS(60);     // Set our game frames-per-second
    //--------------------------------------------------------------------------------------

    // Main game loop
    while (!WindowShouldClose())    // Detect window close button
    {
        UpdateDrawFrame();
    }
#endif

    // De-Initialization
    //--------------------------------------------------------------------------------------
    ScreenStateCleanup();
    
    // TODO: Unload all loaded resources at this point

    CloseWindow();        // Close window and OpenGL context
    //--------------------------------------------------------------------------------------

    return 0;
}

//--------------------------------------------------------------------------------------------
// Module Functions Definition
//--------------------------------------------------------------------------------------------
// Update and draw frame
void UpdateDrawFrame(void)
{
    if (IsWindowResized()) {
        ScreenStateResize();
    }
    // Update
    //----------------------------------------------------------------------------------
    // TODO: Update variables / Implement example logic at this point
   
    frameCounter++;
    //----------------------------------------------------------------------------------

    // Draw
    //----------------------------------------------------------------------------------
    ScreenState *screen_state = ScreenStateGet();
    BeginTextureMode(screen_state->target);
        ClearBackground(RAYWHITE);
        
        // TODO: Draw your game screen here
        Vector2 target_size = ScreenStateTargetSize();
        DrawRectangle(0, 0, target_size.x, target_size.y, BLACK);
        DrawRectangle(10, 10, target_size.x -20, target_size.y -20, RAYWHITE);
        DrawText("raylib", 30, 30, 40, BLACK);

        // DrawText("6.x", 290, 90 - 26, 280, BLACK);
        // DrawText("GAMEJAM", 70, 90 + 210, 120, MAROON);

        // if ((frameCounter/20)%2) DrawText("are you ready?", 160, 500, 50, BLACK);
        
        // DrawRectangleLinesEx((Rectangle){ 0, 0, screen_state->width, screen_state->height }, 16, BLACK);
        
    EndTextureMode();
    
    // Render to screen (main framebuffer)
    BeginDrawing();
        ClearBackground(RAYWHITE);
        ScreenStateDrawTarget();
        
        // // Draw render texture to screen, scaled if required
        // DrawTexturePro(screen_state->target.texture, (Rectangle){ 0, 0, (float)screen_state->target.texture.width, -(float)screen_state->target.texture.height }, 
        //     (Rectangle){ 0, 0, (float)screen_state->target.texture.width, (float)screen_state->target.texture.height }, (Vector2){ 0, 0 }, 0.0f, WHITE);

        // TODO: Draw everything that requires to be drawn at this point, maybe UI?

    EndDrawing();
    //----------------------------------------------------------------------------------  
}
