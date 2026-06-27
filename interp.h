// interp.h
//
// Client-side snapshot interpolation - the standard cure for "stuttery/jerky"
// networked motion. The server broadcasts authoritative state at 60 Hz, but over
// Wi-Fi those packets arrive irregularly; snapping each entity straight to the
// newest packet renders smooth server motion as teleporting.
//
// Instead we keep a short history of timestamped snapshots and render remote
// entities slightly in the PAST (now - INTERP_DELAY), interpolating between the
// two snapshots that bracket that render time. Late packets just mean we briefly
// hold at the newest sample rather than jitter.
//
// Networked client only. The LOCAL player is excluded (its camera is smoothed
// directly in main.cpp so your own view stays responsive, not rendered in the
// past). No server or protocol change - packets already carry tick + entity ids.

#pragma once

#include "raylib.h"
#include "raymath.h"

#include <deque>
#include <unordered_map>
#include <cstdint>

#include "gamespace.h"

class SnapshotInterpolator {
public:
    // Record current server state as a timestamped snapshot. Call once per frame,
    // ONLY when a new packet was applied to gs this frame (so samples are spaced
    // at the server tick rate, not the client frame rate).
    void commit(GameSpace& gs, double now) {
        Snapshot s;
        s.t = now;
        for (const Player&   p : gs.getPlayers())   s.e[p.id] = { p.position, p.yaw, p.pitch };
        for (const Asteroid& a : gs.getAsteroids()) s.e[a.id] = { a.position, 0.0f, 0.0f };
        for (const Rocket&   r : gs.getRockets())   s.e[r.id] = { r.position, 0.0f, 0.0f };
        buf_.push_back(std::move(s));
        while (buf_.size() > 2 && now - buf_.front().t > HISTORY_SECONDS) buf_.pop_front();
    }

    // Write interpolated transforms into gs for rendering. Call every frame before
    // draw. `localId` (the local player's Player::id) is skipped.
    void apply(GameSpace& gs, double now, float delay, uint32_t localId) {
        if (buf_.size() < 2) return;
        const double renderT = now - delay;

        // Find the snapshot pair bracketing renderT (default to the newest pair).
        const Snapshot* s0 = &buf_[buf_.size() - 2];
        const Snapshot* s1 = &buf_[buf_.size() - 1];
        for (size_t i = 0; i + 1 < buf_.size(); ++i) {
            if (buf_[i].t <= renderT && renderT <= buf_[i + 1].t) {
                s0 = &buf_[i]; s1 = &buf_[i + 1];
                break;
            }
        }
        const float span  = (float)(s1->t - s0->t);
        const float alpha = Clamp(span > 1e-6f ? (float)((renderT - s0->t) / span) : 1.0f, 0.0f, 1.0f);

        for (Player& p : gs.getPlayers()) {
            if (p.id == localId) continue;                 // local handled by camera smoothing
            applyOne(p.id, p.position, &p.yaw, &p.pitch, *s0, *s1, alpha);
        }
        for (Asteroid& a : gs.getAsteroids()) applyOne(a.id, a.position, nullptr, nullptr, *s0, *s1, alpha);
        for (Rocket&   r : gs.getRockets())   applyOne(r.id, r.position, nullptr, nullptr, *s0, *s1, alpha);
    }

private:
    struct Tf { Vector3 pos; float yaw; float pitch; };
    struct Snapshot { double t; std::unordered_map<uint32_t, Tf> e; };

    std::deque<Snapshot> buf_;
    static constexpr double HISTORY_SECONDS = 1.0;

    // Shortest-arc interpolation for yaw (wraps at +/-PI); pitch is clamped, no wrap.
    static float lerpAngle(float a, float b, float t) {
        float d = b - a;
        while (d >  PI) d -= 2.0f * PI;
        while (d < -PI) d += 2.0f * PI;
        return a + d * t;
    }

    static void applyOne(uint32_t id, Vector3& pos, float* yaw, float* pitch,
                         const Snapshot& s0, const Snapshot& s1, float alpha) {
        auto i0 = s0.e.find(id);
        auto i1 = s1.e.find(id);
        if (i0 != s0.e.end() && i1 != s1.e.end()) {
            pos = Vector3Lerp(i0->second.pos, i1->second.pos, alpha);
            if (yaw)   *yaw   = lerpAngle(i0->second.yaw, i1->second.yaw, alpha);
            if (pitch) *pitch = i0->second.pitch + (i1->second.pitch - i0->second.pitch) * alpha;
        } else if (i1 != s1.e.end()) {                     // just appeared: snap to newest
            pos = i1->second.pos;
            if (yaw)   *yaw   = i1->second.yaw;
            if (pitch) *pitch = i1->second.pitch;
        } else if (i0 != s0.e.end()) {                     // about to vanish: hold at older
            pos = i0->second.pos;
            if (yaw)   *yaw   = i0->second.yaw;
            if (pitch) *pitch = i0->second.pitch;
        }
        // else: in neither snapshot -> leave the latest value from applyMessage.
    }
};
