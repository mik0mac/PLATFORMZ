# Jetpack exhaust plume — archived (deprecated)

The jetpack **exhaust plume** was a continuous cone of orange sparks streamed out
of each thrusting player's bottom (visual only). It has been removed from the
game in both local and network play. This document preserves the deprecated code
so it can be reinstated later.

The exhaust was only ever spawned in the **local single-player** branch of the
game loop; the networked branch never emitted it, so there is no separate network
emission to restore.

**What was kept:** the `Spark` class, the `SpawnSparkBurst()` emitter, the
spark update/draw/erase pipeline, and the destruction bursts (asteroid break-up
and player elimination). Only the *continuous jetpack stream* and its dedicated
`SpawnSparkCone()` emitter + `PLAYER_EXHAUST_*` constants were removed.

---

## How to restore

1. Re-add the `PLAYER_EXHAUST_*` constants to `constants.h` (see below).
2. Re-add `SpawnSparkCone()` to `elements.h` (next to `SpawnSparkBurst`).
3. Re-add the emission block to the local branch of the game loop in `main.cpp`,
   immediately before `gameSpace.updatePositions(dt);`.

All three pieces depend only on symbols that still exist: `GameSpace::getSparks()`,
the `Spark` class, `RandomFloat`, and the raymath helpers.

---

## Deprecated code

### `constants.h` — exhaust tuning constants

```cpp
// Jetpack exhaust: spawned spark particles streaming down out of the bottom
// while the jetpack fires (see SpawnSparkCone / main.cpp emission).
const float PLAYER_EXHAUST_RATE = 140.0f;     // sparks spawned per second while thrusting
const float PLAYER_EXHAUST_CONE = 0.35f;      // cone half-angle around straight-down, radians
const float PLAYER_EXHAUST_SPEED_MIN = 8.0f;  // spark ejection speed, units/sec
const float PLAYER_EXHAUST_SPEED_MAX = 16.0f;
const float PLAYER_EXHAUST_INHERIT = 0.3f;    // fraction of player velocity added to each spark

// Jetpack exhaust spark lifetime (short - sparks fade fast behind the player).
const float PLAYER_EXHAUST_LIFETIME_MIN = 0.30f;
const float PLAYER_EXHAUST_LIFETIME_MAX = 0.70f;
```

### `elements.h` — the cone emitter

```cpp
// Emit `count` sparks from `origin` within a cone of half-angle `coneAngle`
// (radians) around `dir`. Small angle + dir = {0,-1,0} gives an exhaust plume;
// `inheritVel` (e.g. the emitter's velocity) is added to every spark so the
// stream trails the mover. Visual only - pushes into the sparks vector.
inline void SpawnSparkCone(std::vector<Spark>& out, Vector3 origin, Vector3 dir,
                           float coneAngle, float speedMin, float speedMax,
                           int count, Color color, float lifeMin, float lifeMax,
                           Vector3 inheritVel = {0, 0, 0}) {
    Vector3 n = Vector3Normalize(dir);
    Vector3 up = (fabsf(n.y) < 0.99f) ? Vector3{0, 1, 0} : Vector3{1, 0, 0};
    Vector3 u = Vector3Normalize(Vector3CrossProduct(up, n)); // basis in the cone's base plane
    Vector3 v = Vector3CrossProduct(n, u);
    for (int i = 0; i < count; i++) {
        float az = RandomFloat(0.0f, 2.0f * PI);
        float polar = coneAngle * sqrtf(RandomFloat(0.0f, 1.0f)); // denser toward the axis
        Vector3 radial = Vector3Add(Vector3Scale(u, cosf(az)), Vector3Scale(v, sinf(az)));
        Vector3 d = Vector3Add(Vector3Scale(n, cosf(polar)), Vector3Scale(radial, sinf(polar)));
        Spark s;
        s.position = origin;
        s.velocity = Vector3Add(Vector3Scale(d, RandomFloat(speedMin, speedMax)), inheritVel);
        s.lifetime = RandomFloat(lifeMin, lifeMax);
        s.color = color;
        out.push_back(s);
    }
}
```

### `main.cpp` — the emission block (local branch, before `updatePositions`)

```cpp
// Jetpack exhaust: spawn spark particles streaming down out of each
// thrusting player's bottom. Visual only - emitted here (game-state
// phase) so velocity/jetpack state is current; ticked in updatePositions.
for (Player& pl : players) {
    if (!(pl.isUsingJetpack && pl.hasFuel())) continue;
    float want = PLAYER_EXHAUST_RATE * dt;          // fractional count -> integer with random carry
    int n = (int)want;
    if (RandomFloat(0.0f, 1.0f) < (want - (float)n)) n++;
    if (n <= 0) continue;
    Vector3 origin = pl.position;
    origin.y -= pl.radius;                          // bottom of the body
    SpawnSparkCone(gameSpace.getSparks(), origin, {0, -1, 0}, PLAYER_EXHAUST_CONE,
                   PLAYER_EXHAUST_SPEED_MIN, PLAYER_EXHAUST_SPEED_MAX, n,
                   Color{255, 200, 80, 255},
                   PLAYER_EXHAUST_LIFETIME_MIN, PLAYER_EXHAUST_LIFETIME_MAX,
                   Vector3Scale(pl.velocity, PLAYER_EXHAUST_INHERIT));
}
```
