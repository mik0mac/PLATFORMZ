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


//MARK: Music Cue
// Each cue (ogg file in the assets/music) will get its own instance with its own loop points, fade-in/out, etc..
// Volume can be individualized as well.  Pan will likely remain centered.
// Usage: Create a MusicCue instance for each cue, load() it.  Use fadeIn() and fadeOut() instead of play() and stop().
class MusicCue {
public:
    std::string filename;
    float volume;
    float pan = 0.5f;
    bool isPlaying() { return IsMusicStreamPlaying(music); }
    
    bool loop = true; // default: loop the whole track (raylib's built-in looping).  If false, play once and stop.
    float loopStart = 0.0f; // in seconds
    float loopEnd = -1.0f;   // negative means "end of track" (resolved in load() once the music's length is known)
    int num_of_loops = -1; // number of REPEATS (seek-backs to loopStart): 0 = play the section once, 1 = twice, etc. -1 = loop forever
    int loopCount = 0; // number of times the music has looped (incremented each time it loops)

    float fadeInDuration = 0.0f; // in seconds
    float fadeOutDuration = 0.5f; // in seconds
    bool isFadingIn = false;
    bool isFadingOut = false;
    float fadeTimer = 0.0f; // in seconds

    MusicCue (const std::string& filename, float volume = 1.0f, float fadeInDuration = 0.0f, float fadeOutDuration = 0.5f,
              bool loop = true, float loopStart = 0.0f, float loopEnd = -1.0f, int num_of_loops = -1)
        : filename(filename), volume(volume), fadeInDuration(fadeInDuration), fadeOutDuration(fadeOutDuration),
          loop(loop), loopStart(loopStart), loopEnd(loopEnd), num_of_loops(num_of_loops) {}

    void load() {
        music = LoadMusicStream(filename.c_str());
        // A failed load yields an invalid Music; every raylib call on it is
        // skipped, so a missing file just means silence.
        if (!IsMusicValid(music)) return;
        if (loopEnd < 0.0f) loopEnd = GetMusicTimeLength(music);
        SetMusicVolume(music, volume);
        // raylib defaults Music.looping to true - a non-looping cue must opt out.
        music.looping = loop;
    }

    void reset() {
        if (!IsMusicValid(music)) return;
        if (loopEnd < 0.0f) loopEnd = GetMusicTimeLength(music);
        SetMusicVolume(music, volume);
        // raylib defaults Music.looping to true - a non-looping cue must opt out.
        music.looping = loop;
    }

    void unload() {
        if (IsMusicValid(music)) UnloadMusicStream(music);
    }

    void play() {
        if (!IsMusicValid(music)) return;
        StopMusicStream(music); // PlayMusicStream alone resumes from the old position
        PlayMusicStream(music);
        loopCount = 0;
        music.looping = loop; // may have been cleared by a finished loop quota
    }

    void stop() {
        if (IsMusicValid(music)) StopMusicStream(music);
    }

    void fadeIn() {
        isFadingIn = true;
        isFadingOut = false;
        fadeTimer = 0.0f;
        SetMusicVolume(music, 0.0f);
    }

    void fadeOut() {
        isFadingOut = true;
        isFadingIn = false;
        fadeTimer = 0.0f;
    }

    // Per-frame: feed the stream and handle the custom loop points. While the
    // cue is looping, music.looping stays true as a safety net - if a frame
    // hiccup skips past a loopEnd at the very end of the track, raylib wraps
    // to 0 instead of going silent.
    void update(float dt) {
        if (!IsMusicValid(music)) return;
        UpdateMusicStream(music);
        if (loop) {
            if (GetMusicTimePlayed(music) >= loopEnd) {
                if (num_of_loops < 0 || loopCount < num_of_loops) {
                    SeekMusicStream(music, loopStart);
                    loopCount++;
                } else {
                    music.looping = false; // quota used up: play out to the end, stop
                }
            }
        }
        if (isFadingIn) {
            // start playback using the built-in function, which checks for a valid Music
            // and restarts the music from the beginning.
            if (IsMusicStreamPlaying(music) == false) play();
            fadeTimer += dt;
            // duration <= 0 means instant: skip the divide (0/0 would be NaN)
            float fadeProgress = (fadeInDuration > 0.0f) ? fadeTimer / fadeInDuration : 1.0f;
            if (fadeProgress >= 1.0f) {
                isFadingIn = false;
                SetMusicVolume(music, volume);
            } else {
                SetMusicVolume(music, volume * fadeProgress);
            }
        } else if (isFadingOut) {
            fadeTimer += dt;
            float fadeProgress = (fadeOutDuration > 0.0f) ? fadeTimer / fadeOutDuration : 1.0f;
            if (fadeProgress >= 1.0f) {
                isFadingOut = false;
                stop();
                reset(); // reset the music to its initial state for the next play
            } else {
                SetMusicVolume(music, volume * (1.0f - fadeProgress));
            }
        }
    }
private:
    Music music = {}; // zero until load(); IsMusicValid guards every use
};
