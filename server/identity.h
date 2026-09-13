// server/identity.h
//
// The server-issued identity token (D3): "the same client as last time", proved
// rather than asserted.
//
// WHY, GIVEN clientId ALREADY EXISTS. `clientId` is minted by the client and
// lives in a file the player owns, so it is an identifier and not a credential -
// anyone can present anyone's. That is fine for what reads it today (D2's
// 15-second slot restore: the window is short, the slot must be sitting vacant,
// and the prize is somebody's half-dead body). It is useless for anything that
// outlives a match, which is why the scoreboard is still keyed on display name
// and why two players typing MIKE share a row.
//
// A token fixes that without a user table. The server mints a random id, signs
// it with its own secret, and hands back id‖tag. When it comes back it re-computes
// the tag and compares - so the server stores NO record of the player and still
// knows the id is one it issued. Same trick as E1's handshake cookie, same
// secret, same helpers (crypto.h).
//
// WHAT IT DOES NOT BUY, stated plainly because the name invites more:
// it proves CONTINUITY, not identity. Nothing stops a player copying their own
// token to a second machine, or running several clients to farm a leaderboard.
// It is pseudonymous, not authenticated - enough for a friends-and-family
// ranking, and not enough for a competitive public one, which would need real
// accounts. Do not let a future feature quietly assume otherwise.
//
// HEX, NOT BASE64, which is what the plan wrote. A token rides the WebSocket
// upgrade URL as a query parameter and sits in JSON on the UDP hello, so it has
// to be URL-safe; base64 is not, base64url is, and hex is both without a codec
// to write or get wrong. The cost is 64 characters instead of 43, in a field no
// human reads. The security is identical - this is an encoding, not a cipher.

#pragma once

#include "crypto.h"

#include <string>
#include <vector>

namespace ident {

// 128-bit id, 128-bit tag. The id needs enough width that two players never
// collide (birthday-bound at 2^64 issued tokens, which is not a number of
// players); the tag needs enough that forging one is hopeless without the
// secret. Truncating HMAC-SHA256 to 128 bits is normal and safe (RFC 2104 §5).
constexpr size_t ID_BYTES    = 16;
constexpr size_t TAG_BYTES   = 16;
constexpr size_t TOKEN_CHARS = (ID_BYTES + TAG_BYTES) * 2;   // 64 hex chars

// The identity string a token proves: the id half, as hex. This - not the token -
// is what anything downstream should key on. The token is a bearer credential and
// belongs only in the client's profile and on the wire; the id is safe to log, to
// write to the score file, and to show in a debug overlay.
inline std::string IdOf(const std::vector<uint8_t>& raw) {
    return pz::ToHex(raw.data(), ID_BYTES);
}

// Sign an id. Split out so Mint and Verify cannot drift apart - a signer and a
// verifier that disagree by one byte is a bug with no symptom except that
// everybody silently becomes a new player.
inline std::string TagFor(const std::string& secret, const uint8_t* id) {
    return pz::HexPrefix(pz::HmacSha256(secret, id, ID_BYTES), TAG_BYTES);
}

// A fresh token. The id is random, not derived from anything the client said -
// signing a client-chosen value would let anyone mint a token for any identity,
// which is the whole failure this replaces.
inline std::string Mint(const std::string& secret) {
    const std::string idHex = pz::RandomHex(ID_BYTES);
    std::vector<uint8_t> id;
    if (!pz::FromHex(idHex, id, ID_BYTES) || id.size() != ID_BYTES) return "";
    return idHex + TagFor(secret, id.data());
}

// True if `token` is one we issued under this secret; `idOut` then receives the
// identity it carries.
//
// Returns false for anything malformed, anything signed by a previous secret,
// and anything a client made up. Every caller must treat that as "no token" and
// mint a fresh one - NEVER as a reason to refuse the join. A player whose token
// went stale because the operator rotated the secret is still a player, and
// locking them out of the game to protect a leaderboard row would be a bad trade
// made silently.
inline bool Verify(const std::string& secret, const std::string& token,
                   std::string& idOut) {
    idOut.clear();
    if (secret.empty() || token.size() != TOKEN_CHARS) return false;
    std::vector<uint8_t> raw;
    if (!pz::FromHex(token, raw, ID_BYTES + TAG_BYTES)) return false;
    if (raw.size() != ID_BYTES + TAG_BYTES) return false;

    const std::string want = TagFor(secret, raw.data());
    const std::string have = token.substr(ID_BYTES * 2);
    // Constant-time: a byte-at-a-time compare leaks how much of a forged tag was
    // right, which is how you recover one without the secret.
    if (!pz::ConstantTimeEqual(want, have)) return false;

    idOut = IdOf(raw);
    return true;
}

} // namespace ident
