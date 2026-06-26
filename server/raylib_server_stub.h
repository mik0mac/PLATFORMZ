// server/raylib_server_stub.h
//
// Minimal stub replacing raylib.h + raymath.h + rlgl.h for the server build.
// The server needs the math types and functions used by game logic, but has
// no display, no OpenGL, and no audio - so the real raylib headers aren't
// available or needed on the Linux VM.
//
// Only covers what elements.h / gamespace.h / collisions.h actually use.
// If you add new raylib calls to the game logic headers, add the stub here.
//
// HOW IT WORKS:
// The server Makefile passes -I server/ before -I ../ so this file shadows
// the real raylib.h when compiling server_main.cpp and the game headers it
// pulls in. The game headers themselves are unchanged.

#pragma once

#include <cmath>
#include <algorithm>
#include <cstdint>

// -------------------------------------------------------------------------
// Core types - mirrors raylib's structs exactly so game code compiles
// against this stub without any changes.
// -------------------------------------------------------------------------

struct Vector2 { float x = 0, y = 0; };
struct Vector3 { float x = 0, y = 0, z = 0; };
struct Vector4 { float x = 0, y = 0, z = 0, w = 0; };

struct Color {
    unsigned char r = 0, g = 0, b = 0, a = 255;
    Color() = default;
    Color(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
        : r(r), g(g), b(b), a(a) {}
};

static const Color RAYWHITE = {245, 245, 245, 255};
static const Color WHITE    = {255, 255, 255, 255};
static const Color BLACK    = {0,   0,   0,   255};
static const Color RED      = {230, 41,  55,  255};
static const Color GREEN    = {0,   228, 48,  255};
static const Color BLUE     = {0,   121, 241, 255};
static const Color GRAY     = {130, 130, 130, 255};
static const Color DARKGRAY = {80,  80,  80,  255};

// -------------------------------------------------------------------------
// Math constants
// -------------------------------------------------------------------------
#ifndef PI
#define PI 3.14159265358979323846f
#endif
#ifndef DEG2RAD
#define DEG2RAD (PI / 180.0f)
#endif
#ifndef RAD2DEG
#define RAD2DEG (180.0f / PI)
#endif

// -------------------------------------------------------------------------
// Scalar math
// -------------------------------------------------------------------------
inline float Clamp(float value, float min, float max) {
    return value < min ? min : (value > max ? max : value);
}
inline float Lerp(float start, float end, float amount) {
    return start + amount * (end - start);
}
inline float Normalize(float value, float start, float end) {
    return (value - start) / (end - start);
}

// -------------------------------------------------------------------------
// Vector2 math
// -------------------------------------------------------------------------
inline Vector2 Vector2Add(Vector2 a, Vector2 b)      { return {a.x+b.x, a.y+b.y}; }
inline Vector2 Vector2Subtract(Vector2 a, Vector2 b) { return {a.x-b.x, a.y-b.y}; }
inline Vector2 Vector2Scale(Vector2 v, float s)      { return {v.x*s, v.y*s}; }
inline float   Vector2Length(Vector2 v)               { return sqrtf(v.x*v.x + v.y*v.y); }
inline float   Vector2LengthSqr(Vector2 v)            { return v.x*v.x + v.y*v.y; }
inline Vector2 Vector2Normalize(Vector2 v) {
    float len = Vector2Length(v);
    return len > 0 ? Vector2{v.x/len, v.y/len} : Vector2{0,0};
}

// -------------------------------------------------------------------------
// Vector3 math
// -------------------------------------------------------------------------
inline Vector3 Vector3Add(Vector3 a, Vector3 b)      { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline Vector3 Vector3Subtract(Vector3 a, Vector3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline Vector3 Vector3Scale(Vector3 v, float s)      { return {v.x*s, v.y*s, v.z*s}; }
inline float   Vector3Length(Vector3 v)               { return sqrtf(v.x*v.x + v.y*v.y + v.z*v.z); }
inline float   Vector3LengthSqr(Vector3 v)            { return v.x*v.x + v.y*v.y + v.z*v.z; }
inline float   Vector3Distance(Vector3 a, Vector3 b)  { return Vector3Length(Vector3Subtract(a, b)); }
inline float   Vector3DistanceSqr(Vector3 a, Vector3 b) { return Vector3LengthSqr(Vector3Subtract(a, b)); }
inline Vector3 Vector3Normalize(Vector3 v) {
    float len = Vector3Length(v);
    return len > 0 ? Vector3{v.x/len, v.y/len, v.z/len} : Vector3{0,0,0};
}
inline Vector3 Vector3CrossProduct(Vector3 a, Vector3 b) {
    return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
inline float   Vector3DotProduct(Vector3 a, Vector3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline Vector3 Vector3Lerp(Vector3 a, Vector3 b, float t) {
    return {a.x + t*(b.x-a.x), a.y + t*(b.y-a.y), a.z + t*(b.z-a.z)};
}
inline Vector3 Vector3Zero() { return {0, 0, 0}; }
inline Vector3 Vector3Reflect(Vector3 v, Vector3 normal) {
    float dot = Vector3DotProduct(v, normal);
    return Vector3Subtract(v, Vector3Scale(normal, 2.0f * dot));
}

// -------------------------------------------------------------------------
// Color math - structs need to compile even though server doesn't render.
// -------------------------------------------------------------------------
inline Color Fade(Color color, float alpha) {
    color.a = (unsigned char)(255.0f * Clamp(alpha, 0.0f, 1.0f));
    return color;
}
inline Color ColorAlpha(Color color, float alpha) { return Fade(color, alpha); }
inline Color ColorBrightness(Color color, float factor) {
    auto c = [](float v) -> unsigned char { return (unsigned char)Clamp(v, 0.0f, 255.0f); };
    return { c(color.r * factor), c(color.g * factor), c(color.b * factor), color.a };
}

// -------------------------------------------------------------------------
// Prevent real raylib/raymath/rlgl headers from being included if something
// pulls them in transitively - these guards match what those headers define.
// -------------------------------------------------------------------------
#define RAYLIB_H
#define RLGL_H
#define RAYMATH_H
#ifndef RAYMATH_STANDALONE
#define RAYMATH_STANDALONE
#endif
