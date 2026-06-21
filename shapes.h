// How to draw the wireframe shapes for the various elements.

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <vector>
#include <cmath>

void DrawPlatform(const Platform& platform) {
    DrawShadedWireCube(platform.position, platform.size.x, 0.0f, platform.color_outline, platform.color_fill);
}

void DrawPlayer(const Player& player) {
    DrawShadedWireCube(player.position, player.size.x, 0.0f, player.color_outline, player.color_fill);
}

void DrawAsteroid(const Asteroid& asteroid) {
    DrawShadedSphere(asteroid.position, asteroid.size, asteroid.color_fill); // Fill pass for glowing
    DrawSphereWires(asteroid.position, asteroid.size, 16, 16, asteroid.color_outline); // Wireframe pass for crisp edges
}

void DrawRocket(const Rocket& rocket) {
    // Simple wireframe pyramid for the rocket, no fill
    DrawWirePyramid(rocket.position, 0.0f, 1.5f, 0.5f, rocket.color_outline);
}

void ExplosionEffect(const Vector3& position, float size) {
    // Draw a quick explosion effect using a transient wireframe sphere that expands and fades
    float explosionDuration = 0.5f; // seconds
    float elapsed = 0.0f;

    while (elapsed < explosionDuration) {
        float t = elapsed / explosionDuration; // 0 to 1 over the duration
        float currentSize = size * (1.0f + t); // Expand from initial size to larger
        Color col = {255, 200, 0, (unsigned char)(255 * (1.0f - t))}; // Fade out over time

        DrawSphereWires(position, currentSize, 8, 8, col);

        elapsed += GetFrameTime();
    }
}