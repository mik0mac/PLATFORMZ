// server/jsonmin.h
//
// The hand-rolled JSON *writers* the server's packet builders share. Split out
// of server_main.cpp when the match state moved into match.h (A1): the state and
// welcome builders are Match methods now, and both they and the server-level
// builders (leaderboard, full) need these, so they can no longer live below the
// point where Match is defined.
//
// Writers only - the matching parsers stay in server_main.cpp, which is the only
// thing that reads inbound JSON. Deliberately not a general JSON library: this
// covers exactly what the packets need (floats, ints, bools, strings).

#pragma once

#include <string>
#include <cstdint>
#include <sstream>
#include <iomanip>

// 2 decimal places is sufficient precision for positions/velocities.
inline std::string jf(float v) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << v;
    return ss.str();
}
inline std::string ji(int v)      { return std::to_string(v); }
inline std::string ju(uint32_t v) { return std::to_string(v); }
inline std::string jb(bool v)     { return v ? "true" : "false"; }

// Quote + escape a string for JSON. Client names are limited to printable
// chars 32-125 (see UiTextField), so only " and \ need escaping.
inline std::string js(const std::string& v) {
    std::string out = "\"";
    for (char c : v) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}
