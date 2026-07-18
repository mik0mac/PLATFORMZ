// Distant starry-vista background: a procedural shell of short line dashes at
// STARFIELD_RADIUS, re-centered on the camera every frame so it never
// parallaxes (reads as infinitely far; rotates with look only). Star
// placement/color/twinkle are fully deterministic via Hash01, so the same sky
// appears on every screen and every run with no stored state.
//
// CLIENT-ONLY: this header includes raylib and is included ONLY from main.cpp.
// Never include it from a header the server build can see.

#pragma once

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <cmath>
#include <vector>

#include "random.h"    // Hash01
#include "constants.h" // STARFIELD_* tuning knobs

//MARK: Star data
struct StarfieldStar {
    Vector3 dir;       // unit direction from the shell center
    Vector3 t1, t2;    // tangent basis at dir - the dash (and hero cross) axes
    float phase;       // twinkle phase offset, radians
    float speed;       // twinkle speed, rad/s
    float baseAlpha;   // brightness before twinkle
    Color color;       // cool<->warm white
    bool hero;         // brighter, cross-shaped star
};

// Built once, lazily. Deterministic (Hash01, same idiom as the asteroid
// tumble) - not RandomFloat - so every run gets the identical sky.
inline const std::vector<StarfieldStar>& StarfieldStars() {
    static std::vector<StarfieldStar> stars = [] {
        std::vector<StarfieldStar> s;
        s.reserve(STARFIELD_STAR_COUNT);
        for (int i = 0; i < STARFIELD_STAR_COUNT; i++) {
            float fi = (float)i;
            // Uniform direction on the sphere: z uniform in [-1,1], azimuth in [0,2pi).
            float z = 2.0f * Hash01(fi, 1.0f, 17.0f) - 1.0f;
            float azimuth = 2.0f * PI * Hash01(fi, 2.0f, 29.0f);
            float r = sqrtf(fmaxf(0.0f, 1.0f - z * z));
            Vector3 dir = { r * cosf(azimuth), z, r * sinf(azimuth) };

            // Tangent basis: dash axes perpendicular to the view ray from the
            // center, so a dash never degenerates to zero apparent length.
            Vector3 t1 = (fabsf(dir.y) > 0.99f)
                ? Vector3Normalize(Vector3CrossProduct(dir, {1, 0, 0}))
                : Vector3Normalize(Vector3CrossProduct(dir, {0, 1, 0}));
            Vector3 t2 = Vector3CrossProduct(dir, t1);

            StarfieldStar star;
            star.dir = dir;
            star.t1 = t1;
            star.t2 = t2;
            star.phase = 2.0f * PI * Hash01(fi, 3.0f, 43.0f);
            star.speed = STARFIELD_TWINKLE_SPEED_MIN
                       + (STARFIELD_TWINKLE_SPEED_MAX - STARFIELD_TWINKLE_SPEED_MIN)
                         * Hash01(fi, 4.0f, 57.0f);
            star.hero = Hash01(fi, 5.0f, 71.0f) < STARFIELD_HERO_FRACTION;
            star.baseAlpha = star.hero ? 255.0f : 140.0f + 100.0f * Hash01(fi, 6.0f, 83.0f);
            float warmth = Hash01(fi, 7.0f, 97.0f);
            star.color = ColorLerp(STARFIELD_COLOR_COOL, STARFIELD_COLOR_WARM, warmth);
            s.push_back(star);
        }
        return s;
    }();
    return stars;
}

//MARK: Draw (in-scene)
// Must be called inside BeginMode3D, BEFORE any scene geometry. Depth-mask-off
// bracket mirrors BeginTranslucentFill (shapes.h): stars never write depth, so
// everything drawn afterward paints straight over them.
inline void DrawStarfield(Vector3 center, float time) {
    rlDrawRenderBatchActive(); // flush prior batch before touching depth state
    rlDisableDepthMask();
    for (const StarfieldStar& s : StarfieldStars()) {
        // Gentle shimmer: brightness swings STARFIELD_TWINKLE_AMOUNT around base.
        float tw = 1.0f - STARFIELD_TWINKLE_AMOUNT * 0.5f
                 + STARFIELD_TWINKLE_AMOUNT * 0.5f * sinf(time * s.speed + s.phase);
        Color c = s.color;
        c.a = (unsigned char)Clamp(s.baseAlpha * tw, 0.0f, 255.0f);

        Vector3 p = Vector3Add(center, Vector3Scale(s.dir, STARFIELD_RADIUS));
        float len = STARFIELD_STAR_LENGTH * (s.hero ? STARFIELD_HERO_SCALE : 1.0f);
        Vector3 a1 = Vector3Scale(s.t1, len * 0.5f);
        DrawLine3D(Vector3Subtract(p, a1), Vector3Add(p, a1), c);
        if (s.hero) {
            Vector3 a2 = Vector3Scale(s.t2, len * 0.5f);
            DrawLine3D(Vector3Subtract(p, a2), Vector3Add(p, a2), c);
        }
    }
    rlDrawRenderBatchActive(); // flush the stars before re-enabling depth writes
    rlEnableDepthMask();
}

//MARK: Draw (2D-screen backdrop)
// Star backdrop for the 2D screens (title / countdown / game over): a fixed
// camera at the origin drifting slowly in yaw, so the sky moves just enough to
// feel alive behind the UI. Call between ClearBackground and the 2D drawing.
inline void DrawStarfieldBackdrop(float time) {
    float yaw = time * STARFIELD_BACKDROP_YAW_SPEED * DEG2RAD;
    Camera3D cam = {0};
    cam.position = {0, 0, 0};
    cam.target = { cosf(yaw), 0.12f, sinf(yaw) };
    cam.up = {0, 1, 0};
    cam.fovy = 60.0f;
    cam.projection = CAMERA_PERSPECTIVE;
    BeginMode3D(cam);
    DrawStarfield({0, 0, 0}, time);
    EndMode3D();
}
