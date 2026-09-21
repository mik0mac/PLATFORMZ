# PLATFORMZ → multi-match hosting + find-a-match

*Plan of record, written 2026-08-29. Every numbered item below is filed as a
GitHub issue (#71-#98) across milestones 1-4; the table near the end maps them.*

## Context

PLATFORMZ today is **one server process = one global match**. Every piece of match
state in `server/server_main.cpp` is a file-scope global — `gameSpace` (L143),
`collisionGrid`, `gameMutex`, `botController` (L182), `gamePhase`/`matchEpoch`
(L190–207), the ~18 `pending*` option atomics (L212–231), the `clients` registry
(L245) and `udpIndex` (L248) — all driven by a single detached `SimulationLoop()`
thread at 60 Hz. A client connects to a baked-in address, gets the one and only
game, and the title screen doubles as its lobby (`main.cpp:819–1165`).

That was the right shape for friends-and-family — it's what `PLATFORMZ_KEY` and
the invite link were built for. It's a dead end for a published game: two
strangers can't get a game without landing in everyone else's, and there is no way
to *discover* a game at all.

**Goal:** one server process hosts many concurrent matches, and players find a game
from inside the client — a browsable match list, a QUICK MATCH button, and
CREATE MATCH.

## Decisions taken

| Question | Decision |
|---|---|
| Multi-match architecture | **Rooms in one process.** Hoist the globals into a `Match`; one `MatchRegistry`. One systemd unit, one port, unchanged deploy. |
| Discovery UX | **Match browser + QUICK MATCH + CREATE**, plus join-by-code for friends. |
| Identity | **Local profile file** — persistent name + `clientId` + settings, no accounts and no backend — plus a **server-signed token** (D3) so "same player as last time" is verifiable. Pseudonymous, not authenticated. |
| Leaderboards | **Mostly built already** on the `high-score` branch (persistence, server-side credit at match end, wire + client modal). D3 supplies the identity it needs; D4 re-keys it off display names. |
| Publish target | **Steam (macOS + Windows)**, web build maintained. Tracked as a separate epic (F) — it is a build/packaging problem, not a matchmaking one. |

## Architecture target

```
gameserver  (one process, port 9000: TCP/WS + UDP)
│
├── Directory ......... connections not yet bound to a match
│     list / create / join <code> / quick / leave
│
├── MatchRegistry ..... code -> shared_ptr<Match>,  cap MAX_MATCHES
│     ├── Match "7QK2"   GameSpace · CollisionGrid · BotController
│     │                  phase · epoch · MatchOptions · clients · mutex
│     ├── Match "M3XZ"   ...
│     └── Match "BB91"   ...
│
├── SimScheduler ...... ticks every Match at 60 Hz
└── Listener (Beast WS) + UdpListener (one shared socket)
      → route each inbound packet to Directory or to the owning Match
```

**The load-bearing idea: a client never reconnects.** One socket (or one UDP
endpoint) stays open for the whole session; a `join`/`leave` message re-binds it
between the Directory and a Match. This keeps UDP endpoints stable, avoids port
juggling and firewall ranges, and means the browser client needs no second
WebSocket.

## Layering — what a `Match` is, and is not

A `Match` is **not** a server. There is one server process, one port, one
`io_context`, one WS listener and one UDP socket for the whole box. A `Match` is a
*room inside* that process — the gameplay layer only.

| Layer | Owns | Lifetime |
|---|---|---|
| **Transport** — `Listener`, `UdpListener`, `Session` | sockets, the WS handshake, the join-key gate | process |
| **Connection** — the `Conn` table | who is attached: transport sink, `clientId`, name, and an atomic `matchId` | one socket |
| **Directory** — `MatchRegistry` | which rooms exist, codes, `list`/`create`/`join`/`quick`/`leave`, caps, reaping | process |
| **Match** (N of these) | `GameSpace`, `CollisionGrid`, `BotController`, phase, epoch, `MatchOptions`, roster, the 60 Hz tick | one match |

**The inversion that makes it work: a connection outlives its match membership.**
That is the whole reason A3 splits the `Conn` table from the per-match roster. A
match can be created, played and reaped while the same socket stays open
throughout; match membership is a *mutable property of a live connection*, not a
property of the socket. Get that backwards and you are back to reconnecting.

So, concern by concern:

- **Finding/joining** — entirely above `Match`. A `Match` never knows it is
  discoverable; the Directory owns that.
- **Player identity** — spans the Connection layer, never the Match. D1 puts the
  profile in a local file on the client; the hello carries `clientId` plus D3's
  server-signed token, and the verified result lives on `Conn`, because your
  identity outlives any one match. Its only job today is D2: matching a
  reconnecting player back to their held-open slot. `Match` never sees it.
- **Leaderboards** — deferred, but deliberately **unblocked**. See below.

### Leaderboards: already largely built on `high-score`

**This section was wrong in the first draft.** It called persistence deferred and
the match-end result hook hypothetical. Both already exist, on the **`high-score`**
branch — 5 commits, branched off current `main`, last touched 2026-08-29. It is
newer than `main` and does **not** overlap any file `publishing-start` touches, so
it merges cleanly today.

**What is already done there:**

- **`scoreboard.h`** (195 lines) — a `Scoreboard` class with an in-memory table,
  `generateLeaderboard()` (`partial_sort` with an inverted comparator, so it comes
  out best-first), a `<score>\t<name>` TSV file, **atomic save** (write `.tmp`,
  then `rename` — an interrupted save can never leave a half-file), and a tolerant
  load that skips malformed lines and commits only on full success.
- **Server-owned and server-credited.** `Scoreboard scoreboard` + `scoreboardMutex`
  in `server_main.cpp`, credited **once per match on the match-end phase edge**
  (tracked via a `prevPhase` local) and `save()`d there. That is exactly the seam
  this plan predicted — it is already cut, in the right place.
- **Wire + client.** A JSON-only `leaderboard` message (`lb: [{n, s}]`) and
  `ServerMessage::Type::Leaderboard`, sent just behind the welcome and again on
  each credit; the client renders a LEADERBOARD modal. JSON-only means it is
  already byte-0-dispatch compatible and needs no binary tag.
- **Path handling.** `SCOREBOARD_FILEPATH = "scores"`, relative so it resolves
  against the systemd `WorkingDirectory`, with a `PLATFORMZ_SCORES` override.
  Name sanitization rejects tab/newline so a display name cannot corrupt the TSV.

**What has to change to use the D3 identity (tracked as D4):**

1. **The key.** `std::map<std::string /*name*/, int>` is keyed by *display name*.
   Names are neither unique nor owned: two players typing `MIKE` share one row, and
   anyone can take someone else's row by typing their name. Re-key on the D3
   identity, and demote the display name to a *property* of the row — so renaming
   yourself keeps your history and the board shows your current name.
2. **File format.** `<score>\t<name>` becomes `<score>\t<id>\t<name>`. Any
   existing `scores` file needs a migration path, or a deliberate one-time reset —
   worth deciding rather than defaulting into.
3. **Multi-match.** The scoreboard is correctly a **process-level global** — it
   belongs above the Match layer — so it survives A1 unchanged as a global. But the
   credit fires per match-end, so with N matches `scoreboardMutex` is contended by
   every sim thread and up to N full-file rewrites can land together. A1/A3 must
   slot `scoreboardMutex` innermost in the lock order (the branch already documents
   `gameMutex -> clientMutex -> scoreboardMutex`), and `save()` likely wants
   debouncing rather than a synchronous rewrite per match end.

**Knock-on for the deploy story — DONE.** The server is no longer stateless, and
has not been since the scoreboard landed. `docs/deploy-vultr.md` no longer claims
"reads no files, writes no files"; it now carries a **"What the server keeps on
disk"** section naming the `scores` file and `/etc/platformz.env`, what happens if
each is lost, and how to back them up. E1's and D3's shared secret joins that
list when it lands.

Still deliberately absent: any notion of an *account*. D3 gives pseudonymous
continuity, which is the right strength for this board — see the honesty note in
D3 about what a copied token still allows.

---

# Epic A — Server: many matches in one process

### A1. Extract a `Match` struct (pure refactor, still one match)
**Why:** nothing else can start until the globals are encapsulated, and this is
the change most likely to introduce a subtle threading bug. Ship it alone, prove
it behaves identically, then build on it.

**Scope:** new `server/match.h` holding a `Match` struct with what are today
globals: `GameSpace gameSpace`, `CollisionGrid grid`, `BotController bots`,
`std::mutex mut`, `Phase phase`, `startRequested`, `countdownRemaining`,
`matchEpoch`, the `pending*` values (collapse the 18 atomics into a plain
`MatchOptions pending` + `mapSizePreset pendingMap` guarded by the match mutex —
they are only read at the `startRequested` sync point), the cached `welcomeStatic`,
and the match's own `clients` map.

Every free function that touches those globals becomes a `Match` method or takes
`Match&`: `ClaimFreeSlot` (L273), `ReapIdleUdpClients` (L292),
`CompactConnectedSlots` (L318), `refreshBotSlots` (L355), `HandleMidMatchLeavers`
(L382), `isHostConn` (L419), `buildWelcome`/`buildWelcomeBinary` (L551/561),
`buildStateBodyJson`/`buildStateBodyBinary` (L603/779), `BroadcastState` (L1288),
`SimulationLoop` (L1371).

`main()` creates exactly one `Match` and runs exactly one sim thread. **No
behaviour change, no protocol change.**

**Locking — this plan was wrong, corrected during implementation.** It called for
merging `gameMutex` and `clientMutex` into one mutex per match. **Don't.** Several
call sites take `gameMutex` and then `clientMutex` *inside* it (the sim tick does
it five times), so one mutex would self-deadlock on the second acquire — and the
heartbeat takes `clientMutex` alone precisely so it can't deadlock against the
sim. Merging is not a mechanical change; it means hunting down every nested
acquisition, which is exactly the kind of risk A1 exists to avoid.

The stated reason for merging — "that ordering discipline does not survive N of
each" — was also just wrong. Nothing ever locks two matches at once, so the order
stays per-match and the existing discipline holds unchanged. **Keep both mutexes
as `Match` members.** Globals that survive: `nextConnId`, `g_udp` +
`udpSendMutex`, `joinKey`, `scoreboard` + `scoreboardMutex`, and the new registry.

**Files:** `server/server_main.cpp`, new `server/match.h`.
**Done when:** `make -C server` clean; the native + browser LAN tests in
`docs/multiplayer-testing-archive.md` pass with no observable difference;
`git diff` shows no logic change, only re-homing.

---

### A1b. Per-tick allocation churn — fix before multiplying by N
**Why:** three costs are invisible at one match and likely dominant at twenty.
This lands between the refactor and the registry, still one match, still no wire
change, so it can be measured cleanly.

**Scope:**
- **`CollisionGrid::Rebuild` opens with `cells.clear()`** (`collisions.cpp:9`),
  destroying every `GridCell` and freeing all four of its vectors **every tick**.
  Platforms are 12–24 units against `cellSize = 8`, so each buckets into ~4–16
  cells: MEDIUM (128 platforms) touches ~700 cells, XL (576) ~3000. That is
  hundreds-to-thousands of map-node *plus* vector allocations per tick per match.
  Clear each cell's vectors **in place** (keeping capacity), leave the map nodes
  alive, and evict cells untouched for K ticks. Highest-leverage change here.
- **The grid is fully transient** — rebuilt each tick, never read between ticks.
  With a sequential sim loop (A4 option 1) **one shared scratch grid serves every
  match**: make it a `CollisionGrid&` passed into the tick rather than a per-match
  member. Saves N grids of RAM and keeps one buffer warm. (This is a real argument
  for option 1 over the pool.)
- **GAMEOVER simulates forever.** The sim deliberately keeps running through
  GAMEOVER for local-parity during the client's 5 s death-FX countdown
  (`server_main.cpp:1588`). At ×20 that's full-cost ticks for matches nobody is
  playing. Stop simulating after `GAMEOVER_SIM_SECONDS` and return to LOBBY
  (freeing the world) after `GAMEOVER_LOBBY_SECONDS` — both well past
  `GAME_OVER_TIMER = 5`, which two `static_assert`s now enforce.

> **Retuned to 7 s / 10 s** (was 15 s / 60 s). A room in GAMEOVER is deliberately
> unjoinable, so at 60 s a popular room advertised itself as unavailable for a
> full minute after every match — dead air a match browser cannot afford. Lowering
> the lobby delay alone would have made the 15 s sim-idle unreachable; the asserts
> exist so that ordering cannot invert silently again.
- **Drop the per-tick `std::set`s.** `gatherClaimedSlots()` and
  `BroadcastState`'s `connectedSlots` each build a red-black tree per tick for a
  roster of ≤ 8. Use a `uint8_t` bitmask.

**Files:** `collisions.cpp`, `collisions.h`, `server/server_main.cpp`.
**Depends on:** A1. **Re-measure after this** — A4's caps come from the new numbers.

---

### A2. `MatchRegistry` + match identity and lifecycle
**Why:** the container, ids, and the rules for when a match is born and dies.

**Scope:** new `server/registry.h`.
- `std::unordered_map<std::string, std::shared_ptr<Match>>` guarded by a registry
  mutex. `shared_ptr` so an in-flight io handler can't touch a destroyed match.
- **Match code:** 4 characters from an unambiguous alphabet (no `0/O/1/I`) —
  typeable, shareable, and doubles as the invite code. Collision-retry on create.
- Fields per match: code, display name, host `connId`, `private` flag, optional
  per-match join code, created-at, `MatchOptions` + chosen map preset.
- **Reaping:** a match with zero connected clients for `MATCH_EMPTY_GRACE_SEC`
  (~30 s — long enough to survive the last player's reconnect) is destroyed. Also
  hard-cap wall-clock match age so a wedged match can't leak.
- **`MAX_MATCHES`** cap (env-tunable, default set by A4's measurement). Over cap,
  `create` fails with a typed reason rather than silently.

**Files:** new `server/registry.h`, `server/server_main.cpp` (`main()` wiring).
**Done when:** the server can create/destroy matches from a test harness and the
`/status` counters (A6) reflect it. Not yet reachable by clients — that's A3.

---

### A3. Route connections to matches without reconnecting
**Why:** the join path, and the piece with the most transport-specific edges.

**Scope:**
- Global `std::map<uint64_t connId, ConnBinding>` where `ConnBinding` = transport
  sink (`shared_ptr<Session>` or `udp::endpoint`) + `matchCode` (empty ⇒ in the
  Directory) + slot. Per-match `clients` maps stay as they are; this is the index
  that answers "where does this packet go?".
- **WS:** `Session::Start` already parses the HTTP target for `?key=` (L920–936)
  — add `QueryParam(target, "match")`. Present ⇒ auto-join that match (this is how
  an invite link works). Absent ⇒ land in the Directory.
- **UDP:** the hello already carries `"key"`; add `"match"`. Same rule.
- `HandleClientMessage` (L1128) grows a front half: directory verbs are handled
  before any match lookup; everything else resolves `connId → match` and
  dispatches into that match.
- **`playerId` is a slot index the client only learns from a welcome** — so every
  bind, unbind, and `CompactConnectedSlots` re-slot must push a fresh welcome.
  That invariant already exists (L318 comment); the new paths must honour it.
- `leave` returns the connection to the Directory: free the slot, re-welcome the
  compacted survivors, send the client a `matchlist`.

**Locking — the load-bearing rule of the whole refactor.** Split by role rather
than putting a `matchId` field on one global registry: a global `Conn` table
(transport identity + an atomic `matchId`, io-thread owned) and a per-match roster
(gameplay state, sim-thread owned). A single global registry would force the sim
loop to *filter by matchId* on all seven of its per-tick passes, inside one
critical section every match serializes on — quadratic in exactly the dimension
being scaled.

> Lock order: `connMutex` > `match.gameMutex` > `match.rosterMutex` >
> `udpSendMutex`. **The sim thread never acquires `connMutex`.**

That holds if each roster entry carries its own `shared_ptr<Conn>` (so broadcast
reaches the sink without the global map) and eviction is just
`conn->matchId.store(0)`. Enforce it with a debug-only lock-level guard that
aborts on out-of-order acquisition, plus a `-fsanitize=thread` CI job. Hold
matches as `shared_ptr<Match>` and **never reuse a match id**, so a datagram in
flight for a reaped match can't land on a new one.

**Files:** `server/server_main.cpp` (Session, UdpListener, HandleClientMessage).
**Depends on:** A1, A2, B1.
**Done when:** two clients can be in two different matches on one server and
neither sees the other; a client can leave one and join the other on the same
socket.

---

### A4. Sim scheduling for N matches — measure, then choose
**Why:** the target box is a **1 vCPU / 1 GB Vultr shared-CPU** instance. One
detached 60 Hz thread per match would put 10–20 threads on one core; that fails
not from CPU cost but from scheduler jitter blowing the tick deadline.

**Measure three things before writing any of it, on the actual box, not the Mac**
(the "p95 ~8 ms" in `constants.h:106` is a *client render* number and does not
transfer):
1. **Tick cost, split three ways** — the `gameMutex` block, `BroadcastState`, and
   `CollisionGrid::Rebuild` separately. p50/p95/max.
2. **Egress per match.** 8 clients × 60 Hz × ~830 B ≈ **400 KB/s per match**.
   Twenty matches ≈ 64 Mbit/s ≈ ~20 TB/month against the $6 plan's 2 TB
   (`docs/deploy-vultr.md:33`). **Bandwidth, not CPU, is the likely ceiling** —
   so derive `MAX_MATCHES` from the transfer quota too, not just the tick budget.
   If egress binds, decouple broadcast from sim and send every 2nd–3rd tick:
   `GameSpace::extrapolate(dt)` (`gamespace.h:363`) already exists for exactly
   this. Halve the *broadcast* rate, never the sim rate — 60 Hz physics parity
   with the client's `ApplyPlayerInput` is not negotiable.
3. **Steal time** (`vmstat 1`, `st` column). 10–30 % is normal on shared CPU and
   silently eats the budget. Budget **10 ms**, not 16.6, to leave room for it,
   the io threads, and Caddy's TLS on the same box.

**Then implement the smallest thing the number allows,** in this order of
preference:
1. **One sim thread, all matches ticked sequentially per 60 Hz beat.** Simplest,
   no cross-match locking, perfectly cache-friendly. Viable while
   `matches × p95_tick` stays well under ~10 ms.
2. **A small worker pool** (`hardware_concurrency`, min 2), sharded by
   `matchId % K`, behind `PLATFORMZ_SIM_THREADS` (default 1). On a 1-vCPU box
   K > 1 is strictly worse, so build the partition now and switch it on after a
   box upsize. **Blocker to fix first:** `random.h`'s `RandomFloat` /
   `ShuffledIndices` share a `static std::mt19937` — not thread-safe, and two
   matches generating a world at once would race it. Make it `thread_local`
   before enabling K > 1.
- Either way, publish an overload signal: if a beat overruns repeatedly, stop
  accepting new matches (and say so in the `create` failure) rather than degrading
  every match at once. `MAX_MATCHES` gets its real default here.

**Subtlety to preserve:** `BroadcastState` (L1288) deliberately reads `gameSpace`
*without* `gameMutex`, justified by "the sim thread is its only mutator and this
runs on that same thread, sequentially after the locked sim step". That invariant
survives only if the same thread both ticks *and* broadcasts a given match. Any
scheduler must keep tick-and-broadcast on one thread per match — never hand the
broadcast to a separate worker.

**Files:** `server/server_main.cpp`, new `server/sim_scheduler.h` if option 2.
**Depends on:** A1.
**Done when:** the load harness (E3) sustains the target match count with p95 tick
under budget, and exceeding the cap is refused cleanly instead of degrading.

---

### A5. Join-in-progress: take over a bot slot
**Why:** a match browser is only alive if PLAYING matches are joinable. This
*partly* works today by accident — the roster is sized
`clamp(pendingPlayers, connectedHumans, 8)` at start, so if the host picked 8 and
only 2 humans were present, `ClaimFreeSlot` hands a latecomer one of the 6 bot
slots. It fails when roster == humans, and the takeover itself is unspecified.

**Scope:** make it deliberate. Decide and implement the takeover semantics — the
human should inherit the bot's *body* (position, health, score) or get a fresh
spawn, but it must be one of them, consistently, with a kill-feed line. Note the
constraint that makes this delicate: **slot compaction is LOBBY-only** (L1528) —
once `generate()` has run, a slot index also means a spawned body, so a mid-match
join must never renumber anyone. Then surface `joinable` honestly in the
`matchlist` (B1) so the browser doesn't offer a JOIN that will bounce.

**Files:** `server/server_main.cpp` (`ClaimFreeSlot`, `refreshBotSlots`),
`messages.h` + `constants.h` (kill-feed line — `MessageType` crosses the wire as a
raw int, so a new value must be **appended last**, per the note at
`constants.h:74`).
**Depends on:** A1, A3.

---

### A7. Bucket static platforms once per match — **DONE** (#99)
*Not in the original plan. A4's measurement found the grid rebuild was still ~80%
of a tick after A1b removed its allocation churn, and ~99% of that was platforms,
which never move. Bucketed once per match into a static layer keyed on a platform
epoch; grid p95 on the box is 0.03 ms. Verified at 6,591 probe points against a
brute-force reference, because `probe.py` cannot see a wrong platform bucket.*

### A8. UDP state packets exceeded the MTU budget — **DONE** (#100)
*Not in the original plan. Found running A4 on the box: state packets chunked at
18 asteroids against a documented cap of 42, because `ACTION_HEADROOM` was 65 B
(sized for the 2-player roster this began as) against ~617 B of real 8-player
action. Raised to 200 B — and, more importantly, chunking was made survivable:
the client's reassembler held ONE message, so a chunked state arriving mid-welcome
destroyed it and a player joining a busy match never learned their slot.*

> **Both of these came out of measuring rather than planning**, which is what A4
> was for. The plan is the map, not the territory — when the numbers disagree with
> it, the numbers win and the plan gets updated.

### A9. OFFICIAL vs CUSTOM: governance is not visibility — **DONE** (#107)
*Not in the original plan. A2 shipped public rooms as hostless, locked and
self-starting, and expressed that by deriving both flags from `isPrivate`:*

```cpp
g_registry.Create(..., /*optionsLocked*/ !isPriv, /*autoStart*/ !isPriv, ...);
```

*That conflates two independent questions. **Visibility** is whether a room is
advertised in the browser; **governance** is who sets its rules and who starts it.
Tying them together made one of the four combinations — public **and** host-run —
impossible to express, and that combination is exactly what a public custom match
is. A player who wants to run their own game and let strangers find it could only
have one or the other.*

**The model.** A room's `MatchKind` (`options.h`) decides governance; `isPrivate`
decides visibility, and they do not interact:

| | Official | Custom |
|---|---|---|
| Options | fixed preset, locked to everyone | the host sets them |
| Starting | itself, at `PUBLIC_MIN_PLAYERS` | the host starts and ends it |
| Host | none | the creator |
| Visibility | public | public **or** invite-only |
| Created by | the server | a player |

**Where the fix went.** `optionsLocked` and `autoStart` are now derived inside
`MatchRegistry::Create` from the kind and nowhere else — they are not independent
choices and must never disagree, so no call site gets to pass them. `create` from
a client is always `MatchKind::Custom`; only the server mints official rooms, or
the preset they promise guarantees nothing.

**A2's open question, answered: official rooms are resident.** One per entry in
`matchOptionPresets`, created at boot and pinned. The cheap alternative — create
on demand from QUICK MATCH — leaves the browser empty until somebody asks, and
"NO MATCHES YET" is a bad first screen for a game whose premise is finding a
match. An idle LOBBY room costs almost nothing to tick (which is A1b's doing), and
it is what QUICK MATCH lands in, so the common path never pays for creation.
Adding a preset adds its room with no code change.

**QUICK MATCH now targets official rooms only.** It promises a game that starts;
dropping someone into a stranger's public custom room hands their experience to a
host who picked the rules and may never press START. That room is still one click
away in the browser.

### A10. The host is the creator, and it sticks — **DONE** (#108)
*The other half of A9. The split gave custom rooms a host; this decides who that
is. Host was "the lowest connected non-bot slot", recomputed on every question and
implemented independently on both sides (`isHostConn` on the server, `amHost` in
the client).*

**Why that looked fine and wasn't.** In LOBBY, slots compact every tick, so the
lowest slot *is* the longest-present player and the rule behaves. Compaction stops
once `generate()` has run — and mid-match the rule comes apart:

```
host (slot 0) leaves mid-match      -> slot 1 inherits the room
their body is botified after the grace
a latecomer joins and claims slot 0 -> and instantly becomes host
```

*Confirmed against `main` before fixing it: a player who joined seconds earlier
sent `endmatch` and ended everyone's match.*

**The fix.** `Match::hostConn` holds the host's connId — the creator's, set when
they are placed in the room they made. `ResolveHostLocked()` returns it, migrating
to the **lowest remaining slot** only when the holder has actually gone. Sticky
rather than continuously recomputed, so a lower slot opening up beside the host
changes nothing.

Resolved **lazily**, on ask, rather than hooked into every departure path — there
are five of those (detach, disconnect, the UDP idle reaper, room moves, lobby
compaction) and a lazy resolve cannot forget one. An official room resolves to
*no* host at all, which is what stops `isHostConn` and `optionsLocked` from
contradicting each other.

**The client stopped deriving it.** It now reads a server-set flag, because the
rule is no longer computable client-side: the creator is not identifiable from
slot order, and an official room has no host to find. Two implementations of one
rule was the deeper bug — a divergence shows up as a START button the server
refuses, which reads as the game ignoring you.

**Wire cost: none.** The per-player flags byte had bits 32/64/128 free, so the
host flag needed no layout change and no `STATE_BIN_VERSION` bump.

**The default room stays CUSTOM.** It is where a client that names no room lands,
and keeping it host-run means a solo player can still start a game exactly as
before. Once the client always picks a room explicitly (C3/C4), it has no reason
to exist and the resident official room becomes the landing place.

### A6. Per-match logging + a `/status` endpoint — **DONE** (#77)
**Why:** today's operational signal is one `tick N players C asteroids A` line —
and the GH Actions idle-watchdog greps it. With N matches that's meaningless, and
"is anyone playing?" becomes unanswerable.

**Scope:**
- Heartbeat becomes one summary line (matches, total players, worst tick) plus a
  per-match line at a lower cadence. *(This also had to keep
  `.github/workflows/gameserver.yml`'s idle watchdog working, which grepped
  `players [0-9]+`. That workflow has since been retired.)*
- `Session::Start` already reads the raw HTTP request; a **non-upgrade `GET
  /status`** returns JSON (uptime, match count, player count, version tags) and
  closes. Free monitoring, and a "N players online" badge for the site later.
  Guard it so it can't be used as an amplifier (small fixed response, no query
  echo).

**Files:** `server/server_main.cpp`, `docs/deploy-vultr.md`.

---

# Epic B — Protocol: the directory

### B1. Directory message types
**Why:** the client needs to see and pick a match.

**Scope:** client→server is always JSON, and the client dispatches inbound on
**byte 0** (`'{'` ⇒ JSON, else binary tag — `wire.h` `applyMessage` ~L470). So
**the whole directory can be JSON-only, on both transports** — no new binary tags,
no new decoders, and the browser build gets it for free.

**Cap the reply to one datagram and page with a cursor** — ~1160 bytes, 8 rows
per page.

*Originally this was justified on two grounds and one of them has since gone
away.* The correctness argument was that `UdpTransport::Reassemble` held a single
in-flight message, so a chunked `matchlist` racing a chunked welcome would destroy
one of them. **A8 (#100) replaced that with a multi-slot reassembler**, so
chunking is now survivable and the cap is no longer load-bearing for correctness.

What remains is the amplification argument, and it is enough on its own: an
unauthenticated UDP `list` returning kilobytes is a reflector until E1 lands.

**Page size must stay below `MATCH_MAX_CONCURRENT`.** Set at or above it and every
list fits one page, so the paging path never executes and rots until the day the
match cap is raised — at which point the browser silently shows a truncated list.
A `static_assert` enforces it.

Client → server (new builders in `wire.h`, parsed with the existing
`parseString`/`parseUInt` helpers on the server):

| Type | Fields |
|---|---|
| `list` | — |
| `create` | `name`, `map`, `private`, `code`, + the `writeOptionKeys` bundle |
| `join` | `match`, optional `code` |
| `quick` | optional filters (map size) |
| `leave` | — |

Server → client:

| Type | Shape |
|---|---|
| `matchlist` | `[{id, name, host, players, max, map, phase, locked}]`, public matches only |
| `joinfail` | `reason`: `notfound` \| `full` \| `badcode` \| `inprogress` \| `server_full` \| `rate_limited` |

Join success needs no new message — it is the existing **welcome**, which already
carries slot + static world.

**Files:** `wire.h`, `server/server_main.cpp`.

---

### B2. Welcome carries the match code — **DONE** (#79, with #111)
**Why:** the client must display "you are in 7QK2" and build an invite link, and
after a `CompactConnectedSlots` re-welcome it must be able to tell "same match, new
slot" from "different match".

**Scope:** add `match` to both welcome forms and bump `WELCOME_BIN_VERSION`
in `netbin.h` — **0x02 → 0x0A, not 0x03**. `0x03` is `CHUNK_VERSION`; taking it
would trip `netbin.h`'s distinct-tags `static_assert`. 0x05/0x06/0x07/0x09 are
taken or burned, and the file's rule is that values are **never recycled**, so go
past the high-water mark. Server prints its tags at boot
(`server_main.cpp:1858`) — keep that honest.

**This is a hard break with every deployed native binary**, with no graceful
degradation: the client latches `Type::VersionMismatch` permanently
(`main.cpp:1444`, never cleared) and shows "SERVER VERSION MISMATCH". So bump
exactly once, at the multi-match cut, and ship server + client together. A
corollary worth exploiting: with the bump already spent, **make `matchEpoch`
server-wide unique** (one global counter; each match start takes `++g_epoch`)
instead of per-match. The existing epoch gate in the input handler (L1265) then
rejects input aimed at the wrong match for free — **zero wire change to the 60 Hz
packet**. Stop accepting `ep == 0` at the same time; that allowance existed for
pre-epoch clients, which this bump excludes anyway.

**Files:** `netbin.h`, `wire.h`, `server/server_main.cpp`.

*As shipped, the welcome carries the code **and the room's KIND**. The kind was
the harder half of #111: nothing on the wire said how a room you were already
standing in was governed, so the lobby could not tell a room waiting on a host
from one that starts itself. It rides the same bump rather than spending a second
one later.*

*The **server-wide epoch** was deliberately left out. Input is routed by connId to
that connection's own match, so an epoch aimed at the wrong match cannot arrive in
the first place — the change would have added risk to a protocol PR for a
guarantee that already holds structurally. The `ep == 0` allowance is still there
for the same reason: it costs nothing until something proves it does.*

*The auto-start countdown needed no bump at all. The state packet's `countdown`
float is written only during the pre-match COUNTDOWN phase and is 0 throughout
LOBBY, so the official room's timer rides in it — a nonzero countdown in LOBBY is
unambiguous, since nothing else sets one.*

---

### B3. Move map size into `MatchOptions` — **DONE** (#80)
**Why:** map size is currently chosen by *which* of four START buttons you press
(`wire.h:150`, `main.cpp:944–953`) — so it exists only at the instant of starting.
A lobby everyone can see needs the selected map to be visible *before* start, and
the match browser needs to advertise it.

**Scope:** add a map field to `MatchOptions` (`options.h`) and to `writeOptionKeys`
(`wire.h:153`); the four START buttons become a map *selector* plus one START.
The server's `pendingHalf/Plat/Roid` fold into the same pending-options bundle
from A1. Keep the server-side clamp to `nb::MaxAsteroidsForRoster` at start.

**Files:** `options.h`, `wire.h`, `main.cpp`, `server/server_main.cpp`.

*The plan said to land this with B2's version bump. **It needed no bump at all.**
The options flags byte in the state packet used four of its eight bits, so the map
index rides bits 16/32 — the same spare-bits trick the host flag used in A10. B2
shipped separately and this cost nothing extra.*

*`pendingHalf/Plat/Roid` did not fold into the options bundle so much as collapse:
they are now a single `pendingMap` index, and half-size, platform count and
asteroid count are derived from it at start. Three values that could disagree with
each other — and with what the lobby was showing — became one that cannot.*

*Bench mode kept its explicit dimensions. `./platformz bench 120 128 18` picks
numbers well outside any preset, which is the point of benching, so the local
world builder is now a separate `startLocalWorld(half, plat, roid)` that normal
play reaches through the chosen preset.*

---

# Epic C — Client: find a match

### C1. Lift the game shell out of `main()` (pure refactor)
**Why:** the entire shell lives inside `main()` — a ~1750-line function, with
`GameScreen` declared *inside* it at L404 and the title/lobby block spanning
L819–1165. Two more screens cannot go in there.

**Scope:** new `screens.h` (2D shell, mirroring how `ui.h` owns the widgets and
`shapes.h` owns the 3D look). Hoist `GameScreen` to file scope, gather the
title-screen state into a `ShellState` struct, and move the TITLE, COUNTDOWN and
GAME_OVER blocks into `DrawTitle(...)` / `DrawCountdown(...)` / `DrawGameOver(...)`.
Pure motion — no behaviour change.

**Files:** `main.cpp`, new `screens.h`, `Makefile` (nothing — headers are wildcarded).

---

### C2. `GameScreen::BROWSE` — the match list
**Scope:** a new screen built from existing `ui.h` widgets (`UiPanel`, `UiButton`,
`UiTextCentered`): scrollable rows of `name / players / map / phase`, a REFRESH
button, auto-refresh on a timer while the screen is open, and JOIN per row. Empty
and error states (`joinfail` reasons from B1) must both read clearly.

```
  FIND A MATCH                          [ REFRESH ]
 ┌──────────────────────────────────────────────────┐
 │ MIKE'S GAME        3/8    LARGE     LOBBY   [JOIN]│
 │ ROCKET PARTY       6/8    MEDIUM    PLAYING [JOIN]│
 │ ASTEROIDS ONLY     1/8    XL        LOBBY   [JOIN]│
 └──────────────────────────────────────────────────┘
   [ QUICK MATCH ]   [ CREATE MATCH ]   [ JOIN CODE ]
```

**Files:** `screens.h`, `main.cpp`, `wire.h`.
**Depends on:** B1, C1.

---

### C3. `GameScreen::LOBBY` — split the lobby off the title screen
**Why:** TITLE is currently both the menu and the live per-match lobby
(`main.cpp:820`). Once there are many matches those are different places.

**Scope:** TITLE becomes name entry + PLAY ONLINE / PLAY SOLO / OPTIONS / QUIT.
LOBBY is the existing roster + OPTIONS modal + START, now showing the match name
and its code, with a LEAVE button that sends `leave` and returns to BROWSE. Host
gating (`amHost`, L874–879) is unchanged — lowest connected non-bot slot, per
match.

**Files:** `screens.h`, `main.cpp`.
**Depends on:** C1, A3.

---

### C4. Quick match and create match
**Scope:** QUICK MATCH sends `quick`; the server joins the fullest joinable LOBBY
match, else creates one — one round trip, no list needed. CREATE MATCH is a small
modal (name, map, public/private, optional code) reusing `UiTextField`/`UiToggle`,
which sends `create` and drops the creator into LOBBY as host.

**Files:** `screens.h`, `main.cpp`, `wire.h`, `server/server_main.cpp`.

---

### C5. Invite links and join-by-code
**Scope:** `?match=CODE` on the web URL (alongside the existing `?server=` /
`?key=` handling at `main.cpp:125–138`) auto-joins on connect. Native takes
`--match CODE` or a `platformz://` URL later. In-client, JOIN CODE is a text field.
The LOBBY screen shows the code and a COPY INVITE action.

**Files:** `main.cpp`, `screens.h`.

---

### C6. No room until you choose one

**The problem.** A connection has always had to be *somewhere*. Connect without
naming a room and the server quietly seats you in a landing room you never
picked — for most of this project's life a public CUSTOM room called PLATFORMZ,
left over from when the server held exactly one match and there was nowhere else
to be.

That room was worse than redundant. Being custom, it had a host; being
server-created, it had no creator to *be* the host, so the fallback handed the
role to whoever held the lowest slot — in practice the first stranger to connect.
They could retune every rule and press START in a public room everyone else also
lands in, and when they left, control passed silently to the next person in line.
That is precisely the problem A9 fixed for official rooms; the landing room was
simply never brought along.

Pointing the landing room at the first OFFICIAL room (done, alongside E5) fixes
the accidental host — official rooms have no host and nobody can start them. But
it does not fix the real thing: **you still arrive somewhere you did not choose**,
and it is now a room that will start a match around you on its own schedule.

**Scope.** Connecting puts you in the directory and nowhere else. You see the
browser, you pick, and only then do you hold a slot. LEAVE returns you to the
browser rather than to another room.

**Most of this already exists.** `HandleUnseatedMessage` already serves a parked
connection the whole directory (`list`/`join`/`create`/`quick`), and
`ParkConn`/`g_unseated` already hold a live connection with no room — that path
runs today whenever the server is full, and `probe_capacity` covers it. This is
not new machinery; it is removing the forced seat and teaching the client that
"connected" and "seated" are different things.

Three things force a seat today, and the second is what makes it circular:

1. `SeatOrPark` defaults an empty room code to `g_defaultCode`.
2. The client re-sends `hello` every 0.5 s while it has no slot, and the unseated
   handler's `hello` branch calls `SeatOrPark`. So a parked connection re-seats
   itself half a second later, however carefully the server parked it.
3. `leave` moves you to `g_defaultCode` instead of to nothing.

**A refusal leaves you parked.** Asking for a room that is full or gone gets a
`joinfail` and nothing else — no fallback room, no consolation seat. The browser
is already the right place to be told, and a fallback would reintroduce exactly
the "somewhere you did not choose" this entry removes.

**One new message, JSON.** The client learns it is connected by receiving a
*welcome*, which cannot exist without a seat. A parked connection needs an
equivalent ack. JSON on both transports, like everything except the welcome and
the per-tick state — so no binary tag and no `STATE_BIN_VERSION` bump.

**The trap.** `UDP_CLIENT_TIMEOUT_LOBBY` is **3 seconds**, and the client's
keepalive is gated on holding a slot (`main.cpp`). A parked UDP client survives
today only because its own hello retry doubles as a heartbeat. Stop the retry
without moving the keepalive gate and every parked UDP player is reaped three
seconds after arriving — invisible over WebSocket, where TCP keeps the session up.

**`myIndex < 0` is the client's whole difficulty.** It currently means both
"still handshaking" and "connected, no room". Splitting those two is the change;
the screen routing, the keepalive gate, and `shell.serverFull` — which stops
being an error and becomes the normal resting state — all fall out of it.

**Retires** `g_defaultMatch` / `g_defaultCode` entirely. Heartbeat and perf
reporting read them for a tick source and need repointing first.

**Files:** `server/server_main.cpp` (`SeatOrPark`, `HandleUnseatedMessage`,
`leave`, boot), `wire.h`, `main.cpp`, `screens.h`, `server/test/probe.py`.

---

# Epic D — Identity

### D1. `profile.h` — persistent local profile — **DONE**
*Landed on `d1-local-profile`. Verified three ways: `test/run.sh` (26 native
checks — round trip, corrupt file, hand-edited hostile values, partial file,
UUID shape and uniqueness), a node harness that runs **emcc's own emitted EM_JS**
against a localStorage shim, and two real launches of the built client proving
the `clientId` survives a relaunch and the file lands 0600.*

**Why:** name, volume and options reset on every launch today; there is no
persistence layer anywhere in the project.

**Scope:** new `profile.h` writing small JSON:
- macOS: `~/Library/Application Support/PLATFORMZ/profile.json`
- Windows: `%APPDATA%\PLATFORMZ\profile.json`
- Web: `localStorage` via `EM_JS` (same pattern as the existing
  `PlatformzSetModalOpen` at `main.cpp:23`)

Holds `name`, a random `clientId` (UUID, generated once), master volume, last
`MatchOptions`, last server, last match — **plus a `token` field, empty until the
server issues one (D3).** Reserve the field now even though D3 fills it; the
format is much easier to get right before the first build ships than after.
**Must not write next to the binary** — inside the signed `.app`,
`Contents/MacOS/` is code (see the cwd-anchoring note in `CLAUDE.md`).

**Files:** new `profile.h`, `main.cpp`.

#### What shipped, and the two decisions worth knowing
**Storage on the web is `localStorage`, not cookies.** A cookie rides along on
every request the browser makes to the origin — the id would be shipped to the
web host on every asset fetch for no reason, and the whole domain shares a ~4 KB
budget. `localStorage` is a per-origin drawer that goes nowhere unless we send
it. It is best-effort, and the ways it evaporates bound what D4 can key a
leaderboard on: it is per-origin (github.io and the real domain are two different
players), per-browser and per-device with no sync, discarded by a private window,
and **evicted by Safari's tracking prevention after 7 days without a visit**. A
web `clientId` is "stable across sessions, usually" — the most a browser will
promise without the account this project has deliberately chosen not to have.

**Two remembered rule sets, not one.** `lastLocalOptions` seeds the LOCAL
screen; `lastCustomOptions` seeds the CUSTOM one. Same reason `main.cpp` keeps
`localOpt` and `onlineOpt` apart — a solo practice arena and the room you host
for friends are different habits, and a change to one must not quietly retune the
other.

The custom bundle is captured **only from a room that is ours**: the CUSTOM setup
screen, or a LOBBY for a room **we created** (the code from the server's `created`
reply) and still host. Host alone was the original rule and it was a bug: walking
into an empty room — the default room every connection lands in — makes you its
host by default, so its stock rules were saved over your own setup. The
exclusions are the point. Joining someone else's room fills `onlineOpt` from
*their* echo, and an official room's are a locked preset with no host at all —
neither is this player's setup, so saving either would silently overwrite it.
`screens.h` grew a shared `HostSlot()` so `main.cpp` and the lobby cannot drift
on who the host is.

The room's **name and invite-only flag** ride along with those rules. The name is
stored *empty* while it still matches the derived `"<YOUR NAME>'S MATCH"` — the
screen re-derives that from the current display name, and storing the derived
string would freeze it, so renaming yourself to MIKE would leave your rooms
called PLAYER 1'S MATCH forever. `MATCH_NAME_MAX_CHARS` moved into `constants.h`
so the entry field and the profile's clamp cannot disagree about what fits.

**Bug found and fixed while testing this.** The state echo applies the server's
options to `onlineOpt` on every packet — including while the CUSTOM setup screen
is up, where `onlineOpt` is a *draft* for a room that does not exist yet and the
connection is still bound to whatever room it auto-joined. So every rule the host
set before pressing CREATE snapped back a frame after the click, and the room was
created with the *other* room's rules. Only the map survived, because the map is
the one field that block does not touch — which is why B3's work looked fine. The
echo is now skipped while `screen == GameScreen::CUSTOM`; the draft is pushed once
the room exists, and from then on the echo is the host's own values coming back.

`main.cpp` samples the live values into the profile every frame and lets
`profile::Autosave` decide whether that is worth a write (at most one every 2 s,
and only when something differs from what is stored). Sampling rather than a
`MarkDirty()` at each edit site is deliberate: the name field, the volume slider,
the `+`/`-` keys and every OPTIONS control would each need one, and the one that
got forgotten would silently stop persisting. The autosave — not the teardown
save — is also what makes the web build work at all: closing a tab runs no
teardown.

---

### D3. Server-issued identity token — **DONE** (#97)
**Why:** `clientId` alone proves nothing — the client generates it and owns the
file. That is fine for D2's 15-second slot restore, and useless for anything
persistent. A server-signed token makes "same player as last time" verifiable, and
it is far cheaper to design in now than to retrofit around a shipped profile
format and a shipped hello.

**Scope — no per-user rows, no database.** (Not "the server stores nothing": it
already keeps a `scores` file, and the secret below is itself persistent config.
What D3 avoids is a *table of users* — it verifies its own signature instead.)
- Server holds a secret in `PLATFORMZ_IDENTITY_SECRET`, alongside `PLATFORMZ_KEY`
  in `/etc/platformz.env`.
- `hello` with **no** token → mint `token = base64(uuid ‖ HMAC(secret, uuid))`,
  return it; the client stores it in its profile.
- `hello` **with** a token → verify the HMAC. Valid ⇒ identity trusted. Invalid or
  from an old secret ⇒ treat as no token and mint fresh (never hard-fail a join
  over it).
- The server stores **no record of the player** — it just re-computes the tag and
  compares. Same primitive as E1's cookie, sharing one secret and one pair of
  sign/verify helpers; see E1 for the HMAC-not-prefix-hash and constant-time
  compare notes, which apply here too.

**Be honest about what this does and doesn't buy.** It proves *continuity* — the
same client as before — not that a human is who they claim. Someone can still copy
their own token to a second machine, or run several clients to farm. That is
enough for a friends-and-family ranking; a competitive public leaderboard would
still want real accounts. Pseudonymous, not authenticated.

Note the secret must be **persisted**, not regenerated at boot, or every token
invalidates on restart. That is the one piece of server state this introduces.
Nothing is baked into the web build — browser tokens live in `localStorage`.

**Files:** `server/server_main.cpp`, `wire.h`, `profile.h`, `docs/deploy-vultr.md`.
**Depends on:** D1.

#### What shipped

`server/identity.h`, on top of E1's `crypto.h` exactly as planned — the secret,
the HMAC and the constant-time compare were already there and already tested, so
this was the small half of its own design. `profile.h` needed no change at all:
D1 put the `token` field in a year of decisions ago, which is the whole argument
for doing this before the format shipped rather than after.

**Hex, not base64, which is what the scope above says.** The token rides the
WebSocket upgrade URL as a query parameter, so it has to be URL-safe: base64 is
not, base64url is, and hex is both without a codec to write or get wrong. 64
characters instead of 43, in a field no human reads, for identical security —
it is an encoding, not a cipher.

**Two strings, not one.** `Identity{id, issue}`: the **id** is what the server
keys on and may log, the **issue** is a bearer credential that goes to one client
and nowhere else. A single "the identity" value would eventually have somebody
write the token into the score file. The seat log prints the first 8 chars of the
id for correlation and never the token.

**The token is established on the CONNECT paths, and re-established on `hello`.**
Connect is the reliable moment (a WS client is welcomed the instant it connects
and may never send a hello at all), but over UDP the issued token is a single
datagram and a datagram can be lost — so a client that never received one asks
again on its next hello, which it is already sending. A verified token costs one
HMAC and changes nothing.

**What it deliberately does not do:** nothing consumes the identity yet. It sits
on `ConnectedClient` waiting for D4, which is the point — the expensive half was
getting it into a shipped `hello` and a shipped profile format.

Tests: `server/test/identity_test.cpp` spends most of its checks on tokens that
must FAIL (forged tags, swapped halves, spliced tokens, the wrong secret, and the
near-misses a lenient hex parser would wave through), because a token that
verifies when it should not just means somebody else's leaderboard row, silently.
`probe_identity.py` proves the policy over UDP; `loadtest --mode ws-smoke` proves
it over WebSocket, which is the only automated coverage that transport has.

---

### D2. Reconnect into your own slot — **DONE**
*Landed on `d2-reconnect-slot`. `server/test/probe_reconnect.py` drops a player
mid-match and proves the four things that matter: the same id gets the same slot
back with its body and score intact, a different id does not get that slot, no id
at all does not get it either, and once the body is gone a returner is revived
fresh rather than seated into a corpse.*

**Built on `clientId`, not on D3's token — deliberately.** The plan's own D1
section says why: restoring your own slot inside a 15-second window is low-stakes
and self-defeating to cheat. The prize is someone else's half-dead body in a match
already in progress. D3 lands beside `clientId` as a second field on
`ConnectedClient` and upgrades the check from *claimed* to *verified*; nothing
here has to move for that.

**Why:** the machinery already exists and is unused — a mid-match leaver's body is
held open for `MID_MATCH_LEAVE_GRACE_SEC = 15 s` (`constants.h:118`,
`HandleMidMatchLeavers` L382), but nothing can prove "I am that player", so a
dropped player comes back as a new slot while their body drifts off.

**Scope:** carry the D3 token (and `clientId`) in the hello; the match remembers
the **verified** identity per slot; on rejoin within grace, restore the original
slot with its score and body instead of claiming a fresh one. Also makes the
UDP-endpoint-change case (laptop sleep, new NAT mapping — already called out at
L1806) actually work.

**Files:** `wire.h`, `server/server_main.cpp`, `elements.h`.
**Depends on:** D1, A3. (D3 upgrades it; it is not a blocker — see above.)

#### What shipped
**One way to seat a player.** Claim and take-over were two calls at three call
sites, and the comment at the WebSocket one records what that cost: a path that
forgot the second seated people into bot bodies. They are now a single
`Match::SeatPlayer`, which either resumes a held slot or takes a fresh one and
resets it. A reconnect must never route through `TakeOverSlot` — that resets
health, ammo, position and score, so it would hand the player their own body
wiped clean.

**`ClaimFreeSlot` got a second pass.** A slot whose body is being held for a
reconnecting player is skipped on the first pass, so a newcomer arriving during
someone's grace takes an untouched slot instead of walking into their body. The
second pass gives those slots up anyway when nothing else is free: refusing a
player entry to protect a leaver who may never return is the worse trade.

**The two clocks had to be reconciled.** A client gives up after ~3 s of silence
and re-runs the handshake; the server does not free a quiet UDP slot for 10 s. In
that window a laptop that woke with a new NAT mapping — the exact case this
feature exists for — arrives as a stranger while its old endpoint still owns the
slot, so the resume finds it occupied and the body it came back for drifts off
and dies. `SupersedeStaleTwin` drops an *already quiet* connection holding the
same id. The silence guard is what keeps that from being a footgun: two clients
run from one machine share a profile and therefore a `clientId`, which LAN
testing does routinely, so a twin that is still sending is left alone.

**Slot ownership is per-match**, cleared at match start. `resetPlayersForMatch`
deliberately leaves `leaveGraceSec` alone, so a countdown armed in the previous
match can still be running on a slot in this one; without clearing, that player
rejoining would "resume" a body from a match they never played.

**`clampName` was the wrong clamp for an id.** It caps at
`PLAYER_NAME_MAX_CHARS` (32) and a UUID is 36, so it quietly sawed the last four
characters off every id. `clampClientId` + `CLIENT_ID_MAX_CHARS` (64) replaces
it, with room for D3's longer token.

The WebSocket carries the id as `?cid=` on the upgrade URL, for the same reason
`?key=` travels that way: a WS connection claims its slot during the handshake,
before any hello could arrive. It rides the *dial* URL, kept apart from the
`serverUrl` the profile records and the UI shows.

---

### D4. Re-key the `high-score` scoreboard onto the identity token — **DONE** (#98)
**Why:** the leaderboard is already built on the **`high-score`** branch, but its
table is `std::map<std::string /*display name*/, int>`. Display names are neither
unique nor owned — two players typing `MIKE` share a row, and anyone can claim
someone else's row by typing their name. D3 supplies the identity that fixes it.

**Scope:**
- Re-key the table on the D3 identity; the display name becomes a **property** of
  the row rather than its key, so renaming keeps your history and the board shows
  your current name.
- File format `<score>\t<name>` → `<score>\t<id>\t<name>`. **Decide the
  migration** for an existing `scores` file — carry names forward as
  provisional ids, or take a deliberate one-time reset. Do not default into it.
- Keep the good parts as they are: atomic `.tmp` + `rename` save, the tolerant
  skip-a-bad-line load, the JSON-only `leaderboard` wire message, the
  `PLATFORMZ_SCORES` override.
- **Multi-match:** keep `scoreboard` a process-level global (it lives above the
  Match layer, so A1 leaves it alone), slot `scoreboardMutex` innermost in A3's
  lock order, and debounce `save()` — with N matches, N match-ends can each
  trigger a full-file rewrite in quick succession.

**Merge status:** `high-score` was merged into `publishing-start` on 2026-08-30
(`c210f47`), so the scoreboard code is already here — this is purely the re-key.

**Files:** `scoreboard.h`, `server/server_main.cpp`, `constants.h`,
`docs/deploy-vultr.md`.
**Depends on:** D3, and merging `high-score`.


#### What shipped, and the two decisions that were Mike's

**Migration: a clean reset, with the old file kept.** Every line of the old
`<score>\t<name>` format has two fields, so the tolerant loader already skips it -
the format change *is* the migration, and it announces itself once at boot rather
than per line. The alternative was carrying names forward as provisional ids,
which either leaves a dead ghost row beside every returning player or re-opens
the very "type someone's name to take their row" hole D4 closes. Of the 102 rows
in the local file, most were bot names and the human ones were exactly the
collided rows; carrying that forward would have preserved the bug's output.
Nothing is actually required on the box: the loader skips the old lines and the
first credited match rewrites the file. Deleting it (server stopped, or a live one
rewrites it straight back) only skips the `ignored N pre-D4 line(s)` boot line.

**Bots stay on the board, in their own class.** They have no identity to be
issued - nothing signs for a bot - so a bot's row is keyed on its name behind a
`-`. That prefix is doing real work: a human id is 32 hex characters and can only
start with `[0-9a-f]`, while `-` is 0x2D, below `'0'` at 0x30. The two classes
are therefore **disjoint by construction**, "is this a bot" is a single character
compare, and bots sort ahead of every human for free in both the map and the file
— the ergonomics of a negative number without turning a 128-bit id into an
integer, which was the shape originally suggested and would have meant re-issuing
every D3 token to buy `< 0` over `[0] == '-'`.

The wire carries the top rows of **both** classes, tagged, so the client's
LEADERBOARD modal toggles PLAYERS/BOTS without a round trip. It shows players by
default: a bot plays every single match, so a combined board is nothing but bots
within a day.

**One regression repaired on the way.** Debouncing `save()` - which the issue
asked for, because N match-ends each rewrote the whole file - meant up to five
seconds of credits could be lost on a restart, where the synchronous save lost
nothing. And `systemctl restart` sends SIGTERM, so those were exactly the seconds
an operator would lose on every deploy. The server now catches SIGTERM/SIGINT,
flushes on the way out, and has a graceful shutdown it did not have before.

---

### D5. The leaderboard as an arcade board — **DONE** (#132)
**Why:** the all-time table answers "who has the most points ever", which is a
career stat — it only goes up, the same person sits at the top for months, and
there is nothing to chase on a Tuesday night. An arcade cabinet answers something
better: **what were the best runs.**

**Two tables, because neither derives from the other.** A capped run list has
thrown away everybody's non-top runs, so you cannot sum it back into a career
total; a career total has no idea which matches made it up.

```
C  <score>  <matches>  <id>  <name>                          career totals
R  <score>  <when>  <map>  <kind>  <matchName>  <id>  <name>  one run
```

`matches` on C is the one int that lets that board be ranked by **average per
match** rather than by volume. Without it, it permanently rewards whoever has
played most and no newcomer can catch up — the duller question it currently looks
like it is answering.

**One file, with a line-type column.** A match end writes both a run row and a
career credit; two files means two saves, and a crash between them leaves a run
that no career total reflects. One file is one `rename()`, so it is both or
neither — and the existing atomic-save and skip-the-bad-line loader carry over
untouched. Old-format lines fail on the first field and are skipped, the same
free migration D4 got.

The format holds only because every text field goes through `clampName`, which
strips tabs. `matchName` and `name` are both user-chosen free text, so "everything
past the last tab is the name" stops being true; tab-separated fields work, but
any future field that is not clamped breaks it.

**The name rule differs between the tables, deliberately.** R freezes the name at
record time (a run is a historical event: *MIKE scored 999 that Tuesday*, and
renaming yourself does not rewrite what happened); C follows renames, because a
career total belongs to a person. Written down because it reads as an
inconsistency and somebody will otherwise "fix" one to match the other.

**Rules:** 3 rows per player, so one great night cannot take every slot and
"beat your own third place" stays alive. The personal best pinned below the top N,
because a global top N is invisible to everyone not in it. **Bots are IN, capped at
one row** - the factory high score an arcade cabinet ships with, there to be
knocked off; no filter and no toggle. Map stored but not displayed; ranking stays
global rather than splitting a small player base four ways.

**Official-only must filter on READ, not on record.** Filtering at record time
throws the custom runs away forever, so flipping the switch back shows an empty
board; filtering on read makes the switch free in both directions and retroactive
on rows already on disk — which is the whole reason `kind` is stored now rather
than later. Shape it like the other operator knobs
(`PLATFORMZ_SCORES_OFFICIAL_ONLY=1` decides what goes on the wire).

That decides the trim too: **top N of each kind, not top N overall.** Keep the
top N overall and a run of high-scoring custom matches evicts the official rows,
so the switch reveals a near-empty board months later. Cheap now, unfixable then.

`kind` and `matchName` are not redundant: names are user-chosen and somebody can
call their custom room `OFFICIAL MATCH`. The kind is server-assigned truth and is
the only thing the filter can key on safely.

**A run IS a match — settled by construction.** In an arcade game a run is one
*life*: die, score goes up, next player. PLATFORMZ has no respawn — `isAlive =
true` happens only in `resetPlayersForMatch` at match start and in
`TakeOverSlot` when a NEW player claims a slot. You die once, spectate, and the
match ends when every human is dead or one player is left standing. So `score` is
simply the player's score at match end. Written down rather than dropped, because
it stops being true the day a lives or respawn mode appears — and R rows written
either side of that would mean different things. Two edges survive and neither is
a blocker: a mid-match joiner starts at 0 and plays a partial match, and a leaver
has their score frozen when the grace expires.

**The clock: wall time for display, steady time for behaviour.** `NowSec()` is
`steady_clock`, whose zero is arbitrary (boot, in practice, and different every
restart) — a steady timestamp cannot be turned into a date at all, because there
is no anchor to pick. `system_clock` has one, the Unix epoch, which is what makes
it displayable and equally what makes it jumpy when NTP steps the box. A wrong
wall clock puts a wrong date on a leaderboard row, which is cosmetic; a wrong
clock in the reap timer or the mid-match grace would tear down live matches. That
is why everything else here is steady, and why this is a safe exception rather
than a crack in the rule. **Store `to_time_t` from day one even though nothing
displays it** — it is the one field that cannot be retrofitted, since a row
written without a timestamp can never acquire one.

**N = 10** - nine human slots and one bot row - **and a player sees exactly one
thing:** that board, with their own best run appended below. No tabs, no toggle, no filter. Displaying
career totals is a **future upgrade**, and D4's PLAYERS/BOTS toggle goes with it.

The C table is still written and maintained regardless. Nothing renders it, which
is fine - but stop RECORDING it and the future upgrade has no history to show,
because a career total cannot be reconstructed from a capped run list. Same
reasoning as the timestamp: the cheap half is keeping the data, and it is the half
that cannot be retrofitted. `matches` keeps counting too.

Two rules for the pinned row: do not show it twice if that run is already in the
top 10, and a player with no recorded runs has nothing to pin.

**Bots hold exactly one row.** Left alone they would own the board: there are 9
bot names, and if each were its own identity the 3-per-player cap would allow up
to 27 bot rows against a board of 10 - reserving no space for humans at all - and
bots play every single match where a person plays some of them. So every bot
shares one identity, `-BOT`, capped at 1 row where a human gets 3. The board is
nine human slots and one line reading *the best a bot has ever managed*, which is
a better thing to chase than ten of them.

That works because R rows already store the display name APART from the identity:
the row is keyed `-BOT` for capping but still displays `GEOFF`, whichever bot set
it. The decoupling exists for the rename rule and happens to pay for this too -
worth knowing before somebody tidies up the apparent duplication.

Cap it per KIND in storage, not once globally. A single best-bot row overall may
be from a custom match, and flipping the official-only switch would then leave
ZERO bot rows despite plenty of official ones to choose from. Within each kind,
`-BOT` keeps its best 1 and humans keep their best 3 - the same shape as the
per-kind trim, and the same trap.

**The modal has to grow.** It is `{250, 140, 500, 420}` today, which fits nine
rows (420 less a 60px header and ~70px of buttons, at 30px per row). Ten plus a
pinned personal best needs eleven, plus a gap separating the pin from the ranked
rows - about 500 tall, which still ends inside the 700px screen. A real change,
not a drop-in, though the button row shrinks back to just CLOSE.

**Files:** `scoreboard.h`, `server/server_main.cpp`, `screens.h`, `wire.h`.
**Depends on:** D4.

#### What shipped

Built to the design above with one addition the design had missed, found on
writing the trim.

**The trim had to keep everyone's personal best, not the global top ten.** The
board pins a player's own best run underneath it - and a global top-N trim throws
away the personal best of everybody outside the top ten, which is most people,
which is exactly who the pin is for. So the cap is purely per identity per kind
(3 human, 1 for the bots between them) with no global N at all. That also
subsumes the per-kind concern the design DID call out, and more safely: nothing
can evict an official row, because rows only ever compete with others sharing
their identity and kind. The file then grows with distinct players rather than
with matches played - at most three rows each per kind, which is the same shape
the career table already had.

**The pinned row is decided server-side.** `buildLeaderboard` takes the client's
identity, and omits `best` when they have no runs OR when their best is already on
the board. "Do not show it twice" therefore has one implementation rather than one
per platform, and the leaderboard message became per-client - eleven rows apiece,
sent on join and at match end, not per tick.

**The modal grew to 500 and lost its button row**, as predicted; it is titled HIGH
SCORES, ranks ten, rules a line, and pins YOUR BEST below. Bot rows draw in the
dimmer outline colour so the one line they hold reads as furniture rather than as
a rival.

Two smaller things: `Match` gained `matchName` (copied in at creation beside the
code and kind, since a run row records which room it happened in), and the boot
log now reports careers AND runs - it counted only `scores.size()`, so a file with
ten runs and no careers announced itself as "0 names".

Tests: `scoreboard_test.cpp` covers the cap, the shared bot identity, the read-time
filter, the per-kind independence, the frozen-vs-following name rule, and a
round trip with two free-text fields on one line. `probe_scoreboard.py` proves it
over a socket. Verified in the client too, including the pin - by fixing
`PLATFORMZ_IDENTITY_SECRET`, reconnecting with a stored token, and watching the
server recognise it and pin the matching run.

---

### D6. A LOCAL high-score board — **DONE**
**Why:** the SCORES button was networked-only, because the only table was the
server's. So an offline player - which is every player before they type a server
URL, and every player whose connection is down - had no high-score table at all,
and the bots they had just lost to had no record of having won. LOCAL MATCH was
the one mode where a score went nowhere.

**Scope:** a second arcade board, kept on the client, fed only by local matches
and never mixed with the online one.

**Files:** new `runboard.h`, new `local_scores.h`, `scoreboard.h`, `profile.h`,
`screens.h`, `main.cpp`, new `test/local_scores_test.cpp`.
**Depends on:** D1 (storage), D5 (the rules).

#### What shipped
**Two boards, not one board with a flag.** Nothing about a local match is
refereed: the client hosts its own sim, so the score is whatever the client says
it is, and the rules are whatever the player set them to. Five bots on EASY in an
XL arena is not the same game as an official room. Merging the two tables would
either corrupt the shared one or force the local one to pretend it was earned.
They answer different questions - *how do I compare to everyone* versus *how do I
compare to my own best evening, and to the bots* - so they are a tab apart in one
modal rather than one table with a source column.

**The RULES were extracted rather than reimplemented.** `runboard.h` now holds
`RunRow`, the bot-id convention, and `TrimRuns`/`TopRuns`/`BestRunFor` as free
functions over a vector; `scoreboard.h` keeps the career table and the file on the
box and delegates the rest, and `local_scores.h` adds only its own persistence.
The alternative - a second, simpler implementation for the local case - is how the
two boards would have quietly grown different tie-breaks, a different cap, and a
different answer to "does a bot get its own row", so that a player would have had
to learn the same table twice.

**The bot cap matters more here, not less.** All bots share `RUN_BOT_ID` and hold
one row between them, exactly as they do online. Online that keeps nine bots from
crowding out the humans; locally there is exactly **one** human on the board, so
per-bot rows would have left them a single line on their own machine.

**Keyed on a constant, deliberately NOT on `clientId`.** There is one person at
this keyboard. Keying the board on the profile's id means a corrupt or deleted
profile mints a fresh `clientId` and every run already on disk silently becomes
somebody else's - the rows would still be listed, but YOUR BEST would go blank and
no new score could ever match them. `LOCAL_PLAYER_ID` cannot do that. Renaming
yourself still behaves as it does online: `name` is frozen per row, so old runs
keep the name they were set under.

**Its own file, in the profile's drawer.** `scores.json` beside `profile.json`
(and `platformz.scores` beside `platformz.profile` in `localStorage`), through
profile.h's storage layer - which had the two hard parts solved already on both
platforms: write-then-`rename` so a crash cannot truncate, and localStorage with
its exceptions caught. Generalising `ReadRaw`/`WriteRaw` to take a key was
cheaper than a second copy of either. **Not** a key inside `profile.json`, so
clearing your scores cannot cost you your name or your identity token, and a
parse failure in one cannot take the other down. It needs no size cap: `TrimRuns`
bounds it at three player rows plus one bot row, forever.

**The board is shown at the moment it is earned.** The local game-over screen
prints where the run landed (`LOCAL HIGH SCORE #3`), because a high-score table
nobody is shown at the instant they set a score is a table nobody knows exists.
The rank is read off the trimmed board rather than computed by counting better
scores - those two disagree exactly where it matters, at a fourth personal best,
which beats nothing above it AND is trimmed away on arrival. Counting would call
that "#4"; looking says it is not on the board, which is the truth.

**All three local endings credit the run** - the manual `M`, the solo
clear-or-die, and last-man-standing - through one `endLocalMatch()`. They were
three separate `screen = GAME_OVER` assignments, which is precisely the shape
that ends up crediting two of three. BENCH mode is excluded: its map numbers come
off the command line, well outside any preset, so its scores are not comparable to
anything.

Tests: `test/local_scores_test.cpp` (33 checks) spends most of its time on the two
things that can actually hurt - the per-identity cap, which is the only thing
bounding the file on a machine nobody is watching, and hand-edited files, because
the player owns this one and it will get edited. `test/web_profile_test.py` gained
a check that two keys are two drawers, which is the property the whole arrangement
rests on and the only place it could go wrong (natively they are two files).
Verified in the client end to end: a local match with three bots wrote two rows -
one human, one shared bot row - and the modal ranked them.

---

# Epic E — Ops, abuse, and proof it scales

### E1. UDP handshake token (anti-spoof / anti-amplification) — **DONE** (#88)
**Why:** UDP source addresses are spoofable and **the vector already exists
today** — a ~60 B `hello` from an unknown endpoint is answered immediately
(`RegisterPeer` L1793) with a LARGE-map welcome of ~3 KB. `PLATFORMZ_KEY` masks
it only if you treat the key as secret, and it isn't: it ships in invite URLs and
in baked handout builds (`PLATFORMZ_DEFAULT_SERVER_KEY`). Adding a directory that
answers unauthenticated packets makes the server a genuinely useful reflector.

**Scope — layered:**
1. **Return-routability challenge that keeps no per-client state.** A `hello`
   from an unknown endpoint gets back only `{"type":"challenge","c":"<24 hex>"}`
   (~48 B) — smaller than the hello, so the amplification factor is **below 1**.
   The cookie is
   `truncate(HMAC(secret, src_ip ‖ src_port ‖ time_bucket))` over 30 s buckets
   (previous bucket also accepted). The client echoes `c` in its next hello and
   the server verifies it against the address it actually observes; a spoofer
   never receives the challenge, so it never registers. Standard QUIC/DTLS retry.
   **Preserve silence-on-bad-key:** a wrong `key` gets no challenge either.

   **Use HMAC, not `hash(secret ‖ data)`.** An earlier draft of this plan wrote
   the latter. Gluing a secret onto the front of a Merkle–Damgård hash (SHA-256
   included) is the classic prefix-MAC mistake: given one valid tag you can
   sometimes extend the message and produce another valid tag *without knowing
   the secret*. The fixed-shape input here makes that hard to exploit in
   practice, but HMAC is the construction built to close exactly this hole, it
   is no harder to call, and it is the same primitive D3 needs anyway.

   **Compare tags in constant time.** A plain `==` on the returned cookie bails
   out at the first wrong byte; the timing difference leaks the correct value one
   byte at a time. Use a constant-time compare for this and for D3's token.

   **"Stateless" here means this mechanism stores nothing per client** — a flood
   of spoofed hellos costs one HMAC and one small send each and allocates
   nothing, so it cannot be turned into memory exhaustion. It does *not* mean the
   process writes nothing to disk; the scoreboard already does, and the secret
   below has to outlive a restart. See "What the server keeps on disk" in
   `docs/deploy-vultr.md`.
2. **Nothing large to an unregistered endpoint** — `list`/`join`/`create`/`quick`
   are honoured only for an endpoint already in `udpIndex`.
3. **Token-bucket the list** even for registered connections: one reply per second
   per connection, extras dropped *silently* (an error reply is still a reply).
4. **One-datagram cap on the list** (B1) — bounds the worst case at ~1200 B.
5. **Rate-limit bad join codes** (5/min/connection). A 4-char code space is
   otherwise brute-forceable in seconds.

Client cost is one extra RTT: stash the challenge and re-send `hello` immediately
rather than waiting out the 0.5 s retry (`main.cpp:761`). **WS needs none of this**
— the TCP handshake already proves the address — only the rate limit.

**The secret, and why it is shared with D3.** Both this cookie and D3's identity
token are "stamp something with a key only the server has, then check the stamp
later without having written anything down". One secret in
`PLATFORMZ_IDENTITY_SECRET` (beside `PLATFORMZ_KEY` in `/etc/platformz.env`), one
pair of sign/verify helpers, loaded once at boot. Build it here; D3 inherits it —
which is why E1 comes first in the milestone order.

It **must persist across restarts.** Generate it at boot instead and every issued
token silently becomes invalid on every deploy, which for D3 means every player
looks like a new person after each release. It is root-owned config, not game
state, so it belongs in the env file rather than `/var/lib/platformz` — but it is
part of the backup surface either way.

#### What shipped, and the three places it differs from the plan above

`server/crypto.h` is the shared primitive: SHA-256 and HMAC-SHA256, a
constant-time compare, and `LoadServerSecret`. No OpenSSL — the server links
Boost and nothing else, and a new package on the deploy box is a worse trade than
80 lines with RFC 4231 vectors behind them (`server/test/crypto_test.cpp`). D3
inherits all of it. The cookie itself is 96 bits of truncated HMAC over
`family ‖ addr ‖ port ‖ bucket`, minted and checked in `server_main.cpp`'s
"UDP handshake cookie" section; the client stashes it in `udpCookie` and re-sends
`hello` on the next frame rather than waiting out the 0.5 s retry.

**1. The amplification factor is not below 1, and the plan's arithmetic was
optimistic.** The challenge is 51 bytes. A real client's hello is ~90, so it *is*
below 1 for anyone playing — but the smallest hello a script can craft is
`{"type":"hello"}` at 16 bytes, making the worst case 3.2x on payload, or 1.8x
once both directions' 28 bytes of IP+UDP header are counted. That is down from
~33x, and nobody builds a reflector at 1.8x. Strictly below 1 would mean a binary
challenge tag (~13 B) instead of JSON; not worth a message type for the gap.
`probe_cookie.py` prints the measured numbers rather than asserting a ratio.

**2. The list limit is a token bucket with a burst of 3, not a flat one per
second.** A hard interval silently swallows a second and third click inside one
second — which is what a person paging the browser actually does — and leaves the
screen waiting on a reply that is never coming. The refill rate (1/s) is what
bounds a script; the burst is what keeps the UI honest. `probe_directory.py`
caught this: its two `list` calls 0.6 s apart both have to be answered.

**3. Once the join budget is spent it refuses *every* join, not only wrong
codes.** Whether a code is a guess is only knowable after the lookup that answers
the guess, so a limiter that let good codes through would answer every guess.
Only wrong codes are *charged*, so hopping rooms or bouncing off a full one costs
nothing — but five typos does mean waiting out the minute. That is the same
bargain a login lockout makes, and it is worth stating because the tempting
"softer" version is not a limit at all.

Scope item 2 needed no code: an unknown UDP endpoint only ever reaches the
`hello` branch, so `list`/`join`/`create`/`quick` from a stranger were already
dropped unread. There is now a comment saying so, and a probe check, because that
is a property somebody could helpfully break.

**Files:** `server/server_main.cpp`, `net_client.h`/`main.cpp` (echo the cookie).
**Depends on:** B1. **Must land before the server is publicly advertised.**

---

### E2. Caps, rate limits, and the key story — **DONE** (#89)
**Scope:**
- `MAX_MATCHES` (E: from A4), matches-created-per-address, joins-per-second.
- **`PLATFORMZ_KEY` stays as the server-wide front door** (unchanged semantics:
  wrong key ⇒ no reply at all). Per-match codes are a *separate*, softer thing:
  they hide a match from the public list and gate joining it. Don't conflate them.
- Sanitize match names the same way player names already are
  (`clampName`, printable 32–125).
- Public server ⇒ drop the key; friends-only ⇒ keep it. Document both.

**Files:** `server/server_main.cpp`, `docs/deploy-vultr.md`.

#### What shipped

**Fullness no longer ends a connection, and that was most of the work.** The old
shape was: no free slot → send a `full` packet → stop reading (WS) or never
register the peer (UDP). Over UDP that is indistinguishable from an unreachable
server, so the client sat re-helloing into nothing behind "MATCH IN PROGRESS —
WAITING FOR A SLOT...". Being hung up on is the worst possible answer to "this
room is full", because the one thing you want next is the list of rooms that are
not.

So the server grew a third state for a connection: **unseated**. A connection is
now in exactly one of two places — some match's `clients` map, or `g_unseated` —
and an unseated one is a working client that can list, join, and create. Its
`hello` (which the client is already re-sending every 0.5 s while it has no slot)
doubles as "is there a seat yet?", so a freeing slot is taken within half a
second with no new client code at all. Both transports funnel through one
`SeatOrPark`: the room you asked for, else the default room plus a `full`
refusal so you know your invite did not land, else parked with `server_full`.

`FULL_BIN_VERSION` (0x06) and `{"type":"full"}` are both gone, and `0x06` is
burned in `netbin.h`. Fullness travels as a `joinfail`, which is JSON and so
behaves identically on both transports. The client maps a `full`/`server_full`
refusal received *before* it has a slot onto the same `serverFull` flag the old
message drove, so the existing UI string survived the protocol change.

**Two things needed no code.** Match names already go through `clampName` — the
same printable-ASCII, length-capped filter player names use — and the join-code
alphabet was already the unambiguous 32-character one with no `O`/`0`/`I`/`1`,
4 chars, private rooms hidden from the list. Both now have a comment saying so
and a probe check, because they are properties somebody could helpfully break.

**Three departures from the scope above:**

**1. `MAX_ACTIVE_MATCHES` ships OFF.** A4 measured that CPU fits ~16 matches and
that *transfer* is the binding constraint at roughly 7×, which is a judgement
about the hosting plan and not about the code — so picking a live-match number
here would have been inventing one. It is `PLATFORMZ_MAX_ACTIVE`, defaulting to
the room cap (so it can never fire), reported by `/status`. When it does bite the
start is **held, not refused**: the room waits in its lobby and begins the moment
a live match ends. Nobody's button press is lost, and there is no new failure the
client would have to be taught to explain.

**2. The per-address creation budget is tunable, because an address is a coarse
identity.** A LAN party, an office, a household all arrive from one NAT and share
one bucket, so the fourth person to make a room would be refused for something
someone else did. That is a real scenario, not a hypothetical:
`PLATFORMZ_MAX_ROOMS_PER_ADDR` (default 3, 0 disables). `probe_directory.py`
found this immediately — it fills the registry from one address to test paging.

**3. "Joins-per-second" became "moves-per-second", covering `leave` too.**
Join-leave-join-leave is the cheapest roster churn there is, and every accepted
move costs a welcome plus a leaderboard — the two biggest packets the server
sends. Budgeting only the join half would have made the budget meaningless.

Tests: `server/test/probe_capacity.py` (a full room does not drop you; an
unseated client can still browse and join; it seats itself when a slot frees; the
address and move budgets; name sanitising).

---

### E3. Load harness + CI smoke test — **DONE** (#90)
**Why:** A4's cap number and E1's mitigations are unprovable by hand, and the
"stale server binary" class of bug has bitten this project before.

**Scope:** a headless `server/loadtest.cpp` (WS, reuses `wire.h`) that opens N
connections, creates/joins M matches, sends input at 60 Hz, and reports per-match
tick times and packet loss. Add a CI job to `build.yml`: boot the server, create 3
matches, assert state packets flow and the protocol tags match.

**Files:** new `server/loadtest.cpp`, `server/Makefile`,
`.github/workflows/build.yml`.

#### What shipped

CI went from one compile job to three: **Compile**, **Tests** (the standalone C++
tests, all eleven protocol probes, and a smoke gate), and **ThreadSanitizer**. The
repo already had fifteen tests that only ran when somebody remembered; wiring them
in was the cheapest part of this and by far the largest gain.

`server/loadtest.cpp` opens M×N UDP clients, creates and fills M rooms, starts
them all, and holds 60 Hz input while measuring what a client can actually see:
delivered state rate, worst inter-packet gap, and bytes. It reports the rate the
*generator* achieved too — if the harness could not keep 60 Hz, every other
number is about the test machine and not the server, and saying so is cheaper
than someone discovering it later.

**The finding is in `perf-measurements.md`: ten matches measured 325 KB/s each
against an estimate of ~310, so the arithmetic behind `MATCH_MAX_CONCURRENT = 12`
was right — and confirms that twelve simultaneously FULL matches is ~10 TB/month
against a 2 TB plan, while the sim cost p50 0.00 ms.** CPU was never the wall.
That is the first real evidence for choosing a `PLATFORMZ_MAX_ACTIVE` number
(E2) instead of guessing one: **~6 live matches fits the quota.**

**Three departures from the issue:**

**1. The load half drives UDP, not WebSocket.** The numbers that matter are UDP's
— that is what the native client speaks, where the binary state packet and the
chunked welcome live, and E1's cookie exists nowhere else. WebSocket still gets
covered, as `--mode ws-smoke` in the same binary, which CI runs: one connection,
assert a welcome and a stream of state. That transport had *no* automated
coverage before this, so it is a gain either way.

**2. The harness cannot report per-match tick times, and does not pretend to.**
Those are the server's own and it already prints them (`PLATFORMZ_PERF=1`). A
client-side tool inventing a server-side number would be worse than not having it.

**3. TSan needed a four-line suppression file** (`server/test/tsan.supp`) for
`std::cout`, which [iostream.objects] guarantees is race-free but libc++ does not
annotate. That was the *only* report across the whole probe suite — the server's
own locking, including E2's new `g_unseated` and the budget maps, came out clean.

Two things the harness caught in itself, both worth writing down because they are
the same mistakes a *real* client could make: it did not send the keepalive, so
the 3s lobby reaper culled its clients mid-setup and the resulting mess looked
like a server fault; and it only kept *seated* clients alive, so E2's parked ones
were swept while rooms were being created.

Also: `probe_leaderboard` asserted that a leaderboard arrives before the GAMEOVER
state packet. The server does send them in that order, but that is an arrival
order over **UDP**, which promises nothing — the assertion failed about one run in
five. It is reported now, with the hard check moved onto what is actually
guaranteed (the table arrives, credited). A probe that cries wolf is worse than no
probe once CI is watching it.

---

### E4. Docs refresh — **DONE** (#91)
**Scope:** `docs/deploy-vultr.md` (multi-match section, `/status`, capacity
numbers from A4, the key-vs-code distinction) and `docs/play-web-via-github.md`
(since retired).

**~~Correct the "stateless server" claim~~ — DONE 2026-09-10.** The doc asserted
"reads no files, writes no files" twice; the scoreboard made that false. Those
lines are gone and `docs/deploy-vultr.md` now has a **"What the server keeps on
disk"** section: the `scores` file and `/etc/platformz.env`, the consequence of
losing each, and a backup snippet. Anything E1/D3 adds (the shared secret) goes in
the same list.

**~~Write a fresh `docs/multiplayer-testing.md`~~ — DONE 2026-08-30.** Covers the
lobby/START flow, both transports and how they differ, the `PLATFORMZ_KEY` gate,
the scoreboard, and the trap that a bare `./platformz` connects to the *live*
server because `secrets.mk` bakes the host in. The pre-lobby version stays at
`docs/multiplayer-testing-archive.md` for its Emscripten build record.

**~~Still owed: a "two matches, four clients, one server" section~~ — DONE.** It
is in `multiplayer-testing.md`, written as what to *look for* rather than what to
type: C and D must stay in their lobby while A's match starts, the heartbeat must
read `matches 2 (1 active)`, and mixing transports inside one room must be
invisible. Alongside it, a "testing without a GUI" section, because by now the
fastest way to answer most of these questions is a probe rather than four windows.

**~~`docs/matchmaking.md`~~ — DONE.** The directory protocol as the reference for
both ends: what a room is, the two gates and why they are not the same gate, the
UDP handshake, every message in both directions with its real field names, every
`joinfail` reason, every limit, and where each concern lives in the tree.

#### What else was stale, and is not now

`multiplayer-testing.md` had been overtaken by rooms in ways that would have
actively misled someone: it said the title screen *is* the lobby (it is a router
now — QUICK / FIND / CUSTOM / LOCAL), that the host is the lowest connected slot
(it is the room's creator, and an official room has no host at all), and that map
size is chosen by *which* START button you press (B3 moved it into OPTIONS). Its
boot transcript and heartbeat format both predated the registry.

`play-web-via-github.md` still advertised **2 player slots** (8 per room, up to
12 rooms). That doc and its workflow have since been retired entirely.

`deploy-vultr.md` gained the multi-match section it was owed — what the two boot
rooms are for, how to read the rollcall — plus a field-by-field `/status` table
and, from E3's measurements, the capacity paragraph that says plainly that **CPU
is not the constraint and ~6 live matches is what 2 TB/month pays for.** The
key-vs-code distinction and the abuse-limit table landed earlier, with E2.

### E5. Empty slots: `maxBots` and `minHumansToStart` — **DONE**

Two new `MatchOptions` rules: `maxBots` caps how many unclaimed roster slots get
bot-filled, and `minHumansToStart` is the head count an official room's auto-start
arms on. Both default to the old behaviour, so a LOCAL or CUSTOM match is
unchanged.

**They have no OPTIONS slider, and that is the only thing unusual about them.**
They are authored by a preset rather than dialed by a player. The first cut put
them on `MatchPreset` instead, which kept them off the wire entirely — but it
meant a preset was written two ways, `o.speedBoost = …` inside the tune lambda
and a `WithMaxBots(…)` wrapper around it, and the second is exactly the kind of
positional afterthought the `MakePreset` comment argues against. Being in
`MatchOptions` costs a range, a clamp, a profile key and two wire keys; it buys
one way to write a preset, and the round-trip that lets a host's START echo the
room's own values back instead of resetting them.

**LOCAL ignores `maxBots`** and fills every slot. An empty slot exists so a human
can walk into it later; offline nobody ever can, so one there would just be a hole
in the match. At the default the two are identical anyway.

**How an empty slot is represented.** A server-owned `Player::isVacant`, always
carried with `isAlive = false`. Rejected alternatives:

- *Shrink the roster to `humans + bots`.* `registry.h`'s `joinable` test is
  `players < rosterSize`, so a shrunk room advertises as FULL and stops being
  joinable — the opposite of the intent. Growing it back mid-match would also
  append a `Player` with no `placePlayersSpread` and change
  `MaxAsteroidsForRoster` under a live match.
- *Reuse `isSpectating`.* It is wire-synced, drives the client's greyscale ramp,
  and `updateFuel` would keep topping up a tank belonging to nobody.
- *Derive it* from `!claimed && !isBot && !isAlive`. The two guards that most need
  it (`gamespace.h`'s death burst, `elements.h`'s spectator promotion) live in
  shared headers with no access to `clients`.

**Bots fill low, vacancies collect high.** Not arbitrary: humans are compacted
into the lowest slots, and `setPlayerCount` pops from the tail, so a match start
that shrinks the roster discards empty slots first and never disturbs the bot set.
Bot names are indexed by slot rather than by bot ordinal, so nothing thrashes as
humans come and go.

**The one place this was load-bearing** rather than cosmetic: match-end counted
`players.size() >= 2` to decide whether the single-survivor clause applied. That
is roster size. With empty slots in the roster, a one-human `maxBots = 0` match
satisfied "only one player left standing" on its first PLAYING tick and ended
instantly. It now counts participants. `probe_maxbots.py` is the regression test.

**One version bump, covering both.** The options block grew two `u8`s, which cost
`STATE_BIN_VERSION` `0x09 → 0x0B` (the flags byte has two free bits; these need
four each). Worth it for `minHumansToStart` alone: without it the lobby could no
longer say *how many more* players it was waiting for, which is a real loss in
exactly the rooms the feature exists for. `maxBots` rides along for free, and the
client never needed it to *render* an empty slot — the per-player `active` flag
already told it to skip one, and widening that to exclude vacant slots was a
one-line change at each of the two builders.

---

# Epic F — Road to Steam (macOS + Windows), web maintained

Separate track. Nothing here blocks Epics A–E, and A–E don't depend on it — but
it is where the matchmaking work has to land, so the ordering matters at the end.

### F1. Cross-platform build
The Makefile is macOS + Homebrew only (`CLAUDE.md` says so explicitly). Adding
Windows means a **CMake build** covering macOS, Windows and the Linux server, with
the existing `make app`/`sign`/`notarize` chain preserved (it works — don't break
it to be tidy).

### F2. Windows port of the client
**Smaller than it sounds.** The POSIX surface is nearly all in one place:
`net_client.h:132–135` (`sys/socket.h`, `netdb.h`, `unistd.h`, `fcntl.h`) and
`UdpTransport` L252–267/293 — needs Winsock (`WSAStartup`, `ioctlsocket`,
`closesocket`). IXWebSocket is already cross-platform; raylib is; `std::signal`
for SIGINT/SIGTERM (`main.cpp:94–95`) works on MSVC. Plus: profile path (D1),
asset path anchoring, and a Windows-appropriate window/DPI pass.

### F3. Windows packaging + signing
Authenticode certificate, installer or plain zip, and the equivalent of the
`spctl` assertion the Mac chain already makes before it ships a zip.

### F4. Steamworks integration — decide the boundary
Steam offers its own lobbies/matchmaking and Steam Datagram Relay. **Recommendation:
keep the custom directory as the cross-platform path** (the web build has no Steam,
and you want it maintained) and use Steamworks for distribution, the overlay, and
friend-invite → your own join code. One matchmaking system, not two.

### F5. Keep web green
Add the Emscripten build to CI so a protocol change can't silently break the
browser client (today CI builds only the server).

---

## Suggested order

**Milestone 1 — server can hold many matches (invisible to players)**
A4-measure → A1 → A1b → A4-remeasure → A2 → A6

Measure first, before any refactor: the numbers decide whether 10–20 concurrent
matches is even the right target on a 1 vCPU / 2 TB-month box. A1, A1b and A2 are
each independently shippable with **no observable behaviour change** — three
deployable checkpoints before anything a player can see.

**Milestone 2 — players can find and join matches**
B1 → B2+B3 → A3 → A5 → C1 → C2 → C3 → C4 → C5

**Milestone 3 — safe to advertise publicly**
E1 → E2 → A4-implement → E3 → E4 → D1 → D3 → D2

D3 shares its HMAC-over-a-server-secret machinery with E1's cookie, so land E1
first and reuse the secret-loading code.

**E1 is a prerequisite for Milestone 2, not a follow-up, if the server ever runs
without `PLATFORMZ_KEY`.**

**Milestone 4 — Steam**
F1 → F2 → F5 → F3 → F4

A1 and C1 are the two pure refactors, and both are prerequisites for most of what
follows — do them first and separately, so the risky changes land on a clean base.

Branch per issue. A1, A1b, C1 and A3 in particular are large mechanical diffs
that are far easier to review on their own branches than mixed into a shared one.

## Issue list

Filed 2026-08-30 as [#71-#98](https://github.com/mik0mac/PLATFORMZ/issues?q=is%3Aissue+label%3Aserver%2Cclient%2Cprotocol%2Csecurity%2Cbuild%2Cops), grouped into milestones 1-4.

| # | Issue | Title | Epic | Depends on |
|---|---|---|---|---|
| A1 | #71 | Server: extract match state into a `Match` struct (no behaviour change) | server | — |
| A1b | #72 | Server: kill per-tick allocation churn (grid reuse, GAMEOVER idle, bitmask slots) | server, perf | A1 |
| A2 | #73 | Server: `MatchRegistry` — match codes, creation, empty-match reaping, `MAX_MATCHES` | server | A1 |
| A3 | #74 | Server: route connections to matches without reconnecting (WS + UDP) | server | A1, A2, B1 |
| A4 | #75 | Server: measure tick cost **and egress**, then pick the sim scheduler and the caps | server, perf | A1b |
| A5 | #76 | Server: join-in-progress by taking over a bot slot | server | A1, A3 |
| A6 | #77 | Server: per-match heartbeat + `GET /status` (and fix the Actions idle watchdog) | server, ops | A2 |
| A7 | #99 | Server: bucket static platforms once per match (found by A4) | server, perf | A4 |
| A8 | #100 | Protocol: state packets over the MTU budget + unsurvivable chunking (found by A4) | server, protocol | A4 |
| A9 | #107 | Server: split online matches into OFFICIAL (preset, auto-start) and CUSTOM (host-run) | server, protocol | A2 |
| A10 | #108 | Server: host is the room's creator and sticks; migrates to the lowest remaining slot | server, client | A9 |
| B1 | #78 | Protocol: directory messages — `list`/`create`/`join`/`quick`/`leave` + `matchlist`/`joinfail` | protocol | A2 |
| B2 | #79 | Protocol: welcome carries the match code (`WELCOME_BIN_VERSION` 0x02→0x0A) + server-wide epoch | protocol | B1 |
| B2b | #111 | Client: official lobby shows the head count and the auto-start countdown | client | B2 |
| B3 | #80 | Protocol: move map size into `MatchOptions` so the lobby shows it before start | protocol | B2 |
| C1 | #81 | Client: lift the game shell out of `main()` into `screens.h` (no behaviour change) | client | — |
| C2 | #82 | Client: `BROWSE` screen — match list, refresh, join | client | B1, C1 |
| C3 | #83 | Client: split `LOBBY` off the title screen, add LEAVE | client | C1, A3 |
| C4 | #84 | Client: QUICK MATCH and CREATE MATCH | client | C2, B1 |
| C5 | #85 | Client: invite links (`?match=`) and join-by-code | client | C2 |
| C6a | #149 | Protocol: an ack for a connection that holds no slot | protocol | B1 |
| C6b | #150 | Server: stop seating a connection that asked for no room | server | C6a |
| C6c | #151 | Client: tell "connected" apart from "seated" | client | C6a |
| C6d | #152 | Client: connect lands in the match browser, not a room | client | C6c |
| C6e | #153 | Server: retire the default landing room | server | C6b, C6d |
| C6f | #154 | Testing: probe harness for a connection with no room | testing | C6b |
| D1 | #86 | Client: persistent local profile (name, `clientId`, `token`, volume, options) | client | — |
| D3 | #97 | Server-issued identity token (stateless HMAC; unblocks leaderboards later) | server, security | D1 |
| D2 | #87 | Reconnect into your own slot (use the existing 15 s grace) | server, client | D1, D3, A3 |
| D4 | #98 | Re-key the scoreboard onto the identity token (display names collide today) | server, security | D3 |
| D5 | #132 | Leaderboard as an arcade board: rank RUNS, not career totals | client, server | D4 |
| E1 | #88 | Server: UDP handshake cookie — anti-spoofing / anti-amplification | security | B1 |
| E2 | #89 | Server: caps and rate limits; separate `PLATFORMZ_KEY` from per-match codes | security | A2 |
| E3 | #90 | Load harness + CI smoke test for multi-match | testing | A3 |
| E4 | #91 | Docs: multi-match deploy, matchmaking reference, fix the stale "2 player slots" | docs | A3 |
| F1 | #92 | Build: CMake covering macOS, Windows and the Linux server | build | — |
| F2 | #93 | Windows port of the client (Winsock `UdpTransport`, paths, DPI) | build | F1 |
| F3 | #94 | Windows packaging + Authenticode signing | build | F2 |
| F4 | #95 | Steamworks: distribution, overlay, friend invite → join code | steam | F2, C5 |
| F5 | #96 | CI: build the Emscripten web client so protocol changes can't break it | build, ci | — |

Labels created for this work: `server`, `client`, `protocol`, `security`,
`build`, `ops`, `perf`, `testing`, `steam`, `ci`. A1, A3, B1 and E1 are the ones
worth writing an explicit design comment on before coding.

## Verification

Each issue carries its own "done when", but the end-to-end proof is:

1. `make -C server && make` — both build clean. CI green (`build.yml`).
2. **Two matches, four clients, one server** (procedure per
   `docs/multiplayer-testing-archive.md` until E4 replaces it):
   run `./gameserver` locally; launch two native clients
   (`./platformz ws://localhost:9000`) and two browser clients
   (`make web && python3 -m http.server 8080` → `?server=ws://localhost:9000`).
   Client 1 creates a match, client 2 joins from the browser; clients 3 and 4
   create and join a second. Confirm: neither pair sees the other's players,
   rockets or kill-feed; each has its own host, options and countdown.
3. **Transport parity:** repeat with `udp://localhost:9000` for the native pair —
   binary state packets, chunked welcomes, and the E1 cookie all exercised.
4. **Leave/rejoin:** leave a match back to BROWSE, join the other one, confirm the
   slot re-welcome lands (no ghost body, no wrong-slot input) and that after D2 a
   force-quit inside the grace window restores score and position.
5. **Capacity:** `server/loadtest.cpp` at the target match count on the actual
   Vultr box; p95 tick under 16.6 ms; exceeding `MAX_MATCHES` returns
   `joinfail: server_full` rather than degrading live matches.
6. **Abuse:** a spoofed-source UDP `list` gets only a cookie, never a match list.
7. **Version gate:** an old client against the new server shows "SERVER VERSION
   MISMATCH", not garbage. Watch this one closely — the client latches
   `protoMismatch` **permanently** (`main.cpp:1444`, never cleared), so a single
   server message whose byte 0 is neither `{` nor a registered tag bricks the
   client UI with no recovery. Keep every new message JSON, and keep
   `netbin.h`'s distinct-tags `static_assert` honest.

## Open question for A2/E2: what happens to `full`

Today a full server sends `{"type":"full"}` / `FULL_BIN_VERSION` and **drops the
WS socket**, leaving the client re-helloing forever behind "MATCH IN PROGRESS —
WAITING FOR A SLOT…". With a directory that becomes wrong: the connection should
**never be dropped for fullness** — `join` fails, the client stays in the
directory, and that match's row now reads 8/8 so the player picks another.
Retrying becomes a user action instead of an infinite hello loop. Map the failure
onto the existing `ServerMessage::Type::Full` so the current UI string survives,
and retire `FULL_BIN_VERSION` — `0x06` is then burned permanently, per
`netbin.h`'s no-recycling rule.
