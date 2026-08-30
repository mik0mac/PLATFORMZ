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

**Knock-on for the deploy story:** the server is no longer stateless once this
lands. `docs/deploy-vultr.md` asserts "reads no files, writes no files" **twice**,
and there is now a `scores` file to back up. E4 must fix that.

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

**Locking:** each `Match` owns one mutex covering *both* its sim state and its
client list, replacing today's `gameMutex → clientMutex` pair. That ordering
discipline is spread across every call site and does not survive N of each. One
mutex per match is simpler, and the contention it removes is the whole point of
splitting matches up. Globals that survive: `nextConnId`, `g_udp` +
`udpSendMutex`, `joinKey`, and the new registry.

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
  playing. Stop simulating after ~15 s and return to LOBBY (freeing the world)
  after ~60 s — both well past `GAME_OVER_TIMER = 5`.
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

### A6. Per-match logging + a `/status` endpoint
**Why:** today's operational signal is one `tick N players C asteroids A` line —
and the GH Actions idle-watchdog greps it. With N matches that's meaningless, and
"is anyone playing?" becomes unanswerable.

**Scope:**
- Heartbeat becomes one summary line (matches, total players, worst tick) plus a
  per-match line at a lower cadence. **Update `.github/workflows/gameserver.yml`'s
  idle watchdog** — it greps `players [0-9]+`, which the new format must keep
  satisfying or the watchdog silently stops working.
- `Session::Start` already reads the raw HTTP request; a **non-upgrade `GET
  /status`** returns JSON (uptime, match count, player count, version tags) and
  closes. Free monitoring, and a "N players online" badge for the site later.
  Guard it so it can't be used as an amplifier (small fixed response, no query
  echo).

**Files:** `server/server_main.cpp`, `.github/workflows/gameserver.yml`,
`docs/deploy-vultr.md`.

---

# Epic B — Protocol: the directory

### B1. Directory message types
**Why:** the client needs to see and pick a match.

**Scope:** client→server is always JSON, and the client dispatches inbound on
**byte 0** (`'{'` ⇒ JSON, else binary tag — `wire.h` `applyMessage` ~L470). So
**the whole directory can be JSON-only, on both transports** — no new binary tags,
no new decoders, and the browser build gets it for free.

**But the match list must fit ONE datagram — do not let it chunk.**
`UdpTransport::Reassemble` (`net_client.h:311`) is **single-slot**: `parts_` /
`chunkGen_` hold exactly one in-flight message, and a chunk with a new `gen`
discards a half-done buffer. Today only welcomes chunk, one at a time, so that is
safe. A chunked `matchlist` racing a chunked re-welcome would silently destroy
one of them. Fix it in the protocol, not the transport: **hard-cap the reply to
~1160 bytes and page with a cursor** (~60 B/entry ⇒ ~19 matches; page at 12 for
slack). This also bounds the amplification factor E1 is about, so it is the right
call on two independent grounds.

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

### B2. Welcome carries the match code (version bump)
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

---

### B3. Move map size into `MatchOptions`
**Why:** map size is currently chosen by *which* of four START buttons you press
(`wire.h:150`, `main.cpp:944–953`) — so it exists only at the instant of starting.
A lobby everyone can see needs the selected map to be visible *before* start, and
the match browser needs to advertise it.

**Scope:** add a map field to `MatchOptions` (`options.h`) and to `writeOptionKeys`
(`wire.h:153`); the four START buttons become a map *selector* plus one START.
The server's `pendingHalf/Plat/Roid` fold into the same pending-options bundle
from A1. Keep the server-side clamp to `nb::MaxAsteroidsForRoster` at start.

**Files:** `options.h`, `wire.h`, `main.cpp`, `server/server_main.cpp`.
**Note:** this is a protocol shape change — land it with B2's version bump, not
separately.

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

# Epic D — Identity

### D1. `profile.h` — persistent local profile
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

---

### D3. Server-issued identity token
**Why:** `clientId` alone proves nothing — the client generates it and owns the
file. That is fine for D2's 15-second slot restore, and useless for anything
persistent. A server-signed token makes "same player as last time" verifiable, and
it is far cheaper to design in now than to retrofit around a shipped profile
format and a shipped hello.

**Scope — stateless, no database:**
- Server holds a secret in `PLATFORMZ_IDENTITY_SECRET`, alongside `PLATFORMZ_KEY`
  in `/etc/platformz.env`.
- `hello` with **no** token → mint `token = base64(uuid ‖ HMAC(secret, uuid))`,
  return it; the client stores it in its profile.
- `hello` **with** a token → verify the HMAC. Valid ⇒ identity trusted. Invalid or
  from an old secret ⇒ treat as no token and mint fresh (never hard-fail a join
  over it).
- The server stores **nothing** — it just verifies its own signature. Same trick
  as E1's cookie, and the two should share the secret-loading code.

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

---

### D2. Reconnect into your own slot
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
**Depends on:** D1, D3, A3.

---

### D4. Re-key the `high-score` scoreboard onto the identity token
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

---

# Epic E — Ops, abuse, and proof it scales

### E1. UDP handshake token (anti-spoof / anti-amplification) — **do not skip**
**Why:** UDP source addresses are spoofable and **the vector already exists
today** — a ~60 B `hello` from an unknown endpoint is answered immediately
(`RegisterPeer` L1793) with a LARGE-map welcome of ~3 KB. `PLATFORMZ_KEY` masks
it only if you treat the key as secret, and it isn't: it ships in invite URLs and
in baked handout builds (`PLATFORMZ_DEFAULT_SERVER_KEY`). Adding a directory that
answers unauthenticated packets makes the server a genuinely useful reflector.

**Scope — layered:**
1. **Stateless return-routability challenge.** A `hello` from an unknown endpoint
   gets back only `{"type":"challenge","c":"<24 hex>"}` (~48 B) — smaller than the
   hello, so the amplification factor is **below 1**. The token is
   `truncate(hash(secret ‖ src_ip ‖ src_port ‖ time_bucket))` over 30 s buckets
   (previous bucket also accepted). **Stateless** — a flood of spoofed hellos
   costs one hash and one small send each and stores *nothing*, so it can't be
   turned into memory exhaustion either. The client echoes `c` in its next hello
   and the server verifies it against the address it actually observes; a spoofer
   never receives the challenge, so it never registers. Standard QUIC/DTLS retry.
   **Preserve silence-on-bad-key:** a wrong `key` gets no challenge either.
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

**Files:** `server/server_main.cpp`, `net_client.h`/`main.cpp` (echo the cookie).
**Depends on:** B1. **Must land before the server is publicly advertised.**

---

### E2. Caps, rate limits, and the key story
**Scope:**
- `MAX_MATCHES` (E: from A4), matches-created-per-address, joins-per-second.
- **`PLATFORMZ_KEY` stays as the server-wide front door** (unchanged semantics:
  wrong key ⇒ no reply at all). Per-match codes are a *separate*, softer thing:
  they hide a match from the public list and gate joining it. Don't conflate them.
- Sanitize match names the same way player names already are
  (`clampName`, printable 32–125).
- Public server ⇒ drop the key; friends-only ⇒ keep it. Document both.

**Files:** `server/server_main.cpp`, `docs/deploy-vultr.md`.

---

### E3. Load harness + CI smoke test
**Why:** A4's cap number and E1's mitigations are unprovable by hand, and the
"stale server binary" class of bug has bitten this project before.

**Scope:** a headless `server/loadtest.cpp` (WS, reuses `wire.h`) that opens N
connections, creates/joins M matches, sends input at 60 Hz, and reports per-match
tick times and packet loss. Add a CI job to `build.yml`: boot the server, create 3
matches, assert state packets flow and the protocol tags match.

**Files:** new `server/loadtest.cpp`, `server/Makefile`,
`.github/workflows/build.yml`.

---

### E4. Docs refresh
**Scope:** `docs/deploy-vultr.md` (multi-match section, `/status`, capacity
numbers from A4, the key-vs-code distinction) and `docs/play-web-via-github.md`.

**Correct the "stateless server" claim.** `docs/deploy-vultr.md` asserts "reads
no files, writes no files" twice. Once `high-score` lands that is false — there is
a `scores` file to back up, and the redeploy steps must stop implying the box
holds nothing worth keeping.

**~~Write a fresh `docs/multiplayer-testing.md`~~ — DONE 2026-08-30.** Covers the
lobby/START flow, both transports and how they differ, the `PLATFORMZ_KEY` gate,
the scoreboard, and the trap that a bare `./platformz` connects to the *live*
server because `secrets.mk` bakes the host in. The pre-lobby version stays at
`docs/multiplayer-testing-archive.md` for its Emscripten build record.

**Still owed here:** a "two matches, four clients, one server" section, once
multi-match actually exists.

Also add a short `docs/matchmaking.md` describing the directory protocol as the
reference for both ends.

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
| B1 | #78 | Protocol: directory messages — `list`/`create`/`join`/`quick`/`leave` + `matchlist`/`joinfail` | protocol | A2 |
| B2 | #79 | Protocol: welcome carries the match code (`WELCOME_BIN_VERSION` 0x02→0x0A) + server-wide epoch | protocol | B1 |
| B3 | #80 | Protocol: move map size into `MatchOptions` so the lobby shows it before start | protocol | B2 |
| C1 | #81 | Client: lift the game shell out of `main()` into `screens.h` (no behaviour change) | client | — |
| C2 | #82 | Client: `BROWSE` screen — match list, refresh, join | client | B1, C1 |
| C3 | #83 | Client: split `LOBBY` off the title screen, add LEAVE | client | C1, A3 |
| C4 | #84 | Client: QUICK MATCH and CREATE MATCH | client | C2, B1 |
| C5 | #85 | Client: invite links (`?match=`) and join-by-code | client | C2 |
| D1 | #86 | Client: persistent local profile (name, `clientId`, `token`, volume, options) | client | — |
| D3 | #97 | Server-issued identity token (stateless HMAC; unblocks leaderboards later) | server, security | D1 |
| D2 | #87 | Reconnect into your own slot (use the existing 15 s grace) | server, client | D1, D3, A3 |
| D4 | #98 | Re-key the scoreboard onto the identity token (display names collide today) | server, security | D3 |
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
