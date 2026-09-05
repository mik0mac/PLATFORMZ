# Server performance measurements (A4)

How many concurrent matches this process can hold, and what actually limits it.
A4 exists because that number has to come from measurement — `MATCH_MAX_CONCURRENT`
was a placeholder.

## How to measure

Instrumentation is **off unless asked for**. Set `PLATFORMZ_PERF=1` and the server
prints one `PERF` line every 5 s:

```bash
cd server && PLATFORMZ_PERF=1 ./gameserver &
python3 test/probe_load.py --clients 8 --map XL --seconds 60
```

```
PERF sim p50/p95/max 4.08/6.73/78.98 ms | broadcast 0.14/0.28/4.67 |
     grid 3.02/5.43/59.18 | total p95 7.02 ms | egress 624 KB/s |
     clients 8 roster 8 | fits ~1 matches
```

- **sim** — the whole `gameMutex` block
- **grid** — `CollisionGrid::Rebuild` alone, *inside* sim
- **broadcast** — serialize + hand to sockets, off `gameMutex`
- **fits** — `10 / total_p95`. Budget **10 ms per 60 Hz beat, not 16.6**: the rest
  goes to io threads, Caddy's TLS on the same box, and shared-CPU steal time.

`--clients 8` matters. Tick cost scales with the **roster** (bots fill empty
slots), but egress scales with **connected clients** — the server sends a full
state packet to each, 60 times a second.

## Results — 2026-08-31, Mike's Mac (superseded; load ran on the same machine)

**After #99** (static platform layer):

| Map | sim p95 | grid p95 | broadcast p95 | total p95 | egress | fits |
|---|---|---|---|---|---|---|
| MEDIUM | 0.7 ms | 0.05 ms | 0.4 ms | **1.1 ms** | ~270 KB/s | ~9 |
| XL | 2.6 ms | 0.11 ms | 0.5 ms | **3.0 ms** | ~490 KB/s | ~3 |

Before #99, for comparison:

| Map | sim p95 | grid p95 | total p95 | fits |
|---|---|---|---|---|
| MEDIUM | 1.9 ms | 1.6 ms | 2.2 ms | ~4 |
| XL | 6.8 ms | 5.9 ms | 7.0 ms | ~1 |

The grid went from ~80 % of the tick to ~4 % of it, and **broadcast is now the
largest single line item**. That matters for what to optimise next: serialization
and socket writes, not physics.

> ⚠️ **These are Mac numbers and they are not the deployment numbers.** A 1 vCPU
> shared Vultr instance is substantially slower and loses another 10–30 % to steal
> time. Re-run on the box before trusting any cap. They are also pessimistic in one
> specific way: `probe_load.py` runs 8 clients × 2 threads on the *same machine* as
> the server, so the measurement competes with what it measures. Real clients are
> elsewhere.

The `max` column stays pinned at one large value (18 ms MEDIUM, 79 ms XL) across
consecutive reports — it is the match-start tick (`generate()` plus the first grid
build) sitting in the 900-sample window, not a steady-state stall.

## Results — 2026-09-05, the actual Vultr box, 8 remote clients, MEDIUM

**These are the numbers that count.** Server on `platformz.space`, load driven from
a different machine, so the generator was not competing with what it measured.

Steady state while PLAYING:

| | p50 | p95 | max |
|---|---|---|---|
| sim | 0.28 ms | **0.45 ms** | 1.36 ms |
| broadcast | 0.10 ms | 0.16 ms | 0.39 ms |
| grid | 0.02 ms | 0.03 ms | 0.48 ms |
| **total** | | **~0.6 ms** | |

Egress ~310–430 KB/s. `fits ~16–19 matches`.

### The box is roughly 2× *faster* than the Mac figures below

Not because the hardware is better — because the Mac run had 8 Python clients (two
threads each) fighting the server for the same cores. That distortion was flagged
as a caveat when those numbers were taken; it turns out to have been worth about a
factor of two. **Measure from another machine.**

### CPU is not the constraint. Egress is, by ~7×

- **CPU:** ~0.6 ms per tick → ~16 concurrent active matches inside a 10 ms budget.
- **Egress:** ~310 KB/s per full match ≈ **815 GB/month**. Against the $6 plan's
  2 TB that is **~2.4 permanently-full matches**.

Real rooms are not full around the clock, so the practical figure is higher — but
the ratio is the point. Optimising the tick further buys nothing until egress is
addressed, and the lever for that is broadcasting every 2nd–3rd tick with
`GameSpace::extrapolate` rather than making physics cheaper.

### A1b and A7 are visible in this log

- **grid p95 0.03 ms.** Before A7 it was the dominant cost; it is now noise.
- `Match sim idle after 15s in GAMEOVER` — sim p95 drops **0.31 → 0.01 ms**
  immediately after. A1b's wind-down, doing exactly what it was built for.
- `Match world freed after 60s in GAMEOVER; back to lobby` — asteroid count 17 → 0.
- **Broadcast is now the largest line item**, and once a match is idle it is the
  *only* one.

### One bug this run found

```
[server] state packet 1288 B > 1200 (chunking; lossy per-tick)
```

At 18 asteroids, against a documented cap of 42. `ACTION_HEADROOM` is 65 B and the
real action content was ~617 B — 9.5× over. Tracked as **#100**.

## Finding 1 — the grid rebuild was ~80 % of the tick (FIXED in #99)

A1b removed the grid's *allocation* churn (13,037 → 0 allocs/tick on XL). It did
not remove the *work*. Rebuild is still the single most expensive thing in a tick.

**And ~99 % of that work is platforms, which never move.**

```
MEDIUM  platforms=128  | rebuild 0.017 ms | dynamic-only 0.000 ms | platforms 99%
LARGE   platforms=256  | rebuild 0.048 ms | dynamic-only 0.000 ms | platforms 99%
XL      platforms=576  | rebuild 0.200 ms | dynamic-only 0.000 ms | platforms 100%
```

Platforms are fixed once `generatePlatforms()` runs, yet all 576 were re-bucketed
into their 4–32 cells apiece, every tick, 60 times a second.

**#99 fixed this**, bucketing them once per match into a separate `staticCells`
layer, invalidated by a `platformEpoch` on `GameSpace`:

```
             before              after
MEDIUM   0.017 ms/rebuild    0.001 ms   platforms 99% -> 8% of it
XL       0.200 ms/rebuild    0.000 ms   platforms 100% -> 9% of it
```

Verified equivalent, not just faster: `server/test/grid_equiv_test.cpp` compares
`GatherPlatformNeighbors` against a brute-force reference at 6,591 probe points
across three map sizes — zero mismatches. A stale platform bucket would show up as
"rockets occasionally pass through that one platform", which no phase transcript
would ever catch.

The first version of this carried a hedge for future moving platforms — the epoch
was re-bumped every tick if any platform reported `isMoving`, degrading back to
per-tick bucketing. **Moving platforms were ruled out after gameplay testing
(2026-08-31), so that is gone**, along with `Platform::isMoving` and the empty
`Platform::updatePos`. The epoch now changes only when a layout is generated,
which means once per match. Platforms are not ticked at all.

(The isolated bench reads far lower than the live p95 — 0.2 ms vs 5.9 ms on XL —
because the bench runs alone while the live server contends with 8 local probe
clients and its own io threads. The *ratio* is the finding, not the absolute.)

## Finding 2 — egress is a real constraint, and does not care about CPU

One continuously-full MEDIUM match is ~300 KB/s ≈ **780 GB/month**. The $6 Vultr
plan includes 2 TB, so roughly **2.5 permanently-full matches** exhausts it. Real
rooms are not full 24/7, but this is the constraint that no amount of optimisation
touches — it is a property of sending every client a full snapshot at 60 Hz.

If it binds, the lever is `GameSpace::extrapolate(dt)` (`gamespace.h`): broadcast
every 2nd or 3rd tick and let the client extrapolate between. Halve the
**broadcast** rate, never the sim rate — 60 Hz physics parity with the client's
`ApplyPlayerInput` is not negotiable.

## Decision — one sequential sim thread

Confirmed as the right scheduler (plan option 1). Even the optimistic Mac numbers
fit only ~4 MEDIUM matches in the budget, and the target box is a single shared
vCPU: a worker pool buys nothing without more cores, and would cost the shared
warm `CollisionGrid` that A1b's design depends on.

Revisit only after a box upsize, and fix `random.h`'s shared `static std::mt19937`
first — it is not thread-safe, and two matches generating a world at once would
race it.

**`MATCH_MAX_CONCURRENT` stays conservative until measured on the box.** Whatever
number comes out, take the lower of the CPU answer and the transfer-quota answer.

## Still to do on the box

1. `PLATFORMZ_PERF=1` on the Vultr instance, driven by `probe_load.py` from a
   *different* machine so the measurement does not compete with the server.
2. `vmstat 1` alongside it — the `st` column is steal time, and it comes straight
   out of the tick budget.
3. `/proc/net/dev` deltas for a real egress figure including IP/UDP overhead,
   which the server's own byte count excludes.
