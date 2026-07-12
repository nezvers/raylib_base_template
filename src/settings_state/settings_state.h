#ifndef SETTINGS_STATE_H
#define SETTINGS_STATE_H

#include <stdbool.h>

// ----------------------------------------------------------------------------
//  Settings: global user-selected options, shared across ALL app states.
//
//  Same singleton pattern as ScreenState (see screen_state.h): one file-static
//  instance, reached through SettingsGet(). Any state can read/write it, so a
//  choice made in the main menu (e.g. gui_scale, volume) is visible everywhere.
// ----------------------------------------------------------------------------
// Window mode: how the window fills (or doesn't fill) the monitor.
// Stored as int in Settings so it binds directly to GuiToggleGroup's int *active.
typedef enum {
    WINDOW_MODE_WINDOWED = 0,   // 1280x720 window, resizable
    WINDOW_MODE_FULLSCREEN,     // real fullscreen (changes monitor video mode)
    WINDOW_MODE_BORDERLESS,     // "fullscreen (windowed)": borderless at desktop res
} WindowMode;

typedef struct {
    int gui_scale_wish;   // user's GUI-scale pick: 0=Small 1=Medium 2=Large (persisted)
    float gui_scale;      // runtime EFFECTIVE scale 1/2/3 (clamped to fit; recomputed, not persisted)
    int window_mode;      // one of WindowMode; default WINDOW_MODE_WINDOWED
    float music_volume;   // 0.0..1.0; drives master volume (SetMasterVolume)
    int difficulty;       // 0=Easy 1=Normal 2=Hard
    bool persist;         // if true, settings are saved to disk on quit
} Settings;

Settings *SettingsGet();   // pointer to the singleton (mutate through it)
void SettingsReset();      // restore defaults (gui_scale = 1.0f, windowed, no persist)

// Reconfigure the ACTUAL window to `mode` (one of WindowMode) and store it.
// Idempotent: normalizes to windowed first, then applies the target, because
// raylib's ToggleFullscreen/ToggleBorderlessWindowed are relative toggles.
void SettingsApplyWindowMode(int mode);

// Push the stored master volume (music_volume) to the audio engine.
// Single owner of SetMasterVolume; needs InitAudioDevice() first.
void SettingsApplyVolume();

// Apply ALL stored settings to the live engine: volume (always) + window mode
// (only when it differs from the actual window, to avoid flicker) + letterbox
// resize. Call at boot (after window + audio init) and after any GUI change.
void SettingsApply();

// Disk persistence (plain-text file, one "key value" per line).
void SettingsSave();   // write the singleton to disk
bool SettingsLoad();   // read into the singleton; false if no/invalid file (defaults kept)

#endif // SETTINGS_STATE_H
