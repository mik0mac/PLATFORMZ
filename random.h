#pragma once

#include <random>
#include <cmath>

// Utility function to generate a random float between min and max.
inline float RandomFloat(float min, float max) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(min, max);
    return dist(gen);
}

// Cheap deterministic hash of three floats -> [0,1) (the classic GLSL
// fract(sin*) trick). Same seed always yields the same value, so it drives
// stable per-object "randomness" (asteroid shape/spin) without storing anything.
inline float Hash01(float a, float b, float c) {
    float h = sinf(a * 12.9898f + b * 78.233f + c * 37.719f) * 43758.5453f;
    return h - floorf(h);
}