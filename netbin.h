// netbin.h
//
// Little-endian binary codec for the UDP state packet. The per-tick state packet
// is the one high-frequency, size-critical message; as JSON it ran ~2 KB (over
// the safe UDP MTU, so it IP-fragments and drops badly over the internet). The
// binary form is ~4x smaller and fits one datagram.
//
// Scope: UDP only. WebSocket/TCP has no MTU limit, so browser (and native ws://)
// clients keep the JSON state path untouched. The client tells the two apart by
// the first byte: '{' (0x7B) = JSON, anything else = binary (see wire.h
// applyMessage). We tag binary packets with STATE_BIN_VERSION as byte 0.
//
// Encoding is explicit little-endian (shift-based, so host endianness doesn't
// matter) and floats travel as their IEEE-754 bit pattern - identical on every
// target we build for (x86-64, arm64, wasm32, all little-endian). No raylib types
// here, so the headless server can include it too.

#pragma once

#include <string>
#include <cstdint>
#include <cstring>
#include <cmath>

namespace nb {

// Byte 0 tags each binary packet, and (being != '{') keeps it from colliding
// with a JSON message. One value per binary message type; bump a value if its
// layout ever changes incompatibly.
static const uint8_t STATE_BIN_VERSION   = 0x05; // per-tick state packet (bumped: position/velocity/angle/scalar fields quantized to shrink the per-object wire cost)
static const uint8_t WELCOME_BIN_VERSION = 0x02; // welcome (slot + static world)
static const uint8_t CHUNK_VERSION       = 0x03; // fragment of an oversized message (see below)
static const uint8_t FULL_BIN_VERSION    = 0x06; // rejection: every player slot is claimed (no payload)

// ---- chunking: oversized UDP messages ----
// A datagram over ~1400 bytes IP-fragments, and some home routers/NATs drop
// fragmented UDP outright - which made the LARGE-map welcome (128 platforms
// ≈ 3.1 KB) undeliverable to those clients, hello-retry included. Any message
// larger than UDP_SAFE_DATAGRAM is instead split into chunks that each fit one
// unfragmented datagram:
//   [u8 CHUNK_VERSION][u8 gen][u8 index][u8 count][payload slice]
// `gen` identifies one logical message (server increments it per chunked
// send), so the client's reassembler can't mix chunks of an old welcome with
// a re-sent one; a chunk from a new gen discards any half-done buffer. There
// are no per-chunk acks: if any chunk is lost the message just never
// completes, and the existing hello-retry loop requests a fresh welcome.
// Max message size: 255 chunks * CHUNK_PAYLOAD ≈ 300 KB.
static const size_t UDP_SAFE_DATAGRAM = 1200;                 // fits every sane MTU
static const size_t CHUNK_HEADER     = 4;
static const size_t CHUNK_PAYLOAD    = UDP_SAFE_DATAGRAM - CHUNK_HEADER;

// ---- state-packet size budget ----
// Approximate per-record sizes of the binary state packet (buildStateBinary in
// server_main.cpp) - keep in sync if that layout changes. Used to clamp the
// asteroid count at match start so a full tick fits ONE unfragmented datagram
// (an oversized tick is chunked with no retransmit: one lost chunk drops the
// whole tick).
// Position/velocity/angle/small-scalar fields are quantized (see the putQ*
// helpers below), which is why these are roughly half their pre-quantization
// size - that's the point: it lets more asteroids fit the same datagram.
static const size_t STATE_OVERHEAD  = 33;  // version/tick/seq/phase/options + section counts
static const size_t PLAYER_BYTES    = 36;  // 29 fixed (quantized) + a typical name (1 + ~6)
static const size_t ASTEROID_BYTES  = 19;  // id + qpos + qvel + qsize + health + qflash
static const size_t ACTION_HEADROOM = 65;  // in-flight rockets (16 B) / explosions (8 B) / audio events (12 B)

// Max asteroids that keep a full state tick under UDP_SAFE_DATAGRAM for a
// roster of nPlayers. 6 players -> 46, 7 players -> 44, 8 players -> 42.
inline int MaxAsteroidsForRoster(int nPlayers) {
    long budget = (long)UDP_SAFE_DATAGRAM - (long)STATE_OVERHEAD - (long)ACTION_HEADROOM
                - (long)nPlayers * (long)PLAYER_BYTES;
    return budget > 0 ? (int)(budget / (long)ASTEROID_BYTES) : 0;
}

// ---- writers: append to a std::string byte buffer ----
inline void putU8 (std::string& b, uint8_t  v) { b.push_back((char)v); }
inline void putU16(std::string& b, uint16_t v) { b.push_back((char)(v & 0xff)); b.push_back((char)((v >> 8) & 0xff)); }
inline void putU32(std::string& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((char)((v >> (8 * i)) & 0xff)); }
inline void putI16(std::string& b, int16_t  v) { putU16(b, (uint16_t)v); }
inline void putI32(std::string& b, int32_t  v) { putU32(b, (uint32_t)v); }
inline void putF32(std::string& b, float    v) { uint32_t u; std::memcpy(&u, &v, 4); putU32(b, u); }
// Short string: 1-byte length (clamped to 255) + raw bytes. Fine for names.
inline void putStr(std::string& b, const std::string& s) {
    uint8_t n = (uint8_t)(s.size() < 255 ? s.size() : 255);
    putU8(b, n);
    b.append(s.data(), n);
}

// ---- quantized writers: fixed-width encodings for bounded fields ----
// The per-tick state packet is the size-critical one (see the budget above),
// and most of its fields live in small, known ranges - full 32-bit floats are
// far more precision than the game needs. These trade a little precision
// (imperceptible at this game's scale) for roughly half the bytes.

// Position component: int16 over +/-QPOS_RANGE. Covers the largest map preset
// (XL, 360 half-size) times GAMESPACE_OUT_OF_BOUNDS_FACTOR (1.5) = 540, plus
// margin. ~1.8cm/step - well under the smallest object radius (PLAYER_RADIUS
// 2.0, ASTEROID_MIN_SIZE 6.0).
static const float QPOS_RANGE = 600.0f;
// Velocity component: int16 over +/-QVEL_RANGE. Covers worst-case rocket speed
// (ROCKET_SPEED 120 * speedBoost max 2 * rocketSpeedScale max 2 = 480) plus
// inherited shooter-velocity headroom.
static const float QVEL_RANGE = 700.0f;

inline int16_t quantizeToI16(float v, float range) {
    if (v > range) v = range;
    if (v < -range) v = -range;
    return (int16_t)std::lround(v / range * 32767.0f);
}
inline float dequantizeFromI16(int16_t q, float range) {
    return (float)q / 32767.0f * range;
}

inline void putQPos(std::string& b, float v) { putI16(b, quantizeToI16(v, QPOS_RANGE)); }
inline void putQVel(std::string& b, float v) { putI16(b, quantizeToI16(v, QVEL_RANGE)); }

// Angle (yaw/pitch): uint16 over a full turn (0..2*PI -> 0..65535).
inline void putQAngle(std::string& b, float radians) {
    const float twoPi = 6.283185307179586f;
    float wrapped = std::fmod(radians, twoPi);
    if (wrapped < 0.0f) wrapped += twoPi;
    putU16(b, (uint16_t)std::lround(wrapped / twoPi * 65535.0f));
}
inline float qAngleToRadians(uint16_t q) {
    const float twoPi = 6.283185307179586f;
    return (float)q / 65535.0f * twoPi;
}

// Generic bounded-scalar: uint8 over [0, max]. Used for every small 0..known-
// max field (health/fuel/ammo already fit uint8 directly; flash timers,
// spectating countdowns, asteroid size, explosion radius, and audio volume
// scale need a max-value scale).
inline void putQFrac(std::string& b, float v, float maxValue) {
    if (v < 0.0f) v = 0.0f;
    if (v > maxValue) v = maxValue;
    uint8_t q = maxValue > 0.0f ? (uint8_t)std::lround(v / maxValue * 255.0f) : 0;
    putU8(b, q);
}
inline float qFracToFloat(uint8_t q, float maxValue) {
    return (float)q / 255.0f * maxValue;
}

// ---- reader: bounds-checked cursor over a byte buffer ----
// Past-the-end reads return 0 / empty and set overran, so a truncated or
// malformed packet degrades to zeros instead of reading out of bounds.
struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool overran = false;

    explicit Reader(const std::string& s)
        : p(reinterpret_cast<const uint8_t*>(s.data())),
          end(reinterpret_cast<const uint8_t*>(s.data()) + s.size()) {}

    uint8_t u8() {
        if (p >= end) { overran = true; return 0; }
        return *p++;
    }
    uint16_t u16() { uint16_t v = u8(); v |= (uint16_t)u8() << 8; return v; }
    uint32_t u32() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= (uint32_t)u8() << (8 * i); return v; }
    int16_t  i16() { return (int16_t)u16(); }
    int32_t  i32() { return (int32_t)u32(); }
    float    f32() { uint32_t u = u32(); float f; std::memcpy(&f, &u, 4); return f; }
    float    qPos()   { return dequantizeFromI16(i16(), QPOS_RANGE); }
    float    qVel()   { return dequantizeFromI16(i16(), QVEL_RANGE); }
    float    qAngle() { return qAngleToRadians(u16()); }
    float    qFrac(float maxValue) { return qFracToFloat(u8(), maxValue); }
    std::string str() {
        uint8_t n = u8();
        std::string s;
        s.reserve(n);
        for (int i = 0; i < n; ++i) s.push_back((char)u8());
        return s;
    }
};

} // namespace nb
