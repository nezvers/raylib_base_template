#include "raylib.h"
#include "../app_state/app_state.h"
#include "../screen_state/screen_state.h"
#include <stddef.h> // used for NULL

#define SOUND_EFFECT_IMPLEMENTATION
#define SOUND_EFFECT_RAYLIB_IMPLEMENTATION
#include "sound_effect_raylib.h"


// Forward declare functions
static void Enter();
static void Update();
static void Exit();
static void Draw();
static void Gui();
                               /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_sfx_example = {Enter, Exit, Update, Draw, Gui, "SFX Example"};

Sound button_sound;
Sound damage_sound;

SoundEffect button_sfx = {
    .volume = 1.f,
    .pitch_rand_min = 0.9f,
    .pitch_rand_max = 1.2f,
    .pitch_min = 0.5f,
    .pitch_max = 1.75f,
    .pitch_increment = 0.01f,     // Pitch change on fast retrigger
    .retrigger_treshold = 0.02f,  // Don't play if sooner than this
    .retrigger_interval = 0.5f,   // Applay pitch_increment
    .pitch_return = 1,
};

SoundEffect damage_sfx = {
    .volume = 0.5f,
    .pitch_rand_min = 0.9f,
    .pitch_rand_max = 1.2f,
    .pitch_min = 0.5f,
    .pitch_max = 1.75f,
    .pitch_increment = 0.01f,
    .retrigger_treshold = 0.02f,
    .retrigger_interval = 0.5f,
    .pitch_return = 1,
};


static void Enter(){
    // InitAudioDevice(); // Should be initialized by Settings
    // RESOURCES_PATH is a macro definition from compiler
    button_sound = LoadSound(RESOURCES_PATH "/sounds/button_sound.wav");
    damage_sound = LoadSound(RESOURCES_PATH "/sounds/damage_sound.wav");
}

static void Exit(){
    UnloadSound(button_sound);
    UnloadSound(damage_sound);
    // CloseAudioDevice(); // should be closed by Settings
}

static void Update(){
    f32 current_time = GetTime();
    f32 rand_f = (GetRandomValue(0, 1000000) / 1000000.f) / 1.0f;

    if (IsKeyPressed(KEY_SPACE)) {
        SoundEffectPlayRaylib(&button_sfx, &button_sound, current_time, rand_f);
    }

    if (IsKeyPressed(KEY_ENTER)) {
        SoundEffectPlayRaylib(&damage_sfx, &damage_sound, current_time, rand_f);
    }
}

static void Draw(){
    ScreenState *screen_state = ScreenStateGet();
    Vector2 target_size = ScreenStateTargetSize();
    DrawText("Space = button sound", 10, 10, 10, BLACK);
    DrawText("Enter = damage sound", 10, 50, 10, BLACK);
}

static void Gui() {

}