# Platform ports: Windows, the browsers, Steam (for LLM agents)

Written for a model working in this repo, not for a person following steps. It
records **what had to change, why, and what must stay true** for the three ports
beyond the macOS dev build:
1. the native Windows client;
2. the web client in Safari and Chrome;
3. the Steam edition.

Each section ends with its invariants, the rules a later change must not break.

Human-facing step lists live elsewhere, and this doc doesn't repeat them:
- `docs/build-steam-windows.md`: building on a Windows machine;
- `docs/package-steam.md`: Steam commands;
- `docs/package-mac.md` and `docs/deploy-vultr.md`: the Mac handout;
- `docs/multiplayer-testing.md`: serving the web client locally.

Issue numbers: F1 #92 (CMake), F2 #93 (Windows client), F3 #94 (Windows
packaging), F4 #95 (Steam), #24 (fullscreen), #192–#196 (raylib 6.2).

---

## 0. Shared ground: raylib 6.2 everywhere

The game is built against **raylib 6.2**. Four separate copies of raylib exist,
and they must be the same version:

| Copy | Used by | Where it comes from |
|---|---|---|
| Homebrew `/opt/homebrew` | `make` (dev), CMake on a Mac | `brew install raylib` |
| `~/raylib-macos13` | `make app` / `dist-pack` / `steam-depot` (Mac handouts) | built from source with `-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0` (`docs/deploy-vultr.md`) |
| `~/raylib` | `make web` | `make -C ~/raylib/src PLATFORM=PLATFORM_WEB` → `libraylib.web.a` |
| CMake FetchContent | Windows, CI | `CMakeLists.txt`: the 6.2 tarball, SHA256-checked |

How a mismatch is caught:
- `main.cpp` opens with `static_assert(RAYLIB_VERSION_MAJOR == 6 && RAYLIB_VERSION_MINOR == 2)`.
- CMake uses `find_package(raylib 6.2 EXACT)`, because raylib's version file
  otherwise accepts any newer raylib.

**Include order matters.** `/opt/homebrew/include` holds Homebrew's `raylib.h`
whether or not Homebrew's raylib is the one being linked. Any build that links
another copy must put that copy's headers **first**:
- the handout: `RAYLIB_INC=$(RAYLIB_STATIC_INC)` in the `app` recipe;
- web: `-I$(RAYLIB_WEB_INC)` before `-I$(JSON_INC)`.

The static_assert found both of these the day it was added.

raylib 6 changes that touched this code:

| Change | Where handled |
|---|---|
| `SetSoundPan` range is -1 (left) .. +1 (right), was 0..1 | `audio.h` `trigger()`: `pan = Clamp(side, -1, 1)`, default `0.0f` |
| The web library is `libraylib.web.a` | `Makefile` `RAYLIB_WEB_LIB`; CI clones `--branch 6.2` with cache key `raylib-web-6.2-…` |
| Static GLFW needs QuartzCore (`CAMetalLayer`) | `Makefile` `app`: `RAYLIB_LINK` adds `-framework QuartzCore` |
| Web pointer lock no longer sets `IsCursorHidden()` | `display::IsMouseCaptured()`; see §2 |
| `ChangeDirectory` returns `int`, 0 = success (was `bool`, true = success) | `main.cpp` ignores the result; never write `if (ChangeDirectory(…))` |
| `SetMousePosition` doesn't update the previous position; leaving the window sets the mouse to (0,0) | checked on Mac and Windows: no visible effect |
| Windows borderless fullscreen rewritten | checked on Windows (#195): fine |

Unchanged in 6.2, checked:
- the raymath functions the game uses (the server's shims depend on them);
- the shader attribute and uniform names;
- `GetWindowHandle()`, which still returns the NSWindow;
- miniaudio still needs `-sEXPORTED_RUNTIME_METHODS=HEAPF32` on the web.

---

## 1. The Windows client

Windows builds **only through CMake** (`cmake --preset client`, or `steam`). The
Makefiles are macOS-only. No Windows machine runs in CI: the `CMake Windows`
job is the automatic compile, and humans test on a real machine with
`docs/build-steam-windows.md`.

### What had to change, and why

1. **`<windows.h>` collides with `raylib.h`.** Both declare `Rectangle`,
   `CloseWindow`, `DrawText`, `ShowCursor` and `PlaySound`. IXWebSocket's
   headers pull in `<windows.h>` on Windows. The fix was structural: the
   desktop transports (WebSocket via IXWebSocket, raw UDP) moved out of
   `net_client.h` into **`net_native.cpp`**, a translation unit that never
   includes `raylib.h`. `net_client.h` keeps only the `ITransport` interface,
   the `MakeTransport()` factory, `NetClient`, and the emscripten backend.
   `steam_api.h` collides in the same way, which is why `platform_steam.cpp` is
   separate too (§3).
2. **Winsock in place of POSIX sockets**, in `net_native.cpp`:
   - `SOCKET` / `INVALID_SOCKET`, `closesocket`, and `ioctlsocket(FIONBIO)` for
     non-blocking sockets;
   - `WSAStartup` via `ix::initNetSystem()`;
   - `ws2_32` is linked by IXWebSocket's CMake target;
   - a socket that can't be made non-blocking is now an error, not a frozen frame.
3. **TLS:** IXWebSocket uses **mbedTLS 3.6.2** on Windows. It is fetched and
   hash-checked when no copy is installed; macOS uses Secure Transport.
4. **Atomic file replace:** `profile.h` uses `std::filesystem::rename`, because
   the CRT's `rename` refuses to overwrite an existing file. Without the fix,
   every profile save after the first fails.
5. **No console window:** Release builds link as a GUI app; Debug keeps the
   console. As a result the Release exe can't show its own error messages.
6. **Self-contained exe:** `CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded…"`
   (`/MT`) links the C++ runtime statically, for every target including fetched
   dependencies. The exe imports only Windows system DLLs.
7. **Version resource:** `packaging/windows/platformz.rc.in` is stamped from
   `project(VERSION …)` in `CMakeLists.txt`. Keep it in step with `APP_VERSION`
   in the `Makefile`.
8. **The baked-in server:** CMake reads `secrets.mk`'s `EXTRA_CXXFLAGS +=`
   lines and applies them to the client. Only that one line shape is understood.

### Packaging (`packaging/windows/pack.ps1`)

Like `dist-pack`, the script checks what was built before it zips:
- the version resource is present;
- the exe imports no DLL outside Windows;
- if signing is configured, the signature is valid and timestamped.

There is **no certificate yet**, so the output is always
`PLATFORMZ-UNSIGNED-windows-x64.zip`. Signing plugs in through one variable,
`PLATFORMZ_SIGNTOOL_ARGS`. `-Steam` stages the Steam depot folder instead (§3).

### Snags from the first real Windows build

Each of these is now in `docs/build-steam-windows.md`.

| Symptom | Cause | Fix |
|---|---|---|
| PowerShell says `git` is not recognized | The window was open during the install, so it never saw the new program locations | Reopen PowerShell (or the whole terminal app) |
| CMake can't find the SDK | Unzipping on Windows added an extra folder level | `Test-Path C:\dev\steamworks_sdk\public\steam\steam_api.h` must be True |
| The game says "OFFLINE - START WITH A SERVER URL" | `secrets.mk` was missing at configure time, or saved as `secrets.mk.txt` (Windows hides extensions) | Put it in place, re-run `cmake --preset …`, and look for `secrets.mk: 2 client flag(s) baked in` |
| "Could NOT find Python3" | A configure-time probe in a dependency, most likely mbedTLS | Harmless |
| Stuck on "CONNECTING TO SERVER..." | Windows Firewall | Allow `platformz.exe` (Private and Public) |
| `pack.ps1 -Steam` reports "unsigned" | No certificate | Expected; Steam doesn't need a signature |

### Invariants
- Never include `raylib.h`, or a header that includes it, in `net_native.cpp`
  or `platform_steam.cpp`. Never include a socket or Steam header anywhere else
  in the client.
- A new `.cpp` goes in **both** `SRCS` (Makefile) and `PLATFORMZ_CLIENT_SOURCES`
  (CMakeLists).
- CMake refuses Emscripten, because it bakes in `secrets.mk` and the web bundle
  is public.
- Never size anything off `GetScreenWidth()/Height()`, which is the window. Use
  `display::CANVAS_W/H` (1000x700).

---

## 2. The web client in Safari and Chrome

Build: `make web RAYLIB_WEB_DIR=$HOME/raylib` → `web/platformz.{html,js,wasm,data}`.
Set `EMSDK_PYTHON` to a Python ≥ 3.10 (`rebuild-all.sh` does this). The page
template is `shell.html`, baked in at compile time, so rebuild after editing it.
**The committed `web/` bundle is what ships:** the server box serves it straight
from the checkout. Commit the rebuild as its own commit.

### Rendering
- The web raylib is OpenGL ES2 / **WebGL1**. Every shader needs a GLSL ES 100
  variant under `#if defined(__EMSCRIPTEN__)`:
  - `#version 100` and `precision mediump float;`
  - `varying`, `gl_FragColor` and `texture2D()`.
- Guard shader use with `IsShaderValid(s) && s.id != rlGetShaderIdDefault()`.
  An invalid program breaks every draw inside the `BeginShaderMode` block.
  `main.cpp`'s grayscale shader shows the pattern.

### Audio
- `-sEXPORTED_RUNTIME_METHODS=HEAPF32` is required. miniaudio reads
  `Module.HEAPF32.buffer`, which current emscripten no longer attaches by
  default. Without it the audio callback throws on every frame and the game
  is silent. This was still true on raylib 6.2.
- Browsers start the AudioContext suspended. `shell.html` resumes it on the
  first gesture, **key or click**: miniaudio's own auto-unlock only listens for
  click and touch, and the title screen can be started with a key.

### Mouse capture (pointer lock)
Game controls are gated on the mouse being captured. Four mechanisms work
together:

1. **Safari ignores raylib's deferred lock request.** raylib's `DisableCursor()`
   uses emscripten's deferred `requestPointerLock`. Chrome honours it on the
   next click; Safari doesn't reliably. `shell.html` therefore requests the lock
   **synchronously inside the canvas `mousedown` handler**, which both
   browsers honour.
2. **Menus must not take the lock.** Each frame `main.cpp` calls
   `PlatformzSetUiOwnsMouse(!playing || modalOpen)`, and `shell.html` skips the
   lock while `Module.uiOwnsMouse` is set. "Playing" is PLAYING or COUNTDOWN.
   The flag is stated as "not playing" rather than as a list of menu screens,
   so a new screen is covered automatically.
3. **raylib 6 broke `IsCursorHidden()` on the web.** raylib 5.5's web build set
   `cursorHidden` from the `pointerlockchange` event. raylib 6.2 sets a separate
   `cursorLocked` flag, which has no public getter, so `IsCursorHidden()` stays
   false while the pointer is locked. Symptom: the match starts and no key or
   mouse moves the player. Fix: **`display::IsMouseCaptured()`** (`display.h`).
   - On the web it asks the browser, via
     `EM_JS PlatformzPointerLocked`, whether `document.pointerLockElement` is set.
   - On the desktop it returns `IsCursorHidden()`, since 6.2's `DisableCursor()`
     there still sets both flags.

   Both input gates in `main.cpp` (online and local) use it.
4. **A browser locks the pointer only right after a click or key press.** The
   `DisableCursor()` at the end of the countdown comes seconds after the last
   click, so the lock waits for the next click. This was true on 5.5 as well.
   - After `WEB_CLICK_TO_PLAY_DELAY` (2 s, `constants.h`) of PLAYING without
     capture, `main.cpp` draws **"CLICK TO PLAY"** (`MARK: CLICK TO PLAY`,
     web only).
   - The timer (`uncapturedFor`) doesn't run while paused or while a modal is
     open.
   - When `IsMouseCaptured()` changes from false to true (`wasCaptured`), the
     game sets `consumeLookFrames` and `consumeFirstFire`. The lock arrives a
     moment after its click, so without this the click that takes the lock would
     fire a rocket and the capture would jerk the view.

### Fullscreen (#24)
- A page may enter fullscreen only inside the key's own event handler, so
  **`shell.html` owns the F key**. The game says each frame whether F is free
  (not while a text field has focus), via `display::SetFullscreenKeyEnabled`
  → `Module.fullscreenKeyOk`. Esc, or the browser itself, leaves fullscreen.
- `shell.html` fullscreens the `#stage` wrapper, **not the canvas**. A
  fullscreened canvas is letterboxed with `object-fit`, while emscripten maps
  mouse positions over the element's whole box, so every click would land off
  target. The canvas's backing store stays 1000x700, and CSS scales the element.
- **Safari cut off the top and bottom edges.** The canvas was sized with
  `min(100vw, 100vh*10/7)`, but Safari's fullscreen area can be shorter than
  `100vh`: on a MacBook it leaves the camera notch's strip black. The canvas
  came out too tall and lost its edges, and the messages near the bottom were
  cut off. Fix: the stage gets `container-type: size`, and the canvas is sized
  with `min(100cqw, calc(100cqh*10/7))` × `min(100cqh, calc(100cqw*7/10))`.
  The `vw/vh` declaration stays first as the fallback for browsers without
  container units (Safari 16+ has them).
- The native fullscreen is different (`display.h`):
  - macOS uses the NSWindow's own `toggleFullScreen:`, because raylib's
    `ToggleFullscreen` changes a Retina display's resolution;
  - Windows uses `ToggleBorderlessWindowed()`;
  - both draw letterboxed through a 1000x700 render texture, with raylib's mouse
    offset and scale set so that `GetMousePosition()` stays in canvas pixels.

### Testing the web build locally
Run two servers: `cd server && ./gameserver` (port 9000) and
`cd web && python3 -m http.server 8080`. Then open
`http://localhost:8080/platformz.html`. **Hard-reload (Cmd+Shift+R)** after
every rebuild, or the browser keeps its cached wasm. Check both Safari and
Chrome:
- pointer lock, and "CLICK TO PLAY";
- F and Esc;
- sound starting after the first key or click;
- typing and pasting in the name field;
- the shaders.

### Invariants
- Gate game input on `display::IsMouseCaptured()`, **never `IsCursorHidden()`**.
- The web build never sees `secrets.mk`. `make web` keeps `EXTRA_CXXFLAGS` out
  of it, and CMake refuses Emscripten.
- Keep `HEAPF32` exported, and keep `shell.html`'s AudioContext resume.
- Keep the fullscreen element a wrapper sized in container units, not the canvas.
- CI's `check-web-bundle-fresh.sh` fails if `netbin.h` or `wire.h` changed after
  the committed wasm was built. After a protocol change, rebuild the web bundle.

---

## 3. The Steam edition

### Shape: two builds from one source tree
- **`platform.h`** is the storefront layer: `Init`, `Update`, `Shutdown`,
  `SetJoinableRoom` and `TakeJoinRequest`. Every build except Steam gets
  inline stand-ins that do nothing and link nothing of Valve's.
- **`platform_steam.cpp`** holds the Steamworks implementation. It is compiled
  only with `PLATFORMZ_STEAM=1`, as its own translation unit, because
  `steam_api.h` collides with `raylib.h` the way `<windows.h>` does. It:
  - calls `SteamAPI_InitEx`, and if that fails, logs a readable reason and plays
    on without Steam;
  - calls `RunCallbacks` every frame;
  - publishes the current room as rich presence `connect = "--match CODE"`;
  - receives `GameRichPresenceJoinRequested_t` when a friend clicks "Join game".
  - `SteamAPI_RestartAppIfNecessary` is deliberately not called before release.
- **Steam is used only for what only Steam can do:** the overlay, and friends
  joining friends. Matchmaking, rooms, scores and networking stay on our own
  server, so browser and Steam players share one world.
- **An invite is the existing command-line flag.** If the game isn't running,
  Steam launches it with `--match CODE` appended, and `main.cpp` parses it as
  usual. If the game is running, `TakeJoinRequest()` hands over the same
  string, and `ParseJoinString()` reads it strictly: letters and digits, at
  most 8. Tests: `test/platform_test.cpp`.

### The SDK is never in the repo
The repo is public, and Valve's terms forbid republishing the SDK. It is
unzipped **beside** the checkout as `../steamworks_sdk`. Either the `sdk/`
folder or the zip's top folder works. CI never builds the Steam edition.

### Development build (CMake `steam` preset)
- `cmake --preset steam && cmake --build --preset steam` → `build/cmake-steam/`.
- `libsteam_api.dylib` or `steam_api64.dll` is copied next to the binary. So is
  **`steam_appid.txt`**, containing 480: Valve's Spacewar test app, used until
  PLATFORMZ has its own App ID. `-DPLATFORMZ_STEAM_APPID=` replaces it.
- Steam adds its overlay only to games **it** launched. On a Mac, "Add a
  Non-Steam Game" won't accept a bare binary, so the `steam-test-app` target
  builds `PLATFORMZ Steam Test.app` to add once.
- Verified: Steam shows "Spacewar"; Shift+Tab opens the overlay, on Mac and
  Windows; online play works.

### What ships: depots
Steam uploads **folders**, one per platform, under `dist/steam/`. Shipped
content is not the CMake build:
- **Mac:** `make steam-depot` runs the handout chain with `STEAM=1`:
  - raylib for macOS 13, `platform_steam.cpp`, and `libsteam_api.dylib` copied
    into `Contents/MacOS/`, which the binary links as `@loader_path`;
  - signing with **`packaging/steam/entitlements.plist`**, then notarization and
    stapling, into `dist/steam/mac/PLATFORMZ.app`;
  - then `check-depot.sh`.
- **Windows:** `pack.ps1 -Steam` on the Windows machine stages
  `dist\steam\windows\`: `platformz.exe`, `steam_api64.dll` and `assets\`. The
  folder is copied to the Mac's `dist/steam/windows/`.
- **The entitlements are load-bearing.** The hardened runtime's library
  validation refuses a dylib signed by a different Team ID, and
  `libsteam_api.dylib` is Valve-signed. Without
  `com.apple.security.cs.disable-library-validation` the game **does not
  start**. `allow-dyld-environment-variables` lets Steam inject its overlay.
  Valve's dylib keeps its own signature, and notarization accepts it.

### Upload
- `make steam-upload STEAM_USER=<login> [STEAM_BRANCH=beta] [STEAM_PREVIEW=1]`
  runs `packaging/steam/upload.sh`.
- `upload.sh` first runs **`packaging/steam/check-depot.sh`**, which refuses
  the depot when it finds:
  - anywhere: a `steam_appid.txt` or `.DS_Store`;
  - in the Mac depot:
    - anything other than `PLATFORMZ.app`;
    - a missing exe, dylib or assets;
    - no `@loader_path/libsteam_api.dylib` link, or a link to Homebrew;
    - a `minos` other than 13.0;
    - a codesign failure, or a missing entitlement;
    - Gatekeeper not reporting "Notarized Developer ID";
  - in the Windows depot: a missing `platformz.exe`, `steam_api64.dll` or
    `assets\`.
- `upload.sh` then writes an `app_build` VDF in a temp folder, for whichever
  platform folders exist, and runs SteamCMD.
  - The SDK's macOS SteamCMD is an Intel binary and runs under Rosetta.
  - Its launcher script breaks on paths with spaces, so it runs from a copy in
    `~/Library/Caches/PLATFORMZ/steamcmd`, made executable and unquarantined.
- Login is interactive: SteamCMD asks for the password and the Steam Guard
  code itself.
- `upload.sh` never sets the **default** branch live. That is done by hand on
  partner.steamgames.com.
- `STEAM_APP_ID`, `STEAM_DEPOT_MAC` and `STEAM_DEPOT_WIN` in the Makefile are
  empty until the store entry exists. They aren't secrets; commit them.

### Invariants
- The Steamworks SDK never enters the repo, and the Steam edition never builds
  in CI.
- `steam_appid.txt` never ships in a depot. `check-depot.sh` enforces this, and
  `pack.ps1 -Steam` fails on it too.
- Nothing outside `platform_steam.cpp` includes a Steamworks header, and
  `platform_steam.cpp` never includes `raylib.h`.
- Keep both entitlements in `packaging/steam/entitlements.plist`.
- A Steam make build writes to `build/steam/platformz` and `dist/steam/…`, never
  over the dev `./platformz` or the website zip.
- Notary credentials and Steam logins are typed in by a person, never scripted.
