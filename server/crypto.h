// server/crypto.h
//
// The server's one pair of sign/verify helpers: HMAC-SHA256 over a secret only
// the server holds, plus the constant-time compare that checks a tag.
//
// Two features need exactly this - "stamp something, hand it out, and later
// check the stamp without having written anything down":
//   - E1's UDP handshake cookie (server_main.cpp): proves a datagram's source
//     address is real before we answer it with anything big.
//   - D3's identity token (not yet built): proves a returning player is who
//     they claim, beyond the self-asserted clientId.
// One secret, one construction, one place to get it wrong - which is why this
// is a header of its own rather than a few lines inlined at each call site.
//
// WHY HMAC AND NOT hash(secret || data). Gluing the secret onto the front of a
// Merkle-Damgard hash (SHA-256 included) is the classic prefix-MAC mistake:
// SHA-256's output IS its full internal state, so given one valid tag an
// attacker can resume the hash from it, append their own bytes and produce a
// valid tag for the longer message WITHOUT knowing the secret. Our inputs are
// fixed-shape, which makes that hard to exploit here in particular - but HMAC
// is the construction built to close exactly this hole and costs one extra
// compression block. There is no reason to hand-roll the weaker one.
//
// No OpenSSL. The server links Boost (header-only) and nothing else; pulling in
// libcrypto for one MAC would mean a new package on the deploy box and a new
// way for the build to differ between the Mac and the Linux server. SHA-256 is
// ~80 lines and the RFC 4231 vectors in test/crypto_test.cpp prove this one is
// the real thing.

#pragma once

#include <algorithm>  // std::min (Sha256::Update's buffer fill)
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdlib>    // getenv
#include <chrono>
#include <random>
#include <string>

namespace pz {

//MARK: SHA-256
namespace detail {

inline uint32_t Ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

// FIPS 180-4 round constants: the first 32 bits of the fractional parts of the
// cube roots of the first 64 primes.
inline const uint32_t* Sha256K() {
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    return k;
}

// Streaming SHA-256. Streaming rather than one-shot because HMAC hashes two
// pieces back to back (pad then message) and copying them into one buffer first
// would mean an allocation per MAC on the packet path.
struct Sha256 {
    uint32_t h[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                     0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    uint64_t bits = 0;
    uint8_t  buf[64] = {};
    size_t   have = 0;

    void Block(const uint8_t* p) {
        const uint32_t* K = Sha256K();
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16)
                 | ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = Ror(w[i-15],7) ^ Ror(w[i-15],18) ^ (w[i-15] >> 3);
            const uint32_t s1 = Ror(w[i-2],17) ^ Ror(w[i-2],19)  ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = Ror(e,6) ^ Ror(e,11) ^ Ror(e,25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            const uint32_t S0 = Ror(a,2) ^ Ror(a,13) ^ Ror(a,22);
            const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    void Update(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        bits += (uint64_t)n * 8;
        while (n > 0) {
            const size_t take = std::min(n, sizeof(buf) - have);
            std::memcpy(buf + have, p, take);
            have += take; p += take; n -= take;
            if (have == sizeof(buf)) { Block(buf); have = 0; }
        }
    }

    void Final(uint8_t out[32]) {
        const uint64_t total = bits;
        uint8_t one = 0x80;
        Update(&one, 1);
        const uint8_t zero = 0;
        while (have != 56) Update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(total >> (56 - 8*i));
        Update(len, 8);                 // completes the final block
        for (int i = 0; i < 8; ++i) {
            out[i*4]   = (uint8_t)(h[i] >> 24);
            out[i*4+1] = (uint8_t)(h[i] >> 16);
            out[i*4+2] = (uint8_t)(h[i] >> 8);
            out[i*4+3] = (uint8_t)(h[i]);
        }
    }
};

} // namespace detail

using Mac = std::array<uint8_t, 32>;

inline Mac Sha256(const void* msg, size_t n) {
    detail::Sha256 s;
    s.Update(msg, n);
    Mac out{};
    s.Final(out.data());
    return out;
}

//MARK: HMAC-SHA256
// RFC 2104 over SHA-256. `key` is raw bytes, not hex or text - whatever the
// secret happens to be.
inline Mac HmacSha256(const std::string& key, const void* msg, size_t n) {
    uint8_t k[64] = {};
    if (key.size() > sizeof(k)) {
        const Mac kh = Sha256(key.data(), key.size());
        std::memcpy(k, kh.data(), kh.size());
    } else {
        std::memcpy(k, key.data(), key.size());
    }
    uint8_t ipad[64], opad[64];
    for (size_t i = 0; i < sizeof(k); ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }

    uint8_t inner[32];
    { detail::Sha256 s; s.Update(ipad, sizeof(ipad)); s.Update(msg, n); s.Final(inner); }
    Mac out{};
    { detail::Sha256 s; s.Update(opad, sizeof(opad)); s.Update(inner, sizeof(inner)); s.Final(out.data()); }
    return out;
}

//MARK: Tag encoding + comparison
// Lowercase hex of the first `bytes` of a MAC. Truncating a MAC is normal and
// safe (RFC 2104 §5); 12 bytes = 96 bits = the 24 hex chars the cookie carries,
// which is far past what a remote attacker can search one datagram at a time.
inline std::string HexPrefix(const Mac& mac, size_t bytes) {
    static const char* hexDigits = "0123456789abcdef";
    if (bytes > mac.size()) bytes = mac.size();
    std::string out;
    out.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        out += hexDigits[mac[i] >> 4];
        out += hexDigits[mac[i] & 0x0f];
    }
    return out;
}

// Compare two tags without leaking where they first differ.
//
// A plain `==` returns as soon as it finds a mismatching byte, so the time it
// takes says how many leading bytes were right - and an attacker who can measure
// that recovers a valid tag one byte at a time instead of searching all 2^96.
// This always walks the whole string and folds every difference into one
// accumulator. The length check up front is fine: the length is public (it is
// fixed by the protocol), only the contents are secret.
inline bool ConstantTimeEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= (unsigned)(uint8_t)a[i] ^ (unsigned)(uint8_t)b[i];
    return diff == 0;
}

//MARK: The server secret
// One secret for every tag the server issues, read once at boot from
// PLATFORMZ_IDENTITY_SECRET (it lives beside PLATFORMZ_KEY in /etc/platformz.env
// - see docs/deploy-vultr.md).
//
// It MUST persist across restarts. Generated fresh each boot, every tag already
// in the wild becomes invalid on every deploy: harmless for E1 (clients just
// take one more handshake round trip) but fatal for D3, where it would mean
// every player looks like a brand new person after each release.
//
// Unset falls back to a random one so a dev server still works out of the box -
// with a warning, because that is the mode that quietly breaks D3 later.
// `warnOut` receives a line to log when that happens (empty otherwise); this
// returns the secret rather than printing, so it stays testable.
inline std::string LoadServerSecret(std::string& warnOut) {
    warnOut.clear();
    if (const char* s = std::getenv("PLATFORMZ_IDENTITY_SECRET"); s && *s) {
        const std::string secret(s);
        // Short enough to brute-force offline is worse than useless - it reads
        // as configured while proving nothing. Say so and use it anyway; the
        // operator's choice, not ours to override at boot.
        if (secret.size() < 16)
            warnOut = "PLATFORMZ_IDENTITY_SECRET is shorter than 16 bytes - use a long random string";
        return secret;
    }
    warnOut = "PLATFORMZ_IDENTITY_SECRET unset - using a random per-boot secret "
              "(handshake cookies do not survive a restart)";
    std::random_device rd;
    std::string secret;
    secret.reserve(32);
    for (int i = 0; i < 8; ++i) {
        // Mix the clock in too: random_device is a real CSPRNG on macOS and
        // Linux, but it is allowed to be deterministic, and this is the one
        // path where nobody would ever notice.
        const uint32_t r = rd() ^ (uint32_t)std::chrono::steady_clock::now()
                                      .time_since_epoch().count();
        for (int b = 0; b < 4; ++b) secret += (char)(uint8_t)(r >> (8 * b));
    }
    return secret;
}

} // namespace pz
