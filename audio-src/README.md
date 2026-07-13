# audio-src/ — audio working material (never shipped)

Nothing in this directory is built, preloaded, or deployed. The rule that keeps
the web bundle small: **`assets/` contains only files the game reads at
runtime** — the web build packs `assets/` wholesale into `platformz.data`
(`--preload-file assets` in the Makefile), and the Vultr deploy rsyncs it.

- `masters/` — hi-res WAV masters for the music tracks (source for the .oggs).
  Gitignored (large; lives in Dropbox).
- `wip/` — work-in-progress sounds and experiments. Gitignored.
- `retired/` — sounds that used to ship and were swapped out. Gitignored.
- `notes/` — sound design notes. Tracked.

Workflow: edit/export in `masters/` or `wip/`, then convert and drop the
shipping file into `assets/`:

    python3 scripts/to_ogg.py audio-src/masters/gameplay.wav
    mv audio-src/masters/gameplay.ogg assets/music/
