// server/registry.h
//
// MatchRegistry - which rooms exist, and the rules for when one is born and dies.
//
// Sits ABOVE Match in the layering (see docs/matchmaking-plan.md): a Match is the
// gameplay layer and knows nothing about being discoverable. The registry owns
// codes, creation, the concurrency cap, and reaping.
//
// THE LOCKING RULE THAT SHAPES THIS FILE. The match list must be buildable while
// holding ONLY registryMutex - never a Match's gameMutex or clientMutex, which
// the sim thread holds every tick. So each entry splits its facts by mutability:
//
//   static  (in Entry)          code, name, private, joinCode, presetName,
//                               optionsLocked, autoStart, createdAt
//   live    (atomics on Match)  gamePhase, connectedCount
//
// Listing reads the statics straight out of the Entry and the live pair through
// atomics, so a browsing client never waits on a simulation.
//
// Codes are NEVER reused. A datagram still in flight for a reaped match must not
// be able to land on a new one that happens to have taken its code back.

#pragma once

#include "match.h"
#include "../options.h"      // MatchPreset, MatchPresetByName

#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

//MARK: Entry
struct MatchEntry {
    std::shared_ptr<Match> match;

    std::string code;        // 4 chars, also the invite code
    std::string name;        // display name in the browser
    bool        isPrivate = false;
    std::string joinCode;    // set only when private; gates joining
    std::string presetName = "DEFAULT";
    bool        optionsLocked = false;
    bool        autoStart     = false;
    Match::Clock::time_point createdAt{};

    // Set when the room first had nobody in it; reset the moment someone joins.
    // Reaping measures from here, not from createdAt.
    Match::Clock::time_point emptySince{};
    bool                     everOccupied = false;
};

//MARK: One row of the browser list
// A flat snapshot, taken under registryMutex, so callers can build a packet
// without holding any lock at all.
struct MatchListing {
    std::string code, name, presetName;
    Phase phase = Phase::LOBBY;
    int   players = 0;
    int   maxPlayers = GAMESPACE_NUMBER_OF_PLAYERS;
    bool  isPrivate = false;
    bool  joinable = false;
};

//MARK: Registry
class MatchRegistry {
public:
    // Reasons creation can fail, so callers can report something specific rather
    // than a bare null.
    enum class CreateResult { Ok, AtCapacity };

    explicit MatchRegistry(int maxMatches) : maxMatches_(maxMatches) {}

    // Create a room seeded from a named preset. Returns nullptr (and sets `why`)
    // if the process is already at capacity.
    std::shared_ptr<Match> Create(const std::string& name,
                                  const std::string& presetName,
                                  bool isPrivate,
                                  const std::string& joinCode,
                                  bool optionsLocked,
                                  bool autoStart,
                                  std::string& codeOut,
                                  CreateResult& why) {
        std::lock_guard<std::mutex> lk(mutex_);
        if ((int)entries_.size() >= maxMatches_) { why = CreateResult::AtCapacity; return nullptr; }

        MatchEntry e;
        e.code          = MintCode();
        e.name          = name.empty() ? e.code : name;
        e.isPrivate     = isPrivate;
        e.joinCode      = joinCode;
        e.presetName    = presetName;
        e.optionsLocked = optionsLocked;
        e.autoStart     = autoStart;
        e.createdAt     = Match::Clock::now();
        e.emptySince    = e.createdAt;
        e.match         = std::make_shared<Match>();

        e.match->optionsLocked = optionsLocked;
        e.match->autoStart     = autoStart;
        e.match->ApplyPreset(MatchPresetByName(presetName));

        codeOut = e.code;
        why     = CreateResult::Ok;
        auto m  = e.match;
        entries_.emplace(e.code, std::move(e));
        return m;
    }

    std::shared_ptr<Match> Find(const std::string& code) const {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = entries_.find(code);
        return it == entries_.end() ? nullptr : it->second.match;
    }

    // Copy of an entry's static facts, for the join gate (private/joinCode).
    bool FindEntry(const std::string& code, MatchEntry& out) const {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = entries_.find(code);
        if (it == entries_.end()) return false;
        out = it->second;
        return true;
    }

    // Every match, ready to serialize. Public-only by default - a private room is
    // reachable by its code but never advertised.
    std::vector<MatchListing> List(bool includePrivate = false) const {
        std::vector<MatchListing> out;
        std::lock_guard<std::mutex> lk(mutex_);
        out.reserve(entries_.size());
        for (const auto& [code, e] : entries_) {
            if (e.isPrivate && !includePrivate) continue;
            MatchListing r;
            r.code       = e.code;
            r.name       = e.name;
            r.presetName = e.presetName;
            r.isPrivate  = e.isPrivate;
            // Live fields via atomics only - never the match's own mutexes.
            r.phase      = e.match->gamePhase.load();
            r.players    = e.match->connectedCount.load();
            r.maxPlayers = GAMESPACE_NUMBER_OF_PLAYERS;
            r.joinable   = r.players < r.maxPlayers;
            out.push_back(std::move(r));
        }
        return out;
    }

    size_t Size() const { std::lock_guard<std::mutex> lk(mutex_); return entries_.size(); }

    // Every live match, for the sim loop to tick. Returns shared_ptrs so a room
    // reaped mid-beat stays alive until this beat is done with it - the alternative
    // is the driver holding the registry lock across every tick, which would put
    // every join and every list behind the whole simulation.
    std::vector<std::shared_ptr<Match>> All() const {
        std::vector<std::shared_ptr<Match>> out;
        std::lock_guard<std::mutex> lk(mutex_);
        out.reserve(entries_.size());
        for (const auto& [code, e] : entries_) out.push_back(e.match);
        return out;
    }

    // Totals for the heartbeat and /status. Reads the live fields as atomics, so
    // like List() it never takes a match's own mutexes and cannot be delayed by a
    // simulation mid-tick.
    struct Totals { int matches = 0, players = 0, active = 0; };
    Totals Summarise() const {
        Totals t;
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& [code, e] : entries_) {
            t.matches++;
            t.players += e.match->connectedCount.load();
            const Phase ph = e.match->gamePhase.load();
            if (ph == Phase::COUNTDOWN || ph == Phase::PLAYING) t.active++;
        }
        return t;
    }

    // Destroy rooms that have been empty past their grace, or that have simply
    // been alive too long. Returns the codes destroyed, so the caller can log or
    // evict any connection still pointing at them.
    //
    // Grace is longer while a match is live: a lobby-wide network blip must not
    // tear down a match whose bodies are still held open for reconnects
    // (MID_MATCH_LEAVE_GRACE_SEC).
    std::vector<std::string> Reap(Match::Clock::time_point now) {
        std::vector<std::string> gone;
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto it = entries_.begin(); it != entries_.end(); ) {
            MatchEntry& e = it->second;
            if (pinned_.count(e.code)) { ++it; continue; }   // never reap the default match
            const int live = e.match->connectedCount.load();
            if (live > 0) { e.everOccupied = true; e.emptySince = now; ++it; continue; }

            const Phase ph = e.match->gamePhase.load();
            const double grace = (ph == Phase::COUNTDOWN || ph == Phase::PLAYING)
                               ? MATCH_EMPTY_GRACE_LIVE_SEC : MATCH_EMPTY_GRACE_IDLE_SEC;
            const double emptyFor = std::chrono::duration<double>(now - e.emptySince).count();
            const double age      = std::chrono::duration<double>(now - e.createdAt).count();

            if (emptyFor >= grace || age >= MATCH_MAX_AGE_SEC) {
                gone.push_back(e.code);
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
        return gone;
    }

    // Exempt a room from reaping - used for the process's default match, which
    // must outlive every client so today's single-match behaviour is unchanged.
    void Pin(const std::string& code) {
        std::lock_guard<std::mutex> lk(mutex_);
        pinned_.insert(code);
    }

private:
    // 4 chars from an alphabet with no O/0/I/1, so a code can be read aloud and
    // typed without ambiguity. Retries on collision; retired codes stay retired.
    std::string MintCode() {
        static const char* kAlphabet = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
        static std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> pick(0, 31);
        for (int attempt = 0; attempt < 1000; ++attempt) {
            std::string c;
            for (int i = 0; i < 4; ++i) c.push_back(kAlphabet[pick(rng)]);
            if (entries_.count(c) == 0 && retired_.count(c) == 0) { retired_.insert(c); return c; }
        }
        return "0000"; // unreachable in practice; a code is better than a throw
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, MatchEntry> entries_;
    std::unordered_set<std::string> retired_;  // codes are never reused
    std::unordered_set<std::string> pinned_;
    int maxMatches_;
};
