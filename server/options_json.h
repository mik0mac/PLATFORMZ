// server/options_json.h
//
// The server's INBOUND JSON parsers, and the match-rule bundle as JSON in both
// directions. Split out of server_main.cpp so a test can push a real client
// message through the same code the server runs (test/options_wire_test.cpp) -
// the client writes these messages with a real JSON library and the server
// reads them by string search, and nothing used to check the two agreed on
// every rule.
//
// jsonmin.h holds the matching writers (jf/ji/jb/js).

#pragma once

#include "jsonmin.h"
#include "../options.h"   // MatchOptions, VisitMatchOptions

#include <cstdint>
#include <string>

// -------------------------------------------------------------------------
// Packet parsing - JSON input from client into PlayerInput.
// Minimal hand-rolled parser: finds keys by string search.
// Robust enough for a known fixed schema; not a general JSON parser.
// -------------------------------------------------------------------------
inline float parseFloat(const std::string& json, const std::string& key, float def = 0.0f) {
    auto pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos) return def;
    pos += key.size() + 3;
    try { return std::stof(json.substr(pos)); } catch (...) { return def; }
}
inline bool parseBool(const std::string& json, const std::string& key, bool def = false) {
    auto pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos) return def;
    pos += key.size() + 3;
    return json.substr(pos, 4) == "true";
}
inline uint32_t parseUInt(const std::string& json, const std::string& key, uint32_t def = 0) {
    auto pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos) return def;
    pos += key.size() + 3;
    try { return (uint32_t)std::stoul(json.substr(pos)); } catch (...) { return def; }
}
// Read a JSON string value ("key":"..."). Unescapes \" and \\ (the only escapes
// nlohmann emits for the client's printable-only names). Stops at the closing ".
inline std::string parseString(const std::string& json, const std::string& key,
                               const std::string& def = "") {
    auto pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos) return def;
    pos += key.size() + 3;                 // skip past "key":
    if (pos >= json.size() || json[pos] != '"') return def;
    ++pos;                                  // skip the opening quote of the value
    std::string out;
    for (; pos < json.size(); ++pos) {
        char c = json[pos];
        if (c == '\\' && pos + 1 < json.size()) { out.push_back(json[++pos]); continue; }
        if (c == '"') break;                // closing quote
        out.push_back(c);
    }
    return out;
}

//MARK: Options as text
// The two directions the rule bundle crosses the wire as JSON, both walking the
// one list in options.h (VisitMatchOptions) rather than keeping their own.

// Bundle -> the body of a JSON object: "nplayers":4,"maxbots":7,...
struct OptionJsonWriter {
    std::string& s;
    bool first = true;
    void key(const char* k) { s += first ? "\"" : ",\""; first = false; s += k; s += "\":"; }
    void integer(const char* k, int v)                { key(k); s += ji(v); }
    void real   (const char* k, float v)              { key(k); s += jf(v); }
    void flag   (const char* k, bool v)               { key(k); s += jb(v); }
    void text   (const char* k, const std::string& v) { key(k); s += js(v); }
};

// A client's "start"/"options" message -> bundle. A key the message does not
// carry leaves the field as it was, so WHAT THE BUNDLE HELD GOING IN is the
// fallback: the room's current rules for "options" (a partial update), a fresh
// MatchOptions for "start" (absent means the compile-time default).
struct OptionJsonReader {
    const std::string& msg;
    void integer(const char* k, int& v)   { v = (int)parseUInt(msg, k, (uint32_t)v); }
    void real   (const char* k, float& v) { v = parseFloat(msg, k, v); }
    void flag   (const char* k, bool& v)  { v = parseBool(msg, k, v); }
    void text   (const char* k, std::string& v) {
        const std::string t = parseString(msg, k);
        if (!t.empty()) v = t;
    }
};
