// platform_test - the invite string a Steam friend's "Join game" carries.
//
// platform.h's ParseJoinString reads text that came from ANOTHER player's
// machine (their rich presence, relayed by Steam), and what it returns is sent to
// our server as a room code. So the interesting half of this test is what it
// must REFUSE. The other half pins the round trip: the string we publish has to
// be one we accept, or every invite would silently do nothing.
//
// Built WITHOUT PLATFORMZ_STEAM, which is also the point: the stand-ins every
// non-Steam build uses must compile on their own and do nothing.
//
//   g++ -std=c++17 test/platform_test.cpp
#include "../platform.h"

#include <cstdio>
#include <string>

static int failures = 0;

static void eq(const std::string& got, const std::string& want, const std::string& what) {
    const bool ok = got == want;
    printf("  %s %s  ->  \"%s\"\n", ok ? "ok:  " : "FAIL:", what.c_str(), want.c_str());
    if (!ok) { printf("        got: \"%s\"\n", got.c_str()); failures++; }
}

int main() {
    printf("round trip\n");
    eq(platform::ParseJoinString(platform::JoinStringFor("7R4Z")), "7R4Z", "what we publish is what we read");
    eq(platform::JoinStringFor("7R4Z"), "--match 7R4Z", "and it is the command-line flag, so a launch reads it too");

    printf("accepted\n");
    eq(platform::ParseJoinString("--match 7r4z"), "7R4Z", "lower case is uppercased");
    eq(platform::ParseJoinString("  --match   7R4Z  "), "7R4Z", "surrounding spaces");
    eq(platform::ParseJoinString("--match\t7R4Z"), "7R4Z", "a tab separates too");
    eq(platform::ParseJoinString("--match ABCD2345"), "ABCD2345", "up to 8 characters, room for longer codes");

    printf("refused\n");
    eq(platform::ParseJoinString(""), "", "nothing");
    eq(platform::ParseJoinString("7R4Z"), "", "a bare code with no flag");
    eq(platform::ParseJoinString("--match"), "", "the flag with no code");
    eq(platform::ParseJoinString("--match "), "", "the flag and a space");
    eq(platform::ParseJoinString("--match7R4Z"), "", "flag and code run together");
    eq(platform::ParseJoinString("--matchbox 7R4Z"), "", "a longer word that starts with the flag");
    eq(platform::ParseJoinString("--match ABCDEFGH9"), "", "more than 8 characters");
    eq(platform::ParseJoinString("--match 7R4Z extra"), "", "trailing words");
    eq(platform::ParseJoinString("--match 7R4Z --server evil"), "", "a second flag smuggled in");
    eq(platform::ParseJoinString("--match 7R\"4Z"), "", "a quote, which would end a JSON string");
    eq(platform::ParseJoinString("--match 7R-4Z"), "", "punctuation");
    eq(platform::ParseJoinString("--match 7R\xC3\x84Z"), "", "non-ASCII bytes");
    eq(platform::ParseJoinString("+connect 7R4Z"), "", "some other game's connect syntax");

    printf("the stand-ins (no PLATFORMZ_STEAM)\n");
    {
        const bool ok = !platform::Init() && !platform::Active() && platform::TakeJoinRequest().empty();
        platform::Update();
        platform::SetJoinableRoom("7R4Z");
        platform::Shutdown();
        printf("  %s do nothing and report no Steam\n", ok ? "ok:  " : "FAIL:");
        if (!ok) failures++;
    }

    printf(failures ? "\nsome platform checks FAILED\n" : "\nall platform checks passed\n");
    return failures ? 1 : 0;
}
