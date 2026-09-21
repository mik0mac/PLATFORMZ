# Multiplayer testing — local & LAN

How to run PLATFORMZ against a server on your own machine.

---

## Quick start — two players, one Mac

```bash
# 1. Build the server and the native client
make -C server          # -> server/gameserver
make                    # -> ./platformz

# 2. Start the server (leave it running in its own terminal)
cd server && ./gameserver
```

```bash
# 3. In two more terminals, launch two clients. The URL is REQUIRED - see the
#    warning below. Either transport works; mix them freely.
./platformz ws://localhost:9000
./platformz udp://localhost:9000
```

**4. Play.** Both clients land on the **title screen**, which offers four ways in:

| | Does |
|---|---|
| **QUICK MATCH** | drops you into the fullest official room that is still filling, or makes one. One click, no decisions |
| **FIND A MATCH** | the browser: every public room, its map, phase and 3/8 count. JOIN one, or CREATE / enter a code |
| **CUSTOM MATCH** | make a room you run — you own the options and the START button |
| **LOCAL MATCH** | offline, against bots. No server involved |

The quickest two-player test is **QUICK MATCH on both clients** — they land in
the same official room, which starts itself once two humans are in it.

To test the *host* path instead, press CUSTOM MATCH on one client and JOIN its
room from the other. In a custom room the **creator** is the host: only they see
OPTIONS and START, and it migrates if they leave. A 5-second countdown runs, then
the match begins.

> ### ⚠️ Always pass the URL when testing locally
> `./platformz` with **no arguments** connects to **`platformz.space`** — the live
> public server — because `secrets.mk` bakes that host into the build. It does
> *not* default to localhost. Pass `ws://localhost:9000` or `udp://localhost:9000`
> explicitly, every time.
>
> `./platformz local` forces offline single-player against no server at all.

### Confirming it worked

The server prints this on startup:

```
PLATFORMZ server | port 9000 (TCP/WebSocket + UDP) | 60 Hz
Protocol: state tag 0x09, welcome tag 0x0a | qpos +/-2400 | qvel +/-700
Join key: none (open server; set PLATFORMZ_KEY to require one)
Match caps: 12 rooms, 12 live (no live cap; set PLATFORMZ_MAX_ACTIVE to add one), 3 per address
UDP handshake cookie: ON (HMAC-SHA256, 30s buckets)
WARNING: PLATFORMZ_IDENTITY_SECRET unset - using a random per-boot secret (…)
Scoreboard: 0 names from scores
Match registry: default room 7CGD (cap 12)
GameSpace: lobby ready, 8 player slots (waiting for a player to start)
Match registry: official room A6SS preset=DEFAULT (locked, auto-starts at 2 players)
```

The `PLATFORMZ_IDENTITY_SECRET` warning is **expected locally** — it only matters
on a deployed server, where a per-boot secret means handshake cookies do not
survive a restart. See the deploy doc.

Two rooms exist at boot: a **default** room (where a connection with no opinion
lands) and an **official** one (locked preset, starts itself at two players).

Then a heartbeat once per second:

```
tick 60  matches 2 (0 active)  players 0  worst 0.01ms
```

`matches` counts rooms, `(N active)` counts the ones actually in countdown or
playing, and `worst` is the slowest tick in that second across **all** of them.
Every ten seconds it also lists each room:

```
    7CGD  lobby    slots 2/8  asteroids 0
    A6SS  playing  slots 4/8  asteroids 18
```

`asteroids 0` in a lobby is correct — **a room has no world until its match
starts.**

---

## Transports

The client picks its transport from the URL **scheme**. The server speaks both at
once on the same port, so a UDP client and a browser client share one match.

| URL | Transport | Payload | Notes |
|---|---|---|---|
| `ws://host:9000` | WebSocket / TCP | JSON | Also what the browser uses |
| `udp://host:9000` | Raw UDP | Quantized binary (`netbin.h`) | Native only — browsers can't open raw UDP |

They are **not** the same wire format. UDP carries the compact binary state so a
full 8-player tick fits one 1200-byte datagram; WebSocket has no MTU limit and
keeps the JSON. If you're changing the protocol, test both.

UDP has no disconnect event, so a client that quits just goes quiet. The server
frees its slot after **10 s** of silence mid-match, or **3 s** in the lobby.
Quitting therefore shows up as `UDP player N timed out` a few seconds later, not
instantly.

---

## LAN — a second machine

Find the server machine's IP:

```bash
ipconfig getifaddr en0      # Wi-Fi; try en1 if blank
```

Then from the other Mac, replacing the address:

```bash
./platformz udp://192.168.4.21:9000
```

If it can't connect: different subnet/VLAN, or macOS firewall is blocking
`gameserver` (allow incoming connections for it).

---

## Browser client

Needs **two servers running at once**, in separate terminals. They do different
jobs:

| Terminal | Command | Port | Job |
|---|---|---|---|
| 1 — game server | `cd server && ./gameserver` | 9000 | Runs the game |
| 2 — web server | `cd web && python3 -m http.server 8080` | 8080 | Just serves the files |

Then open **`http://localhost:8080/platformz.html`** and **click the canvas** —
pointer lock and audio both need a user gesture.

The page auto-connects to `ws://<whatever-host-you-loaded>:9000`, so:

- **Two players, one machine:** open the URL in two browser windows
- **Another device on the LAN:** `http://192.168.4.21:8080/platformz.html`
- **Point at a different server:** `…/platformz.html?server=ws://192.168.4.21:9000`

Browser and native players share one match — that's the interesting test.

### Building the browser client

Only needed if you changed the client. One-time, build raylib for web:

```bash
git clone --depth 1 --branch 5.5 https://github.com/raysan5/raylib.git ~/raylib
export EMSDK_PYTHON=/opt/homebrew/opt/python@3.14/bin/python3.14
make -C ~/raylib/src PLATFORM=PLATFORM_WEB -B
```

Then, and after every client change:

```bash
export EMSDK_PYTHON=/opt/homebrew/opt/python@3.14/bin/python3.14
make web RAYLIB_WEB_DIR=$HOME/raylib      # -> web/platformz.{html,js,wasm,data}
```

> **The `EMSDK_PYTHON` gotcha.** Homebrew's `emcc` picks `$EMSDK_PYTHON`, else the
> first `python3` on `PATH`, and fails its `>=3.10` assert if that's too old. An
> activated virtualenv can shadow `python3` with 3.9 *and* leave `EMSDK_PYTHON`
> unset. Export it for every `emcc`/`make web`/raylib-web command.
>
> **Serving** is unaffected — `python3 -m http.server` runs on any Python.

`shell.html` is baked in at compile time, so re-run `make web` after editing it.

---

## Rooms, lobbies, and who the host is

The title screen is a **router**, not the lobby. Picking any of the three online
destinations puts you in a **room**, and that room has its own lobby.

- **Host** = **the player who created the room**. Only they see OPTIONS and START;
  it migrates to the lowest remaining slot if they leave.
- **Official rooms have no host at all.** Their options are locked (the preset is
  the point) and they start themselves once two humans are present — so START and
  OPTIONS are absent for everyone, not just for you.
- **OPTIONS** are match-wide and sync live, so you can watch a slider move in the
  other window. **Map size lives in OPTIONS**, not on the START button, so the
  lobby shows everyone which arena is coming before anyone presses anything.
- **LEAVE** puts you back where you came from, so you can hop rooms without
  restarting the client.
- **Joining mid-match** works when the roster has a free slot — set NUMBER OF
  PLAYERS above the humans present and a latecomer takes a bot's slot.
- **`M` ends the match**, host only.

### Invite links and codes

A room's 4-character code is its invite. The lobby shows it and has **COPY
INVITE** — in a browser that copies the whole link, key and all; natively it
copies the code, since there is no link to hand out.

```bash
./platformz udp://localhost:9000 --match 7QK2      # native, straight into a room
```
```
http://localhost:8080/platformz.html?match=7QK2    # browser
```

A **private** room is hidden from FIND A MATCH entirely, so its code is the only
way in — which is exactly what makes the code worth something. An empty room is
destroyed after 30 s (60 s if it was mid-match), so a stale code comes back as a
refusal on the browse screen, not a hang.

---

## Two matches, four clients, one server

The thing multi-match actually has to survive: rooms that neither know nor affect
each other, ticked on the same 60 Hz beat.

```bash
cd server && ./gameserver           # one server, as always
```

Four clients in four terminals. Two make rooms, two join them:

```bash
./platformz udp://localhost:9000    # A - CUSTOM MATCH, note the code (say 7QK2)
./platformz ws://localhost:9000     # B - FIND A MATCH, JOIN 7QK2
./platformz udp://localhost:9000    # C - CUSTOM MATCH, note the code (say M4XD)
./platformz ws://localhost:9000     # D - FIND A MATCH, JOIN M4XD
```

Now start **only** A's match and watch the server:

```
    7QK2  countdown  slots 2/8  asteroids 0
    M4XD  lobby      slots 2/8  asteroids 0
```

What to look for, in rough order of what has actually broken before:

- **C and D stay in their lobby.** A start is a room's own business; if both rooms
  go to countdown, routing is broken.
- **A and B see the same countdown, C and D see none.** The state packet carries
  the phase per room.
- **`matches 2 (1 active)`** in the heartbeat — one room playing, one not.
- **Start M4XD too.** Both play at once; `worst` should stay well under a
  millisecond on a dev machine. This is the case the sim loop exists for.
- **Mix transports inside one room** (A on UDP, B on WebSocket). They share a
  world and neither can tell.
- **Have B LEAVE mid-match.** A's match carries on; B lands back on the browser
  and can join M4XD instead.

Driving four GUI clients by hand gets old. `server/test/probe_multimatch.py` does
the same thing headlessly in about twenty seconds, and the harness below does it
at ten rooms.

---

## Starting from a clean slate

The client remembers your name, volume, the rules you last played with, an
install id, the server-issued identity token, and the local high-score board —
so the second launch never shows you what the first one does. To test defaults
as a new player meets them:

```bash
scripts/reset-prefs.sh --list            # what's stored now; changes nothing
scripts/reset-prefs.sh                   # back up and clear the lot
scripts/reset-prefs.sh --keep-identity   # reset the RULES, keep who you are
scripts/reset-prefs.sh --restore         # put the newest backup back
```

Two things worth knowing before you reach for it:

**It refuses while a client is running**, and that guard is the point. The
client samples its live values into the profile every frame and writes them at
most every two seconds (`profile::Autosave`), plus once more on exit — so files
cleared under a running game are rewritten from memory seconds later, and you
end up testing the very preferences you thought you had just cleared.
`--force` overrides it for a wedged process.

**A full reset costs you your online leaderboard rows.** `profile.json` carries
the D3 identity token, which is how the server knows a run was yours. Clear it
and the rows stay on the board under an identity you no longer hold — you can't
beat your own score, because the server no longer believes it was you. Use
`--keep-identity` when you only want the rules back at their defaults. Nothing
is ever deleted outright; every file is copied to a timestamped `.bak-` first.

The **web** build keeps the same two blobs in `localStorage`, which no shell
script can reach — the script prints the console one-liners. They're per origin
and per browser, so clearing Chrome leaves Safari's copy alone.

## Testing without a GUI

Everything below runs against a plain `./gameserver` and needs no client.

```bash
./server/test/run_all.sh        # standalone C++ tests. Seconds, no server needed
./server/test/run_probes.sh     # live protocol probes, each on a fresh server
./server/test/ci_smoke.sh       # protocol tags + WebSocket + a two-match load run
```

The last two **build what they are about to test** — `run_probes.sh` the server,
`ci_smoke.sh` the server and the load harness — so you cannot accidentally test
the previous build. They used to only check the binary existed, and a suite that
passes against a binary you did not just compile is worse than one that fails,
because you believe it. `make` is incremental, so this costs nothing when nothing
changed; `PLATFORMZ_NO_BUILD=1` skips it if the binary came from somewhere else.

**The probes** (`server/test/probe*.py`) are headless protocol clients, one per
question — the lobby and host rules, the directory, reconnecting into your own
slot, join-in-progress, the handshake cookie, the capacity budgets. Two cover the
sliderless roster rules: `probe_maxbots.py` (a `maxBots = 0` room fields no bots,
leaves its other slots genuinely empty, and — the regression that matters — keeps
playing instead of ending on its first tick) and `probe_minhumans.py` (an official
room arms on its own preset's head count, not the compile-time one; it waits out a
countdown that must *not* fire, so it takes ~30 s). Each gets its
own fresh server, because several leave state behind that would fail the next one
for the wrong reason. Run one on its own while poking at the server:

```bash
cd server && ./gameserver &
python3 server/test/probe_capacity.py
```

**The load harness** (`server/loadtest.cpp`) is the only way to see many rooms
under real load:

```bash
make -C server loadtest
server/loadtest --matches 10 --clients 8 --seconds 30
server/loadtest --mode ws-smoke          # one WebSocket client, end to end
```

It reports delivered state rate, worst inter-packet gap, and egress per match —
and the rate the *generator* itself achieved, because if the harness could not
keep 60 Hz then none of the other numbers are about the server. **Run it from a
different machine than the server** whenever the numbers matter; on one box you
are mostly measuring the two competing. Tick costs come from the server's own
`PLATFORMZ_PERF=1` output, not from the harness.

Creating ten rooms from one address trips the per-address budget, so:

```bash
PLATFORMZ_MAX_ROOMS_PER_ADDR=0 ./gameserver
```

**Under ThreadSanitizer**, when a hang or a corrupted-looking state smells like a
race:

```bash
make -C server tsan
PLATFORMZ_SERVER_BIN=gameserver-tsan ./server/test/run_probes.sh
```

All three run in CI on every push (`.github/workflows/build.yml`), so a red tick
on GitHub usually means one of these commands reproduces it locally.

---

## Scoreboard

The all-time score table is **owned, credited and persisted by the server** — the
client only renders what it's sent. Open it from the title screen's
**LEADERBOARD** button (networked play only).

Scores are credited once per match, at match end, then written to disk.

```bash
# Default: a file named "scores" next to the working directory
cd server && ./gameserver          # -> server/scores

# Override the path
PLATFORMZ_SCORES=/tmp/test-scores ./gameserver
```

`server/scores` and `server/scores.tmp` are gitignored. Delete the file to reset
the board. Saves are atomic (write `.tmp`, then rename), so an interrupted write
can't corrupt it.

> **Known limitation:** the table is keyed on **display name**, so two players
> typing the same name share one row, and anyone can claim another player's row by
> typing their name. Being fixed — see D4 in `docs/matchmaking-plan.md`.

---

## Join key

Setting `PLATFORMZ_KEY` closes the server to anyone without it. A wrong or missing
key gets **no reply at all** — to a scanner the port looks dead.

```bash
PLATFORMZ_KEY=test123 ./gameserver
```

Clients pass it in the URL query, on either transport:

```bash
./platformz "udp://localhost:9000?key=test123"
```

Browser: `http://localhost:8080/platformz.html?key=test123`

Startup logs `Join key: REQUIRED` when it's set. Leave it unset for local testing.

A wrong key gets **no reply at all** — not even the UDP handshake challenge. If a
client sits on "CONNECTING TO SERVER…" against a server that is definitely up,
the key is the first thing to check.

### The other secret

`PLATFORMZ_IDENTITY_SECRET` is unrelated and is **never shared with players**. It
is the key behind the UDP handshake cookie (and, later, the identity token). Unset
it and the server mints a random one per boot, which is fine locally — the boot
warning is expected — and not fine on a deployed server, where it means every
cookie in flight dies on restart. See
[`deploy-vultr.md`](deploy-vultr.md#the-identity-secret).

---

## Controls

WASD move · mouse look · left-click fire rocket · Space jetpack (up) · hold Left
Shift for stronger (earth) gravity · **M** end match (host only) · Esc toggle
cursor capture · F3 perf overlay · `+`/`-` volume.

---

## Other launch modes

```bash
./platformz local                    # offline single-player, no server
./platformz bench 240 256 24 4       # perf run: halfSize platforms asteroids [players]
                                     # skips the title, spawns bots, F3 overlay on
./platformz udp://localhost:9000 --match 7QK2    # straight into a room, skipping the browser
```

`--match` is the native half of an invite link, and it is the fastest way to put
a second client in a specific room without clicking through FIND A MATCH.

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| Client connects to the wrong server | You ran `./platformz` with no URL — it uses the baked-in `platformz.space`. Pass `ws://localhost:9000`. |
| `bind: Address already in use` | Something already holds 9000. `lsof -nP -iTCP:9000 -sTCP:LISTEN` — often a `gameserver` you left running in a closed terminal. |
| Browser stuck on "CONNECTING TO SERVER…" | No game server on 9000, or wrong host. Confirm with `lsof -nP -iTCP:9000 -sTCP:LISTEN`. Both terminals must be up. |
| "SERVER VERSION MISMATCH" | Client and server disagree on the protocol tags. Compare the server's startup `Protocol:` line against `netbin.h` and rebuild both. |
| Nothing happens after connecting | You're in a room's lobby. Someone has to press START — and only the room's **creator** sees it. In an *official* room nobody does: it starts itself at two humans. |
| `asteroids 0` in the heartbeat | Correct before START. A room has no world until its match begins. |
| "NO FREE SLOT - WAITING FOR ONE TO OPEN..." | Every slot is taken. You are still connected and can go back and pick another room — or wait, and your client takes the next seat that frees up. |
| UDP client never gets in, WebSocket does | It cannot echo the handshake cookie — almost always an **old binary** against a new server. Rebuild the client. |
| "TOO MANY ATTEMPTS - WAIT A MOMENT" | You spent a rate limit: five wrong room codes a minute, or five room moves in quick succession. Both clear within a minute. |
| Rooms refuse to be created, `server_full` | Either 12 rooms already exist, or you've minted 3 from this address. `PLATFORMZ_MAX_ROOMS_PER_ADDR=0` for testing. |
| Everything looks right but `matches` never grows | You created rooms with a client that then quit — an empty room is reaped 30 s later. |
| `make web` fails with `python 3.10 or above` | `EMSDK_PYTHON` unset and a 3.9 venv is shadowing `python3`. Export it, or `deactivate` first. |
| Browser blank / 404 on `.wasm` | You opened `file://` or served the wrong directory. Serve from `web/`. |
| Other machine can't reach the server | Different subnet, or macOS firewall is blocking `gameserver`. |
| Mouse won't capture in Safari | Click the canvas. Over plain-HTTP LAN, Safari may refuse pointer lock — try localhost. |

---

## See also

- `docs/matchmaking.md` — the directory protocol: rooms, codes, every message, every limit.
- `docs/deploy-vultr.md` — running the server on a public VPS.
- `docs/perf-measurements.md` — what the server actually costs, measured.
- `docs/matchmaking-plan.md` — where multi-match hosting and the match browser are going.
- `docs/multiplayer-testing-archive.md` — the pre-lobby version of this doc.
- `server/test_client.html` — poke the protocol from a browser console without a client.
