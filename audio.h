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

        // pan: where is the source relative to the listener's facing?
        // Project the (flattened) source-relative vector onto the listener's
        // right vector so turning around correctly swaps left/right. Depends on
        // angle only, not distance (distance is handled by volume above).
        Vector3 toSrc = { sourcePos.x - listenerPos.x, 0.0f, sourcePos.z - listenerPos.z };
        float pan = 0.5f; // center by default (source on top of listener)
        if (Vector3LengthSqr(toSrc) > 1e-6f) {
            Vector3 right = Vector3Normalize(Vector3CrossProduct(listenerDirection, {0, 1, 0}));
            float side = Vector3DotProduct(Vector3Normalize(toSrc), right); // -1 L .. +1 R
            pan = (Clamp(side, -1.0f, 1.0f) + 1.0f) * 0.5f;
        }
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
