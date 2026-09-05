// nb::ChunkReassembler - does a chunked message survive another one arriving in
// the middle of it?
//
// It did not, before #100. The client held exactly ONE half-built message and
// reset on any chunk with a different gen. That was fine while only oversized
// welcomes chunked. Once 8-player state packets started exceeding the MTU too,
// chunked state arrived 60x a second and a welcome (3 chunks on MEDIUM) could
// essentially never assemble - so a client joining a busy match retried its hello
// forever and never learned its slot.
//
//   g++ -std=c++17 -O2 -I . server/test/reassembly_test.cpp -o /tmp/re && /tmp/re
#include "netbin.h"
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

// Frame `payload` the way SendToClient does.
static std::vector<std::string> chunk(uint8_t gen, const std::string& payload) {
    std::vector<std::string> out;
    size_t count = (payload.size() + nb::CHUNK_PAYLOAD - 1) / nb::CHUNK_PAYLOAD;
    for (size_t i = 0; i < count; ++i) {
        std::string c;
        nb::putU8(c, nb::CHUNK_VERSION);
        nb::putU8(c, gen);
        nb::putU8(c, (uint8_t)i);
        nb::putU8(c, (uint8_t)count);
        c += payload.substr(i * nb::CHUNK_PAYLOAD, nb::CHUNK_PAYLOAD);
        out.push_back(std::move(c));
    }
    return out;
}
static std::string body(char fill, size_t n) { return std::string(n, fill); }

int main() {
    const std::string welcome = body('W', 3000);   // ~MEDIUM: 128 platforms
    const std::string state   = body('S', 1288);   // what the box actually logged

    printf("basic\n");
    {
        nb::ChunkReassembler r;
        std::string got;
        auto parts = chunk(1, welcome);
        for (size_t i = 0; i + 1 < parts.size(); ++i)
            check(!r.Feed(parts[i].data(), parts[i].size(), got), "incomplete so far");
        check(r.Feed(parts.back().data(), parts.back().size(), got), "completes on the last chunk");
        check(got == welcome, "payload round-trips exactly");
    }

    printf("interleaved - the #100 case\n");
    {
        nb::ChunkReassembler r;
        auto w = chunk(1, welcome);      // 3 chunks
        auto s = chunk(2, state);        // 2 chunks
        std::string got;
        std::string gotWelcome, gotState;

        // welcome starts, state arrives entirely in the middle of it, welcome ends
        check(!r.Feed(w[0].data(), w[0].size(), got), "welcome chunk 0");
        check(!r.Feed(s[0].data(), s[0].size(), got), "state chunk 0 interleaves");
        check(!r.Feed(w[1].data(), w[1].size(), got), "welcome chunk 1");
        if (r.Feed(s[1].data(), s[1].size(), got)) gotState = got;
        check(gotState == state, "state completes mid-welcome");
        if (r.Feed(w[2].data(), w[2].size(), got)) gotWelcome = got;
        check(gotWelcome == welcome, "AND the welcome still completes");
    }

    printf("state storm during a welcome\n");
    {
        // The realistic shape: state chunks pouring in at 60 Hz while a welcome
        // trickles through. Against the old single-slot code the welcome never
        // finished.
        nb::ChunkReassembler r;
        auto w = chunk(7, welcome);
        std::string got, gotWelcome;
        int states = 0;
        for (size_t i = 0; i < w.size(); ++i) {
            if (r.Feed(w[i].data(), w[i].size(), got)) gotWelcome = got;
            auto s = chunk((uint8_t)(100 + i), state);   // a fresh gen each tick
            for (auto& part : s) if (r.Feed(part.data(), part.size(), got)) states++;
        }
        check(gotWelcome == welcome, "welcome survives a stream of chunked state");
        check(states == (int)w.size(), "every interleaved state completed too");
    }

    printf("robustness\n");
    {
        nb::ChunkReassembler r;
        std::string got;
        auto p = chunk(3, welcome);
        r.Feed(p[0].data(), p[0].size(), got);
        check(!r.Feed(p[0].data(), p[0].size(), got), "duplicate chunk is ignored");

        // A message that never completes must not pin a slot forever.
        for (int g = 10; g < 40; ++g) {
            auto q = chunk((uint8_t)g, welcome);
            r.Feed(q[0].data(), q[0].size(), got);        // first chunk only
        }
        check(r.InFlight() <= 4, "abandoned messages are reclaimed, slots bounded");

        std::string tiny = "\x03\x01";                     // truncated header
        check(!r.Feed(tiny.data(), tiny.size(), got), "short datagram rejected");
        std::string bad;
        nb::putU8(bad, nb::CHUNK_VERSION); nb::putU8(bad, 5);
        nb::putU8(bad, 9); nb::putU8(bad, 2);              // index 9 of 2
        check(!r.Feed(bad.data(), bad.size(), got), "index past count rejected");
    }

    printf("\n%s\n", failures ? "FAILURES" : "all reassembly checks passed");
    return failures ? 1 : 0;
}
