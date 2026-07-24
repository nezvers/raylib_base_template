#include "settings_state.h"
#include "raylib.h"
#include "../screen_state/screen_state.h"
#include "../audio_state/audio_state.h"
#include <stdio.h>

// Binary persistence via the project's SimpleSave system. The header is
// header-only; its IMPLEMENTATION is already compiled into this binary by
// examples/simple_save_example.c, so we include for DECLARATIONS ONLY here (a
// second SIMPLE_SAVE_IMPLEMENTATION would be a duplicate-symbol link error).
#include "simple_save.h"

#define SETTINGS_FILE "settings.sav"

// On-disk layout: ONLY the persisted fields, in a fixed order, behind a version
// tag. The effective gui_scale is recomputed every frame and never stored.
// Bump SETTINGS_SAVE_VERSION whenever this struct's layout changes - a mismatch
// makes SettingsLoad fall back to defaults instead of reading garbage.
#define SETTINGS_SAVE_VERSION 1
typedef struct {
    int   version;
    int   gui_scale_wish;
    int   window_mode;
    float music_volume;
    int   difficulty;
    int   persist;        // stored as int for a stable, padding-free layout
} SettingsSaveData;

static Settings state;

Settings *SettingsGet() {
    return &state;
}

void SettingsReset() {
    state.gui_scale_wish = 0;   // Small
    state.gui_scale = 1.0f;     // effective; recomputed each frame
    state.window_mode = WINDOW_MODE_WINDOWED;
    state.music_volume = 0.5f;
    state.difficulty = 0;
    state.persist = false;

    // Settings orchestrates the layer below it: reset its subsystems to defaults too.
    ScreenStateReset();
    AudioStateReset();
}

void SettingsSave() {
    SettingsSaveData data = {
        .version        = SETTINGS_SAVE_VERSION,
        .gui_scale_wish = state.gui_scale_wish,   // persist the WISH, not effective
        .window_mode    = state.window_mode,
        .music_volume   = state.music_volume,
        .difficulty     = state.difficulty,
        .persist        = state.persist ? 1 : 0,
    };
    SimpleSave(SETTINGS_FILE, (char *)&data, sizeof(data));
}

bool SettingsLoad() {
    SettingsSaveData data = {0};
    // No file / short read -> keep defaults. A version mismatch means the layout
    // changed under an old save: ignore it rather than read garbage.
    if (!SimpleLoad(SETTINGS_FILE, (char *)&data, sizeof(data))) return false;
    if (data.version != SETTINGS_SAVE_VERSION) return false;

    state.gui_scale_wish = data.gui_scale_wish;
    state.window_mode    = data.window_mode;
    state.music_volume   = data.music_volume;
    state.difficulty     = data.difficulty;
    state.persist        = (data.persist != 0);

    // Guard against a corrupt file putting us in an invalid state.
    if (state.window_mode < WINDOW_MODE_WINDOWED ||
        state.window_mode > WINDOW_MODE_BORDERLESS) {
        state.window_mode = WINDOW_MODE_WINDOWED;
    }
    if (state.difficulty < 0 || state.difficulty > 2) state.difficulty = 0;
    if (state.gui_scale_wish < 0 || state.gui_scale_wish > 2) state.gui_scale_wish = 0;
    if (state.music_volume < 0.0f) state.music_volume = 0.0f;
    if (state.music_volume > 1.0f) state.music_volume = 1.0f;

    ScreenStateLoad();   // load any persisted screen state (no-op today)
    return true;
}

void SettingsApplyWindowMode(int mode) {
    // 1. Normalize to a plain windowed baseline. raylib's toggles are relative,
    //    so we clear whatever we're in before applying the target.
    if (IsWindowFullscreen()) ToggleFullscreen();
    if (IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE)) ToggleBorderlessWindowed();

    // 2. Apply the target mode.
    switch (mode) {
        case WINDOW_MODE_WINDOWED: {
            ScreenState *ss = ScreenStateGet();
            SetWindowSize(ss->width, ss->height);  // back to 1280x720
            break;
        }
        case WINDOW_MODE_FULLSCREEN: {
            // Real fullscreen matches the video mode to the window size, so size
            // the window to the monitor first -> native-res fullscreen, not a
            // stretched 720p.
            int m = GetCurrentMonitor();
            SetWindowSize(GetMonitorWidth(m), GetMonitorHeight(m));
            ToggleFullscreen();
            break;
        }
        case WINDOW_MODE_BORDERLESS:
            ToggleBorderlessWindowed();  // resizes to monitor res automatically
            break;
    }

    state.window_mode = mode;
    ScreenStateResize();  // rebuild letterbox/render-texture for the new size
}

// What window mode the ACTUAL window is currently in, so SettingsApply can
// reapply the stored mode only when reality disagrees (avoids re-toggling
// fullscreen/borderless -> visible flicker on every settings change).
static int CurrentActualWindowMode() {
    if (IsWindowFullscreen()) return WINDOW_MODE_FULLSCREEN;
    if (IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE)) return WINDOW_MODE_BORDERLESS;
    return WINDOW_MODE_WINDOWED;
}

void SettingsApplyVolume() {
    SetMasterVolume(state.music_volume);   // single owner of the volume push
}

void SettingsApply() {
    SettingsApplyVolume();   // always cheap, no flicker

    if (state.window_mode != CurrentActualWindowMode()) {
        SettingsApplyWindowMode(state.window_mode);   // also rebuilds letterbox
    } else {
        ScreenStateResize();   // mode already correct; still rebuild for boot / gui_scale
    }
}
