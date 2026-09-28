# Archived player shapes

The player is rendered as a regular **dodecahedron** (`DrawPlayerDodeca`,
`shapes.h`). Earlier in development the player silhouette was selectable between
several prototype shapes via a `PLAYER_SHAPE` enum. Those variants — **DART**,
**DELTA**, **LANDER**, and **POD** — were removed from `shapes.h` / `constants.h`
on 2026-06-25 to keep the live rendering path lean. Their code is preserved here
so any of them can be brought back.

## How to re-enable one

1. Paste the shape's draw function (and its exclusive helpers, listed per shape)
   back into `shapes.h`, above `DrawPlayer`.
2. Restore the `PlayerShape` enum and the `PLAYER_SHAPE` selector in
   `constants.h` (bottom of this doc), and any shape-specific constants (POD
   needs the `PLAYER_PLUME_*` block).
3. Restore the dispatch in `DrawPlayer`:
   ```cpp
   inline void DrawPlayer(const Player& player) {
       if (PLAYER_SHAPE == PLAYER_SHAPE_DODECA) { DrawPlayerDodeca(player); return; }
       if (PLAYER_SHAPE == PLAYER_SHAPE_POD)    { DrawPlayerPod(player);    return; }
       if (PLAYER_SHAPE == PLAYER_SHAPE_LANDER) { DrawPlayerLander(player); return; }
       // verts: 0=nose, 1=back-left, 2=back-right, 3=top ridge, 4=bottom keel.
       static const Vector3 dartVerts[5] = { /* ... see DART/DELTA below ... */ };
       static const Vector3 deltaVerts[5] = { /* ... */ };
       DrawPlayerShip(player, PLAYER_SHAPE == PLAYER_SHAPE_DELTA ? deltaVerts : dartVerts);
   }
   ```

Note: `ICOSA_T`, `ICOSA_VERTS`, and `ICOSA_FACES` remain in `shapes.h` (the
asteroid mesh uses them, and `DODECA_INV_T` derives from `ICOSA_T`), so the POD
code below does not need them re-added.

---

## DART / DELTA — low-poly arcade ship

Two vertex layouts (`dartVerts`, `deltaVerts`) over a shared 5-vertex /
6-triangle hull (`SHIP_FACES`), oriented so the nose tracks the player's aim.

```cpp
// Low-poly arcade ship. Same shaded-wire approach as DrawAsteroidShape, but a
// tiny fixed hull: 5 vertices, 6 triangular faces, 9 edges. Local space has
// +X = nose/forward, +Y = up, +Z = right. The 5 verts are: 0=nose,
// 1=back-left, 2=back-right, 3=top ridge (cockpit), 4=bottom keel. The same
// face topology fits any of these vertex layouts, so the ship variants only
// differ in vertex positions (see DrawPlayer).
static const int SHIP_FACES[6][3] = {
    {0, 3, 1}, {0, 2, 3}, // nose -> top, both sides
    {0, 1, 4}, {0, 4, 2}, // nose -> keel, both sides
    {1, 3, 2}, {1, 2, 4}  // rear cap (top + bottom)
};

inline void DrawPlayerShip(const Player& player, const Vector3 localVerts[5]) {
    Color outline, fill;
    PlayerFlashColors(player, outline, fill);

    // Orient the nose (local +X) to the player's look/aim direction. Axis-angle
    // from {1,0,0} to Forward() avoids fiddling with yaw/pitch sign conventions.
    Vector3 fwd = player.Forward();
    Vector3 xAxis = {1, 0, 0};
    Vector3 axis = Vector3CrossProduct(xAxis, fwd);
    float axisLen = Vector3Length(axis);
    float angle = acosf(Clamp(Vector3DotProduct(xAxis, fwd), -1.0f, 1.0f));
    if (axisLen < 1e-6f) {
        axis = {0, 1, 0}; // fwd is parallel to ±X; angle is 0 or PI, any Y axis works
    } else {
        axis = Vector3Scale(axis, 1.0f / axisLen);
    }

    const float s = 0.5f * player.size.y; // ship scale, ties to PLAYER_SCALE

    // Transform the local hull into world space.
    Vector3 verts[5];
    for (int i = 0; i < 5; i++) {
        Vector3 local = Vector3Scale(localVerts[i], s);
        local = Vector3RotateByAxisAngle(local, axis, angle);
        verts[i] = Vector3Add(player.position, local);
    }

    // Translucent fill, both windings so the glow reads from any angle.
    BeginTranslucentFill();
    for (int f = 0; f < 6; f++) {
        Vector3 a = verts[SHIP_FACES[f][0]];
        Vector3 b = verts[SHIP_FACES[f][1]];
        Vector3 c = verts[SHIP_FACES[f][2]];
        DrawTriangle3D(a, b, c, fill);
        DrawTriangle3D(a, c, b, fill);
    }
    EndTranslucentFill();

    // Wire edges on top, each undirected edge drawn once via the i<j test.
    for (int f = 0; f < 6; f++) {
        for (int e = 0; e < 3; e++) {
            int i = SHIP_FACES[f][e];
            int j = SHIP_FACES[f][(e + 1) % 3];
            if (i < j) DrawLine3D(verts[i], verts[j], outline);
        }
    }
}
```

The two vertex layouts (these lived as `static const` locals inside `DrawPlayer`):

```cpp
// verts: 0=nose, 1=back-left, 2=back-right, 3=top ridge, 4=bottom keel.
static const Vector3 dartVerts[5] = {
    { 1.0f,  0.00f,  0.0f}, // nose
    {-0.7f,  0.00f, -0.6f}, // back-left
    {-0.7f,  0.00f,  0.6f}, // back-right
    {-0.2f,  0.35f,  0.0f}, // top ridge (cockpit)
    {-0.2f, -0.25f,  0.0f}  // bottom keel
};
static const Vector3 deltaVerts[5] = {
    { 1.0f,  0.00f,  0.0f}, // nose
    {-0.8f,  0.00f, -0.9f}, // wide back-left
    {-0.8f,  0.00f,  0.9f}, // wide back-right
    {-0.6f,  0.30f,  0.0f}, // low tail fin
    {-0.5f, -0.15f,  0.0f}  // shallow keel
};
```

---

## LANDER — tank / moon-lander

A radially-symmetric hexagonal body with a tapered booster nozzle and a
look-tracking turret barrel. Uses two exclusive helpers, `DrawShadedDrum` and
`DrawShadedWireBoxLocal`, which must be restored alongside it.

```cpp
// Like DrawShadedWireBox but draws at a LOCAL offset and does NOT push its own
// matrix - so multiple parts compose under a single parent transform (e.g. the
// translate+yaw+pitch set up for the lander's turret barrel).
inline void DrawShadedWireBoxLocal(Vector3 center, float width, float height, float depth, Color wireColor, Color fillColor) {
    BeginTranslucentFill();
    DrawCube(center, width, height, depth, fillColor);
    EndTranslucentFill();
    DrawCubeWires(center, width, height, depth, wireColor);
}

// Shaded-wire n-gon "drum" (prism or frustum) centered on a vertical axis at
// `center`. Two rings of `sides` verts - bottom radius rBottom at center.y +
// yBottom, top radius rTop at center.y + yTop - with translucent fill (sides +
// triangle-fan caps) under an opaque wireframe (two rings + vertical edges, no
// cap spokes to keep it clean). rBottom == rTop -> straight prism; unequal ->
// tapered funnel. Built procedurally like DrawAsteroidShape.
inline void DrawShadedDrum(Vector3 center, int sides, float rBottom, float rTop,
                           float yBottom, float yTop, Color wireColor, Color fillColor) {
    const int N = sides;
    const float yB = center.y + yBottom;
    const float yT = center.y + yTop;

    // Ring vertices.
    Vector3 bot[16], top[16]; // 16 is plenty (hexagon uses 6)
    for (int i = 0; i < N; i++) {
        float a = (float)i / (float)N * 2.0f * PI;
        float cx = cosf(a), cz = sinf(a);
        bot[i] = {center.x + rBottom * cx, yB, center.z + rBottom * cz};
        top[i] = {center.x + rTop    * cx, yT, center.z + rTop    * cz};
    }
    Vector3 botC = {center.x, yB, center.z};
    Vector3 topC = {center.x, yT, center.z};

    // Fill: side quads (two triangles, both windings) + cap fans.
    BeginTranslucentFill();
    for (int i = 0; i < N; i++) {
        int j = (i + 1) % N;
        // side quad bot[i]-bot[j]-top[j]-top[i]
        DrawTriangle3D(bot[i], bot[j], top[j], fillColor);
        DrawTriangle3D(bot[i], top[j], top[i], fillColor);
        DrawTriangle3D(bot[i], top[j], bot[j], fillColor); // reverse winding
        DrawTriangle3D(bot[i], top[i], top[j], fillColor);
        // caps
        DrawTriangle3D(topC, top[i], top[j], fillColor);
        DrawTriangle3D(topC, top[j], top[i], fillColor);
        DrawTriangle3D(botC, bot[j], bot[i], fillColor);
        DrawTriangle3D(botC, bot[i], bot[j], fillColor);
    }
    EndTranslucentFill();

    // Wire: bottom ring, top ring, vertical edges.
    for (int i = 0; i < N; i++) {
        int j = (i + 1) % N;
        DrawLine3D(bot[i], bot[j], wireColor);
        DrawLine3D(top[i], top[j], wireColor);
        DrawLine3D(bot[i], top[i], wireColor);
    }
}

// Tank / moon-lander: a radially-symmetric hexagonal body, a tapered booster
// nozzle underneath, and a turret on top whose rocket-launcher barrel swivels
// (yaw) and elevates (pitch) to track the player's look. The body is symmetric,
// so only the barrel needs orienting.
inline void DrawPlayerLander(const Player& player) {
    Color outline, fill;
    PlayerFlashColors(player, outline, fill);

    const Vector3 c = player.position; // box center
    const float H = player.size.y;     // total height
    const float R = 0.5f * player.size.x; // body radius (half the footprint)

    // Booster base: tapered hex nozzle, narrow at the bottom.
    DrawShadedDrum(c, 6, 0.30f * R, 0.65f * R, -0.50f * H, -0.22f * H, outline, fill);
    // Hex body: the bulk, a straight hexagonal prism.
    DrawShadedDrum(c, 6, R, R, -0.22f * H, 0.18f * H, outline, fill);
    // Turret head: smaller hex drum on top (symmetric, no rotation needed).
    DrawShadedDrum(c, 6, 0.55f * R, 0.55f * R, 0.18f * H, 0.34f * H, outline, fill);

    // Rocket-launcher barrel: protrudes from the turret center, tracking look.
    const float tY = c.y + 0.26f * H; // turret pivot height
    rlPushMatrix();
    rlTranslatef(c.x, tY, c.z);
    rlRotatef(-player.yaw   * RAD2DEG, 0, 1, 0); // swivel to facing
    rlRotatef( player.pitch * RAD2DEG, 0, 0, 1); // elevate barrel (local +X forward)
    DrawShadedWireBoxLocal({0.55f * R, 0.0f, 0.0f}, 0.9f * R, 0.22f * R, 0.22f * R, outline, fill);
    rlPopMatrix();
}
```

---

## POD — uniform icosahedral pod with jetpack plume

The asteroid icosahedron without jitter/tumble, oriented so its top faces the
direction of travel, with a velocity-driven flame plume out the bottom. Uses the
exclusive helper `DrawThrustPlume` and the `PLAYER_PLUME_*` constants. Relies on
`ICOSA_VERTS` / `ICOSA_FACES`, which remain in `shapes.h`.

```cpp
// A flickering flame cone: a ring of K verts around `origin` (perpendicular to
// `dir`) converging to a tip `length` units along `dir`. Hot translucent fill
// under brighter orange edges. Used as the pod's jetpack exhaust.
inline void DrawThrustPlume(Vector3 origin, Vector3 dir, float length, float radius) {
    // Perpendicular basis around the plume axis.
    Vector3 up = (fabsf(dir.y) < 0.99f) ? Vector3{0, 1, 0} : Vector3{1, 0, 0};
    Vector3 right = Vector3Normalize(Vector3CrossProduct(dir, up));
    Vector3 fwd2  = Vector3Normalize(Vector3CrossProduct(right, dir));

    float flick = 0.85f + 0.15f * sinf((float)GetTime() * 30.0f); // lively length jitter
    Vector3 tip = Vector3Add(origin, Vector3Scale(dir, length * flick));

    const int K = 8;
    Vector3 ring[K];
    for (int i = 0; i < K; i++) {
        float a = (float)i / (float)K * 2.0f * PI;
        Vector3 off = Vector3Add(Vector3Scale(right, radius * cosf(a)),
                                 Vector3Scale(fwd2,  radius * sinf(a)));
        ring[i] = Vector3Add(origin, off);
    }

    const Color flame = {255, 230, 120, 90};  // hot translucent core
    const Color edge  = {255, 140, 30, 200};  // brighter orange wire

    BeginTranslucentFill();
    for (int i = 0; i < K; i++) {
        int j = (i + 1) % K;
        DrawTriangle3D(ring[i], ring[j], tip, flame);
        DrawTriangle3D(ring[i], tip, ring[j], flame); // reverse winding
    }
    EndTranslucentFill();

    for (int i = 0; i < K; i++) {
        int j = (i + 1) % K;
        DrawLine3D(ring[i], ring[j], edge); // nozzle ring
        DrawLine3D(ring[i], tip, edge);     // flame edge to tip
    }
}

// Uniform icosahedral "pod": the asteroid mesh without jitter/tumble, oriented
// so its top (+Y) faces the direction of travel, with a jetpack plume out the
// bottom pointing opposite the movement. Orientation/plume derive from velocity.
inline void DrawPlayerPod(const Player& player) {
    Color outline, fill;
    PlayerFlashColors(player, outline, fill);

    const Vector3 c = player.position;
    const float r = 0.5f * player.size.x; // pod radius, fits the footprint

    // Orient local +Y -> velocity direction (top toward travel) when moving.
    Vector3 v = player.velocity;
    float speed = Vector3Length(v);
    bool moving = speed > PLAYER_PLUME_MIN_SPEED;

    Vector3 axis = {0, 1, 0};
    float angle = 0.0f;
    Vector3 vhat = {0, 1, 0};
    if (moving) {
        vhat = Vector3Scale(v, 1.0f / speed);
        Vector3 yAxis = {0, 1, 0};
        Vector3 cross = Vector3CrossProduct(yAxis, vhat);
        float crossLen = Vector3Length(cross);
        angle = acosf(Clamp(Vector3DotProduct(yAxis, vhat), -1.0f, 1.0f));
        if (crossLen < 1e-6f) axis = {1, 0, 0}; // parallel to ±Y; any perpendicular axis
        else axis = Vector3Scale(cross, 1.0f / crossLen);
    }

    // Build the uniform hull (no per-vertex jitter).
    Vector3 verts[12];
    for (int i = 0; i < 12; i++) {
        Vector3 local = Vector3Scale(Vector3Normalize(ICOSA_VERTS[i]), r);
        if (moving) local = Vector3RotateByAxisAngle(local, axis, angle);
        verts[i] = Vector3Add(c, local);
    }

    BeginTranslucentFill();
    for (int f = 0; f < 20; f++) {
        Vector3 a = verts[ICOSA_FACES[f][0]];
        Vector3 b = verts[ICOSA_FACES[f][1]];
        Vector3 d = verts[ICOSA_FACES[f][2]];
        DrawTriangle3D(a, b, d, fill);
        DrawTriangle3D(a, d, b, fill);
    }
    EndTranslucentFill();

    for (int f = 0; f < 20; f++) {
        for (int e = 0; e < 3; e++) {
            int i = ICOSA_FACES[f][e];
            int j = ICOSA_FACES[f][(e + 1) % 3];
            if (i < j) DrawLine3D(verts[i], verts[j], outline);
        }
    }

    // Jetpack plume out the bottom, opposite the movement direction.
    if (moving) {
        Vector3 plumeDir = Vector3Negate(vhat);
        Vector3 origin = Vector3Add(c, Vector3Scale(plumeDir, r));
        float length = r * PLAYER_PLUME_MAX_LENGTH_RADII * Clamp(speed / PLAYER_PLUME_SPEED_REF, 0.0f, 1.0f);
        DrawThrustPlume(origin, plumeDir, length, r * 0.6f);
    }
}
```

POD-only constants (were in `constants.h`):

```cpp
// Jetpack thrust plume for the POD shape: a flame out the bottom, pointing
// opposite the direction of movement. Driven by the player's velocity.
const float PLAYER_PLUME_MIN_SPEED = 1.0f;        // below this speed: no rotation / no plume
const float PLAYER_PLUME_SPEED_REF = 16.0f;       // speed (= jetpack speed) at which the plume hits full length
const float PLAYER_PLUME_MAX_LENGTH_RADII = 2.5f; // max plume length, in body radii
```

---

## Shape selector (was in `constants.h`)

```cpp
//MARK: Player Shape / Size
// Which player silhouette to render (see DrawPlayer in shapes.h).
enum PlayerShape { PLAYER_SHAPE_DART = 0, PLAYER_SHAPE_DELTA = 1, PLAYER_SHAPE_LANDER = 2, PLAYER_SHAPE_POD = 3, PLAYER_SHAPE_DODECA = 4 };
const PlayerShape PLAYER_SHAPE = PLAYER_SHAPE_DODECA;
```
