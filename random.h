#pragma once

#include <random>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <vector>

//MARK: Why these generators are thread_local
// A `static` std::mt19937 is one shared, mutable object, and advancing it is a
// write - so two threads drawing at once is a data race, and a corrupted
// generator state degrades the one property these exist to provide, silently.
//
// It was safe for a long time by accident: the client is single-threaded, and on
// the server every draw happened on the sim thread (world generation at match
// start). That stopped being true when a Match started shuffling its own bot-name
// order, because a Match is CONSTRUCTED on an io thread when somebody creates a
// room - while the sim thread may be generating a world for a different room.
//
// thread_local gives each thread its own generator, seeded from its own
// random_device. It costs ~2.5 KB per thread that actually draws, and it is what
// A4 already named as the prerequisite for sharding matches across a worker pool.
// Nothing depends on the sequence being reproducible - these are seeded from
// random_device, so there was never a sequence to depend on.

// Utility function to generate a random float between min and max.
inline float RandomFloat(float min, float max) {
    thread_local std::random_device rd;
    thread_local std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(min, max);
    return dist(gen);
}

// A random permutation of 0..n-1. Used to hand out list items (e.g. bot names)
// in random order without repeats, rather than sequentially.
inline std::vector<int> ShuffledIndices(int n) {
    thread_local std::random_device rd;
    thread_local std::mt19937 gen(rd());
    std::vector<int> idx(n);
    std::iota(idx.begin(), idx.end(), 0);
    std::shuffle(idx.begin(), idx.end(), gen);
    return idx;
}

// Cheap deterministic hash of three floats -> [0,1) (the classic GLSL
// fract(sin*) trick). Same seed always yields the same value, so it drives
// stable per-object "randomness" (asteroid shape/spin) without storing anything.
inline float Hash01(float a, float b, float c) {
    float h = sinf(a * 12.9898f + b * 78.233f + c * 37.719f) * 43758.5453f;
    return h - floorf(h);
}