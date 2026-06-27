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
    audioFX(const std::string& file, float volume = 1.0f, bool localOnly = false)
        : filename(file), baseVolume(volume), localPlayerOnly(localOnly) {}

    float baseVolume;
    bool localPlayerOnly;

    void load() {
        sound = LoadSound(filename.c_str());
        SetSoundVolume(sound, baseVolume);
    }

    void unload() {
        UnloadSound(sound);
    }

    void trigger(Vector3 sourcePos, Vector3 listenerPos, float minDist = 1.0f, float maxDist = 40.0f) {
        float dist = Vector3Distance(sourcePos, listenerPos);
        if (dist > maxDist) return;

        float d = fmaxf(dist, minDist);
        float volume = Clamp((minDist * minDist) / (d * d), 0.0f, 1.0f);

        float dx = sourcePos.x - listenerPos.x;
        float pan = (Clamp(dx / maxDist, -1.0f, 1.0f) + 1.0f) * 0.5f;

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

    void flush(Vector3 listenerPos) {
        for (auto& event : queue) {
            event.fx->trigger(event.sourcePos, listenerPos);
        }
        queue.clear();
    }

private:
    std::vector<AudioEvent> queue;
};
