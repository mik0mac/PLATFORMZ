// How to draw the wireframe shapes for the various elements.
#pragma once

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <vector>
#include <string>
#include <algorithm>
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
// Two-pass draw: every shaded element is drawn twice by GameSpace::draw() - once
// for its opaque wireframe (PASS_WIRE, writes depth) and once for its translucent
// fill (PASS_FILL, no depth write). Batching ALL wires then ALL fills means the
// depth-mask toggle + batch flush happens ONCE per frame (below) instead of twice
// per object, which was ~56 forced GPU flushes/frame and the main framerate cost.
enum DrawPass { PASS_WIRE, PASS_FILL };

// Bracket the single per-frame fill pass. Flush so the mask change only affects
// the fill vertices, not the opaque wireframes drawn before them.
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
inline void DrawShadedWireBox(Vector3 position, float width, float height, float depth, float rotY, Color wireColor, Color fillColor, DrawPass pass) {
    rlPushMatrix();
    rlTranslatef(position.x, position.y, position.z);
    rlRotatef(rotY * RAD2DEG, 0, 1, 0);

    if (pass == PASS_FILL) DrawCube(Vector3Zero(), width, height, depth, fillColor);
    else                   DrawCubeWires(Vector3Zero(), width, height, depth, wireColor);

    rlPopMatrix();
}

// Same fill+wireframe layering as DrawShadedWireBox, but for a sphere.
inline void DrawShadedSphere(Vector3 position, float radius, Color wireColor, Color fillColor, DrawPass pass) {
    if (pass == PASS_FILL) DrawSphere(position, radius, fillColor);
    else                   DrawSphereWires(position, radius, 16, 16, wireColor);
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
    // Spacing scales with map size (gridSpacing is the floor / small-map look),
    // capping the grid's line count no matter how big the arena gets. Derived
    // here at the draw site because the networked client sets walls.halfSize
    // straight from the welcome packet, never through configureMap.
    float spacing = fmaxf(walls.gridSpacing, walls.halfSize / WALL_GRID_REF_HALF_SIZE);
    DrawGridRoom(walls.halfSize, spacing, walls.color_outline);
}

// flash (0..1, GameSpace::platformBlastFlash): an explosion's damage radius
// reaches this platform - wire and fill heat toward the blast orange.
inline void DrawPlatform(const Platform& platform, DrawPass pass, float flash = 0.0f) {
    Color outline = platform.color_outline;
    Color fill = platform.color_fill;
    if (flash > 0.0f) {
        Color hot = {255, 170, 60, 255};
        outline = ColorLerp(outline, hot, flash);
        hot.a = PLATFORM_BLAST_FILL_ALPHA;
        fill = ColorLerp(fill, hot, flash);
    }
    DrawShadedWireBox(platform.position, platform.size.x, platform.size.y, platform.size.z, 0.0f, outline, fill, pass);
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
                                 const int faces[][5], int fcount, Color wireColor, Color fillColor,
                                 DrawPass pass) {
    // World-space vertices.
    Vector3 w[20]; // dodecahedron has 20; bump if a larger solid ever reuses this
    for (int i = 0; i < vcount; i++) {
        w[i] = Vector3Add(center, Vector3Scale(Vector3Normalize(verts[i]), radius));
    }

    if (pass == PASS_FILL) {
        // Fill: triangle-fan each pentagon, both windings.
        for (int f = 0; f < fcount; f++) {
            for (int t = 1; t < 4; t++) {
                Vector3 a = w[faces[f][0]];
                Vector3 b = w[faces[f][t]];
                Vector3 d = w[faces[f][t + 1]];
                DrawTriangle3D(a, b, d, fillColor);
                DrawTriangle3D(a, d, b, fillColor);
            }
        }
    } else {
        // Wire: each face's 5 edges, de-duplicated across faces via the i<j test.
        for (int f = 0; f < fcount; f++) {
            for (int e = 0; e < 5; e++) {
                int i = faces[f][e];
                int j = faces[f][(e + 1) % 5];
                if (i < j) DrawLine3D(w[i], w[j], wireColor);
            }
        }
    }
}

// Regular dodecahedron body, upright. (The jetpack exhaust plume was deprecated
// and archived in docs/archive/exhaust-plume.md.)
inline void DrawPlayerDodeca(const Player& player, DrawPass pass) {
    Color outline, fill;
    PlayerFlashColors(player, outline, fill);

    const Vector3 c = player.position;
    // Size the circumradius so the body's vertical extent (r * DODECA_Y_EXTENT)
    // equals player.radius - the sphere collider's radius - so the drawn body
    // spans position +/- radius and matches the collider.
    const float r = player.radius / DODECA_Y_EXTENT;

    DrawShadedPolyhedron(c, r, DODECA_VERTS, 20, DODECA_FACES, 12, outline, fill, pass);
}

// The player renders as a regular dodecahedron. Earlier prototype silhouettes
// (DART, DELTA, LANDER, POD) are archived in docs/archive/player-shapes.md.
inline void DrawPlayer(const Player& player, DrawPass pass) {
    DrawPlayerDodeca(player, pass);
}

//MARK: Reticle
// The reticle is a purely visual in-world object (no collision). It is drawn at
// ret.position in the plane perpendicular to the aim, using the (fwd, right, up)
// basis the caller built from the player's look direction. Two silhouettes,
// chosen by RETICLE_SHAPE (constants.h): a crosshair or corner brackets.

// Crosshair: a ring (with a faint translucent fill disc) + a center cross with a
// gap + a short prong poking forward along the aim so the facing reads in 3D.
inline void DrawReticleCrosshair(const Reticle& ret, Vector3 fwd, Vector3 right, Vector3 up, DrawPass pass) {
    const Vector3 c = ret.position;
    const float s = ret.size;
    const Color wire = ret.color;
    const Color fill = ColorAlpha(ret.color, 0.22f);

    // Ring vertices in the right/up plane (needed by both the fill disc and ring wire).
    const int N = RETICLE_RING_SEGMENTS;
    Vector3 ring[64]; // RETICLE_RING_SEGMENTS is well under this
    for (int i = 0; i < N; i++) {
        float a = (float)i / (float)N * 2.0f * PI;
        Vector3 off = Vector3Add(Vector3Scale(right, s * cosf(a)),
                                 Vector3Scale(up,    s * sinf(a)));
        ring[i] = Vector3Add(c, off);
    }

    if (pass == PASS_FILL) {
        // Faint fill disc (both windings so it reads from either side).
        for (int i = 0; i < N; i++) {
            int j = (i + 1) % N;
            DrawTriangle3D(c, ring[i], ring[j], fill);
            DrawTriangle3D(c, ring[j], ring[i], fill);
        }
        return;
    }

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
inline void DrawReticleBrackets(const Reticle& ret, Vector3 fwd, Vector3 right, Vector3 up, DrawPass pass) {
    (void)fwd;
    if (pass == PASS_FILL) return; // brackets are wire-only (no fill)
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

inline void DrawReticle(const Player& player, DrawPass pass) {
    const Reticle& ret = player.reticle;

    // Orthonormal aim basis, built from world-up so it matches the camera's own
    // up = {0,1,0} and stays screen-aligned. Pitch is clamped to 89° (see
    // elements.h), so cross(fwd, worldUp) is always small-but-nonzero near
    // vertical and never collapses - no reference-up switch needed (switching it
    // would snap the basis and visibly rotate the reticle).
    Vector3 fwd   = player.Forward();
    Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, Vector3{0, 1, 0}));
    Vector3 up    = Vector3Normalize(Vector3CrossProduct(right, fwd));

    // Apply roll (rotation about the aim axis) to the in-plane basis.
    if (ret.rotation != 0.0f) {
        right = Vector3Normalize(Vector3RotateByAxisAngle(right, fwd, ret.rotation));
        up    = Vector3Normalize(Vector3RotateByAxisAngle(up,    fwd, ret.rotation));
    }

    if (RETICLE_SHAPE == RETICLE_SHAPE_BRACKETS) DrawReticleBrackets(ret, fwd, right, up, pass);
    else                                         DrawReticleCrosshair(ret, fwd, right, up, pass);
}

// Builds an irregular, low-poly "vector rock" - the 3D analog of main2d.cpp's
// jittered asteroid polygon. Starts from the unit icosahedron above and
// displaces each vertex radially by a stable per-vertex jitter, so every
// asteroid gets its own lumpy silhouette. The shape (and its spin axis/speed)
// is derived deterministically from the asteroid's startingPosition - its seed
// - so it stays consistent frame to frame without storing any mesh on the
// Asteroid class. Drawn shaded-wire to match the other elements: translucent
// double-sided fill faces with bright edges on top.
// `fade` scales both alphas (1 = normal). Open-space mode passes the asteroid's
// distance-based boundary fade so it dissolves on the way out to the
// out-of-bounds line and re-materializes after wrapping to the far side.
inline void DrawAsteroidShape(const Asteroid& asteroid, DrawPass pass, float fade = 1.0f) {
    // Per-asteroid seed from its (stable) starting position.
    const Vector3 seed = asteroid.startingPosition;

    // Tumble: spin around a per-asteroid axis, advanced by wall-clock time.
    // Hash01 (random.h) gives stable per-vertex "randomness" from the seed.
    Vector3 axis = Vector3{
        Hash01(seed.x, seed.y, 1.0f) - 0.5f,
        Hash01(seed.y, seed.z, 2.0f) - 0.5f,
        Hash01(seed.z, seed.x, 3.0f) - 0.5f
    };
    if (Vector3LengthSqr(axis) < 1e-6f) axis = Vector3{0, 1, 0}; // guard a near-zero axis
    axis = Vector3Normalize(axis);
    float angle = asteroid.spinAngle; // pre-integrated each frame (no GetTime() snap on bounce)

    // Build the displaced, oriented, world-space vertices.
    Vector3 verts[12];
    for (int i = 0; i < 12; i++) {
        Vector3 dir = Vector3Normalize(ICOSA_VERTS[i]);                     // point on the unit sphere
        float jitter = 0.72f + 0.28f * Hash01(seed.x + i, seed.y, seed.z);  // 0.72..1.0, like the 2D version
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
    if (fade < 1.0f) {
        fill.a    = (unsigned char)((float)fill.a    * fade);
        outline.a = (unsigned char)((float)outline.a * fade);
    }

    if (pass == PASS_FILL) {
        // Fill faces - drawn both windings so the translucent glow reads from any
        // angle (backface culling leaves exactly one of each pair visible).
        for (int f = 0; f < 20; f++) {
            Vector3 a = verts[ICOSA_FACES[f][0]];
            Vector3 b = verts[ICOSA_FACES[f][1]];
            Vector3 c = verts[ICOSA_FACES[f][2]];
            DrawTriangle3D(a, b, c, fill);
            DrawTriangle3D(a, c, b, fill);
        }
    } else {
        // Wireframe edges. Each undirected edge appears in two faces with opposite
        // winding, so the i<j test draws it exactly once (30 edges total).
        for (int f = 0; f < 20; f++) {
            for (int e = 0; e < 3; e++) {
                int i = ICOSA_FACES[f][e];
                int j = ICOSA_FACES[f][(e + 1) % 3];
                if (i < j) DrawLine3D(verts[i], verts[j], outline);
            }
        }
    }
}

//MARK: Wrappers
inline void DrawAsteroid(const Asteroid& asteroid, DrawPass pass, float fade = 1.0f) {
    DrawAsteroidShape(asteroid, pass, fade);
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
inline void DrawRocket(const Rocket& rocket, DrawPass pass) {
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

    if (pass == PASS_FILL) {
        // Translucent fill (both windings) under the bright wireframe - house style.
        for (int f = 0; f < 8; f++) {
            Vector3 a = v[STAR_FACES[f][0]];
            Vector3 b = v[STAR_FACES[f][1]];
            Vector3 c = v[STAR_FACES[f][2]];
            DrawTriangle3D(a, b, c, rocket.color_fill);
            DrawTriangle3D(a, c, b, rocket.color_fill); // reverse winding
        }
    } else {
        // Wire: each tetrahedron's 6 edges (all vertex pairs).
        for (int t = 0; t < 2; t++) {
            for (int i = 0; i < 4; i++) {
                for (int j = i + 1; j < 4; j++) {
                    DrawLine3D(v[STAR_TETRA[t][i]], v[STAR_TETRA[t][j]], rocket.color_outline);
                }
            }
        }
    }
}

// A circle facing the eye: lies in the plane through `center` perpendicular to
// the view direction, so it always reads as a clean round vector ring.
// Is p inside the arena cube of half-size `half`? half <= 0 means no cube
// (walls off - open space), so everything is inside.
inline bool InsideArena(Vector3 p, float half) {
    return half <= 0.0f || (fabsf(p.x) <= half && fabsf(p.y) <= half && fabsf(p.z) <= half);
}

// clipHalf > 0 drops the segments that fall outside that arena cube (by their
// midpoint - fine at ring resolution).
inline void DrawBillboardCircle(Vector3 center, float radius, Vector3 eye, int segments, Color col,
                                float clipHalf = 0.0f) {
    Vector3 fwd = Vector3Normalize(Vector3Subtract(center, eye));
    Vector3 up = (fabsf(fwd.y) > 0.99f) ? Vector3{1, 0, 0} : Vector3{0, 1, 0};
    Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, up));
    up = Vector3CrossProduct(right, fwd);
    Vector3 prev = Vector3Add(center, Vector3Scale(right, radius));
    for (int i = 1; i <= segments; ++i) {
        float a = 2.0f * PI * (float)i / (float)segments;
        Vector3 p = Vector3Add(center, Vector3Add(Vector3Scale(right, cosf(a) * radius),
                                                  Vector3Scale(up,    sinf(a) * radius)));
        if (InsideArena(Vector3Lerp(prev, p, 0.5f), clipHalf)) DrawLine3D(prev, p, col);
        prev = p;
    }
}

// The on-screen outline of a sphere of radius r: seen from distance d it is the
// circle r*r/d toward the eye, of radius r*sqrt(1 - r*r/d*d). Drawing that ring
// means it lines up exactly with what is inside the sphere at any range. No
// outline exists from inside the sphere, so it draws nothing there.
inline void DrawSphereSilhouette(Vector3 center, float r, Vector3 eye, Color col, float clipHalf = 0.0f) {
    Vector3 toEye = Vector3Subtract(eye, center);
    float d = Vector3Length(toEye);
    if (d <= r) return;
    Vector3 c = Vector3Add(center, Vector3Scale(toEye, (r * r / d) / d));
    DrawBillboardCircle(c, r * sqrtf(1.0f - (r * r) / (d * d)), eye, EXPLOSION_RING_SEGMENTS, col, clipHalf);
}

// Points filling the unit ball, built once: a Halton sequence (bases 2, 3, 5),
// so ANY prefix of it is still evenly spread - which is what lets the count
// follow the blast's size. Each point: unit direction, radial fraction, and a
// speed factor 1 +/- DOT_SPREAD. The radial fraction is u^(1/RADIAL_POWER):
// 3 (the cube root) spreads them evenly by volume; higher bunches them toward
// the surface (a shell), lower packs the centre and thins the edges.
struct ExplosionDot { Vector3 dir; float frac; float speed; };

inline float Halton(int i, int base) {
    float f = 1.0f, r = 0.0f;
    for (; i > 0; i /= base) { f /= (float)base; r += f * (float)(i % base); }
    return r;
}

inline const std::vector<ExplosionDot>& ExplosionDotPoints() {
    static const std::vector<ExplosionDot> pts = [] {
        std::vector<ExplosionDot> v;
        v.reserve(EXPLOSION_DOT_MAX);
        for (int i = 1; i <= EXPLOSION_DOT_MAX; ++i) {
            float z = 1.0f - 2.0f * Halton(i, 2);
            float a = 2.0f * PI * Halton(i, 3);
            float r = sqrtf(fmaxf(0.0f, 1.0f - z * z));
            float h = fmodf(sinf((float)i * 12.9898f) * 43758.5453f, 1.0f); // -1..1
            v.push_back({{r * cosf(a), z, r * sinf(a)}, powf(Halton(i, 5), 1.0f / EXPLOSION_DOT_RADIAL_POWER),
                         1.0f + EXPLOSION_DOT_SPREAD * h});
        }
        return v;
    }();
    return pts;
}

// How many streaks fill a blast of radius R (see EXPLOSION_DOT_SIZE_POWER).
inline int ExplosionDotCount(float R) {
    float n = EXPLOSION_DOT_COUNT * powf(R / EXPLOSION_DAMAGE_RADIUS, EXPLOSION_DOT_SIZE_POWER);
    return (int)Clamp(n, (float)EXPLOSION_DOT_MIN, (float)EXPLOSION_DOT_MAX);
}

// clipHalf: the arena cube's half-size (0 = walls off). A blast against a wall
// is cut off at it rather than drawing its whole sphere out past the boundary.
inline void DrawBlastRing(const BlastFx& blast, Vector3 eye, float clipHalf = 0.0f) {
    // Splash damage lands in full on the first frame, out to the whole radius
    // (ApplyExplosionSplashDamage). Wire only - the damage zone's interior is
    // lit by the blast light in main.cpp's world shader, and platforms in it
    // flash (DrawPlatform).
    float t = blast.t();
    float R = blast.radius;
    Color flash = {255, 240, 210, 255};

    // Zone ring: the damage radius's outline, there from frame 0 because that is
    // when the damage lands. Near-white easing to orange, and gone early so the
    // blast doesn't leave a hard border behind.
    if (t < EXPLOSION_RING_FADE_T) {
        float k = 1.0f - t / EXPLOSION_RING_FADE_T;
        Color ring = ColorLerp(flash, EXPLOSION_COLOR, Clamp(t / EXPLOSION_FLASH_T, 0.0f, 1.0f));
        ring.a = (unsigned char)(255 * k * k);
        DrawSphereSilhouette(blast.position, R, eye, ring, clipHalf);
    }
}

// The explosion's streak dots (BlastFx, elements.h), on dotT - their own
// lifetime (EXPLOSION_DOT_LIFETIME), so they can outlast the ring and flash.
//
// Shockwave: a 3D ball of spark streaks, filling the sphere, that rushes out,
// decelerates, and carries on past the damage radius, thickening into a cloud
// as it fades. The streaks are the shooter's colour from the first frame (no
// white-hot flash - the platforms already flash), so they stand apart from
// the orange glow; gameSpace.draw() blends them additively, on top. Each streak
// trails back toward the centre along its own direction, like DrawSpark, and its
// length follows its current speed: long while the shell races out, spark-sized
// once it coasts. The far side of the shell draws dimmer; unlike a silhouette
// ring it stays visible from inside. The shell starts at R * DOT_START and
// travels the rest of the way to R * DOT_REACH; DOT_START 0 is the original
// from-the-centre burst.
//
// Everything per BLAST is worked out once here; everything per STREAK is the
// same few lines of maths, run either on the GPU (ShockDotsGpu, below) or, if
// that shader won't build, on the CPU (DrawBlastDotLines).
struct ShockShell {
    float startR, shockR;
    float lenScale;   // shockSpeed * DOT_STREAK_TIME: a streak's length per (speed * frac)
    float alpha;      // 0..1, the whole shell's fade
    Matrix spin;
    bool inside;      // eye within the shell: no far-side dimming
};

// Heat: white-hot (EXPLOSION_DOT_HOT_COLOR) at the point of impact, cooling to
// the shooter's colour at the edge of the damage radius R, and staying that
// colour past it. EXPLOSION_DOT_HOT_POWER shapes the falloff (1 = linear,
// higher = a tighter hot core). Returns 0 (player colour) .. 1 (hottest).
inline float ShockHeat(float distFromCenter, float R) {
    if (R <= 0.0f) return 0.0f;
    return powf(1.0f - Clamp(distFromCenter / R, 0.0f, 1.0f), EXPLOSION_DOT_HOT_POWER);
}

inline bool ShockShellFor(const BlastFx& burst, Vector3 eye, ShockShell& s) {
    float t = burst.dotT();
    float R = burst.radius;
    Vector3 p0 = burst.position;
    s.startR = R * EXPLOSION_DOT_START;
    float travel = R * fmaxf(EXPLOSION_DOT_REACH - EXPLOSION_DOT_START, 0.0f);
    s.shockR = s.startR + travel * (1.0f - powf(1.0f - t, EXPLOSION_DOT_EASE));
    if (s.shockR <= 0.0f) return false;
    s.alpha = EXPLOSION_DOT_ALPHA * powf(1.0f - t, 1.5f);
    // d(shockR)/dt in units/sec: the curve's slope over t, over the burst's life.
    float shockSpeed = travel * EXPLOSION_DOT_EASE
                     * powf(1.0f - t, EXPLOSION_DOT_EASE - 1.0f) / EXPLOSION_DOT_LIFETIME;
    s.lenScale = shockSpeed * EXPLOSION_DOT_STREAK_TIME;
    // Rotate the pattern per blast, hashed from its position: two blasts don't
    // share one, and every client draws the same (no extra synced state).
    s.spin = MatrixRotateXYZ({p0.x * 1.7f + p0.z * 0.3f, p0.y * 2.3f + p0.x * 0.5f, p0.z * 1.1f + p0.y * 0.7f});
    s.inside = Vector3Distance(eye, p0) <= s.shockR;
    return true;
}

// The CPU path: one 1-pixel DrawLine3D per streak. Only used when the GPU
// shader failed to build, so it is capped well below EXPLOSION_DOT_MAX.
inline void DrawBlastDotLines(const BlastFx& burst, Vector3 eye, float clipHalf) {
    ShockShell s;
    if (!ShockShellFor(burst, eye, s)) return;
    const std::vector<ExplosionDot>& pts = ExplosionDotPoints();
    int count = std::min(ExplosionDotCount(burst.radius), EXPLOSION_DOT_CPU_MAX);
    for (int i = 0; i < count; ++i) {
        const ExplosionDot& d = pts[i];
        Vector3 dir = Vector3Transform(d.dir, s.spin);
        // The whole ball expands: each streak sits at its fraction of its own
        // (spread) shell radius, so inner streaks move slower, like a real blast.
        float shell = s.startR + (s.shockR - s.startR) * d.speed;
        float dist = shell * d.frac;
        Vector3 p = Vector3Add(burst.position, Vector3Scale(dir, dist));
        if (!InsideArena(p, clipHalf)) continue;
        float len = Clamp(s.lenScale * d.speed * d.frac,
                          SPARK_STREAK_LENGTH, fmaxf(dist, SPARK_STREAK_LENGTH));
        float k = 1.0f;
        if (!s.inside) {
            float facing = Vector3DotProduct(dir, Vector3Normalize(Vector3Subtract(eye, p)));
            float f = Clamp((facing + 0.2f) / 0.4f, 0.0f, 1.0f);
            k = Lerp(EXPLOSION_DOT_BACK_ALPHA, 1.0f, f * f * (3.0f - 2.0f * f));
        }
        Color c = ColorLerp(burst.color, EXPLOSION_DOT_HOT_COLOR, ShockHeat(dist, burst.radius));
        c.a = (unsigned char)Clamp(255.0f * s.alpha * k, 0.0f, 255.0f);
        DrawLine3D(p, Vector3Subtract(p, Vector3Scale(dir, len)), c);
    }
}

//MARK: Explosion streaks on the GPU
// The streak table (ExplosionDotPoints) never changes - only the blast's
// centre, size, clock, colour and spin do. So it lives on the GPU as one static
// vertex buffer, uploaded once, and a blast is a single draw call: the vertex
// shader does the per-streak maths above. The CPU's cost no longer depends on
// the streak count, which is what lets EXPLOSION_DOT_COUNT be large.
//
// Each streak is a quad (two triangles, 6 vertices, no index buffer): its head
// and tail projected to the screen, widened by EXPLOSION_DOT_WIDTH_PX at right
// angles to the streak and capped by half that at each end, so a streak that
// has slowed to almost nothing is still a visible square dot. Width follows
// perspective (EXPLOSION_DOT_WIDTH_REF_DIST) - the near side of a ball chunky,
// the far side fine - but is clamped in pixels, so a far blast stays legible.
//
// Same two GLSL dialects as main.cpp's world shader. The direction rides in
// `vertexPosition` on purpose: raylib binds that name to attribute 0, and
// WebGL1 is slow (or warns) when attribute 0 isn't an enabled array.
// Fog matches the world shader's; the blast light is deliberately left out, so
// the streaks keep the shooter's colour instead of being tinted orange. The
// fragment shader discards anything outside the arena cube, per pixel, so a
// streak crossing a wall is cut exactly at it.
struct ShockDotsGpu {
    bool tried = false, ok = false;
    Shader shader{};
    unsigned int vao = 0, vbo = 0;
    int locParam = -1;
    int uMvp = -1, uSpin = -1, uCenter = -1, uShell = -1, uEye = -1, uInside = -1;
    int uView = -1, uWidth = -1, uBack = -1, uColor = -1, uFog = -1, uArena = -1, uWidthRef = -1;
    int uDamageR = -1, uHotPower = -1, uHotColor = -1;
};

inline constexpr int SHOCK_DOT_FLOATS = 7; // dir xyz, frac, speed, along, side
inline constexpr int SHOCK_DOT_STRIDE = SHOCK_DOT_FLOATS * (int)sizeof(float);

inline void ShockDotsBindAttribs(const ShockDotsGpu& g) {
    rlSetVertexAttribute(0, 3, RL_FLOAT, false, SHOCK_DOT_STRIDE, 0);
    rlEnableVertexAttribute(0);
    rlSetVertexAttribute((unsigned int)g.locParam, 4, RL_FLOAT, false, SHOCK_DOT_STRIDE, 3 * (int)sizeof(float));
    rlEnableVertexAttribute((unsigned int)g.locParam);
}

// Built on first use (it needs the GL context, so it can't run before
// InitWindow) and kept for the life of the process.
inline ShockDotsGpu& ShockDots() {
    static ShockDotsGpu g;
    if (g.tried) return g;
    g.tried = true;
#if defined(__EMSCRIPTEN__)
    const char* vHead =
        "#version 100\n"
        "#define IN attribute\n"
        "#define OUT varying\n";
    const char* fHead =
        "#version 100\n"
        "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
        "precision highp float;\n" // world distances reach ~1000 (fog)
        "#else\n"
        "precision mediump float;\n"
        "#endif\n"
        "#define IN varying\n"
        "#define OUT_COLOR gl_FragColor\n";
#else
    const char* vHead =
        "#version 330\n"
        "#define IN in\n"
        "#define OUT out\n";
    const char* fHead =
        "#version 330\n"
        "#define IN in\n"
        "out vec4 finalColor;\n"
        "#define OUT_COLOR finalColor\n";
#endif
    std::string vs = std::string(vHead) +
        "IN vec3 vertexPosition;\n"  // the streak's unit direction (before spin)
        "IN vec4 dotParam;\n"        // frac, speed, along (0 head / 1 tail), side (-1 / +1)
        "uniform mat4 mvp;\n"
        "uniform mat4 spin;\n"
        "uniform vec3 center;\n"
        "uniform float damageR;\n"   // heat reaches the player colour at this distance (ShockHeat)
        "uniform vec4 shell;\n"      // startR, shockR, lenScale, min length
        "uniform vec3 eye;\n"
        "uniform float inside;\n"
        "uniform vec2 viewPx;\n"
        "uniform vec3 widthPx;\n"    // width at widthRef, min, max (pixels)
        "uniform float widthRef;\n"  // distance (units) at which a streak is widthPx.x wide
        "uniform float backAlpha;\n"
        "uniform float hotPower;\n"  // heat falloff (ShockHeat)
        "OUT float vAlpha;\n"
        "OUT vec3 vWorld;\n"
        "OUT float vHeat;\n"
        "void main() {\n"
        "    vec3 dir = (spin * vec4(vertexPosition, 0.0)).xyz;\n"
        "    float r = shell.x + (shell.y - shell.x) * dotParam.y;\n"
        "    float dist = r * dotParam.x;\n"
        "    vec3 head = center + dir * dist;\n"
        "    float len = clamp(shell.z * dotParam.y * dotParam.x, shell.w, max(dist, shell.w));\n"
        "    vec3 tail = head - dir * len;\n"
        "    float k = 1.0;\n"
        "    if (inside < 0.5) {\n"
        "        float f = clamp((dot(dir, normalize(eye - head)) + 0.2) / 0.4, 0.0, 1.0);\n"
        "        k = mix(backAlpha, 1.0, f * f * (3.0 - 2.0 * f));\n"
        "    }\n"
        "    vAlpha = k;\n"
        "    vWorld = mix(head, tail, dotParam.z);\n"
        // Per end, so each streak cools along its length (inner end hotter).
        "    vHeat = pow(1.0 - clamp(length(vWorld - center) / max(damageR, 0.001), 0.0, 1.0), hotPower);\n"
        "    vec4 h = mvp * vec4(head, 1.0);\n"
        "    vec4 t = mvp * vec4(tail, 1.0);\n"
        // Behind (or at) the eye: drop the whole streak - every vertex of it
        // takes this branch, so it is clipped as a unit, never half-projected.
        "    if (h.w < 0.05 || t.w < 0.05) { gl_Position = vec4(0.0, 0.0, 2.0, 1.0); return; }\n"
        "    vec2 halfPx = viewPx * 0.5;\n"
        // Perspective width: near streaks thicker, far ones finer (h.w is the
        // head's distance along the view). One width per streak, so it stays a quad.
        "    float w = clamp(widthPx.x * widthRef / h.w, widthPx.y, widthPx.z);\n"
        "    vec2 d = (h.xy / h.w - t.xy / t.w) * halfPx;\n" // head - tail, in pixels
        "    float L = length(d);\n"
        "    vec2 ax = L > 0.001 ? d / L : vec2(1.0, 0.0);\n"
        "    vec2 nrm = vec2(-ax.y, ax.x);\n"
        "    vec4 p = mix(h, t, dotParam.z);\n"
        "    vec2 off = (nrm * dotParam.w + ax * (1.0 - 2.0 * dotParam.z)) * (0.5 * w);\n"
        "    p.xy += off / halfPx * p.w;\n"
        "    gl_Position = p;\n"
        "}\n";
    std::string fs = std::string(fHead) +
        "IN float vAlpha;\n"
        "IN vec3 vWorld;\n"
        "IN float vHeat;\n"
        "uniform vec4 color;\n"
        "uniform vec3 hotColor;\n"
        "uniform float arenaHalf;\n"   // 0 = walls off, no clip
        "uniform vec3 fogParams;\n"  // start, end, max - as the world shader
        "void main() {\n"
        "    if (arenaHalf > 0.0 && any(greaterThan(abs(vWorld), vec3(arenaHalf)))) discard;\n"
        "    float depth = 1.0 / gl_FragCoord.w;\n"
        "    float fog = fogParams.z * clamp((depth - fogParams.x) / (fogParams.y - fogParams.x), 0.0, 1.0);\n"
        "    OUT_COLOR = vec4(mix(color.rgb, hotColor, vHeat), color.a * vAlpha * (1.0 - fog));\n"
        "}\n";
    g.shader = LoadShaderFromMemory(vs.c_str(), fs.c_str());
    if (!IsShaderValid(g.shader) || g.shader.id == rlGetShaderIdDefault()) return g;
    g.locParam = rlGetLocationAttrib(g.shader.id, "dotParam");
    if (g.locParam < 0) return g;
    g.uMvp    = GetShaderLocation(g.shader, "mvp");
    g.uSpin   = GetShaderLocation(g.shader, "spin");
    g.uCenter = GetShaderLocation(g.shader, "center");
    g.uShell  = GetShaderLocation(g.shader, "shell");
    g.uEye    = GetShaderLocation(g.shader, "eye");
    g.uInside = GetShaderLocation(g.shader, "inside");
    g.uView   = GetShaderLocation(g.shader, "viewPx");
    g.uWidth  = GetShaderLocation(g.shader, "widthPx");
    g.uWidthRef = GetShaderLocation(g.shader, "widthRef");
    g.uDamageR  = GetShaderLocation(g.shader, "damageR");
    g.uHotPower = GetShaderLocation(g.shader, "hotPower");
    g.uHotColor = GetShaderLocation(g.shader, "hotColor");
    g.uBack   = GetShaderLocation(g.shader, "backAlpha");
    g.uColor  = GetShaderLocation(g.shader, "color");
    g.uFog    = GetShaderLocation(g.shader, "fogParams");
    g.uArena  = GetShaderLocation(g.shader, "arenaHalf");

    // Six vertices per streak: (along, side) corners of its quad.
    static const float corner[6][2] = {{0, -1}, {0, 1}, {1, 1}, {0, -1}, {1, 1}, {1, -1}};
    const std::vector<ExplosionDot>& pts = ExplosionDotPoints();
    std::vector<float> buf;
    buf.reserve(pts.size() * 6 * SHOCK_DOT_FLOATS);
    for (const ExplosionDot& d : pts)
        for (const auto& c : corner)
            buf.insert(buf.end(), {d.dir.x, d.dir.y, d.dir.z, d.frac, d.speed, c[0], c[1]});

    g.vao = rlLoadVertexArray(); // 0 where VAOs aren't supported (some WebGL1)
    bool vao = rlEnableVertexArray(g.vao);
    g.vbo = rlLoadVertexBuffer(buf.data(), (int)(buf.size() * sizeof(float)), false);
    if (g.vbo == 0) { if (vao) rlDisableVertexArray(); return g; }
    if (vao) { ShockDotsBindAttribs(g); rlDisableVertexArray(); }
    rlDisableVertexBuffer();
    g.ok = true;
    return g;
}

// Draw every streak shell. viewPx is the size of the target being drawn to,
// so the streak width comes out in real pixels; clipHalf is the arena cube's
// half-size (0 = walls off, no clip).
inline void DrawBlastDots(const std::vector<BlastFx>& bursts, Vector3 eye, Vector2 viewPx,
                            float clipHalf) {
    if (bursts.empty()) return;
    ShockDotsGpu& g = ShockDots();
    if (!g.ok) {
        for (const BlastFx& b : bursts) DrawBlastDotLines(b, eye, clipHalf);
        return;
    }
    rlDrawRenderBatchActive(); // anything queued draws first, with its own shader
    rlEnableShader(g.shader.id);
    Matrix mvp = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
    rlSetUniformMatrix(g.uMvp, mvp);
    float view[2] = {viewPx.x, viewPx.y};
    float eyeV[3] = {eye.x, eye.y, eye.z};
    float width[3] = {EXPLOSION_DOT_WIDTH_PX, EXPLOSION_DOT_WIDTH_MIN_PX, EXPLOSION_DOT_WIDTH_MAX_PX};
    float widthRef = EXPLOSION_DOT_WIDTH_REF_DIST, back = EXPLOSION_DOT_BACK_ALPHA;
    float fog[3] = {FOG_START, FOG_END, FOG_MAX};
    rlSetUniform(g.uView, view, RL_SHADER_UNIFORM_VEC2, 1);
    rlSetUniform(g.uEye, eyeV, RL_SHADER_UNIFORM_VEC3, 1);
    rlSetUniform(g.uWidth, width, RL_SHADER_UNIFORM_VEC3, 1);
    rlSetUniform(g.uWidthRef, &widthRef, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(g.uBack, &back, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(g.uFog, fog, RL_SHADER_UNIFORM_VEC3, 1);
    rlSetUniform(g.uArena, &clipHalf, RL_SHADER_UNIFORM_FLOAT, 1);
    float hotPower = EXPLOSION_DOT_HOT_POWER;
    float hot[3] = {EXPLOSION_DOT_HOT_COLOR.r / 255.0f, EXPLOSION_DOT_HOT_COLOR.g / 255.0f,
                    EXPLOSION_DOT_HOT_COLOR.b / 255.0f};
    rlSetUniform(g.uHotPower, &hotPower, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(g.uHotColor, hot, RL_SHADER_UNIFORM_VEC3, 1);

    bool vao = rlEnableVertexArray(g.vao);
    if (!vao) { rlEnableVertexBuffer(g.vbo); ShockDotsBindAttribs(g); }
    // Additive light doesn't need depth order, and shouldn't hide other
    // streaks; it still TESTS depth, so a platform in front covers them. Quads
    // face either way depending on the streak's screen direction - no culling.
    rlDisableDepthMask();
    rlDisableBackfaceCulling();
    for (const BlastFx& b : bursts) {
        ShockShell s;
        if (!ShockShellFor(b, eye, s)) continue;
        float center[3] = {b.position.x, b.position.y, b.position.z};
        float shell[4] = {s.startR, s.shockR, s.lenScale, SPARK_STREAK_LENGTH};
        float inside = s.inside ? 1.0f : 0.0f;
        rlSetUniform(g.uDamageR, &b.radius, RL_SHADER_UNIFORM_FLOAT, 1);
        float color[4] = {b.color.r / 255.0f, b.color.g / 255.0f, b.color.b / 255.0f, s.alpha};
        rlSetUniformMatrix(g.uSpin, s.spin);
        rlSetUniform(g.uCenter, center, RL_SHADER_UNIFORM_VEC3, 1);
        rlSetUniform(g.uShell, shell, RL_SHADER_UNIFORM_VEC4, 1);
        rlSetUniform(g.uInside, &inside, RL_SHADER_UNIFORM_FLOAT, 1);
        rlSetUniform(g.uColor, color, RL_SHADER_UNIFORM_VEC4, 1);
        rlDrawVertexArray(0, ExplosionDotCount(b.radius) * 6);
    }
    rlEnableBackfaceCulling();
    rlEnableDepthMask();
    if (vao) rlDisableVertexArray();
    else {
        rlDisableVertexAttribute(0);
        rlDisableVertexAttribute((unsigned int)g.locParam);
        rlDisableVertexBuffer();
    }
    rlDisableShader();
}
