#ifndef SETTINGS_STATE_H
#define SETTINGS_STATE_H

// ----------------------------------------------------------------------------
//  Settings: global user-selected options, shared across ALL app states.
//
//  Same singleton pattern as ScreenState (see screen_state.h): one file-static
//  instance, reached through SettingsGet(). Any state can read/write it, so a
//  choice made in the main menu (e.g. gui_scale, volume) is visible everywhere.
// ----------------------------------------------------------------------------
typedef struct {
    float gui_scale;   // 1.0 = default; multiplies raygui font/icon/widget sizes
    // Future home for other user options currently living as statics in
    // main_menu.c (music_volume, difficulty, fullscreen). Not migrated yet.
} Settings;

Settings *SettingsGet();   // pointer to the singleton (mutate through it)
void SettingsReset();      // restore defaults (gui_scale = 1.0f)

#endif // SETTINGS_STATE_H
