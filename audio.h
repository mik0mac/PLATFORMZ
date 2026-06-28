#pragma once

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <vector>
#include <cmath>

#include "random.h"
#include "constants.h"

class audioFX {
public:
    audioFX(const std::string& file, float volume = 1.0f, bool localOnly = false, bool spacial = true)
        : filename(file), baseVolume(volume), localPlayerOnly(localOnly), spacial(spacial) {}

    float baseVolume;
    bool localPlayerOnly;
    bool spacial;

    void load() {
        sound = LoadSound(filename.c_str());
        SetSoundVolume(sound, baseVolume);
    }

    void unload() {
        UnloadSound(sound);
    }

    void trigger(Vector3 sourcePos, Vector3 listenerPos, Vector3 listenerDirection, bool spacial = true) {
        // for sounds that don't need to exist in 3D space.
        if (!spacial) {
            SetSoundVolume(sound, baseVolume);
            SetSoundPan(sound, 0.5f);
            PlaySound(sound);
            return;
        }

        float dist = Vector3Distance(sourcePos, listenerPos);

        float volume = 1.0f - dist / AUDIO_MAX_DISTANCE; // linear falloff
        volume = Clamp(volume, AUDIO_MIN_VOLUME, 1.0f);

        float dx = sourcePos.x - listenerDirection.x;
        float pan = (Clamp(dx / AUDIO_MAX_DISTANCE, -1.0f, 1.0f) + 1.0f) * 0.5f;

        //testing
        // volume = 1.0f;
        // pan = 0.5f;
        SetSoundVolume(sound, volume * baseVolume);
        SetSoundPan(sound, pan);
        PlaySound(sound);
    }

private:
    Sound sound;
    std::string filename;
};

// ---------------------------------------------------------------------------
// Audio event queue
// ---------------------------------------------------------------------------

struct AudioEvent {
    audioFX* fx;
    Vector3  sourcePos;
};

class AudioQueue {
public:
    void push(audioFX& fx, Vector3 sourcePos, bool isLocal = false) {
        if (fx.localPlayerOnly && !isLocal) return;
        queue.push_back({ &fx, sourcePos });
    }

    void flush(Player& player) {
        for (auto& event : queue) {
            event.fx->trigger(event.sourcePos, player.position, player.ForwardFlat());
        }
        queue.clear();
    }

private:
    std::vector<AudioEvent> queue;
};
