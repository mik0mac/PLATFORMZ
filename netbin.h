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

namespace nb {

// Byte 0 tags each binary packet, and (being != '{') keeps it from colliding
// with a JSON message. One value per binary message type; bump a value if its
// layout ever changes incompatibly.
static const uint8_t STATE_BIN_VERSION   = 0x01; // per-tick state packet
static const uint8_t WELCOME_BIN_VERSION = 0x02; // welcome (slot + static world)

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
    std::string str() {
        uint8_t n = u8();
        std::string s;
        s.reserve(n);
        for (int i = 0; i < n; ++i) s.push_back((char)u8());
        return s;
    }
};

} // namespace nb
