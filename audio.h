#pragma once

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <vector>
#include <string>
#include <initializer_list>
#include <cmath>

#include "random.h"
#include "constants.h"

// MARK: Audio FX
// Each audioFX owns a small pool of alias voices (LoadSoundAlias) so repeated
// triggers overlap instead of cutting each other off. Options:
//   poolSize       number of concurrent voices (1 = no overlap of itself)
//   pitchJitter    +/- random pitch per play (0 = none) to avoid monotony
// Directional gating (set after construction via blockedBy): this FX is
// suppressed while the referenced FX is still ringing, but it never blocks
// itself and never blocks the other - so its own voices round-robin freely.
class audioFX {
public:
    audioFX(const std::string& file, float volume = 1.0f, bool localOnly = false,
            bool spacial = true, int poolSize = 1, float pitchJitter = 0.0f)
        : baseVolume(volume), localPlayerOnly(localOnly), spacial(spacial),
          poolSize(poolSize < 1 ? 1 : poolSize), pitchJitter(pitchJitter),
          filenames{ file } {}

    // Variation form: pass N distinct files instead of one. Each file becomes
    // its own real voice (LoadSound, no aliasing), and the pool size is the file
    // count - so nextSlot() round-robins across the *variations*, giving a
    // different clip per trigger plus the same overlap a voice pool provides.
    audioFX(std::initializer_list<std::string> files, float volume = 1.0f,
            bool localOnly = false, bool spacial = true, float pitchJitter = 0.0f)
        : baseVolume(volume), localPlayerOnly(localOnly), spacial(spacial),
          poolSize((int)files.size() < 1 ? 1 : (int)files.size()),
          pitchJitter(pitchJitter), filenames(files), distinctFiles(true) {}

    float baseVolume;
    bool  localPlayerOnly;
    bool  spacial;
    int   poolSize;
    float pitchJitter;
    audioFX* blockedBy = nullptr; // don't play while this other FX is audible

    void load() {
        pool.resize(poolSize);
        if (distinctFiles) {
            // Each slot is its own real sound (a variation). A failed load
            // yields an empty Sound; playback on it is a silent no-op, so just
            // leave the slot as-is - nothing to alias.
            for (int i = 0; i < poolSize; i++)
                pool[i] = LoadSound(filenames[i].c_str());
            return;
        }
        pool[0] = LoadSound(filenames[0].c_str());
        // A failed load yields an empty Sound (null buffer); aliasing it would
        // dereference null and crash. Skip aliasing - playback on the empty
        // base is a silent no-op.
        if (!IsSoundValid(pool[0])) return;
        for (int i = 1; i < poolSize; i++)
            pool[i] = LoadSoundAlias(pool[0]);
    }

    bool isPlaying() {
        for (Sound& s : pool)
            if (IsSoundValid(s) && IsSoundPlaying(s)) return true;
        return false;
    }

    void unload() {
        if (distinctFiles) {
            // Every slot is a real sound, not an alias.
            for (Sound& s : pool) UnloadSound(s);
        } else {
            for (int i = 1; i < (int)pool.size(); i++) UnloadSoundAlias(pool[i]);
            if (!pool.empty()) UnloadSound(pool[0]);
        }
        pool.clear();
    }

    void trigger(Vector3 sourcePos, Vector3 listenerPos, Vector3 listenerDirection, float volumeScale = 1.0f) {
        // Directional gate: suppressed while the blocker FX is still audible.
        if (blockedBy && blockedBy->isPlaying()) return;

        Sound& s = nextSlot();

        float volume = baseVolume;
        float pan    = 0.5f; // center (non-spatial, or source on top of listener)

        if (spacial) {
            float dist = Vector3Distance(sourcePos, listenerPos);
            volume = Clamp(1.0f - dist / AUDIO_MAX_DISTANCE, AUDIO_MIN_VOLUME, 1.0f);

            // pan: project the (flattened) source-relative vector onto the
            // listener's right vector so turning around swaps left/right.
            // Angle only - distance is handled by volume above.
            Vector3 toSrc = { sourcePos.x - listenerPos.x, 0.0f, sourcePos.z - listenerPos.z };
            if (Vector3LengthSqr(toSrc) > 1e-6f) {
                Vector3 right = Vector3Normalize(Vector3CrossProduct(listenerDirection, {0, 1, 0}));
                float side = Vector3DotProduct(Vector3Normalize(toSrc), right); // -1 L .. +1 R
                pan = (Clamp(side, -1.0f, 1.0f) + 1.0f) * 0.5f;
            }
            volume *= baseVolume;
        }

        SetSoundVolume(s, volume * volumeScale);
        SetSoundPan(s, pan);
        SetSoundPitch(s, pitchJitter > 0.0f ? 1.0f + RandomFloat(-pitchJitter, pitchJitter) : 1.0f);
        PlaySound(s);
    }

private:
    // Pick a free voice; if all are busy, round-robin (steal the oldest).
    Sound& nextSlot() {
        for (Sound& s : pool)
            if (!IsSoundValid(s) || !IsSoundPlaying(s)) return s;
        Sound& s = pool[cursor];
        cursor = (cursor + 1) % (int)pool.size();
        return s;
    }

    std::vector<Sound> pool;
    int cursor = 0;
    std::vector<std::string> filenames;
    bool distinctFiles = false; // true: N real files (variations); false: N aliases of one
};

// ---------------------------------------------------------------------------
// Audio event queue
// ---------------------------------------------------------------------------

// MARK: Audio Event
struct AudioEvent {
    audioFX* fx;
    Vector3  sourcePos;
    float    volumeScale;
};

// MARK: Audio Queue
class AudioQueue {
public:
    void push(audioFX& fx, Vector3 sourcePos, bool isLocal = false, float volumeScale = 1.0f) {
        if (fx.localPlayerOnly && !isLocal) return;
        queue.push_back({ &fx, sourcePos, volumeScale });
    }

    void flush(Player& player) {
        for (auto& event : queue) {
            event.fx->trigger(event.sourcePos, player.position, player.ForwardFlat(), event.volumeScale);
        }
        queue.clear();
    }

private:
    std::vector<AudioEvent> queue;
};
