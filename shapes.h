// How to draw the wireframe shapes for the various elements.
#pragma once

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <vector>
#include <cmath>

#include "elements.h"
#include "constants.h"

//MARK: Primitive drawing helpers
// These are the low-level wireframe/shaded-fill primitives, pulled in from
// WireframeTests/main3d3.cpp. Higher-level Draw___(const Element&) functions
// below build on top of these.

// Translucent fills should test depth (stay hidden behind solid geometry) but
// not WRITE depth - otherwise a see-through surface occludes whatever is drawn
// behind it later, which is exactly the "can't see through it" bug. Flush the
// batch around each toggle so the mask change only affects the fill vertices,
// not the opaque wireframes queued before/after.
inline void BeginTranslucentFill() {
    rlDrawRenderBatchActive();
    rlDisableDepthMask();
}
inline void EndTranslucentFill() {
    rlDrawRenderBatchActive();
    rlEnableDepthMask();
}

// Draws a wireframe box with a translucent, slightly-glowing fill pass underneath.
// Render solid first (low alpha), then draw the wireframe on top at full opacity
// so edges stay crisp - the core "vector + shading" look used throughout.
inline void DrawShadedWireBox(Vector3 position, float width, float height, float depth, float rotY, Color wireColor, Color fillColor) {
    rlPushMatrix();
    rlTranslatef(position.x, position.y, position.z);
    rlRotatef(rotY * RAD2DEG, 0, 1, 0);

    BeginTranslucentFill();
    DrawCube(Vector3Zero(), width, height, depth, fillColor);
    EndTranslucentFill();
    DrawCubeWires(Vector3Zero(), width, height, depth, wireColor);

    rlPopMatrix();
}

// Like DrawShadedWireBox but draws at a LOCAL offset and does NOT push its own
// matrix - so multiple parts compose under a single parent transform (e.g. the
// translate+yaw+pitch set up for the lander's turret barrel).
inline void DrawShadedWireBoxLocal(Vector3 center, float width, float height, float depth, Color wireColor, Color fillColor) {
    BeginTranslucentFill();
    DrawCube(center, width, height, depth, fillColor);
    EndTranslucentFill();
    DrawCubeWires(center, width, height, depth, wireColor);
}

// Same fill+wireframe layering as DrawShadedWireBox, but for a sphere.
// Used for asteroids and the explosion effect.
inline void DrawShadedSphere(Vector3 position, float radius, Color wireColor, Color fillColor) {
    BeginTranslucentFill();
    DrawSphere(position, radius, fillColor);
    EndTranslucentFill();
    DrawSphereWires(position, radius, 16, 16, wireColor);
}

// A wireframe pyramid (ship/rocket-like silhouette), built manually with line
// segments so we control exactly which edges draw - closer to how the
// original vector hardware worked than using a filled primitive.
inline void DrawWirePyramid(Vector3 pos, float rotY, float height, float baseSize, Color col) {
    rlPushMatrix();
    rlTranslatef(pos.x, pos.y, pos.z);
    rlRotatef(rotY * RAD2DEG, 0, 1, 0);

    Vector3 tip = {0, height, 0};
    Vector3 a = {-baseSize, 0, -baseSize};
    Vector3 b = { baseSize, 0, -baseSize};
    Vector3 c = { baseSize, 0,  baseSize};
    Vector3 d = {-baseSize, 0,  baseSize};

    // base
    DrawLine3D(a, b, col);
    DrawLine3D(b, c, col);
    DrawLine3D(c, d, col);
    DrawLine3D(d, a, col);
    // sides to tip
    DrawLine3D(a, tip, col);
    DrawLine3D(b, tip, col);
    DrawLine3D(c, tip, col);
    DrawLine3D(d, tip, col);

    rlPopMatrix();
}

// Draws a vector-grid pattern across all 6 faces of a large bounding cube,
// used for the play-space boundary walls (Walls::halfSize / color_outline).
inline void DrawGridRoom(float halfSize, float spacing, Color col) {
    int lines = (int)(halfSize / spacing);

    for (int i = -lines; i <= lines; i++) {
        float p = i * spacing;
        // floor / ceiling (X/Z grid)
        DrawLine3D({p, -halfSize, -halfSize}, {p, -halfSize, halfSize}, col);
        DrawLine3D({-halfSize, -halfSize, p}, {halfSize, -halfSize, p}, col);
        DrawLine3D({p, halfSize, -halfSize}, {p, halfSize, halfSize}, col);
        DrawLine3D({-halfSize, halfSize, p}, {halfSize, halfSize, p}, col);

        // front / back (X/Y grid)
        DrawLine3D({p, -halfSize, -halfSize}, {p, halfSize, -halfSize}, col);
        DrawLine3D({-halfSize, p, -halfSize}, {halfSize, p, -halfSize}, col);
        DrawLine3D({p, -halfSize, halfSize}, {p, halfSize, halfSize}, col);
        DrawLine3D({-halfSize, p, halfSize}, {halfSize, p, halfSize}, col);

        // left / right (Y/Z grid)
        DrawLine3D({-halfSize, p, -halfSize}, {-halfSize, p, halfSize}, col);
        DrawLine3D({-halfSize, -halfSize, p}, {-halfSize, halfSize, p}, col);
        DrawLine3D({halfSize, p, -halfSize}, {halfSize, p, halfSize}, col);
        DrawLine3D({halfSize, -halfSize, p}, {halfSize, halfSize, p}, col);
    }
}

//MARK: Element drawing functions
// One function per game-object type, each reading only the fields it needs
// from the corresponding class in elements.h.

inline void DrawWalls(const Walls& walls) {
    DrawGridRoom(walls.halfSize, walls.gridSpacing, walls.color_outline);
}

inline void DrawPlatform(const Platform& platform) {
    DrawShadedWireBox(platform.position, platform.size.x, platform.size.y, platform.size.z, 0.0f, platform.color_outline, platform.color_fill);
}

// A VFX spark: a short streak drawn behind its direction of travel, fading out
// over its lifetime. Plain depth-writing lines (cheap; there may be many).
inline void DrawSpark(const Spark& spark) {
    float speed = Vector3Length(spark.velocity);
    Vector3 dir = (speed > 1e-4f) ? Vector3Scale(spark.velocity, 1.0f / speed) : Vector3{0, 1, 0};
    Vector3 tail = Vector3Subtract(spark.position, Vector3Scale(dir, SPARK_STREAK_LENGTH));
    Color c = spark.color;
    c.a = (unsigned char)(c.a * spark.fade());
    DrawLine3D(spark.position, tail, c);
}

// Brighten the player's colors toward white on a recent hit, same as the
// asteroid damage flash (ColorBrightness preserves alpha, so fills stay glassy).
// At flash == 0 these return the colors unchanged.
inline void PlayerFlashColors(const Player& player, Color& outline, Color& fill) {
    float flash = player.flashIntensity();
    outline = ColorBrightness(player.color_outline, ASTEROID_FLASH_INTENSITY * flash);
    fill    = ColorBrightness(player.color_fill,    ASTEROID_FLASH_INTENSITY * flash);
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

// Low-poly arcade ship. Same shaded-wire approach as DrawAsteroidShape, but a
// tiny fixed hull: 5 vertices, 6 triangular faces, 9 edges. Local space has
// +X = nose/forward, +Y = up, +Z = right. The 5 verts are: 0=nose,
// 1=back-left, 2=back-right, 3=top ridge (cockpit), 4=bottom keel. The same
// face topology fits any of these vertex layouts, so the ship variants only
// differ in vertex positions (see DrawPlayer).
// Unit icosahedron (golden-ratio construction): 12 vertices, 20 triangular
// faces, 30 edges. Shared by DrawPlayerPod (uniform) and DrawAsteroidShape
// (jittered/tumbling). Verts are NOT unit length - normalize before use.
static const float ICOSA_T = 1.6180339887f; // golden ratio
static const Vector3 ICOSA_VERTS[12] = {
    {-1,  ICOSA_T,  0}, { 1,  ICOSA_T,  0}, {-1, -ICOSA_T,  0}, { 1, -ICOSA_T,  0},
    { 0, -1,  ICOSA_T}, { 0,  1,  ICOSA_T}, { 0, -1, -ICOSA_T}, { 0,  1, -ICOSA_T},
    { ICOSA_T,  0, -1}, { ICOSA_T,  0,  1}, {-ICOSA_T,  0, -1}, {-ICOSA_T,  0,  1}
};
static const int ICOSA_FACES[20][3] = {
    {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
    {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
    {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
    {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}
};

// Regular dodecahedron: 20 vertices, 12 pentagonal faces, 30 edges. Verts are
// the 8 cube corners plus the three rectangle quartets (golden-ratio
// construction). NOT unit length - normalize before use.
static const float DODECA_INV_T = 1.0f / ICOSA_T; // 1/phi
static const Vector3 DODECA_VERTS[20] = {
    {-1,-1,-1}, {-1,-1, 1}, {-1, 1,-1}, {-1, 1, 1}, // cube corners 0..3
    { 1,-1,-1}, { 1,-1, 1}, { 1, 1,-1}, { 1, 1, 1}, // cube corners 4..7
    {0, -DODECA_INV_T, -ICOSA_T}, {0, -DODECA_INV_T, ICOSA_T},
    {0,  DODECA_INV_T, -ICOSA_T}, {0,  DODECA_INV_T, ICOSA_T},     // (0, ±1/phi, ±phi) 8..11
    {-DODECA_INV_T, -ICOSA_T, 0}, {-DODECA_INV_T, ICOSA_T, 0},
    { DODECA_INV_T, -ICOSA_T, 0}, { DODECA_INV_T, ICOSA_T, 0},     // (±1/phi, ±phi, 0) 12..15
    {-ICOSA_T, 0, -DODECA_INV_T}, {-ICOSA_T, 0, DODECA_INV_T},
    { ICOSA_T, 0, -DODECA_INV_T}, { ICOSA_T, 0, DODECA_INV_T}      // (±phi, 0, ±1/phi) 16..19
};

// Max |y| of a normalized dodecahedron vertex (the (±1/phi, ±phi, 0) verts):
// phi/sqrt(3). Sizing the body's circumradius by this makes its vertical extent
// fill the box HEIGHT, so it rests on the floor (and, being centrally symmetric,
// its top reaches eye level).
static const float DODECA_Y_EXTENT = 0.9341724f;

// The 12 pentagonal faces, each listing its 5 vertices in cyclic (adjacency)
// order. Precomputed from DODECA_VERTS (min-edge adjacency -> the twelve planar
// 5-cycles -> CCW order around each face's centroid normal); validated as 30
// unique edges, each shared by exactly 2 faces.
static const int DODECA_FACES[12][5] = {
    { 6,10, 2,13,15}, { 3,11, 7,15,13}, {16,17, 3,13, 2},
    {19,18, 6,15, 7}, { 8, 0,16, 2,10}, { 1, 9,11, 3,17},
    { 4, 8,10, 6,18}, { 9, 5,19, 7,11}, { 0,12, 1,17,16},
    { 5,14, 4,18,19}, {14,12, 0, 8, 4}, {12,14, 5, 9, 1},
};

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

// Shaded-wire renderer for a pentagon-faced convex solid (the dodecahedron).
// Verts are scaled to `radius` and offset by `center`; each pentagon fills as a
// triangle fan (both windings) under de-duplicated wire edges.
inline void DrawShadedPolyhedron(Vector3 center, float radius, const Vector3 verts[], int vcount,
                                 const int faces[][5], int fcount, Color wireColor, Color fillColor) {
    // World-space vertices.
    Vector3 w[20]; // dodecahedron has 20; bump if a larger solid ever reuses this
    for (int i = 0; i < vcount; i++) {
        w[i] = Vector3Add(center, Vector3Scale(Vector3Normalize(verts[i]), radius));
    }

    // Fill: triangle-fan each pentagon, both windings.
    BeginTranslucentFill();
    for (int f = 0; f < fcount; f++) {
        for (int t = 1; t < 4; t++) {
            Vector3 a = w[faces[f][0]];
            Vector3 b = w[faces[f][t]];
            Vector3 d = w[faces[f][t + 1]];
            DrawTriangle3D(a, b, d, fillColor);
            DrawTriangle3D(a, d, b, fillColor);
        }
    }
    EndTranslucentFill();

    // Wire: each face's 5 edges, de-duplicated across faces via the i<j test.
    for (int f = 0; f < fcount; f++) {
        for (int e = 0; e < 5; e++) {
            int i = faces[f][e];
            int j = faces[f][(e + 1) % 5];
            if (i < j) DrawLine3D(w[i], w[j], wireColor);
        }
    }
}

// Regular dodecahedron body, upright. Jetpack exhaust is now a spawned spark
// particle system (see SpawnSparkCone / main.cpp), not drawn here.
inline void DrawPlayerDodeca(const Player& player) {
    Color outline, fill;
    PlayerFlashColors(player, outline, fill);

    const Vector3 c = player.position;
    // Size to the box HEIGHT (not width) so the body rests on the platform and,
    // being centrally symmetric, reaches eye level at the top.
    const float r = (0.5f * player.size.y) / DODECA_Y_EXTENT;

    DrawShadedPolyhedron(c, r, DODECA_VERTS, 20, DODECA_FACES, 12, outline, fill);
}

inline void DrawPlayer(const Player& player) {
    if (PLAYER_SHAPE == PLAYER_SHAPE_DODECA) { DrawPlayerDodeca(player); return; }
    if (PLAYER_SHAPE == PLAYER_SHAPE_POD)    { DrawPlayerPod(player);    return; }
    if (PLAYER_SHAPE == PLAYER_SHAPE_LANDER) { DrawPlayerLander(player); return; }

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
    DrawPlayerShip(player, PLAYER_SHAPE == PLAYER_SHAPE_DELTA ? deltaVerts : dartVerts);
}

// Builds an irregular, low-poly "vector rock" - the 3D analog of main2d.cpp's
// jittered asteroid polygon. Starts from the unit icosahedron above and
// displaces each vertex radially by a stable per-vertex jitter, so every
// asteroid gets its own lumpy silhouette. The shape (and its spin axis/speed)
// is derived deterministically from the asteroid's startingPosition - its seed
// - so it stays consistent frame to frame without storing any mesh on the
// Asteroid class. Drawn shaded-wire to match the other elements: translucent
// double-sided fill faces with bright edges on top.
inline void DrawAsteroidShape(const Asteroid& asteroid) {
    // Per-asteroid seed from its (stable) starting position.
    const Vector3 seed = asteroid.startingPosition;

    // Cheap deterministic hash -> [0,1) (classic GLSL fract(sin*) trick).
    auto hash01 = [](float a, float b, float c) {
        float h = sinf(a * 12.9898f + b * 78.233f + c * 37.719f) * 43758.5453f;
        return h - floorf(h);
    };

    // Tumble: spin around a per-asteroid axis, advanced by wall-clock time.
    Vector3 axis = Vector3{
        hash01(seed.x, seed.y, 1.0f) - 0.5f,
        hash01(seed.y, seed.z, 2.0f) - 0.5f,
        hash01(seed.z, seed.x, 3.0f) - 0.5f
    };
    if (Vector3LengthSqr(axis) < 1e-6f) axis = Vector3{0, 1, 0}; // guard a near-zero axis
    axis = Vector3Normalize(axis);
    float spinSpeed = 0.3f + 0.7f * hash01(seed.x, seed.z, 4.0f); // rad/sec, per asteroid
    float angle = (float)GetTime() * spinSpeed;

    // Build the displaced, oriented, world-space vertices.
    Vector3 verts[12];
    for (int i = 0; i < 12; i++) {
        Vector3 dir = Vector3Normalize(ICOSA_VERTS[i]);                     // point on the unit sphere
        float jitter = 0.72f + 0.28f * hash01(seed.x + i, seed.y, seed.z);  // 0.72..1.0, like the 2D version
        Vector3 local = Vector3Scale(dir, asteroid.size * jitter);
        local = Vector3RotateByAxisAngle(local, axis, angle);
        verts[i] = Vector3Add(asteroid.position, local);
    }

    // Damage flash: brighten the orange toward white on a recent hit, easing
    // back over flashIntensity()'s decay. ColorBrightness preserves alpha, so
    // the fill stays glassy. At flash == 0 these return the colors unchanged.
    float flash = asteroid.flashIntensity();
    Color fill    = ColorBrightness(asteroid.color_fill,    ASTEROID_FLASH_INTENSITY * flash);
    Color outline = ColorBrightness(asteroid.color_outline, ASTEROID_FLASH_INTENSITY * flash);

    // Fill faces - drawn both windings so the translucent glow reads from any
    // angle (backface culling leaves exactly one of each pair visible).
    BeginTranslucentFill();
    for (int f = 0; f < 20; f++) {
        Vector3 a = verts[ICOSA_FACES[f][0]];
        Vector3 b = verts[ICOSA_FACES[f][1]];
        Vector3 c = verts[ICOSA_FACES[f][2]];
        DrawTriangle3D(a, b, c, fill);
        DrawTriangle3D(a, c, b, fill);
    }
    EndTranslucentFill();

    // Wireframe edges on top. Each undirected edge appears in two faces with
    // opposite winding, so the i<j test draws it exactly once (30 edges total).
    for (int f = 0; f < 20; f++) {
        for (int e = 0; e < 3; e++) {
            int i = ICOSA_FACES[f][e];
            int j = ICOSA_FACES[f][(e + 1) % 3];
            if (i < j) DrawLine3D(verts[i], verts[j], outline);
        }
    }
}

//MARK: Wrappers
inline void DrawAsteroid(const Asteroid& asteroid) {
    DrawAsteroidShape(asteroid);
}

inline void DrawRocket(const Rocket& rocket) {
    // Simple wireframe for the rocket, no fill.
    // DrawWirePyramid(rocket.position, 0.0f, 1.5f, 0.5f, rocket.color_outline);
    DrawSphereWires(rocket.position, rocket.size, 6, 6, rocket.color_outline);
}

inline void DrawExplosion(const Explosion& explosion) {
    // Two-layer vector explosion, all driven by Explosion::update(dt) (called
    // each frame in gamespace.h), which grows radius 0 -> maxRadius:
    //   - a bright near-white outer shockwave shell (wire only) that flashes
    //     at the leading edge and fades as it expands
    //   - a slower orange core sphere with the translucent fill underneath
    float t = explosion.maxRadius > 0.0f ? (explosion.radius / explosion.maxRadius) : 1.0f;
    t = Clamp(t, 0.0f, 1.0f);

    // Outer shockwave: full current radius, near-white flash fading to nothing.
    unsigned char shockAlpha = (unsigned char)(255 * (1.0f - t));
    Color shockColor = {255, 230, 180, shockAlpha};
    DrawSphereWires(explosion.position, explosion.radius, 16, 16, shockColor);

    // Core: smaller orange sphere lagging the shockwave. Fill alpha is already
    // faded by Explosion::update; fade the wire outline over the lifetime too.
    float coreRadius = explosion.radius * 0.6f;
    unsigned char coreOutlineAlpha = (unsigned char)(255 * (1.0f - t));
    Color coreOutline = {explosion.color_outline.r, explosion.color_outline.g,
                         explosion.color_outline.b, coreOutlineAlpha};
    DrawShadedSphere(explosion.position, coreRadius, coreOutline, explosion.color_fill);
}