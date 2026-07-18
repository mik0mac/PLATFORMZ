# CLAUDE.md

Guidance for working in this repo. PLATFORMZ is a raylib-based 3D **vector**
first-person shooter written in C++17: fly around a bounded cube arena,
shoot drifting asteroids with rockets, dodge them, manage jetpack fuel.

## Build & run
- `make` — build the `platformz` binary.
- `make run` — build (if needed) and run.
- `make clean` — remove the binary.

Toolchain is **macOS + Homebrew raylib**:
`g++ -std=c++17 -I/opt/homebrew/include … -L/opt/homebrew/lib -lraylib
-framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo`.

- `SRCS := main.cpp collisions.cpp`; all `*.h` are dependencies (wildcard).
- Editing a header or an existing `.cpp` needs **no** Makefile change.
- Adding a **new** `.cpp` requires adding it to `SRCS` in the `Makefile`.

## IMPORTANT: Git workflow — commit to the current branch
**Always commit to whatever branch is currently checked out.** Never create a new
branch on your own initiative. If — and only if — you genuinely believe a change
should not go on the current branch, do **not** silently make one: stop and say so
overtly, addressing Mike by name and being blunt about it, e.g. _"Mike, this change
should really go on a new branch. You would be dumb to commit this to main right
now."_ Then wait for his go-ahead. No new branch without his explicit OK.

## IMPORTANT: IDE diagnostics are false positives
The clang/clangd language server isn't configured with `-I/opt/homebrew/include`,
so it reports things like `'raylib.h' file not found`, `Unknown type name
'Vector3'`, `Unknown type name 'Color'`, and "undeclared identifier" for any
`rl*`/raymath symbol. **These are not real errors.** Only the output of `make`
is authoritative for whether the code compiles.

## Architecture
Single-binary game. The main loop in `main.cpp` follows time → update → draw:
read input, mutate game state, then render — no state changes during draw.

Header-only class design (`#pragma once`); most logic lives in headers, with
only `main.cpp` and `collisions.cpp` as translation units.

- `main.cpp` — entry point and game loop. Input handling, gravity toggle, rocket
  firing, HUD text. Per frame: input → `player.updateVelocity/updateFuel` →
  (maybe) spawn rocket → `gameSpace.updatePositions(dt)` →
  `RunCollisionChecks(space, grid)` → `gameSpace.updateActiveObjects()` → draw.
- `elements.h` — the game-object classes: `Platform`, `Player`, `Asteroid`,
  `Rocket`, `Explosion`. Plain data + small per-object methods (`updatePos`,
  `takeDamage`, `update`, etc.). This is the **source of truth** for state.
- `gamespace.h` — `GameSpace` owns the `std::vector`s of every object type.
  `generate()` populates the level; `updatePositions(dt)` ticks everything;
  `updateActiveObjects()` erase-removes destroyed/finished objects; `draw()`
  renders; accessors return the vectors **by reference** for collision/systems
  to mutate. Boundary cube is centered on origin; default `halfSize = 60`
  (`GAMESPACE_HALF_SIZE`), overridden per match by the SMALL/MEDIUM/LARGE/XL
  `mapSizePresets` in `constants.h` (halfSize 90/120/240/360).
- `shapes.h` — all rendering. Low-level primitives (`DrawShadedWireBox`,
  `DrawShadedSphere`, `DrawGridRoom`) and one
  `Draw<Type>(const T&)` per element. No game state is mutated here.
- `collisions.h` / `collisions.cpp` — collision detection **and** response.
  Spatial-grid broad phase + narrow-phase geometry + game-rule reactions.
- `camera.h` — `CameraFromPlayer(player)` builds the first-person `Camera3D`
  from player state each frame (eye at the player-sphere center).
- `random.h` — `RandomFloat(min, max)` (seeded `std::mt19937`).
- `WireframeTests/` — **gitignored** scratch dir of prototypes (2D/3D wireframe
  experiments, Godot tests) that much of this code was pulled from. Not built.

## Collision system (collisions.cpp)
- `RunCollisionChecks()` runs once per frame (after positions update, before
  active-object cleanup): rebuild grid → rocket-vs-{asteroid,platform,wall,player}
  → apply explosion splash damage → asteroid-vs-player → player-vs-player →
  asteroid-vs-platform → player-vs-{platform,wall} → asteroid-vs-wall.
- **Broad phase:** `CollisionGrid` buckets only the *dynamic* objects
  (asteroids, rockets, players) into a 3D spatial hash, rebuilt each frame.
  Narrow phase only tests objects in the same cell or its 26 neighbors.
  Platforms are also bucketed, but since they're larger than a cell each one is
  inserted into **every cell its AABB overlaps** (not just its center cell), so a
  platform index appears in multiple cells — platform queries de-duplicate
  candidates via `CollisionGrid::GatherPlatformNeighbors`. Explosions are **not**
  bucketed (few active at once) — they're brute-forced; the `grid` param is kept
  for signature consistency where unused.
- **Narrow phase:** `SphereIntersectsSphere` (sphere pairs) and
  `SphereIntersectsBox` (sphere vs axis-aligned box, for anything-vs-platform).
  Boxes are AABBs — no rotation. Asteroids/rockets (`size` = radius) and players
  (`radius`) are spheres; platforms are boxes (`size` = full w/h/d).
- **Response** lives next to detection on purpose (this scale: tightly coupled).
  Walls reflect velocity scaled by the Walls element's per-type elasticity
  (`walls.elasticityPlayer` / `elasticityAsteroid`); bounds are inset by the
  object's extent so the whole body (and thus the first-person eye) stays
  inside the cube. Rocket hits spawn an `Explosion`; all rocket damage is dealt
  via `ApplyExplosionSplashDamage` (distance falloff), not a direct hit — even
  the directly-hit asteroid is damaged by the blast it spawns.

## Conventions
- **"Shaded wire" aesthetic:** every object draws a translucent fill (low alpha,
  ~40–60) under a bright **opaque** wireframe (alpha 255). Each element carries
  `color_outline` and `color_fill`.
- **Translucent fills must not write depth.** raylib keeps depth-writing on for
  blended geometry and doesn't sort it, so a see-through surface would otherwise
  occlude whatever's drawn behind it. Fill passes in `shapes.h` are wrapped in
  `BeginTranslucentFill()` / `EndTranslucentFill()` (flush batch →
  `rlDisableDepthMask` → draw fill → flush → `rlEnableDepthMask`). Wireframes
  keep writing depth.
- **Player is authoritative; the camera derives from it**, never the reverse.
- **Units:** 1 unit = 1 meter. Gravity constants live in `constants.h`
  (`MOON_GRAVITY = 3.25`, `EARTH_GRAVITY = 19.61` — tuned up from the real
  1.62/9.81 for game feel); hold Left Shift for earth gravity. Gravity is
  applied once, in `Player::updateVelocity`.
- Files are sectioned with `//MARK:` comments.
- raylib/raymath helpers are preferred for color/vector math (e.g.
  `ColorBrightness` preserves alpha; `Fade`/`ColorAlpha` *set* alpha from 0–1;
  `Vector3Reflect`, `Clamp`).

## Controls
WASD move · mouse look · left-click fire rocket · Space jetpack (up) · hold
Left Shift for stronger (earth) gravity · Esc toggle cursor capture.

## Web build (Emscripten / WASM) — gotchas
`make web RAYLIB_WEB_DIR=$HOME/raylib` builds the browser client; the shell is
`shell.html` (baked in at compile time, so re-run after editing it). The web raylib
at `~/raylib` is built as **OpenGL ES2 / WebGL1** (GLSL ES 100). Things that bite
on the web but not native (see `docs/multiplayer-testing.md` for the full setup):

- **Shaders need a GLSL ES 100 variant.** A desktop `#version 330` shader will not
  compile under WebGL1, leaving `BeginShaderMode` with no valid program — which
  breaks *every* draw inside the shader block. Provide an ES100 version under
  `#if defined(__EMSCRIPTEN__)` (`#version 100` + `precision mediump float;`,
  `varying` not `in`, `gl_FragColor` not `out`, `texture2D()` not `texture()`) and
  guard usage with `IsShaderValid(s) && s.id != rlGetShaderIdDefault()` so a failed
  shader degrades gracefully instead of breaking the frame. See the grayscale
  shader in `main.cpp` for the pattern.
- **`emcc` needs `EMSDK_PYTHON`** set to a ≥3.10 python or it fails its assert and
  silently leaves the old wasm (`export EMSDK_PYTHON=/opt/homebrew/opt/python@3.14/bin/python3.14`).
  `./rebuild-all.sh` sets this and force-rebuilds all three binaries.
- **Audio + `Module.HEAPF32`.** The Makefile passes `-sEXPORTED_RUNTIME_METHODS=HEAPF32`
  because raylib 5.5's miniaudio reads `Module.HEAPF32.buffer` in its Web Audio
  callback, which emscripten 6.x no longer attaches by default. `shell.html` also
  resumes the suspended AudioContext on the first gesture (browsers start it
  suspended; the title screen can be started with a key, which miniaudio's
  click/touchend-only auto-unlock misses).
- **Pointer lock.** `shell.html` requests pointer lock explicitly on canvas
  mousedown (Safari ignores raylib's deferred request); `IsCursorHidden()` gates
  game input, so without the lock the game is uncontrollable.

## Known quirks — leave as-is unless asked
- Rocket gravity and shooter-velocity inheritance are both driven by the OPTIONS
  **ROCKETS OBEY PHYSICS** toggle (`GameSpace::rocketsObeyPhysics`, set onto each
  fired rocket in `input.h`). Gravity is applied in `Rocket::updatePos`
  (`gravityEnabled`), inheritance once at spawn in `input.h` (`velocityInheritance`).
  Both default OFF, so rockets fly straight unless the toggle is on.
- This builds/runs on macOS with Homebrew paths only; no cross-platform build.
