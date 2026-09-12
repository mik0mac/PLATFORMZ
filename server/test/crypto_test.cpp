// server/test/crypto_test.cpp
//
// Proves server/crypto.h is really SHA-256 and really HMAC, against the
// published vectors (FIPS 180-4 for the hash, RFC 4231 for the MAC).
//
// This test exists because a hand-rolled MAC has no failure mode you can see.
// A subtly wrong SHA-256 still produces 32 pretty-looking random bytes, still
// verifies against itself, and would pass every end-to-end handshake probe we
// have - while being a function whose security properties nobody has ever
// analysed. The vectors are the only thing that says "this is the primitive we
// think it is".
//
//   g++ -std=c++17 -O2 -I server -I . server/test/crypto_test.cpp -o /tmp/ct
//
// Run by server/test/run_all.sh.

#include "crypto.h"

#include <iostream>
#include <string>

static int failures = 0;

static void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
    if (!ok) failures++;
}

static std::string hex(const pz::Mac& m) { return pz::HexPrefix(m, m.size()); }

static std::string repeat(char c, size_t n) { return std::string(n, c); }

int main() {
    std::cout << "crypto_test: SHA-256 / HMAC-SHA256 / constant-time compare\n";

    //MARK: SHA-256 (FIPS 180-4 examples)
    {
        const pz::Mac m = pz::Sha256("abc", 3);
        check(hex(m) == "ba7816bf8f01cfea414140de5dae2223"
                        "b00361a396177a9cb410ff61f20015ad", "sha256(\"abc\")");
    }
    {
        const pz::Mac m = pz::Sha256("", 0);
        check(hex(m) == "e3b0c44298fc1c149afbf4c8996fb924"
                        "27ae41e4649b934ca495991b7852b855", "sha256(\"\")");
    }
    {
        // 56 bytes: lands exactly on the length-padding boundary, which is the
        // one place a streaming implementation goes wrong.
        const std::string in = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        const pz::Mac m = pz::Sha256(in.data(), in.size());
        check(hex(m) == "248d6a61d20638b8e5c026930c3e6039"
                        "a33ce45964ff2167f6ecedd419db06c1", "sha256(56-byte input)");
    }
    {
        // A million 'a's: forces thousands of blocks, so a broken Update() that
        // works for one block cannot hide.
        pz::detail::Sha256 s;
        const std::string chunk = repeat('a', 1000);
        for (int i = 0; i < 1000; ++i) s.Update(chunk.data(), chunk.size());
        pz::Mac m{};
        s.Final(m.data());
        check(hex(m) == "cdc76e5c9914fb9281a1c7e284d73e67"
                        "f1809a48a497200e046d39ccc7112cd0", "sha256(1e6 x 'a')");
    }

    //MARK: HMAC-SHA256 (RFC 4231)
    {
        const std::string key = repeat((char)0x0b, 20);
        const std::string msg = "Hi There";
        check(hex(pz::HmacSha256(key, msg.data(), msg.size()))
              == "b0344c61d8db38535ca8afceaf0bf12b"
                 "881dc200c9833da726e9376c2e32cff7", "RFC 4231 case 1");
    }
    {
        const std::string key = "Jefe";
        const std::string msg = "what do ya want for nothing?";
        check(hex(pz::HmacSha256(key, msg.data(), msg.size()))
              == "5bdcc146bf60754e6a042426089575c7"
                 "5a003f089d2739839dec58b964ec3843", "RFC 4231 case 2");
    }
    {
        const std::string key = repeat((char)0xaa, 20);
        const std::string msg = repeat((char)0xdd, 50);
        check(hex(pz::HmacSha256(key, msg.data(), msg.size()))
              == "773ea91e36800e46854db8ebd09181a7"
                 "2959098b3ef8c122d9635514ced565fe", "RFC 4231 case 3");
    }
    {
        // Key longer than the 64-byte block, so it gets hashed first. This is
        // the branch a short-key-only implementation silently skips.
        const std::string key = repeat((char)0xaa, 131);
        const std::string msg = "Test Using Larger Than Block-Size Key - Hash Key First";
        check(hex(pz::HmacSha256(key, msg.data(), msg.size()))
              == "60e431591ee0b67f0d8a26aacbf5b77f"
                 "8e0bc6213728c5140546040f0ee37f54", "RFC 4231 case 6 (long key)");
    }

    //MARK: Truncation
    {
        const pz::Mac m = pz::HmacSha256("Jefe", "what do ya want for nothing?", 28);
        const std::string t = pz::HexPrefix(m, 12);
        check(t.size() == 24, "HexPrefix(12) is 24 hex chars (the cookie's width)");
        check(t == hex(m).substr(0, 24), "truncation is a prefix of the full tag");
    }

    //MARK: Constant-time compare
    {
        check(pz::ConstantTimeEqual("abc123", "abc123"), "equal strings compare equal");
        check(!pz::ConstantTimeEqual("abc123", "abc124"), "last-byte difference caught");
        check(!pz::ConstantTimeEqual("abc123", "zbc123"), "first-byte difference caught");
        check(!pz::ConstantTimeEqual("abc123", "abc1234"), "length difference caught");
        check(!pz::ConstantTimeEqual("", "x"), "empty vs non-empty");
        check(pz::ConstantTimeEqual("", ""), "empty vs empty");
    }

    //MARK: The secret loader
    {
        // Unset -> random, and it warns. Two loads must differ, or "random" is a
        // constant and every cookie in the world is forgeable.
        unsetenv("PLATFORMZ_IDENTITY_SECRET");
        std::string w1, w2;
        const std::string a = pz::LoadServerSecret(w1);
        const std::string b = pz::LoadServerSecret(w2);
        check(!w1.empty(), "unset secret warns");
        check(a.size() >= 16 && a != b, "unset secret is long and random per call");

        setenv("PLATFORMZ_IDENTITY_SECRET", "a-long-enough-test-secret-value", 1);
        std::string w3;
        check(pz::LoadServerSecret(w3) == "a-long-enough-test-secret-value" && w3.empty(),
              "configured secret is used verbatim, no warning");

        setenv("PLATFORMZ_IDENTITY_SECRET", "short", 1);
        std::string w4;
        check(pz::LoadServerSecret(w4) == "short" && !w4.empty(),
              "short secret is honoured but warned about");
        unsetenv("PLATFORMZ_IDENTITY_SECRET");
    }

    if (failures) { std::cout << failures << " CHECK(S) FAILED\n"; return 1; }
    std::cout << "crypto_test: all checks passed\n";
    return 0;
}
