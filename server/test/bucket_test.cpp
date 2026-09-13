// server/test/bucket_test.cpp
//
// The token bucket behind every rate limit the server applies (E1's match-list
// and bad-code budgets, E2's room moves and rooms-per-address).
//
// This test exists because of one bug, and the bug is the point: the bucket used
// to start empty and rely on "now minus zero is a huge number" to fill itself on
// first use. That holds on any machine whose monotonic clock has been running a
// while - which is every developer machine - and fails on a host that has just
// booted, where a brand-new address got a FRACTION of its room-creation budget
// for the first six minutes after every restart. No probe could see it, because
// a probe runs against whatever uptime the machine happens to have. A unit test
// with a clock it controls can.
//
//   g++ -std=c++17 -O2 -I server -I . server/test/bucket_test.cpp -o /tmp/bt
//
// Run by server/test/run_all.sh.

#include "bucket.h"

#include <iostream>
#include <string>

static int failures = 0;

static void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
    if (!ok) failures++;
}

int main() {
    std::cout << "bucket_test: token bucket\n";

    // The server's real numbers, so a change to them trips this too.
    const double LIST_BURST = 3.0,  LIST_PER_SEC   = 1.0;
    const double CREATE_BURST = 3.0, CREATE_PER_SEC = 1.0 / 120.0;

    //MARK: A fresh bucket is full, whatever the clock says
    {
        // t=0: a machine that booted this instant. THE regression case.
        pz::Bucket b;
        int got = 0;
        for (int i = 0; i < 5; ++i) if (pz::TakeToken(b, 0.0, CREATE_BURST, CREATE_PER_SEC)) got++;
        check(got == 3, "at t=0 a new bucket still gives its full burst (got " +
                        std::to_string(got) + " of 3)");
    }
    {
        // t=240: the CI runner that found this. Before the fix, 240/120 = 2.
        pz::Bucket b;
        int got = 0;
        for (int i = 0; i < 5; ++i) if (pz::TakeToken(b, 240.0, CREATE_BURST, CREATE_PER_SEC)) got++;
        check(got == 3, "at t=240 too (the uptime that exposed the bug)");
    }
    {
        // t=1e6: a box up for eleven days, which is where it always worked.
        pz::Bucket b;
        int got = 0;
        for (int i = 0; i < 5; ++i) if (pz::TakeToken(b, 1e6, CREATE_BURST, CREATE_PER_SEC)) got++;
        check(got == 3, "and at t=1e6, which is the only case that used to pass");
    }

    //MARK: Spending and refilling
    {
        pz::Bucket b;
        double t = 100.0;
        int burst = 0;
        while (pz::TakeToken(b, t, LIST_BURST, LIST_PER_SEC)) burst++;
        check(burst == 3, "the burst is spendable all at once (" + std::to_string(burst) + ")");
        check(!pz::TakeToken(b, t, LIST_BURST, LIST_PER_SEC), "and then it is empty");

        t += 1.0;
        check(pz::TakeToken(b, t, LIST_BURST, LIST_PER_SEC), "one second later, one token");
        check(!pz::TakeToken(b, t, LIST_BURST, LIST_PER_SEC), "...and only one");

        t += 0.5;
        check(!pz::TakeToken(b, t, LIST_BURST, LIST_PER_SEC), "half a second buys nothing");
        t += 0.5;
        check(pz::TakeToken(b, t, LIST_BURST, LIST_PER_SEC), "the other half completes it");
    }

    //MARK: The cap
    {
        pz::Bucket b;
        double t = 500.0;
        while (pz::TakeToken(b, t, LIST_BURST, LIST_PER_SEC)) {}
        t += 3600.0;   // an hour idle
        int got = 0;
        while (pz::TakeToken(b, t, LIST_BURST, LIST_PER_SEC)) got++;
        check(got == 3, "idling does not bank more than the burst (" +
                        std::to_string(got) + ")");
    }

    //MARK: Sustained rate is the refill rate
    {
        // A script asking as fast as it can for a minute gets burst + 60, never
        // more. This is the property the limits actually rest on.
        pz::Bucket b;
        int served = 0;
        for (int ms = 0; ms <= 60000; ms += 10) {
            const double t = 1000.0 + ms / 1000.0;
            if (pz::TakeToken(b, t, LIST_BURST, LIST_PER_SEC)) served++;
        }
        check(served >= 61 && served <= 64,
              "6000 requests over 60s served " + std::to_string(served) +
              " (burst 3 + ~60 refilled)");
    }

    //MARK: The slow bucket
    {
        // Rooms per address: 3 in hand, one back every two minutes.
        pz::Bucket b;
        double t = 10.0;
        int got = 0;
        while (pz::TakeToken(b, t, CREATE_BURST, CREATE_PER_SEC)) got++;
        check(got == 3, "three rooms up front");
        t += 119.0;
        check(!pz::TakeToken(b, t, CREATE_BURST, CREATE_PER_SEC), "nothing at 119s");
        t += 1.0;
        check(pz::TakeToken(b, t, CREATE_BURST, CREATE_PER_SEC), "a fourth at 120s");
    }

    if (failures) { std::cout << failures << " CHECK(S) FAILED\n"; return 1; }
    std::cout << "bucket_test: all checks passed\n";
    return 0;
}
