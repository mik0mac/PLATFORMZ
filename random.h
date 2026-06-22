#pragma once

#include <random>

// Utility function to generate a random float between min and max.
inline float RandomFloat(float min, float max) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(min, max);
    return dist(gen);
}