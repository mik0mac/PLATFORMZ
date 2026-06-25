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

// Unit icosahedron (golden-ratio construction): 12 vertices, 20 triangular
// faces, 30 edges. Used by DrawAsteroidShape (jittered/tumbling); ICOSA_T also
// seeds the dodecahedron below. Verts are NOT unit length - normalize before use.
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
    // Size the circumradius so the body's vertical extent (r * DODECA_Y_EXTENT)
    // equals player.radius - the sphere collider's radius - so the drawn body
    // spans position +/- radius and matches the collider.
    const float r = player.radius / DODECA_Y_EXTENT;

    DrawShadedPolyhedron(c, r, DODECA_VERTS, 20, DODECA_FACES, 12, outline, fill);
}

// The player renders as a regular dodecahedron. Earlier prototype silhouettes
// (DART, DELTA, LANDER, POD) are archived in docs/player-shapes-archive.md.
inline void DrawPlayer(const Player& player) {
    DrawPlayerDodeca(player);
}

//MARK: Reticle
// The reticle is a purely visual in-world object (no collision). It is drawn at
// ret.position in the plane perpendicular to the aim, using the (fwd, right, up)
// basis the caller built from the player's look direction. Two silhouettes,
// chosen by RETICLE_SHAPE (constants.h): a crosshair or corner brackets.

// Crosshair: a ring (with a faint translucent fill disc) + a center cross with a
// gap + a short prong poking forward along the aim so the facing reads in 3D.
inline void DrawReticleCrosshair(const Reticle& ret, Vector3 fwd, Vector3 right, Vector3 up) {
    const Vector3 c = ret.position;
    const float s = ret.size;
    const Color wire = ret.color;
    const Color fill = ColorAlpha(ret.color, 0.22f);

    // Ring vertices in the right/up plane.
    const int N = RETICLE_RING_SEGMENTS;
    Vector3 ring[64]; // RETICLE_RING_SEGMENTS is well under this
    for (int i = 0; i < N; i++) {
        float a = (float)i / (float)N * 2.0f * PI;
        Vector3 off = Vector3Add(Vector3Scale(right, s * cosf(a)),
                                 Vector3Scale(up,    s * sinf(a)));
        ring[i] = Vector3Add(c, off);
    }

    // Faint fill disc (both windings so it reads from either side).
    BeginTranslucentFill();
    for (int i = 0; i < N; i++) {
        int j = (i + 1) % N;
        DrawTriangle3D(c, ring[i], ring[j], fill);
        DrawTriangle3D(c, ring[j], ring[i], fill);
    }
    EndTranslucentFill();

    // Ring wire.
    for (int i = 0; i < N; i++) {
        DrawLine3D(ring[i], ring[(i + 1) % N], wire);
    }

    // Center cross: four spokes from the gap radius out to the ring.
    float gap = RETICLE_CROSS_GAP * s;
    Vector3 axes[4] = { right, Vector3Negate(right), up, Vector3Negate(up) };
    for (int k = 0; k < 4; k++) {
        Vector3 inner = Vector3Add(c, Vector3Scale(axes[k], gap));
        Vector3 outer = Vector3Add(c, Vector3Scale(axes[k], s));
        DrawLine3D(inner, outer, wire);
    }

    // Forward prong: a short segment along the aim so others read the facing.
    Vector3 prong = Vector3Add(c, Vector3Scale(fwd, RETICLE_PRONG_LENGTH * s));
    DrawLine3D(c, prong, wire);
}

// Corner brackets: four L-shaped brackets at the corners of a size-half-extent
// square in the right/up plane, plus a small center tick. Open look, no fill.
inline void DrawReticleBrackets(const Reticle& ret, Vector3 fwd, Vector3 right, Vector3 up) {
    (void)fwd;
    const Vector3 c = ret.position;
    const float s = ret.size;
    const float arm = RETICLE_BRACKET_LENGTH * s; // length of each bracket leg
    const Color wire = ret.color;

    // The four corners (±right ± up) at the frame's half-extent.
    Vector3 rt = Vector3Scale(right, s);
    Vector3 upv = Vector3Scale(up, s);
    Vector3 corners[4] = {
        Vector3Add(Vector3Add(c, rt), upv),                 // top-right
        Vector3Add(Vector3Subtract(c, rt), upv),            // top-left
        Vector3Subtract(Vector3Subtract(c, rt), upv),       // bottom-left
        Vector3Subtract(Vector3Add(c, rt), upv),            // bottom-right
    };
    // Inward directions (toward center along each axis) per corner.
    Vector3 inX[4] = { Vector3Negate(right), right, right, Vector3Negate(right) };
    Vector3 inY[4] = { Vector3Negate(up), Vector3Negate(up), up, up };
    for (int k = 0; k < 4; k++) {
        DrawLine3D(corners[k], Vector3Add(corners[k], Vector3Scale(inX[k], arm)), wire);
        DrawLine3D(corners[k], Vector3Add(corners[k], Vector3Scale(inY[k], arm)), wire);
    }

    // Center tick: a tiny cross marking the exact aim point.
    float dot = 0.12f * s;
    DrawLine3D(Vector3Subtract(c, Vector3Scale(right, dot)), Vector3Add(c, Vector3Scale(right, dot)), wire);
    DrawLine3D(Vector3Subtract(c, Vector3Scale(up, dot)),    Vector3Add(c, Vector3Scale(up, dot)),    wire);
}

inline void DrawReticle(const Player& player) {
    const Reticle& ret = player.reticle;

    // Orthonormal aim basis. Guard the near-vertical case where Forward() is
    // parallel to world-up (cross product would collapse).
    Vector3 fwd = player.Forward();
    Vector3 up0 = (fabsf(fwd.y) < 0.99f) ? Vector3{0, 1, 0} : Vector3{1, 0, 0};
    Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, up0));
    Vector3 up    = Vector3Normalize(Vector3CrossProduct(right, fwd));

    // Apply roll (rotation about the aim axis) to the in-plane basis.
    if (ret.rotation != 0.0f) {
        right = Vector3Normalize(Vector3RotateByAxisAngle(right, fwd, ret.rotation));
        up    = Vector3Normalize(Vector3RotateByAxisAngle(up,    fwd, ret.rotation));
    }

    if (RETICLE_SHAPE == RETICLE_SHAPE_BRACKETS) DrawReticleBrackets(ret, fwd, right, up);
    else                                         DrawReticleCrosshair(ret, fwd, right, up);
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

// Stella octangula (stellated octahedron): two interpenetrating regular
// tetrahedra forming an 8-pointed star polyhedron. The 8 verts are the cube
// corners split by sign-parity: product of signs +1 -> tetra A, -1 -> tetra B.
// NOT unit length (corner length = sqrt 3) - scale before use.
static const Vector3 STAR_VERTS[8] = {
    { 1, 1, 1}, { 1,-1,-1}, {-1, 1,-1}, {-1,-1, 1}, // tetra A (sign product +)
    {-1,-1,-1}, {-1, 1, 1}, { 1,-1, 1}, { 1, 1,-1}, // tetra B (sign product -)
};
static const int STAR_TETRA[2][4] = { {0, 1, 2, 3}, {4, 5, 6, 7} };
static const int STAR_FACES[8][3] = {
    {0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3},     // tetra A faces
    {4, 5, 6}, {4, 5, 7}, {4, 6, 7}, {5, 6, 7},     // tetra B faces
};

// The rocket is drawn as a star polyhedron spinning rapidly about its travel
// axis. Collision is unchanged - it still treats the rocket as a sphere of
// `size` (collisions.cpp); this is purely visual. Circumradius == rocket.size,
// so the star's points sit on the collision sphere.
inline void DrawRocket(const Rocket& rocket) {
    // Spin axis = direction of travel (current velocity, falling back to the
    // fired direction), so the spin follows the path even as gravity curves it.
    Vector3 travel = rocket.velocity;
    float speed = Vector3Length(travel);
    Vector3 axisT = (speed > 1e-4f) ? Vector3Scale(travel, 1.0f / speed)
                                    : Vector3Normalize(rocket.direction);
    if (!(Vector3LengthSqr(axisT) > 0.5f)) axisT = Vector3{0, 1, 0}; // last-ditch guard

    // Orientation: map the star's local +Y onto the travel axis (axis-angle).
    Vector3 yAxis = {0, 1, 0};
    Vector3 oCross = Vector3CrossProduct(yAxis, axisT);
    float oLen = Vector3Length(oCross);
    float oAngle = acosf(Clamp(Vector3DotProduct(yAxis, axisT), -1.0f, 1.0f));
    Vector3 oAxis = (oLen < 1e-6f) ? Vector3{1, 0, 0} : Vector3Scale(oCross, 1.0f / oLen);

    // Rapid spin about the (local +Y ->) travel axis, advanced by wall-clock time.
    float spin = (float)GetTime() * ROCKET_SPIN_SPEED;

    const float s = rocket.size / 1.7320508f; // circumradius == rocket.size (corner len = sqrt 3)

    Vector3 v[8];
    for (int i = 0; i < 8; i++) {
        Vector3 p = Vector3Scale(STAR_VERTS[i], s);
        p = Vector3RotateByAxisAngle(p, yAxis, spin);   // spin about local Y...
        p = Vector3RotateByAxisAngle(p, oAxis, oAngle); // ...which now points along travel
        v[i] = Vector3Add(rocket.position, p);
    }

    // Translucent fill (both windings) under the bright wireframe - house style.
    BeginTranslucentFill();
    for (int f = 0; f < 8; f++) {
        Vector3 a = v[STAR_FACES[f][0]];
        Vector3 b = v[STAR_FACES[f][1]];
        Vector3 c = v[STAR_FACES[f][2]];
        DrawTriangle3D(a, b, c, rocket.color_fill);
        DrawTriangle3D(a, c, b, rocket.color_fill); // reverse winding
    }
    EndTranslucentFill();

    // Wire: each tetrahedron's 6 edges (all vertex pairs).
    for (int t = 0; t < 2; t++) {
        for (int i = 0; i < 4; i++) {
            for (int j = i + 1; j < 4; j++) {
                DrawLine3D(v[STAR_TETRA[t][i]], v[STAR_TETRA[t][j]], rocket.color_outline);
            }
        }
    }
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