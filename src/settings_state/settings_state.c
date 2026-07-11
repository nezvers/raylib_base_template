#include "settings_state.h"

static Settings state;

Settings *SettingsGet() {
    return &state;
}

void SettingsReset() {
    state.gui_scale = 1.0f;
}
