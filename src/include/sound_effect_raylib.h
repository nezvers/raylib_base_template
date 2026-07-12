#ifndef SOUND_EFFECT_RAYLIB_H
#define SOUND_EFFECT_RAYLIB_H

#include "raylib.h"
#include "sound_effect.h"



// Remove name mangling for C++
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    SoundEffect sound_effect;
    Sound sound;
} SoundEffectRaylib;

void SoundEffectInit(SoundEffect *sound_effect, Sound *sound);
void SoundEffectInitRaylib(SoundEffectRaylib *sound_effect);
void SoundEffectPlayRaylib(SoundEffectRaylib *sound_effect, f64 time_seconds, f32 rand_f);
void SoundEffectPlayRaylibAlt(SoundEffect *sound_effect, Sound *sound, f64 time_seconds, f32 rand_f);

#ifdef __cplusplus
}
#endif

#endif // SOUND_EFFECT_RAYLIB_H

/* _________________________________________________________________ */

#ifdef SOUND_EFFECT_RAYLIB_IMPLEMENTATION
#undef SOUND_EFFECT_RAYLIB_IMPLEMENTATION

void SoundEffectInit(SoundEffect *sound_effect, Sound *sound) {
    SetSoundVolume(*sound, sound_effect->volume);
}

void SoundEffectInitRaylib(SoundEffectRaylib *sound_effect) {
    SetSoundVolume(sound_effect->sound, sound_effect->sound_effect.volume);
}

void SoundEffectPlayRaylibAlt(SoundEffect *sound_effect, Sound *sound, f64 time_seconds, f32 rand_f) {
    if (!SoundEffectPlay(sound_effect, time_seconds, rand_f)) {
        return;
    }
    // Not neccessary to change volume each time
    // rl.SetSoundVolume(sound^, sound_effect.volume)
    SetSoundPitch(*sound, sound_effect->pitch);
    PlaySound(*sound);
}

void SoundEffectPlayRaylib(SoundEffectRaylib *sound_effect, f64 time_seconds, f32 rand_f) {
    if (!SoundEffectPlay(&sound_effect->sound_effect, time_seconds, rand_f)) {
        return;
    }
    // Not neccessary to change volume each time
    // rl.SetSoundVolume(sound^, sound_effect.volume)
    SetSoundPitch(sound_effect->sound, sound_effect->sound_effect.pitch);
    PlaySound(sound_effect->sound);
}

#endif // SOUND_EFFECT_IMPLEMENTATION