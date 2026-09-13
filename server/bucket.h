// server/bucket.h
//
// The token bucket behind every rate limit the server applies (E1, E2): match
// list replies, room moves, and rooms minted per source address.
//
// Its own header so it can be tested without a server. That is not tidiness -
// the bug that put it here was invisible on any machine that had been up for
// more than a few minutes, so nothing short of a unit test with a controlled
// clock could have held it. See TakeToken.

#pragma once

#include <algorithm>

namespace pz {

// `burst` tokens to spend, refilled at `perSec`, one spent per request.
//
// Buckets rather than flat intervals throughout, because everything being
// limited is something a person does in short bursts and a script does forever:
// the burst is what keeps the UI honest, the refill rate is what bounds the
// script.
struct Bucket {
    // Negative means "never used". A bucket starts FULL on first sight, and this
    // says so explicitly rather than leaning on arithmetic - see TakeToken.
    double tokens = -1.0;
    double filled = 0.0;   // when `tokens` was last topped up
};

// Spend one token if there is one. `now` is any monotonic seconds clock.
//
// THE FIRST CALL FILLS THE BUCKET OUTRIGHT. An earlier version instead let the
// refill maths do it - a fresh Bucket has filled == 0, so `now - 0` is however
// long the clock has been running, which clamps to a full bucket. That is true
// only if `now` is large, and the server's NowSec() is the STEADY clock, whose
// epoch is boot. On a developer's machine, up for days, it always was. On a
// freshly booted host it is not: a server four minutes old handed a brand-new
// address 4*60/120 = 2 of its 3 room-creation tokens, so for the first six
// minutes after every restart players quietly got a smaller budget than the one
// documented. CI caught it on a runner that had been up a couple of minutes. The
// fast buckets (1 token/s) hid it by being full within seconds; the slow one
// could not.
inline bool TakeToken(Bucket& b, double now, double burst, double perSec) {
    if (b.tokens < 0.0) b.tokens = burst;                 // first sight
    else b.tokens = std::min(burst, b.tokens + (now - b.filled) * perSec);
    b.filled = now;
    if (b.tokens < 1.0) return false;
    b.tokens -= 1.0;
    return true;
}

} // namespace pz
