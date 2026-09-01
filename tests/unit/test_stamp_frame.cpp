// THE STAMP'S BASIS (brush-engine spec, add-shared-brush-runtime 5.2, 6.6).
//
// `make_stamp_frame` is the one place in the tree that decides which way "along
// the stamp" points, and five families of brush read it — Rake, Chisel, Clay
// Strips, a directional scratch, a rotated alpha. Two things about it are
// contracts rather than implementation:
//
//   - the basis is ORTHONORMAL AND RIGHT-HANDED, so that `stamp_uv` inverts it
//     and a grain rotated by an angle is rotated by that angle rather than
//     sheared by it;
//   - a ZERO AZIMUTH TAKES NO ROTATION AT ALL, which is D5 and is why
//     `alpha_frame_for` could be reimplemented over `make_stamp_frame` without
//     the fixed mesh's goldens moving.
//
// The second one is the reason this file exists at all. `t * cos 0 + b * sin 0`
// looks like the identity and is not, and the difference is one sign bit that
// no tolerance in this repository can see. The gate below is written so that it
// FAILS if the branch is replaced by the multiplication — which means it has to
// name a direction where the two genuinely disagree, and it does.
//
// AND THIS FILE IS THE ONLY GATE ON IT, WHICH IS MEASURED RATHER THAN ASSUMED.
// Deleting the branch and rebuilding leaves every golden in
// `mesh_sculpt_goldens_*.inc` passing: none of those eighty fixtures happens to
// combine a direction whose basis carries a negative zero with an alpha sample
// near a texel boundary. So the sign bit really can reach a golden — the
// arithmetic is what it is — and today nothing but the two cases at the bottom
// of this file would notice it being lost. That is an argument for keeping them
// sharp, not for relaxing them.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

#include "clay/kernel/deform.h"  // calpha_frame, the basis this file asserts is passed through
#include "clay/mesh/sculpt_common.h"
#include "clay/mesh/sculpt_kernels.h"
#include "clay/mesh/stamp_frame.h"

using namespace clay;
using namespace clay::kernel;
using mesh::AlphaFrame;
using mesh::MeshBrushSettings;
using mesh::StampFrame;
using mesh::StampUv;

namespace {

// BYTES, not a tolerance. Every claim in this file that involves a zero azimuth
// is about the last bit, and `==` on floats would already accept `-0.0f` for
// `+0.0f` — which is precisely the difference D5 turns on.
bool same_bits(cfloat3 a, cfloat3 b) { return std::memcmp(&a, &b, sizeof(cfloat3)) == 0; }

// A set of directions and hints chosen to exercise every branch inside
// `calpha_frame`: an axis-aligned direction, an oblique one, a hint parallel to
// the direction (so the fallback axis is taken), an absent hint, and a hint
// that must be re-orthogonalised. Every component is a power of two or a sum of
// them, so the fixture contributes no rounding of its own.
struct Case {
    cfloat3 direction;
    cfloat3 hint;
};
const Case kCases[] = {
    {cf3(0, 1, 0), cf3(0, 0, 0)},      {cf3(0, 0, 1), cf3(1, 0, 0)},
    {cf3(0, -1, 0), cf3(0, 0, 0)},     {cf3(-1, -1, 0), cf3(-1, 0, 0)},
    {cf3(1, 1, 1), cf3(0, 1, 0)},      {cf3(0.5f, -0.25f, 2), cf3(1, 1, 0)},
    {cf3(1, 0, 0), cf3(1, 0, 0)},      {cf3(-2, 0.5f, -0.5f), cf3(0, 0, 1)},
};

}  // namespace

TEST_CASE("stamp frame: the basis is orthonormal and right-handed") {
    for (const Case& c : kCases) {
        CAPTURE(c.direction.x);
        CAPTURE(c.direction.y);
        CAPTURE(c.direction.z);
        for (float azimuth : {0.0f, 0.5f, 1.5707964f, 3.0f, -0.75f}) {
            CAPTURE(azimuth);
            const StampFrame f = mesh::make_stamp_frame(cf3(1, 2, 3), c.direction, c.hint, azimuth);

            CHECK(clength(f.normal) == doctest::Approx(1.0f).epsilon(1e-6));
            CHECK(clength(f.tangent) == doctest::Approx(1.0f).epsilon(1e-6));
            CHECK(clength(f.bitangent) == doctest::Approx(1.0f).epsilon(1e-6));

            CHECK(cdot(f.normal, f.tangent) == doctest::Approx(0.0f));
            CHECK(cdot(f.normal, f.bitangent) == doctest::Approx(0.0f));
            CHECK(cdot(f.tangent, f.bitangent) == doctest::Approx(0.0f));

            // RIGHT-HANDED, which is what makes `bitangent` the "v" axis rather
            // than its mirror. A left-handed basis would flip every alpha and
            // every rake in the library, and would still pass every
            // orthogonality check above.
            const cfloat3 expected = ccross(f.normal, f.tangent);
            CHECK(clength(expected - f.bitangent) == doctest::Approx(0.0f));

            CHECK(f.rotation == azimuth);
            CHECK(same_bits(f.origin, cf3(1, 2, 3)));
        }
    }
}

TEST_CASE("stamp frame: a degenerate direction falls back rather than producing a NaN") {
    // A brush that landed on a fold can hand this a zero direction, and a basis
    // of NaNs would propagate silently into every displacement the stamp wrote.
    const StampFrame f = mesh::make_stamp_frame(cf3(0, 0, 0), cf3(0, 0, 0), cf3(0, 0, 0), 0.0f);
    CHECK(same_bits(f.normal, cf3(0, 0, 1)));
    CHECK(clength(f.tangent) == doctest::Approx(1.0f).epsilon(1e-6));
    CHECK(clength(f.bitangent) == doctest::Approx(1.0f).epsilon(1e-6));
}

TEST_CASE("stamp frame: stamp_uv inverts the basis") {
    const StampFrame f = mesh::make_stamp_frame(cf3(0.5f, -1.0f, 2.0f), cf3(0, 1, 0), cf3(0, 0, 0),
                                                0.0f);
    const float extent = 4.0f;

    // The origin is the centre of the square, by construction.
    const StampUv centre = mesh::stamp_uv(f, f.origin, extent);
    CHECK(centre.u == doctest::Approx(0.5f));
    CHECK(centre.v == doctest::Approx(0.5f));

    // A point half an extent along the tangent is at the right-hand edge, and
    // one along the bitangent at the top. That pairing — u to tangent, v to
    // bitangent — is what an alpha's row-major layout is read through, so
    // swapping them would transpose every alpha in the library.
    const StampUv along_t = mesh::stamp_uv(f, f.origin + f.tangent * (extent * 0.5f), extent);
    CHECK(along_t.u == doctest::Approx(1.0f));
    CHECK(along_t.v == doctest::Approx(0.5f));

    const StampUv along_b = mesh::stamp_uv(f, f.origin + f.bitangent * (extent * 0.5f), extent);
    CHECK(along_b.u == doctest::Approx(0.5f));
    CHECK(along_b.v == doctest::Approx(1.0f));

    // OFF THE PLANE CHANGES NOTHING. The uv is a projection, and a point above
    // the stamp must sample the same texel as its footprint — otherwise a
    // displaced surface would slide under its own alpha as it rose.
    const StampUv lifted = mesh::stamp_uv(f, f.origin + f.normal * 7.0f, extent);
    CHECK(lifted.u == doctest::Approx(0.5f));
    CHECK(lifted.v == doctest::Approx(0.5f));

    // A zero extent is a refusal rather than a division: it returns the
    // default (0,0) instead of an infinity that would sample nothing.
    const StampUv degenerate = mesh::stamp_uv(f, f.origin + f.tangent, 0.0f);
    CHECK(degenerate.u == 0.0f);
    CHECK(degenerate.v == 0.0f);
}

TEST_CASE("stamp frame: the azimuth rotates in the plane, by the angle it names") {
    const cfloat3 dir = cf3(0, 1, 0);
    const StampFrame base = mesh::make_stamp_frame(cf3(0, 0, 0), dir, cf3(0, 0, 0), 0.0f);

    for (float azimuth : {0.25f, 1.0f, 1.5707964f, -0.5f}) {
        CAPTURE(azimuth);
        const StampFrame turned = mesh::make_stamp_frame(cf3(0, 0, 0), dir, cf3(0, 0, 0), azimuth);

        // IN THE PLANE: the facing does not move, which is what separates an
        // azimuth from a tilt.
        CHECK(same_bits(turned.normal, base.normal));

        // BY THE ANGLE IT NAMES, measured as the angle between the two
        // tangents. A rotation by twice the azimuth — the composition bug
        // `stamp_rotation_of` exists to prevent — fails here.
        const float cosine = cdot(base.tangent, turned.tangent);
        CHECK(cosine == doctest::Approx(std::cos(azimuth)).epsilon(1e-5));

        // AND IN THE RIGHT DIRECTION. `cos` is even, so the check above passes
        // on a rotation the wrong way round; the signed component along the
        // bitangent is what fixes the sense.
        CHECK(cdot(base.bitangent, turned.tangent) == doctest::Approx(std::sin(azimuth)).epsilon(1e-5));
    }
}

TEST_CASE("stamp frame: a full turn returns the basis and a half turn negates it") {
    const StampFrame base = mesh::make_stamp_frame(cf3(0, 0, 0), cf3(1, 1, 0), cf3(0, 0, 1), 0.0f);
    const float pi = 3.14159265358979323846f;

    const StampFrame half = mesh::make_stamp_frame(cf3(0, 0, 0), cf3(1, 1, 0), cf3(0, 0, 1), pi);
    CHECK(clength(half.tangent + base.tangent) == doctest::Approx(0.0f));
    CHECK(clength(half.bitangent + base.bitangent) == doctest::Approx(0.0f));

    const StampFrame full =
        mesh::make_stamp_frame(cf3(0, 0, 0), cf3(1, 1, 0), cf3(0, 0, 1), 2.0f * pi);
    CHECK(clength(full.tangent - base.tangent) == doctest::Approx(0.0f));
}

// -- D5: a zero azimuth takes no rotation at all ------------------------------

TEST_CASE("stamp frame: a zero azimuth is byte-identical to no rotation") {
    // THE ACCEPTANCE GATE OF THIS WHOLE CHANGE, said in the smallest place it
    // can be said. `alpha_frame_for` now builds its basis through
    // `make_stamp_frame`, and the fixed mesh's goldens are only unchanged
    // because the default path through it reaches `kernel::calpha_frame` and
    // returns its output UNTOUCHED.
    for (const Case& c : kCases) {
        CAPTURE(c.direction.x);
        CAPTURE(c.direction.y);
        CAPTURE(c.direction.z);

        cfloat3 n, t, b;
        kernel::calpha_frame(c.direction, c.hint, &n, &t, &b);

        const StampFrame f = mesh::make_stamp_frame(cf3(0, 0, 0), c.direction, c.hint, 0.0f);
        CHECK(same_bits(f.normal, n));
        CHECK(same_bits(f.tangent, t));
        CHECK(same_bits(f.bitangent, b));
    }
}

TEST_CASE("stamp frame: the zero-azimuth branch is not the multiplication it looks like") {
    // WHAT MAKES THE ASSERTION ABOVE A GATE RATHER THAN A TAUTOLOGY.
    //
    // If `t * cos 0 + b * sin 0` really were the identity, the branch in
    // `make_stamp_frame` would be a micro-optimisation and deleting it would be
    // harmless. It is not: `x + 0.0f` yields `+0.0f` when `x` is `-0.0f`, so a
    // basis axis that carries a negative zero comes back with its sign bit
    // cleared. This case is one where it does — found by sweeping the exactly
    // representable directions and hints, of which 1876 of 16464 combinations
    // disagree.
    //
    // So: the naive form differs from the frame here, and the frame agrees with
    // the unrotated basis. Replace the branch with the multiplication and the
    // FIRST check below still passes while the previous test case fails, which
    // is the failure this pair is arranged to produce.
    const cfloat3 dir = cf3(-1, -1, 0);
    const cfloat3 hint = cf3(-1, 0, 0);

    cfloat3 n, t, b;
    kernel::calpha_frame(dir, hint, &n, &t, &b);

    const float c = std::cos(0.0f);
    const float s = std::sin(0.0f);
    const cfloat3 naive_t = t * c + b * s;
    const cfloat3 naive_b = b * c - t * s;

    // The multiplication is NOT the identity on this basis.
    CHECK_FALSE(same_bits(naive_b, b));
    // ...and specifically because a negative zero came back positive.
    CHECK(std::signbit(b.x));
    CHECK_FALSE(std::signbit(naive_b.x));
    // The tangent survives it here; only one of the two axes has to move for a
    // golden to move.
    CHECK(same_bits(naive_t, t));

    // And the frame the engine actually builds is the unrotated one.
    const StampFrame f = mesh::make_stamp_frame(cf3(0, 0, 0), dir, hint, 0.0f);
    CHECK(same_bits(f.bitangent, b));
    CHECK(std::signbit(f.bitangent.x));
}

// -- the explicit rotation replaces the azimuth -------------------------------

TEST_CASE("stamp frame: an explicit rotation replaces the azimuth rather than composing") {
    // A host that pinned its grain AND set an azimuth would otherwise get an
    // angle neither of them asked for, and the zero-azimuth identity above
    // would depend on two numbers instead of one.
    CHECK(mesh::stamp_rotation_of(0.75f, false, 2.0f) == 0.75f);
    CHECK(mesh::stamp_rotation_of(0.75f, true, 2.0f) == 2.0f);
    CHECK(mesh::stamp_rotation_of(0.75f, true, 0.0f) == 0.0f);

    // The composition bug would read 2.75f; the sum is what this rules out.
    CHECK(mesh::stamp_rotation_of(0.75f, true, 2.0f) != 0.75f + 2.0f);

    // AND AN EXPLICIT ZERO IS STILL THE IDENTITY PATH. That is the case a
    // composition would break most quietly: a preset that pins its grain to
    // "unrotated" must reach the branch, not a rotation by 0.75.
    const cfloat3 dir = cf3(-1, -1, 0);
    const cfloat3 hint = cf3(-1, 0, 0);
    const StampFrame pinned = mesh::make_stamp_frame(
        cf3(0, 0, 0), dir, hint, mesh::stamp_rotation_of(0.75f, true, 0.0f));
    cfloat3 n, t, b;
    kernel::calpha_frame(dir, hint, &n, &t, &b);
    CHECK(same_bits(pinned.bitangent, b));
}

// -- the alpha is one reading of the stamp frame ------------------------------

TEST_CASE("stamp frame: the alpha frame is the stamp frame, and the azimuth turns it") {
    // 5.3's claim in bytes: `alpha_frame_for` is now `make_stamp_frame` plus an
    // extent. If somebody re-derived the basis there, these would drift apart
    // at the last bit and nothing else in the suite would notice.
    const float alpha[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    MeshBrushSettings s;
    s.center = cf3(0.5f, 0.25f, -1.0f);
    s.radius = 0.5f;
    s.alpha = alpha;
    s.alpha_width = 2;
    s.alpha_height = 2;
    s.alpha_direction = cf3(0, 1, 0);
    s.alpha_tangent = cf3(0, 0, 0);
    s.alpha_extent = 2.0f;
    REQUIRE(s.has_alpha());

    const AlphaFrame unrotated = mesh::alpha_frame_for(s, cf3(0, 0, 1));
    const StampFrame stamp =
        mesh::make_stamp_frame(s.center, s.alpha_direction, s.alpha_tangent, 0.0f);
    CHECK(same_bits(unrotated.tangent, stamp.tangent));
    CHECK(same_bits(unrotated.binormal, stamp.bitangent));
    CHECK(unrotated.extent == 2.0f);

    // THE AZIMUTH REACHES THE ALPHA. Before this change there was no way to
    // rotate one at all, so a rotated alpha meant re-baking the image.
    s.stamp_azimuth = 1.5707964f;
    const AlphaFrame turned = mesh::alpha_frame_for(s, cf3(0, 0, 1));
    CHECK_FALSE(same_bits(turned.tangent, unrotated.tangent));
    CHECK(cdot(turned.tangent, unrotated.tangent) ==
          doctest::Approx(0.0f));

    // ...and a zero azimuth leaves it exactly where it was, which is what keeps
    // every alpha stamp in the goldens on its own bits.
    s.stamp_azimuth = 0.0f;
    const AlphaFrame again = mesh::alpha_frame_for(s, cf3(0, 0, 1));
    CHECK(same_bits(again.tangent, unrotated.tangent));
    CHECK(same_bits(again.binormal, unrotated.binormal));
}

TEST_CASE("stamp frame: a brush with no alpha still reports the default frame") {
    // `alpha_frame_for` returns early when there is no alpha, and `alpha_at`
    // then returns an exact 1. A frame built anyway would cost every stamp in
    // the library a `calpha_frame` it never reads.
    MeshBrushSettings s;
    s.center = cf3(9, 9, 9);
    s.stamp_azimuth = 1.0f;
    REQUIRE_FALSE(s.has_alpha());

    const AlphaFrame f = mesh::alpha_frame_for(s, cf3(0, 1, 0));
    CHECK(same_bits(f.centre, cf3(0, 0, 0)));
    CHECK(mesh::alpha_at(s, f, cf3(1, 2, 3)) == 1.0f);
}
