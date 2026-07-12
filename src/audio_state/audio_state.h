#ifndef AUDIO_STATE_H
#define AUDIO_STATE_H

#include "raylib.h"
#include <stdbool.h>

// ----------------------------------------------------------------------------
//  AudioState: loaded-once UI sounds, shared across ALL app states.
//
//  Same singleton pattern as ScreenState/Settings (see screen_state.h): one
//  file-static instance, reached through AudioStateGet(). The sounds are loaded
//  once at startup (after InitAudioDevice) rather than per menu Enter(), so
//  entering/leaving states doesn't reload the wavs.
// ----------------------------------------------------------------------------
typedef struct {
    Sound button;   // click feedback for buttons/checkboxes/toggles
    Sound volume;   // volume-change preview ("huh") so the user hears the new level
    bool  loaded;   // guards AudioPlay* if called before AudioStateLoad()
} AudioState;

AudioState *AudioStateGet();   // pointer to the singleton

void AudioStateLoad();     // LoadSound the wavs; call once after InitAudioDevice()
void AudioStateUnload();   // UnloadSound; call before CloseAudioDevice()

void AudioPlayButton();          // play the button click
void AudioPlayVolumePreview();   // play the volume "huh"

#endif // AUDIO_STATE_H
