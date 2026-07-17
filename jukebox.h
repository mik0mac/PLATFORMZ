#pragma once

#include "constants.h"
#include <vector>
#include <random>
#include <algorithm>


// server-side selector of music cues.  Cannot include raylib headers or audio.h.
// usage: each screen gets its own Jukebox instance.  Multiple cues can be added to each.
// Don't clear or reset the instances.  Instead let them persist accross games, so every cue is cycled through.
// When the server loads, add the tracks and shuffle them.  At the beginning of a game (a round of gameplay),
// call getCurrentTrack() and send that musicId to the client(s).  At the end of the game, call next().
// Repeat this for every screen.

class Jukebox {
public:
    void reset() {
        currentTrackIndex = 0;
        previousTrackIndex = 0;
        nextTrackIndex = 0;
    }

    void clear() {
        trackList.clear();
        reset();
    }

    void addTrack(MusicId cueId) {
        auto it = std::find(trackList.begin(), trackList.end(), cueId);
        if (it == trackList.end()) {
            trackList.push_back(cueId);
        }
    }

    void removeTrack(MusicId cueId) {
        auto it = std::find(trackList.begin(), trackList.end(), cueId);
        if (it == trackList.end()) return;
        // Keep currentTrackIndex pointing at the same track when an earlier
        // one is removed; if the current track itself goes, fall through to
        // whatever slides into its slot (wrapping off the end).
        // IF THERE IS POTENTIAL FOR REMOVING THE CURRENT TRACK WHILE IT IS
        // PLAYING, THAT MUST BE HANDLED ON THE CLIENT SIDE.

        size_t erasedIndex = it - trackList.begin();
        trackList.erase(it);
        if (previousTrackIndex == erasedIndex) previousTrackIndex = 0;
        if (erasedIndex < currentTrackIndex) {
            currentTrackIndex--;
        } else if (currentTrackIndex >= trackList.size()) {
            currentTrackIndex = 0;
        }
        nextTrackIndex = (currentTrackIndex + 1) % trackList.size();
    }

    void shuffle() {
        std::shuffle(trackList.begin(), trackList.end(), std::mt19937{std::random_device{}()});
    }

    void next() {
        if (trackList.empty()) return;
        previousTrackIndex = currentTrackIndex;
        currentTrackIndex = (currentTrackIndex + 1) % trackList.size();
        nextTrackIndex = (currentTrackIndex + 1) % trackList.size();
    }

    void previous() {
        if (trackList.empty()) return;
        // move back one track in the list.
        previousTrackIndex = currentTrackIndex;
        nextTrackIndex = currentTrackIndex;
        currentTrackIndex = (currentTrackIndex + trackList.size() - 1) % trackList.size();
    }

    // MUSIC_COUNT means "no track" (empty jukebox).
    MusicId getCurrentTrack() const {
        if (trackList.empty()) return MUSIC_COUNT;
        return trackList[currentTrackIndex];
    }

private:
    std::vector<MusicId> trackList;
    size_t currentTrackIndex = 0;
    size_t previousTrackIndex = 0; // whatever the last played track was, not necessarily currentTrackIndex - 1.
    size_t nextTrackIndex = 0; // whatever the next track will be, not necessarily currentTrackIndex + 1.
};



