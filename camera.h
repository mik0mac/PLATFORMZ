// Free-fly camera for the play space. Pulled from WireframeTests/main3d3.cpp -
// tracks yaw/pitch manually (rather than raylib's built-in CAMERA_FREE mode)
// so movement speed, mouse sensitivity, and pitch clamping are all explicit.
// This is input/render-adjacent, not game logic - kept separate from
// elements.h/gamespace.h on purpose.

#pragma once

#include "raylib.h"
#include "raymath.h"
#include <cmath>

#include "elements.h"

struct FlyCam {
    Vector3 position{0.0f, 3.0f, 14.0f};
    float yaw   = -90.0f * DEG2RAD; // facing -Z initially
    float pitch = 0.0f;

    const float moveSpeed = 8.0f;     // units/sec
    const float mouseSensitivity = 0.0025f;
    const float pitchLimit = 89.0f * DEG2RAD;

    Vector3 Forward() const {
        return { cosf(pitch) * cosf(yaw), sinf(pitch), cosf(pitch) * sinf(yaw) };
    }
    Vector3 Right() const {
        Vector3 fwd = Forward();
        return Vector3Normalize(Vector3CrossProduct(fwd, {0, 1, 0}));
    }

    void Update(float dt) {
        // Mouse look
        Vector2 mouseDelta = GetMouseDelta();
        yaw   += mouseDelta.x * mouseSensitivity;
        pitch -= mouseDelta.y * mouseSensitivity;
        if (pitch > pitchLimit) pitch = pitchLimit;
        if (pitch < -pitchLimit) pitch = -pitchLimit;

        // WASD + Space/Ctrl for vertical movement
        Vector3 fwd = Forward();
        Vector3 right = Right();
        Vector3 move{0, 0, 0};

        if (IsKeyDown(KEY_W)) move = Vector3Add(move, fwd);
        if (IsKeyDown(KEY_S)) move = Vector3Subtract(move, fwd);
        if (IsKeyDown(KEY_D)) move = Vector3Add(move, right);
        if (IsKeyDown(KEY_A)) move = Vector3Subtract(move, right);
        if (IsKeyDown(KEY_SPACE)) move.y += 1.0f;
        if (IsKeyDown(KEY_LEFT_CONTROL)) move.y -= 1.0f;

        if (Vector3LengthSqr(move) > 0.0f) {
            move = Vector3Normalize(move);
            float speed = moveSpeed * (IsKeyDown(KEY_LEFT_SHIFT) ? 2.5f : 1.0f);
            position = Vector3Add(position, Vector3Scale(move, speed * dt));
        }
    }

    Camera3D ToCamera3D() const {
        Camera3D cam{};
        cam.position = position;
        cam.target = Vector3Add(position, Forward());
        cam.up = {0, 1, 0};
        cam.fovy = 60.0f;
        cam.projection = CAMERA_PERSPECTIVE;
        return cam;
    }
};

// Builds a first-person Camera3D directly from Player state - position +
// eye-height offset, and Player's own yaw/pitch (via Forward()) for look
// direction. Player is the authoritative source; this is read-only,
// called fresh each frame after player movement/look updates, never the
// other way around (camera never drives player state).
inline Camera3D CameraFromPlayer(const Player& player) {
    Vector3 eyePos = player.position; // eye is at the sphere center

    Camera3D cam{};
    cam.position = eyePos;
    cam.target = Vector3Add(eyePos, player.Forward());
    cam.up = {0, 1, 0};
    cam.fovy = 60.0f;
    cam.projection = CAMERA_PERSPECTIVE;
    return cam;
}
