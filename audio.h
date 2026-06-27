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

    float baseVolume;  // a trim control to balance the loudness of different FX
    bool localPlayerOnly; // if true, only play for the local player (no network echo)

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
        PlaySoundMulti(sound);
    }

private:
    Sound sound;
    std::string filename;
};

// ---------------------------------------------------------------------------
// Audio event queue
// ---------------------------------------------------------------------------

struct AudioEvent {
    audioFX* fx;         // which sound to play
    Vector3  sourcePos;  // world position of the sound source
};

static std::vector<AudioEvent> audioQueue;

// Push an event onto the queue.
// isLocal: pass true when triggering from input.h (local player action),
//          false when triggered from server state reconciliation (remote players).
inline void pushAudio(audioFX& fx, Vector3 sourcePos, bool isLocal = false) {
    if (fx.localPlayerOnly && !isLocal) return;
    audioQueue.push_back({ &fx, sourcePos });
}

// Call once per frame after all state updates, before the next input poll.
// listenerPos is the local player's current world position.
inline void flushAudioQueue(Vector3 listenerPos) {
    for (auto& event : audioQueue) {
        event.fx->trigger(event.sourcePos, listenerPos);
    }
    audioQueue.clear();
}
