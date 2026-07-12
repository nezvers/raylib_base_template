#include "audio_state.h"

static AudioState state;

AudioState *AudioStateGet() {
    return &state;
}

void AudioStateReset() {
    state.loaded = false;   // volume lives in Settings; nothing to reset here yet
}

void AudioStateLoad() {
    // RESOURCES_PATH is a compiler macro (see CMakeLists.txt) and 
    // it already ends in '/'; existing code uses the leading-slash form (sprite_example.c).
    state.button = LoadSound(RESOURCES_PATH"/sounds/button_sound.wav");
    state.volume = LoadSound(RESOURCES_PATH"/sounds/snd_huh_jump.wav");
    state.loaded = true;
}

void AudioStateUnload() {
    if (!state.loaded) return;
    UnloadSound(state.button);
    UnloadSound(state.volume);
    state.loaded = false;
}

void AudioPlayButton() {
    if (!state.loaded) return;   // safe no-op before load / after unload
    PlaySound(state.button);
}

void AudioPlayVolumePreview() {
    if (!state.loaded) return;
    PlaySound(state.volume);
}
