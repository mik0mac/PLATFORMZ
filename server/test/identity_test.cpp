// server/test/identity_test.cpp
//
// The server-issued identity token (D3).
//
// Most of what matters here is the REJECTION side, and none of it is visible
// from the game: a token that verifies when it should not just means somebody
// else's leaderboard row, silently, forever. So the checks below spend most of
// their time on tokens that must fail - forged tags, swapped halves, the wrong
// secret, and the near-misses a lenient hex parser would wave through.
//
//   g++ -std=c++17 -O2 -I server -I . server/test/identity_test.cpp -o /tmp/it
//
// Run by server/test/run_all.sh.

#include "identity.h"

#include <iostream>
#include <set>
#include <string>

static int failures = 0;

static void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
    if (!ok) failures++;
}

int main() {
    std::cout << "identity_test: server-issued identity token\n";

    const std::string SECRET  = "a-long-random-server-secret-value";
    const std::string ROTATED = "the-operator-rotated-the-secret!!";

    //MARK: Round trip
    std::string id;
    const std::string tok = ident::Mint(SECRET);
    check(tok.size() == ident::TOKEN_CHARS,
          "a token is " + std::to_string(ident::TOKEN_CHARS) + " hex chars (got " +
          std::to_string(tok.size()) + ")");
    check(tok.find_first_not_of("0123456789abcdef") == std::string::npos,
          "...and is hex, so it survives a URL query and a JSON string untouched");
    check(ident::Verify(SECRET, tok, id), "a freshly minted token verifies");
    check(id.size() == ident::ID_BYTES * 2, "it yields a 32-char identity");
    check(tok.rfind(id, 0) == 0, "which is the token's own first half");

    {
        std::string again;
        ident::Verify(SECRET, tok, again);
        check(again == id, "verifying twice yields the same identity");
    }

    //MARK: Identities are distinct
    {
        std::set<std::string> ids;
        for (int i = 0; i < 500; ++i) {
            std::string s;
            if (ident::Verify(SECRET, ident::Mint(SECRET), s)) ids.insert(s);
        }
        check(ids.size() == 500, "500 mints gave 500 distinct identities (got " +
                                 std::to_string(ids.size()) + ")");
    }

    //MARK: Rejection - the half that matters
    {
        std::string out;
        check(!ident::Verify(ROTATED, tok, out),
              "a token signed by the PREVIOUS secret is refused");
        check(out.empty(), "...and yields no identity to accidentally use");
    }
    {
        // Flip one hex digit of the tag. The id is untouched, so a verifier that
        // checked only the shape would hand back a perfectly good-looking id.
        std::string bad = tok;
        bad[ident::TOKEN_CHARS - 1] = (bad[ident::TOKEN_CHARS - 1] == 'a') ? 'b' : 'a';
        std::string out;
        check(!ident::Verify(SECRET, bad, out), "a one-digit change to the tag is refused");
    }
    {
        // Change the id and keep the tag: the classic "claim someone else's
        // identity" attempt.
        std::string bad = tok;
        bad[0] = (bad[0] == 'a') ? 'b' : 'a';
        std::string out;
        check(!ident::Verify(SECRET, bad, out), "an id edited under its own tag is refused");
    }
    {
        // Two valid tokens, spliced. Each half is genuine; the pair is not.
        const std::string other = ident::Mint(SECRET);
        const std::string frankenstein = tok.substr(0, ident::ID_BYTES * 2) +
                                         other.substr(ident::ID_BYTES * 2);
        std::string out;
        check(!ident::Verify(SECRET, frankenstein, out),
              "one token's id with another's tag is refused");
    }
    {
        std::string out;
        check(!ident::Verify(SECRET, "", out), "empty token");
        check(!ident::Verify(SECRET, tok.substr(0, ident::TOKEN_CHARS - 1), out),
              "one character short");
        check(!ident::Verify(SECRET, tok + "0", out), "one character long");
        check(!ident::Verify(SECRET, std::string(ident::TOKEN_CHARS, 'z'), out),
              "right length, not hex");
        check(!ident::Verify(SECRET, std::string(ident::TOKEN_CHARS, '0'), out),
              "right length, all zeroes");
        // A parser that ignored what it could not read would turn this into the
        // all-zero id with an empty tag and then compare two empty strings.
        check(!ident::Verify(SECRET, std::string(ident::TOKEN_CHARS, ' '), out),
              "right length, all spaces");
    }
    {
        // No secret configured at all must not become "everything verifies".
        std::string out;
        check(!ident::Verify("", tok, out), "an empty secret verifies nothing");
    }

    //MARK: The tag really depends on the secret
    {
        // Same id, two secrets, two tags - the property the whole scheme rests
        // on. Minting twice cannot show this (the ids differ), so sign by hand.
        std::vector<uint8_t> raw;
        pz::FromHex(tok.substr(0, ident::ID_BYTES * 2), raw, ident::ID_BYTES);
        check(ident::TagFor(SECRET, raw.data()) != ident::TagFor(ROTATED, raw.data()),
              "the same id signs differently under a different secret");
    }

    if (failures) { std::cout << failures << " CHECK(S) FAILED\n"; return 1; }
    std::cout << "identity_test: all checks passed\n";
    return 0;
}
