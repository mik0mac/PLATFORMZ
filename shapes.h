// How to draw the wireframe shapes for the various elements.
#pragma once

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <vector>
#include <cmath>

#include "elements.h"

//MARK: Primitive drawing helpers
// These are the low-level wireframe/shaded-fill primitives, pulled in from
// WireframeTests/main3d3.cpp. Higher-level Draw___(const Element&) functions
// below build on top of these.

// Draws a wireframe box with a translucent, slightly-glowing fill pass underneath.
// Render solid first (low alpha), then draw the wireframe on top at full opacity
// so edges stay crisp - the core "vector + shading" look used throughout.
inline void DrawShadedWireBox(Vector3 position, float width, float height, float depth, float rotY, Color wireColor, Color fillColor) {
    rlPushMatrix();
    rlTranslatef(position.x, position.y, position.z);
    rlRotatef(rotY * RAD2DEG, 0, 1, 0);

    DrawCube(Vector3Zero(), width, height, depth, fillColor);
    DrawCubeWires(Vector3Zero(), width, height, depth, wireColor);

    rlPopMatrix();
}

// Same fill+wireframe layering as DrawShadedWireBox, but for a sphere.
// Used for asteroids and the explosion effect.
inline void DrawShadedSphere(Vector3 position, float radius, Color wireColor, Color fillColor) {
    DrawSphere(position, radius, fillColor);
    DrawSphereWires(position, radius, 16, 16, wireColor);
}

// A wireframe pyramid (ship/rocket-like silhouette), built manually with line
// segments so we control exactly which edges draw - closer to how the
// original vector hardware worked than using a filled primitive.
inline void DrawWirePyramid(Vector3 pos, float rotY, float height, float baseSize, Color col) {
    rlPushMatrix();
    rlTranslatef(pos.x, pos.y, pos.z);
    rlRotatef(rotY * RAD2DEG, 0, 1, 0);

    Vector3 tip = {0, height, 0};
    Vector3 a = {-baseSize, 0, -baseSize};
    Vector3 b = { baseSize, 0, -baseSize};
    Vector3 c = { baseSize, 0,  baseSize};
    Vector3 d = {-baseSize, 0,  baseSize};

    // base
    DrawLine3D(a, b, col);
    DrawLine3D(b, c, col);
    DrawLine3D(c, d, col);
    DrawLine3D(d, a, col);
    // sides to tip
    DrawLine3D(a, tip, col);
    DrawLine3D(b, tip, col);
    DrawLine3D(c, tip, col);
    DrawLine3D(d, tip, col);

    rlPopMatrix();
}

// Draws a vector-grid pattern across all 6 faces of a large bounding cube,
// used for the play-space boundary walls (GameSpace::halfSize / gridColor).
inline void DrawGridRoom(float halfSize, float spacing, Color col) {
    int lines = (int)(halfSize / spacing);

    for (int i = -lines; i <= lines; i++) {
        float p = i * spacing;
        // floor / ceiling (X/Z grid)
        DrawLine3D({p, -halfSize, -halfSize}, {p, -halfSize, halfSize}, col);
        DrawLine3D({-halfSize, -halfSize, p}, {halfSize, -halfSize, p}, col);
        DrawLine3D({p, halfSize, -halfSize}, {p, halfSize, halfSize}, col);
        DrawLine3D({-halfSize, halfSize, p}, {halfSize, halfSize, p}, col);

        // front / back (X/Y grid)
        DrawLine3D({p, -halfSize, -halfSize}, {p, halfSize, -halfSize}, col);
        DrawLine3D({-halfSize, p, -halfSize}, {halfSize, p, -halfSize}, col);
        DrawLine3D({p, -halfSize, halfSize}, {p, halfSize, halfSize}, col);
        DrawLine3D({-halfSize, p, halfSize}, {halfSize, p, halfSize}, col);

        // left / right (Y/Z grid)
        DrawLine3D({-halfSize, p, -halfSize}, {-halfSize, p, halfSize}, col);
        DrawLine3D({-halfSize, -halfSize, p}, {-halfSize, halfSize, p}, col);
        DrawLine3D({halfSize, p, -halfSize}, {halfSize, p, halfSize}, col);
        DrawLine3D({halfSize, -halfSize, p}, {halfSize, halfSize, p}, col);
    }
}

//MARK: Element drawing functions
// One function per game-object type, each reading only the fields it needs
// from the corresponding class in elements.h.

inline void DrawPlatform(const Platform& platform) {
    DrawShadedWireBox(platform.position, platform.size.x, platform.size.y, platform.size.z, 0.0f, platform.color_outline, platform.color_fill);
}

inline void DrawPlayer(const Player& player) {
    DrawShadedWireBox(player.position, player.size.x, player.size.y, player.size.z, 0.0f, player.color_outline, player.color_fill);
}

inline void DrawAsteroid(const Asteroid& asteroid) {
    DrawShadedSphere(asteroid.position, asteroid.size, asteroid.color_outline, asteroid.color_fill);
}

inline void DrawRocket(const Rocket& rocket) {
    // Simple wireframe pyramid for the rocket, no fill
    DrawWirePyramid(rocket.position, 0.0f, 1.5f, 0.5f, rocket.color_outline);
}

inline void DrawExplosion(const Explosion& explosion) {
    // Two-layer vector explosion, all driven by Explosion::update(dt) (called
    // each frame in gamespace.h), which grows radius 0 -> maxRadius:
    //   - a bright near-white outer shockwave shell (wire only) that flashes
    //     at the leading edge and fades as it expands
    //   - a slower orange core sphere with the translucent fill underneath
    float t = explosion.maxRadius > 0.0f ? (explosion.radius / explosion.maxRadius) : 1.0f;
    t = Clamp(t, 0.0f, 1.0f);

    // Outer shockwave: full current radius, near-white flash fading to nothing.
    unsigned char shockAlpha = (unsigned char)(255 * (1.0f - t));
    Color shockColor = {255, 230, 180, shockAlpha};
    DrawSphereWires(explosion.position, explosion.radius, 16, 16, shockColor);

    // Core: smaller orange sphere lagging the shockwave. Fill alpha is already
    // faded by Explosion::update; fade the wire outline over the lifetime too.
    float coreRadius = explosion.radius * 0.6f;
    unsigned char coreOutlineAlpha = (unsigned char)(255 * (1.0f - t));
    Color coreOutline = {explosion.color_outline.r, explosion.color_outline.g,
                         explosion.color_outline.b, coreOutlineAlpha};
    DrawShadedSphere(explosion.position, coreRadius, coreOutline, explosion.color_fill);
}