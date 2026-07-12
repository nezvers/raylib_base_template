#include "settings_state.h"
#include "raylib.h"
#include "../screen_state/screen_state.h"
#include <stdio.h>

#define SETTINGS_FILE "settings.cfg"

static Settings state;

Settings *SettingsGet() {
    return &state;
}

void SettingsReset() {
    state.gui_scale = 1.0f;
    state.window_mode = WINDOW_MODE_WINDOWED;
    state.music_volume = 0.5f;
    state.difficulty = 0;
    state.persist = false;
}

void SettingsSave() {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (f == NULL) return;
    fprintf(f, "gui_scale %f\n",    state.gui_scale);
    fprintf(f, "window_mode %d\n",  state.window_mode);
    fprintf(f, "music_volume %f\n", state.music_volume);
    fprintf(f, "difficulty %d\n",   state.difficulty);
    fprintf(f, "persist %d\n",      state.persist ? 1 : 0);
    fclose(f);
}

bool SettingsLoad() {
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (f == NULL) return false;   // no file yet -> keep defaults

    // Read known keys; anything missing/malformed just keeps its default value.
    char key[64];
    while (fscanf(f, "%63s", key) == 1) {
        if      (TextIsEqual(key, "gui_scale"))    fscanf(f, "%f", &state.gui_scale);
        else if (TextIsEqual(key, "window_mode"))  fscanf(f, "%d", &state.window_mode);
        else if (TextIsEqual(key, "music_volume")) fscanf(f, "%f", &state.music_volume);
        else if (TextIsEqual(key, "difficulty"))   fscanf(f, "%d", &state.difficulty);
        else if (TextIsEqual(key, "persist")) {
            int v = 0;
            if (fscanf(f, "%d", &v) == 1) state.persist = (v != 0);
        }
        else fscanf(f, "%*s");   // unknown key: skip its value
    }
    fclose(f);

    // Guard against a hand-edited/corrupt file putting us in an invalid state.
    if (state.window_mode < WINDOW_MODE_WINDOWED ||
        state.window_mode > WINDOW_MODE_BORDERLESS) {
        state.window_mode = WINDOW_MODE_WINDOWED;
    }
    if (state.difficulty < 0 || state.difficulty > 2) state.difficulty = 0;
    if (state.music_volume < 0.0f) state.music_volume = 0.0f;
    if (state.music_volume > 1.0f) state.music_volume = 1.0f;
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
            SetWindowSize(ss->game_width, ss->game_height);  // back to 1280x720
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
