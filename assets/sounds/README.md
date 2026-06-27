# Sound assets

The client loads these on startup (`fxTable` in `main.cpp`) and the web build
bundles this directory into the WASM virtual filesystem (`--preload-file assets`
in the `make web` target). Filenames must match exactly — missing files are
non-fatal (raylib warns and the sound is silently skipped).

Required files (one-shot WAVs, mono is fine):

| File                  | Event                          |
|-----------------------|--------------------------------|
| `rocket_launch.wav`   | a rocket is fired              |
| `explosion.wav`       | a rocket detonates             |
| `asteroid_break.wav`  | an asteroid is destroyed       |
| `player_hit.wav`      | a player takes non-fatal damage|
| `player_death.wav`    | a player is eliminated         |

This README also keeps the otherwise-empty directory present in git so the web
build's `--preload-file assets` has something to bundle.
