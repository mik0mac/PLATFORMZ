# Multiplayer testing — native LAN & browser LAN

How to build and test PLATFORMZ multiplayer. There is **one authoritative
server** and **two client builds** that connect to it over WebSocket:

- **Native** client (`platformz`) — the macOS desktop binary, connects via the
  vendored IXWebSocket backend.
- **Browser** client (`web/platformz.html`) — the Emscripten/WASM build, connects
  via the browser's JS WebSocket API.

Both speak the exact same JSON wire protocol; the server (`server/gameserver`)
owns the simulation and supports **2 player slots**. Clients send input and
render the state the server sends back. A single running server can mix native
and browser clients at the same time.

> The server listens on `0.0.0.0:9000`, so it accepts connections from other
> machines on the LAN, not just localhost.

---

## 0. One-time prerequisites

| Need | How |
| --- | --- |
| Native build deps | Homebrew `raylib` 5.5, `nlohmann-json`, `boost` (server). IXWebSocket is a vendored git submodule under `third_party/`. |
| Emscripten (browser build) | `brew install emscripten`. Must run under Python ≥3.10 — see [emscripten setup note](#emscripten-python-gotcha) below. |
| Web build of raylib | raylib 5.5 compiled for `PLATFORM_WEB`, checked out at `~/raylib` (see step 1c). |

### Emscripten Python gotcha
Homebrew's `emcc` (and `emrun`) pick their interpreter as `$EMSDK_PYTHON` →
else the first `python3` on `PATH`, and fail their `>=3.10` assert if that's too
old. An **activated virtualenv** (or a terminal opened before the `~/.zshrc`
edit) can shadow `python3` with Python 3.9 *and* leave `EMSDK_PYTHON` unset — for
example `EZL_Redux/.venv/bin/python3` is a symlink to Xcode's 3.9.6. So every
`emcc`/`make web`/raylib-web command must export this explicitly (it's already in
`~/.zshrc` for fresh interactive terminals):

```bash
export EMSDK_PYTHON=/opt/homebrew/opt/python@3.14/bin/python3.14
```

Serving the built page (step in §4) is **not** affected — use
`python3 -m http.server`, which runs on any Python and needs no Emscripten.

---

## 1. Build everything

### a. Server (required for all multiplayer)
```bash
make -C server          # -> server/gameserver
```

### b. Native client
```bash
make                    # -> ./platformz
```

### c. Browser client (only needed for the browser test)
First build raylib for web **once** (matches desktop raylib 5.5):
```bash
git clone --depth 1 --branch 5.5 https://github.com/raysan5/raylib.git ~/raylib
export EMSDK_PYTHON=/opt/homebrew/opt/python@3.14/bin/python3.14
make -C ~/raylib/src PLATFORM=PLATFORM_WEB -B      # -> ~/raylib/src/libraylib.a
```
Then build the WASM client:
```bash
export EMSDK_PYTHON=/opt/homebrew/opt/python@3.14/bin/python3.14
make web RAYLIB_WEB_DIR=$HOME/raylib               # -> web/platformz.{html,js,wasm,data}
```
The first `make web` also compiles Emscripten's system libs (one-time, cached).

---

## 2. Start the server

Run on the machine that will host the game:
```bash
./server/gameserver
```
Expected output (ticking at 60 Hz, players count rises as clients connect):
```
PLATFORMZ server | port 9000 | 60 Hz
GameSpace: 8 asteroids, 16 platforms, 2 player slots
tick 60  players 0  asteroids 8
```

Find this machine's LAN IP for the other player to connect to:
```bash
ipconfig getifaddr en0      # Wi-Fi, e.g. 192.168.4.21  (try en1 if blank)
```

---

## 3. Test: Native LAN

The native client takes the server URL as its first argument. **No argument =
local single-player** (hosts its own sim, no server needed).

**Same machine as the server (quick smoke test):**
```bash
./platformz ws://localhost:9000
```

**A second player on another Mac on the same LAN** (replace with the server's IP
from step 2):
```bash
./platformz ws://192.168.4.21:9000
```

Each client connects, gets a player slot from the server's welcome packet, and
renders the shared world. Watch the server log climb `players 0 → 1 → 2`.

> Two native clients on the **same** machine also works — just run `./platformz
> ws://localhost:9000` in two terminals (two windows, two slots).

---

## 4. Test: Browser LAN

The browser test needs **two servers running at the same time**, in two separate
terminals. They do different jobs — don't confuse them:

| Terminal | Command | Port | Job |
| --- | --- | --- | --- |
| **1 — game server** | `./server/gameserver` | `9000` | Runs the actual game; the browser talks to it over WebSocket. |
| **2 — web server** | `(cd web && python3 -m http.server 8080)` | `8080` | Just hands the browser the `web/` files (`.html`/`.wasm`/etc). |

> If only the web server (8080) is running, the page loads but you get a
> **WebSocket error** — because there's no game server on 9000 to connect to.
> Both must be up. (The web server needs any Python; `gameserver` is the binary
> from step 1a.)

`emrun` can replace the `http.server` line, but it's only worth it for its
auto-open/test features and it needs Python ≥3.10 (fails under an activated 3.9
venv unless `EMSDK_PYTHON` is exported) — `python3 -m http.server` is simpler.

### Then open the game in a browser
"Open the page" means: launch a web browser (Chrome/Safari/etc.) and type this
into the **address bar**, then Enter:

```
http://localhost:8080/platformz.html
```

That loads the WASM game from terminal 2; it then auto-connects its WebSocket to
the game server in terminal 1. The in-page default server URL is
`ws://<whatever-host-you-loaded>:9000`, so:

- **Same machine (two players):** open
  `http://localhost:8080/platformz.html` in **two** browser windows → both
  connect to `ws://localhost:9000`.
- **Another device on the LAN:** on that device, open
  `http://192.168.4.21:8080/platformz.html` → it auto-connects to
  `ws://192.168.4.21:9000` (replace with the server's IP from step 2).
- **Point at a different server host:** append a query string —
  `http://localhost:8080/platformz.html?server=ws://192.168.4.21:9000`

Click the black game canvas to capture the mouse (pointer lock) and start audio.
Two windows = two players; terminal 1's log should climb to `players 2`.

> Browser + native can share one server: e.g. one player in a browser window and
> one in `./platformz ws://localhost:9000` at the same time.

---

## 5. Controls
WASD move · mouse look · left-click fire rocket · Space jetpack (up) · hold the
earth-gravity key (LEFT_SUPER/⌘) for stronger gravity · Esc toggle cursor capture.

---

## 6. Troubleshooting

| Symptom | Fix |
| --- | --- |
| Browser stuck on "CONNECTING TO SERVER..." | Server not running, wrong host, or firewall. Confirm `lsof -nP -iTCP:9000 -sTCP:LISTEN` shows `gameserver`; check the `?server=` host matches the server's LAN IP. |
| `make web` / raylib-web / `emrun` fails with `python 3.10 or above` | `EMSDK_PYTHON` unset and an active 3.9 venv (or stale shell) is shadowing `python3` — see [the gotcha](#emscripten-python-gotcha). For **serving**, just use `python3 -m http.server` (no version requirement). For **builds**, `export EMSDK_PYTHON=/opt/homebrew/opt/python@3.14/bin/python3.14` or `deactivate` the venv first. |
| Browser shows blank / 404 for `.wasm` or `.data` | You opened `file://` or served the wrong dir. Serve from `web/` over HTTP and load `platformz.html` from that server. |
| Other machine can't reach the server | They're on a different subnet/VLAN, or macOS firewall is blocking `gameserver` (allow incoming connections for it). |
| Native client opens local single-player instead of joining | You ran `./platformz` with no URL arg. Pass `ws://host:9000`. |

## See also
- `net_client.h` — the dual-backend (IXWebSocket / Emscripten) client wrapper.
- `server/server_main.cpp` — authoritative server + wire protocol.
- `server/test_client.html` — standalone JS WebSocket client for poking the
  server's protocol without building a full client.
