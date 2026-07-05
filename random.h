#pragma once

#include <random>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <vector>

// Utility function to generate a random float between min and max.
inline float RandomFloat(float min, float max) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(min, max);
    return dist(gen);
}

// A random permutation of 0..n-1. Used to hand out list items (e.g. bot names)
// in random order without repeats, rather than sequentially.
inline std::vector<int> ShuffledIndices(int n) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
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