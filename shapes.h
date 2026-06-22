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

// Builds an irregular, low-poly "vector rock" - the 3D analog of main2d.cpp's
// jittered asteroid polygon. Starts from a unit icosahedron (12 vertices, 20
// triangular faces, 30 edges) and displaces each vertex radially by a stable
// per-vertex jitter, so every asteroid gets its own lumpy silhouette. The shape
// (and its spin axis/speed) is derived deterministically from the asteroid's
// startingPosition - its seed - so it stays consistent frame to frame without
// storing any mesh on the Asteroid class. Drawn shaded-wire to match the other
// elements: translucent double-sided fill faces with bright edges on top.
inline void DrawAsteroidShape(const Asteroid& asteroid) {
    // --- Unit icosahedron (golden-ratio construction) ---
    const float t = 1.6180339887f; // golden ratio
    static const Vector3 base[12] = {
        {-1,  t,  0}, { 1,  t,  0}, {-1, -t,  0}, { 1, -t,  0},
        { 0, -1,  t}, { 0,  1,  t}, { 0, -1, -t}, { 0,  1, -t},
        { t,  0, -1}, { t,  0,  1}, {-t,  0, -1}, {-t,  0,  1}
    };
    static const int faces[20][3] = {
        {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
        {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
        {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
        {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}
    };

    // Per-asteroid seed from its (stable) starting position.
    const Vector3 seed = asteroid.startingPosition;

    // Cheap deterministic hash -> [0,1) (classic GLSL fract(sin*) trick).
    auto hash01 = [](float a, float b, float c) {
        float h = sinf(a * 12.9898f + b * 78.233f + c * 37.719f) * 43758.5453f;
        return h - floorf(h);
    };

    // Tumble: spin around a per-asteroid axis, advanced by wall-clock time.
    Vector3 axis = Vector3{
        hash01(seed.x, seed.y, 1.0f) - 0.5f,
        hash01(seed.y, seed.z, 2.0f) - 0.5f,
        hash01(seed.z, seed.x, 3.0f) - 0.5f
    };
    if (Vector3LengthSqr(axis) < 1e-6f) axis = Vector3{0, 1, 0}; // guard a near-zero axis
    axis = Vector3Normalize(axis);
    float spinSpeed = 0.3f + 0.7f * hash01(seed.x, seed.z, 4.0f); // rad/sec, per asteroid
    float angle = (float)GetTime() * spinSpeed;

    // Build the displaced, oriented, world-space vertices.
    Vector3 verts[12];
    for (int i = 0; i < 12; i++) {
        Vector3 dir = Vector3Normalize(base[i]);                            // point on the unit sphere
        float jitter = 0.72f + 0.28f * hash01(seed.x + i, seed.y, seed.z);  // 0.72..1.0, like the 2D version
        Vector3 local = Vector3Scale(dir, asteroid.size * jitter);
        local = Vector3RotateByAxisAngle(local, axis, angle);
        verts[i] = Vector3Add(asteroid.position, local);
    }

    // Fill faces - drawn both windings so the translucent glow reads from any
    // angle (backface culling leaves exactly one of each pair visible).
    for (int f = 0; f < 20; f++) {
        Vector3 a = verts[faces[f][0]];
        Vector3 b = verts[faces[f][1]];
        Vector3 c = verts[faces[f][2]];
        DrawTriangle3D(a, b, c, asteroid.color_fill);
        DrawTriangle3D(a, c, b, asteroid.color_fill);
    }

    // Wireframe edges on top. Each undirected edge appears in two faces with
    // opposite winding, so the i<j test draws it exactly once (30 edges total).
    for (int f = 0; f < 20; f++) {
        for (int e = 0; e < 3; e++) {
            int i = faces[f][e];
            int j = faces[f][(e + 1) % 3];
            if (i < j) DrawLine3D(verts[i], verts[j], asteroid.color_outline);
        }
    }
}

inline void DrawAsteroid(const Asteroid& asteroid) {
    DrawAsteroidShape(asteroid);
}

inline void DrawRocket(const Rocket& rocket) {
    // Simple wireframe for the rocket, no fill.
    // DrawWirePyramid(rocket.position, 0.0f, 1.5f, 0.5f, rocket.color_outline);
    DrawWireSphere(rocket.position, rocket.size, rocket.color_outline);
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