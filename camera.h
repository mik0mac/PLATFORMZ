// First-person camera derived from Player state. This is input/render-adjacent,
// not game logic - kept separate from elements.h/gamespace.h on purpose.

#pragma once

#include "raylib.h"
#include "raymath.h"
#include <cmath>

#include "elements.h"

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
