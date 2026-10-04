// invite_test - invite::ExtractRoomCode, the CODE field's paste.
//
// The browser's COPY INVITE hands out a whole link, the desktop's a bare code,
// and Steam relays "--match CODE". A player already in the game can only paste,
// so the field must take all three - and since the text came from somebody else
// and goes to our server as a room code, it must refuse everything else.
//
//   g++ -std=c++17 test/invite_test.cpp
#include "../invite.h"

#include <cstdio>
#include <string>

static int failures = 0;

static void eq(const std::string& got, const std::string& want, const std::string& what) {
    const bool ok = got == want;
    printf("  %s %s  ->  \"%s\"\n", ok ? "ok:  " : "FAIL:", what.c_str(), want.c_str());
    if (!ok) { printf("        got: \"%s\"\n", got.c_str()); failures++; }
}

int main() {
    using invite::ExtractRoomCode;

    printf("the three shapes an invite comes in\n");
    eq(ExtractRoomCode("7QK2"), "7QK2", "a bare code");
    eq(ExtractRoomCode("https://platformz.space/platformz.html?match=7QK2"), "7QK2", "the browser's link");
    eq(ExtractRoomCode("--match 7QK2"), "7QK2", "the command-line / Steam form");

    printf("links as they really arrive\n");
    eq(ExtractRoomCode("https://platformz.space/platformz.html?key=abc123&match=7QK2"), "7QK2", "after a join key");
    eq(ExtractRoomCode("https://platformz.space/platformz.html?match=7QK2&key=abc123"), "7QK2", "before a join key");
    eq(ExtractRoomCode("http://localhost:8080/platformz.html?server=ws://127.0.0.1:9000&match=7qk2"), "7QK2", "a LAN test link, lower case");
    eq(ExtractRoomCode("https://platformz.space/?match=7QK2#top"), "7QK2", "a fragment after it");
    eq(ExtractRoomCode("https://platformz.space/?match=7QK2\n"), "7QK2", "a trailing newline from the copy");
    eq(ExtractRoomCode("  7qk2  "), "7QK2", "a bare code with spaces, lower case");

    printf("refused\n");
    eq(ExtractRoomCode(""), "", "nothing");
    eq(ExtractRoomCode("https://platformz.space/platformz.html"), "", "a link with no match");
    eq(ExtractRoomCode("https://platformz.space/?match="), "", "an empty match");
    eq(ExtractRoomCode("https://platformz.space/?rematch=7QK2"), "", "a parameter that only ends in match=");
    eq(ExtractRoomCode("https://platformz.space/match=7QK2"), "", "match= in the path, not the query");
    eq(ExtractRoomCode("https://platformz.space/?match=7Q%22K2"), "", "an escaped quote");
    eq(ExtractRoomCode("https://platformz.space/?match=ABCDEFGHJ"), "", "a code over 8 characters");
    eq(ExtractRoomCode("join me: https://platformz.space/?match=7QK2"), "", "a sentence around the link");
    eq(ExtractRoomCode("7Q-K2"), "", "punctuation in a bare code");
    eq(ExtractRoomCode("--match 7QK2 extra"), "", "trailing words after the flag");
    eq(ExtractRoomCode("--match7QK2"), "", "flag and code run together");
    eq(ExtractRoomCode("7Q\xC3\x84K"), "", "non-ASCII bytes");

    printf(failures ? "\nsome invite checks FAILED\n" : "\nall invite checks passed\n");
    return failures ? 1 : 0;
}
