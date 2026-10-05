// invite.h - reading a room code out of whatever a player was handed.
//
// One invite, three shapes, depending on where it came from:
//   7QK2                                              read aloud, or COPY INVITE on desktop
//   https://platformz.space/platformz.html?match=7QK2  COPY INVITE in the browser
//   --match 7QK2                                      the command line, and a Steam "Join game"
// The browser hands out the link because a link is the best thing to text
// someone with nothing open - one click and they are in. But a player who is
// ALREADY in the game, especially the desktop app, can only paste, so the CODE
// field has to understand all three. Everything that reads one goes through here,
// so the rules cannot drift apart between paste, Steam and the command line.
//
// Strict on purpose: the text came from somebody else, and what comes out is sent
// to our server as a room code. Letters and digits only, at most 8 (codes are 4
// today), uppercased - or nothing.

#pragma once

#include <string>
#include <cctype>

namespace invite {

// A room code, normalised, or "" when `s` is not one exactly.
inline std::string NormalizeCode(const std::string& s) {
    if (s.empty() || s.size() > 8) return std::string();
    std::string code;
    for (unsigned char c : s) {
        if (c > 127 || !std::isalnum(c)) return std::string();
        code.push_back((char)std::toupper(c));
    }
    return code;
}

// The room code in anything a player might paste, or "" when there isn't one.
inline std::string ExtractRoomCode(const std::string& text) {
    // Trim: a copied link often carries a trailing newline.
    size_t b = 0, e = text.size();
    while (b < e && std::isspace((unsigned char)text[b])) ++b;
    while (e > b && std::isspace((unsigned char)text[e - 1])) --e;
    const std::string t = text.substr(b, e - b);

    // A bare code.
    const std::string bare = NormalizeCode(t);
    if (!bare.empty()) return bare;

    // "--match CODE": one separating run of whitespace, nothing after.
    const std::string flag = "--match";
    if (t.compare(0, flag.size(), flag) == 0) {
        size_t i = flag.size();
        if (i >= t.size() || !std::isspace((unsigned char)t[i])) return std::string();
        while (i < t.size() && std::isspace((unsigned char)t[i])) ++i;
        return NormalizeCode(t.substr(i));
    }

    // A link: the value of a match= query parameter. Only as a real parameter -
    // after '?' or '&' - so "rematch=" or a path containing "match=" does not
    // count. No whitespace inside a link: a pasted sentence is not one.
    for (char c : t) if (std::isspace((unsigned char)c)) return std::string();
    const std::string key = "match=";
    for (size_t at = t.find(key); at != std::string::npos; at = t.find(key, at + 1)) {
        if (at == 0 || (t[at - 1] != '?' && t[at - 1] != '&')) continue;
        const size_t from = at + key.size();
        size_t to = from;
        while (to < t.size() && t[to] != '&' && t[to] != '#') ++to;
        return NormalizeCode(t.substr(from, to - from));
    }
    return std::string();
}

} // namespace invite
