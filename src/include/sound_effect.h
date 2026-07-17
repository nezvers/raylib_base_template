#ifndef SOUND_EFFECT_H
#define SOUND_EFFECT_H

#include <stdint.h>

typedef struct {
    float volume;
    float pitch_rand_min;
    float pitch_rand_max;
    float pitch_min;
    float pitch_max;
    float pitch_increment;    // For fast repeated trigger
    float retrigger_treshold; // How soon can be triggered again
    float retrigger_interval; // If triggered in this time again, a pitch is added
    float pitch_return;       // Time to return to original pitch
    // Calculated at trigger
    float pitch;              // Current pitch
    double last_time;          // Keep track of trigger time
} SoundEffect;

// Remove name mangling for C++
#ifdef __cplusplus
extern "C" {
#endif

#ifdef STATIC_API
#define SERAPI static
#else
#define SERAPI
#endif

SERAPI bool SoundEffectPlay(SoundEffect *sound, double time_seconds, float rand_f);

#ifdef __cplusplus
}
#endif

#endif // SOUND_EFFECT_H

/* _________________________________________________________________ */

#ifdef SOUND_EFFECT_IMPLEMENTATION
#undef SOUND_EFFECT_IMPLEMENTATION

static inline float SoundEffectLerp(float a, float b, float t) {
    return a + (b - a) * t;
}

static void SoundEffectUpdatePitch(SoundEffect *sound, float delta, float rand_f) {
    if (delta < sound->retrigger_interval) {
        if (sound->pitch > sound->pitch_max) {
            sound->pitch = sound->pitch_max + (SoundEffectLerp(sound->pitch_rand_min, sound->pitch_rand_max, rand_f) - 1);
        }
        else if (sound->pitch < sound->pitch_min) {
            sound->pitch = sound->pitch_min + (SoundEffectLerp(sound->pitch_rand_min, sound->pitch_rand_max, rand_f) - 1);
        }
    }
    else if (delta < sound->retrigger_interval + sound->pitch_return) {
        float pitch_default = SoundEffectLerp(sound->pitch_rand_min, sound->pitch_rand_max, 0.5f);
        float t = (delta - sound->retrigger_interval) / sound->pitch_return;
        sound->pitch = SoundEffectLerp(sound->pitch, pitch_default, t);
    } else {
        sound->pitch = SoundEffectLerp(sound->pitch_rand_min, sound->pitch_rand_max, rand_f);
    }
}

SERAPI bool SoundEffectPlay(SoundEffect *sound, double time_seconds, float rand_f) {
    if (time_seconds < sound->last_time + sound->retrigger_treshold) {
        return false;
    }
    float delta = (time_seconds - sound->last_time);
    sound->last_time = time_seconds;
    // GetPitch
    
    return true;
}

#endif // SOUND_EFFECT_IMPLEMENTATION

#undef SERAPI