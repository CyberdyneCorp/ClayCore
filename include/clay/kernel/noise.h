#pragma once

// Gradient noise on an integer lattice (sdf-kernels spec, add-noise-field).
//
// WHY THE HASH IS INTEGER. Cross-backend parity here is tolerance-based rather
// than bit-exact: 1e-6 relative on the CPU backends, 1e-4 on the GPU ones. That
// is comfortable for ordinary arithmetic and fatal for the usual float hash.
// `fract(sin(dot(p, k)) * 43758.5453)` takes whatever `sin` does differently
// between libm, CUDA, Metal and OpenCL — a few units in the last place —
// multiplies it by forty-three thousand and then takes a fractional part, which
// is chaotic by construction. A 1e-7 disagreement becomes an O(1) disagreement.
// It would fail parity on the first case.
//
// The same integer operations produce the same bits on every backend, so the
// hash is integer, and that is what makes the noise reproducible at all. It is
// also why the shim now carries `cuint` — no other kernel header needs one.
//
// Gradient (Perlin-style) rather than value noise: value noise on a lattice
// shows its grid as a visible axis-aligned quilt, which on a sculpted surface
// reads as a manufacturing defect rather than as weathering.

#include "clay/kernel/shim.h"

CLAY_NS_BEGIN

// A small integer avalanche. Three multiply-xor-shift rounds is enough to
// decorrelate neighbouring lattice cells, which is all this needs — it is
// choosing gradients, not generating cryptographic material.
CLAY_FN cuint cnoise_hash(cuint x, cuint y, cuint z, cuint seed) {
    cuint h = seed;
    h = (h ^ x) * 0x9E3779B1u;
    h = (h ^ (h >> 15)) ^ y;
    h = h * 0x85EBCA77u;
    h = (h ^ (h >> 13)) ^ z;
    h = h * 0xC2B2AE3Du;
    return h ^ (h >> 16);
}

// The gradient for a lattice corner: one of twelve directions, taken from the
// hash. Perlin's own set — the midpoints of a cube's edges — because they are
// well distributed and need no normalization or trigonometry.
CLAY_FN cfloat3 cnoise_gradient(cuint h) {
    cuint i = h % 12u;
    // The twelve midpoints of a cube's edges — (+-1,+-1,0), (0,+-1,+-1) and
    // (+-1,0,+-1) — expressed without a lookup table, which the dialect forbids.
    // The order differs from Perlin's listing and does not matter: only the SET
    // does, since the hash decides which corner gets which.
    // The `!= 0u` is not decoration: GLSL has no int-to-bool conversion, so a
    // bare `(i & 1u)` as a condition is a compile error there and silently
    // fine everywhere else.
    float x = (i < 4u) ? ((i & 1u) != 0u ? -1.0f : 1.0f)
                       : ((i < 8u) ? 0.0f : ((i & 1u) != 0u ? -1.0f : 1.0f));
    float y = (i < 4u) ? ((i & 2u) != 0u ? -1.0f : 1.0f)
                       : ((i < 8u) ? ((i & 1u) != 0u ? -1.0f : 1.0f) : 0.0f);
    float z = (i < 4u) ? 0.0f : ((i < 8u) ? ((i & 2u) != 0u ? -1.0f : 1.0f)
                                          : ((i & 2u) != 0u ? -1.0f : 1.0f));
    return cf3(x, y, z);
}

// Perlin's quintic fade: 6t^5 - 15t^4 + 10t^3. Its first AND second derivatives
// vanish at the lattice points, which is what stops the grid showing up as
// creases along the cell boundaries the way a cubic fade leaves them.
CLAY_FN float cnoise_fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

CLAY_FN float cnoise_dot(cuint h, float x, float y, float z) {
    cfloat3 g = cnoise_gradient(h);
    return g.x * x + g.y * y + g.z * z;
}

// One octave of gradient noise, in roughly [-1, 1].
CLAY_FN float cnoise_gradient3(cfloat3 p, cuint seed) {
    float fx = cfloor(p.x), fy = cfloor(p.y), fz = cfloor(p.z);
    // Biased into the unsigned range before converting: a negative coordinate
    // must land on a stable lattice cell, and casting a negative float to an
    // unsigned integer is undefined in C++ and merely unhelpful elsewhere.
    cuint ix = CLAY_UINT((CLAY_INT(fx) + 0x40000000));
    cuint iy = CLAY_UINT((CLAY_INT(fy) + 0x40000000));
    cuint iz = CLAY_UINT((CLAY_INT(fz) + 0x40000000));

    float tx = p.x - fx, ty = p.y - fy, tz = p.z - fz;
    float ux = cnoise_fade(tx), uy = cnoise_fade(ty), uz = cnoise_fade(tz);

    float n000 = cnoise_dot(cnoise_hash(ix, iy, iz, seed), tx, ty, tz);
    float n100 = cnoise_dot(cnoise_hash(ix + 1u, iy, iz, seed), tx - 1.0f, ty, tz);
    float n010 = cnoise_dot(cnoise_hash(ix, iy + 1u, iz, seed), tx, ty - 1.0f, tz);
    float n110 = cnoise_dot(cnoise_hash(ix + 1u, iy + 1u, iz, seed), tx - 1.0f, ty - 1.0f, tz);
    float n001 = cnoise_dot(cnoise_hash(ix, iy, iz + 1u, seed), tx, ty, tz - 1.0f);
    float n101 = cnoise_dot(cnoise_hash(ix + 1u, iy, iz + 1u, seed), tx - 1.0f, ty, tz - 1.0f);
    float n011 = cnoise_dot(cnoise_hash(ix, iy + 1u, iz + 1u, seed), tx, ty - 1.0f, tz - 1.0f);
    float n111 = cnoise_dot(cnoise_hash(ix + 1u, iy + 1u, iz + 1u, seed), tx - 1.0f, ty - 1.0f,
                            tz - 1.0f);

    float x00 = cmix(n000, n100, ux);
    float x10 = cmix(n010, n110, ux);
    float x01 = cmix(n001, n101, ux);
    float x11 = cmix(n011, n111, ux);
    return cmix(cmix(x00, x10, uy), cmix(x01, x11, uy), uz);
}

// Fractal sum. One octave of gradient noise is smooth blobs; a weathered
// surface has detail at several scales, which is the whole reason to sum.
//
// Normalized by the total weight so that raising the octave count adds detail
// WITHOUT growing the overall deviation — otherwise `octaves` and `amplitude`
// would be two controls for the same thing and neither would mean anything.
CLAY_FN float cnoise_fbm(cfloat3 p, int octaves, float gain, cuint seed) {
    float sum = 0.0f;
    float weight = 0.0f;
    float amp = 1.0f;
    cfloat3 q = p;
    for (int i = 0; i < octaves; ++i) {
        sum = sum + amp * cnoise_gradient3(q, seed + CLAY_UINT(i) * 0x9E3779B1u);
        weight = weight + amp;
        amp = amp * gain;
        q = q * 2.0f;  // lacunarity 2: the usual octave doubling
    }
    return weight > 0.0f ? sum / weight : 0.0f;
}

CLAY_NS_END
