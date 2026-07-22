#include "raylib.h"
#include "../../app_state/app_state.h"
#include "../../screen_state/screen_state.h"
#include <stddef.h>

// Forward declare functions
static void Enter();
static void Update();
static void Exit();
static void Draw();
static void Gui();
                               /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_post_process_shader = {Enter, Exit, Update, Draw, Gui, "Post-process shader example"};

#if defined(PLATFORM_DESKTOP)
    #define GLSL_VERSION            330
#else   // PLATFORM_ANDROID, PLATFORM_WEB
    #define GLSL_VERSION            100
#endif

#define SHADER_COUNT         13 // 12 + NONE
static Shader shaders[SHADER_COUNT] = { 0 };
static const char *shader_list[SHADER_COUNT] = {
    "base",         // Default shader that's already used by raylib on everything drawn
    "posterization",
    "grayscale",
    "dream_vision",
    "pixelizer",
    "cross_hatching",
    "cross_stitching",
    "predator",
    "scanlines",
    "fisheye",
    "sobel",
    "bloom",
    "blur",
};
static int shader_index = 0; // Default to NONE

static Camera camera = { 0 };

static void Enter(){
    // don't include NONE for example
    for (int i = 0; i < SHADER_COUNT; i += 1) {
        // NOTE: Defining 0 (NULL) for vertex shader forces usage of internal default vertex shader
        shaders[i] =  LoadShader(0, TextFormat(RESOURCES_PATH"shaders/glsl%i/%s.fs",  GLSL_VERSION, shader_list[i]));
    }
    ScreenStateShader(&shaders[shader_index]);

    ScreenState *screen_state = ScreenStateGet();
    screen_state->clear_color = WHITE;

    
    camera.position = (Vector3){ 2.0f, 3.0f, 2.0f };    // Camera position
    camera.target = (Vector3){ 0.0f, 1.0f, 0.0f };      // Camera looking at point
    camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };          // Camera up vector (rotation towards target)
    camera.fovy = 45.0f;                                // Camera field-of-view Y
    camera.projection = CAMERA_PERSPECTIVE;             // Camera projection type
}

static void Exit(){

}

static void Update(){
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) {
        shader_index = (shader_index + 1) % SHADER_COUNT;
        ScreenStateShader(&shaders[shader_index]);
    }
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A)) {
        shader_index = (shader_index + SHADER_COUNT -1) % SHADER_COUNT;
        ScreenStateShader(&shaders[shader_index]);
    }

    // Raylib utility function
    UpdateCamera(&camera, CAMERA_ORBITAL);
}

static void Draw(){
    BeginMode3D(camera);        // Begin 3d mode drawing

    DrawGrid(10, 1.0f);     // Draw a grid
    // TODO: Draw colorful shapes
    DrawCubeV((Vector3){0, 0.5f, 0}, (Vector3){1,1,1}, BLUE);
    DrawCubeV((Vector3){2, 0.5f, 1}, (Vector3){1,1,1}, YELLOW);
    DrawCubeV((Vector3){-2,0.5f, -1}, (Vector3){1,1,1}, RED);
    DrawCubeV((Vector3){1, 0.5f, -2}, (Vector3){1,1,1}, PINK);
    DrawCubeV((Vector3){-1,0.5f, 2}, (Vector3){1,1,1}, LIME);

    EndMode3D();                // End 3d mode drawing, returns to orthographic 2d mode
}

static void Gui() {
    const char *text = TextFormat("%s", shader_list[shader_index]);
    DrawText(text, 20+2, 10, 40, BLACK);
    DrawText(text, 20-2, 10, 40, BLACK);
    DrawText(text, 20, 10+2, 40, BLACK);
    DrawText(text, 20, 10-2, 40, BLACK);
    DrawText(text, 20, 10,   40, WHITE);
}