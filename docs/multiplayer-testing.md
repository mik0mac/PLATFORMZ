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

**4. Play.** Both clients land on the title screen, which *is* the lobby. The
**host** — the lowest-numbered connected player, i.e. whoever joined first — picks
a map size and presses START. Everyone else waits; they'll see
*"Waiting for … to start the game."* A 5-second countdown runs, then the match
begins.

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
Protocol: state tag 0x09, welcome tag 0x02 | qpos +/-2400 | qvel +/-700
Join key: none (open server; set PLATFORMZ_KEY to require one)
[scoreboard] no file at scores - starting empty
Scoreboard: 0 names from scores
GameSpace: lobby ready, 8 player slots (waiting for a player to start)
```

Then a heartbeat once per second. Watch `players` climb as clients connect:

```
tick 60  players 2  asteroids 0
```

`asteroids 0` is correct before START — **the server boots with no world at all.**
It builds one only when the host starts a match.

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

## The lobby, and who the host is

The title screen doubles as the live lobby in networked play.

- **Host** = the lowest connected human slot. Only the host sees OPTIONS and the
  START buttons; it migrates automatically if that player leaves.
- **OPTIONS** are match-wide and host-only. They sync live to every client, so you
  can watch a slider move on the other window.
- **Map size** is chosen by *which* START button you press (SMALL / MEDIUM /
  LARGE / XL).
- **Joining mid-match** works when the roster has a free slot — set NUMBER OF
  PLAYERS above the number of humans present and the latecomer takes a bot's slot.
- **`M` ends the match**, host only.

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
```

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| Client connects to the wrong server | You ran `./platformz` with no URL — it uses the baked-in `platformz.space`. Pass `ws://localhost:9000`. |
| `bind: Address already in use` | Something already holds 9000. `lsof -nP -iTCP:9000 -sTCP:LISTEN` — often a `gameserver` you left running in a closed terminal. |
| Browser stuck on "CONNECTING TO SERVER…" | No game server on 9000, or wrong host. Confirm with `lsof -nP -iTCP:9000 -sTCP:LISTEN`. Both terminals must be up. |
| "SERVER VERSION MISMATCH" | Client and server disagree on the protocol tags. Compare the server's startup `Protocol:` line against `netbin.h` and rebuild both. |
| Nothing happens after connecting | You're in the lobby. Someone has to press START — and only the host sees the button. |
| `asteroids 0` in the heartbeat | Correct before START. The server has no world until a match begins. |
| `make web` fails with `python 3.10 or above` | `EMSDK_PYTHON` unset and a 3.9 venv is shadowing `python3`. Export it, or `deactivate` first. |
| Browser blank / 404 on `.wasm` | You opened `file://` or served the wrong directory. Serve from `web/`. |
| Other machine can't reach the server | Different subnet, or macOS firewall is blocking `gameserver`. |
| Mouse won't capture in Safari | Click the canvas. Over plain-HTTP LAN, Safari may refuse pointer lock — try localhost. |

---

## See also

- `docs/deploy-vultr.md` — running the server on a public VPS.
- `docs/matchmaking-plan.md` — where multi-match hosting and the match browser are going.
- `docs/multiplayer-testing-archive.md` — the pre-lobby version of this doc.
- `server/test_client.html` — poke the protocol from a browser console without a client.
