// Golden field values for EVERY deformer, so a shared-kernel edit that moves
// one of them cannot land quietly.
//
// WHY THIS FILE EXISTS. Before it, nothing in the tree walked `CDeformType`
// asserting numeric field values, and the three things that looked like they
// did all had a hole in them:
//
//   * `test_parity.cpp` has a guard that was meant to force exactly this
//     ("a deformer was added; widen this test"). It pins `cdeform_noise == 13`
//     — an enumerator in the MIDDLE of the enum, not the last one — so it has
//     been silently inert since #14 (`cdeform_twist_range`) landed, and it did
//     not notice the six deformers added after that.
//   * The CPU/Metal parity suite cannot catch a kernel change AT ALL, by
//     construction: both backends compile this same header, so a wrong warp is
//     wrong identically on both sides and the comparison still passes. Parity
//     answers "do the backends agree", never "is the answer right".
//   * `test_deformers.cpp` checks most deformers against the kernel applied by
//     hand — which is the same source on both sides of the `==`, so it pins
//     the COMPILER's plumbing (slot packing, transform inversion, blob
//     handles) and not the kernel's arithmetic.
//
// CLAY_DEFORM_GRAB had no value oracle anywhere. `test_c_move_brush.cpp`
// asserts `CLAY_OK` and nothing about the field; `test_deformers.cpp`'s
// "outside the radius" subcase opens with `if (clength(p - centre) <= radius)
// continue;`, so every probe it keeps is one the deformer is required to leave
// alone. A too-eager early-out that shaved material off the INSIDE of the rim
// passed that suite unchanged. The rim probes below are sited to close that.
//
// WHAT A ROW HAS TO SHOW, and the second half is the one that matters:
//
//   (a) the value matches a committed literal, to 1e-5;
//   (b) the same probes on the UNDEFORMED item differ at >= 8 of them by
//       >= 1e-3.
//
// Without (b) a table of "the deformer did nothing" reads as full coverage —
// which is precisely the defect the grab tests have today, and the shape
// `test_c_measure_reach.cpp` documents for the automask bits. (b) is modelled
// on that file. It is not decoration: writing this table found that
// `Deformer::twist` on a SPHERE is exactly the identity (a rotation about Y
// applied to a rotationally symmetric field), so the sketch's "sphere for
// most" would have shipped an inert twist row. Every fixture below whose prim
// is not a sphere is a fixture (b) rejected.
//
// PROBES ARE HAND-SITED, NOT RANDOM. An RNG probe set re-derives itself on
// every table regeneration, so the literals stop being reviewable — a diff
// would be 800 changed numbers whether the kernel moved or the seed did. Every
// coordinate is a multiple of 1/8, so the probe itself is exact in binary and
// contributes no rounding of its own to argue about; only the kernel does.
//
// THE FINITE-SUPPORT ROWS (grab, pose, magnify, blob, alpha) get a probe at
// w~1, at w~0.5, at w~0.024 JUST INSIDE the rim, EXACTLY ON it, and outside.
// The region radius is 1 and the offsets are eighths, so `(0, 6/8, 5/8)` lands
// at d = sqrt(61)/8 = 0.97628 — inside by two and a half percent, with every
// coordinate still exact. That probe is the one a zero-weight early-out
// shaves, and the rim and outside probes are the ones a clamp that never
// reaches zero moves.
//
// THE UNDERSHOOTING EASINGS ARE PINNED ON PURPOSE. `cregion_weight` runs its
// [0,1] parameter through `cease`, and four of the curves LEAVE [0,1] on the
// way: ease_in_back and ease_in_out_back undershoot, both elastics ring below
// zero. `cease_in_back(t) = t^2((c1+1)t - c1)` with c1 = 1.70158 is negative
// for t < c1/(c1+1) = 0.6296, and t = 1 - d/r, so it is negative for
// d > 0.3704r — 1 - 0.3704^3 = 94.9% of the ball's volume. A negative weight
// makes grab push the OPPOSITE WAY from the drag over almost the whole region.
// The rows at the end of the table hold that behaviour as it is today. They
// are not an endorsement of it: when the clamp moves, those literals move with
// it, and having to regenerate them is the point — an intentional fix shows up
// as a reviewed diff on exactly the rows that change, instead of as nothing.
//
// That has already happened once, which is the evidence this paragraph is not
// wishful. `cmagnify_point` and `cblob_offset` guarded their falloff with
// `w <= 0.0f` and so threw the undershoot away; correcting both to `w == 0.0f`
// moved 17 literals, all of them on the magnify/ease_in_back row, and left
// every other row in the table untouched to the bit.
//
// THIS GATE HAS BEEN SHOWN TO FAIL, which is the only thing that makes it a
// gate. Two deliberate breaks to `cregion_weight`, each built and run:
//
//   1. clamp lower bound 0.0f -> 0.02f, so the weight never reaches zero.
//      40 assertions here fail across grab, pose, magnify, blob, alpha and the
//      easing rows, first at grab[25] — the probe EXACTLY ON the rim.
//      The existing suite ALSO catches this one: 5 cases fail, all of them
//      variants of "material outside the region is untouched, exactly". So
//      this break was not the interesting one.
//   2. `if (t < 0.05f) return 0.0f;` — a too-eager zero-weight early-out that
//      shaves the inside of the rim without breaking finite support.
//      34 assertions here fail, first at grab[20] = (1, 0.75, 0.625), the
//      probe two and a half percent inside the rim. THE EXISTING SUITE PASSES
//      IT COMPLETELY: 140 cases and 267,763 assertions across
//      test_deformers, test_c_move_brush, test_alpha_stamp, test_parity,
//      test_deform_exactness, test_deformer_cull, test_lattice_gizmo and
//      test_c_mask_brush, all green with the kernel broken.
//
// Break 2 is the one this file exists for, and the probe that catches it is
// the one `test_deformers.cpp`'s `continue` skips.
//
// ONE TABLE, NOT ONE PER PLATFORM, unlike `test_mesh_sculpt_parity.cpp`. That
// file hashes, so a last-bit disagreement between Apple's libm and glibc is a
// different hash; this one compares to 1e-5, which is four orders of magnitude
// above anything a transcendental's last bit can do.
//
// Regenerating: run with CLAY_DEFORM_GOLDEN_REGEN=1 to print the table (and
// write it to CLAY_DEFORM_GOLDEN_OUT, defaulting to a `.generated.inc` beside
// the binary), then re-baseline ONLY in the same commit as the deliberate
// behaviour change — never to turn a red test green.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "clay/kernel/ease.h"
#include "clay/kernel/tape.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"
#include "clay/scene/types.h"

using namespace clay;
using namespace clay::kernel;
using scene::Deformer;

namespace {

// -- probe sets ---------------------------------------------------------------
//
// Four shapes of probe set, sited for the four shapes of deformer here: a
// whole-item warp on a compact item, one that ramps along X, one that ramps
// along Y, and one with finite support about a centre.

// A compact spread over the unit ball and just past it, for the deformers that
// act on the whole item at once.
std::vector<cfloat3> ball_probes() {
    return {
        cf3(0.000f, 0.000f, 0.000f),   cf3(0.125f, 0.000f, 0.000f),
        cf3(0.000f, 0.125f, 0.000f),   cf3(0.000f, 0.000f, 0.125f),
        cf3(0.375f, 0.250f, 0.125f),   cf3(-0.375f, 0.250f, -0.125f),
        cf3(0.250f, -0.375f, 0.125f),  cf3(-0.250f, -0.250f, 0.375f),
        cf3(0.625f, 0.000f, 0.000f),   cf3(0.000f, 0.625f, 0.000f),
        cf3(0.000f, 0.000f, 0.625f),   cf3(-0.625f, 0.000f, 0.000f),
        cf3(0.500f, 0.500f, 0.000f),   cf3(0.500f, 0.000f, 0.500f),
        cf3(0.000f, 0.500f, 0.500f),   cf3(-0.500f, -0.500f, 0.250f),
        cf3(0.875f, 0.000f, 0.000f),   cf3(0.000f, -0.875f, 0.000f),
        cf3(0.000f, 0.000f, -0.875f),  cf3(0.750f, 0.375f, 0.125f),
        cf3(1.000f, 0.000f, 0.000f),   cf3(0.000f, 1.000f, 0.000f),
        cf3(0.000f, 0.000f, 1.000f),   cf3(-1.000f, 0.250f, 0.000f),
        cf3(1.125f, 0.000f, 0.000f),   cf3(0.000f, 1.125f, 0.250f),
        cf3(0.875f, 0.625f, 0.250f),   cf3(-1.125f, -0.250f, 0.125f),
        cf3(1.250f, 0.500f, 0.000f),   cf3(0.000f, -1.250f, 0.500f),
        cf3(0.375f, 0.875f, -0.750f),  cf3(-0.750f, 0.750f, 0.875f),
    };
}

// Along X, for the deformers whose parameter ramps with x: bend, wrap,
// bend_linear, bend_range, bend_curve. The last six leave the axis so the row
// is not asserting a one-dimensional slice of a three-dimensional warp.
std::vector<cfloat3> bar_probes() {
    return {
        cf3(-1.750f, 0.000f, 0.000f),   cf3(-1.500f, 0.125f, 0.000f),
        cf3(-1.250f, 0.000f, 0.125f),   cf3(-1.000f, 0.125f, 0.125f),
        cf3(-0.875f, 0.000f, 0.000f),   cf3(-0.750f, -0.125f, 0.000f),
        cf3(-0.625f, 0.000f, -0.125f),  cf3(-0.500f, 0.125f, 0.000f),
        cf3(-0.375f, 0.000f, 0.125f),   cf3(-0.250f, -0.125f, 0.000f),
        cf3(-0.125f, 0.000f, 0.000f),   cf3(0.000f, 0.000f, 0.000f),
        cf3(0.125f, 0.125f, 0.000f),    cf3(0.250f, 0.000f, 0.125f),
        cf3(0.375f, -0.125f, 0.000f),   cf3(0.500f, 0.000f, 0.000f),
        cf3(0.625f, 0.125f, 0.125f),    cf3(0.750f, 0.000f, 0.000f),
        cf3(0.875f, -0.125f, 0.000f),   cf3(1.000f, 0.000f, 0.125f),
        cf3(1.125f, 0.125f, 0.000f),    cf3(1.250f, 0.000f, 0.000f),
        cf3(1.375f, -0.125f, 0.125f),   cf3(1.500f, 0.000f, 0.000f),
        cf3(1.625f, 0.125f, 0.000f),    cf3(1.750f, 0.000f, 0.125f),
        cf3(-1.000f, 0.375f, 0.000f),   cf3(1.000f, -0.375f, 0.000f),
        cf3(-0.500f, 0.000f, 0.375f),   cf3(0.500f, 0.000f, -0.375f),
        cf3(0.000f, 0.375f, 0.250f),    cf3(0.000f, -0.250f, 0.375f),
    };
}

// The same, along Y: taper, twist_range and pose_line all ramp with y.
std::vector<cfloat3> column_probes() {
    std::vector<cfloat3> out;
    for (cfloat3 p : bar_probes()) out.push_back(cf3(p.y, p.x, p.z));
    return out;
}

// The finite-support set, offset from a region centre. The radius is 1
// everywhere it is used, so the comment beside each group is the WEIGHT the
// probe carries, and the groups are what a support bug moves:
//
//   w ~ 1     the centre, where a scale error shows as a whole-region shift
//   w ~ 0.5   the body of the falloff
//   w ~ 0.02  JUST inside the rim -- what a zero-weight early-out shaves
//   w = 0     exactly ON the rim, and outside it -- what a clamp that never
//             reaches zero moves, and what finite support promises
std::vector<cfloat3> region_probes(cfloat3 c) {
    const cfloat3 offsets[] = {
        // w ~ 1
        cf3(0.000f, 0.000f, 0.000f),    cf3(0.125f, 0.000f, 0.000f),
        cf3(0.000f, 0.125f, 0.000f),    cf3(0.000f, 0.000f, 0.125f),
        cf3(-0.125f, -0.125f, 0.000f),
        // w ~ 0.5
        cf3(0.500f, 0.000f, 0.000f),    cf3(0.000f, 0.500f, 0.000f),
        cf3(0.000f, 0.000f, -0.500f),   cf3(0.375f, 0.250f, 0.250f),
        cf3(-0.375f, 0.250f, -0.250f),
        // the body of the falloff, w from 0.75 down to 0.125
        cf3(0.250f, 0.000f, 0.000f),    cf3(0.000f, -0.250f, 0.000f),
        cf3(0.625f, 0.000f, 0.000f),    cf3(0.000f, 0.000f, 0.625f),
        cf3(0.750f, 0.000f, 0.000f),    cf3(0.000f, -0.750f, 0.000f),
        cf3(0.375f, -0.375f, -0.375f),  cf3(-0.500f, 0.500f, 0.250f),
        cf3(0.875f, 0.000f, 0.000f),    cf3(0.000f, 0.875f, 0.000f),
        // w ~ 0.02: d = sqrt(61)/8 = 0.97628 for the first four, sqrt(59)/8
        // = 0.96014 for the last. Inside by two to four percent, and every
        // coordinate still an exact eighth.
        cf3(0.000f, 0.750f, 0.625f),    cf3(0.750f, 0.625f, 0.000f),
        cf3(0.625f, 0.000f, 0.750f),    cf3(-0.750f, -0.625f, 0.000f),
        cf3(0.875f, 0.375f, 0.125f),
        // exactly ON the rim, d = 1. No off-axis eighth triple has
        // x^2+y^2+z^2 = 1 exactly, so these are the three there are.
        cf3(1.000f, 0.000f, 0.000f),    cf3(0.000f, -1.000f, 0.000f),
        cf3(0.000f, 0.000f, 1.000f),
        // outside
        cf3(1.125f, 0.000f, 0.000f),    cf3(0.000f, 1.250f, 0.000f),
        cf3(1.250f, 0.750f, 0.000f),    cf3(-1.500f, 0.000f, 0.500f),
    };
    std::vector<cfloat3> out;
    for (cfloat3 o : offsets) out.push_back(c + o);
    return out;
}

// The set for the undershooting easings, STRADDLING THE SIGN CHANGE at
// d = 0.3704r rather than spreading evenly like the set above.
//
// MEASURED, not guessed: with the even set, `magnify` under ease_in_back moved
// only SIX of its thirty-two probes and the discriminating half rejected the
// row. That is not a bad probe set finding nothing — it was the row reporting a
// real thing about the kernel, and this probe set is what caught it.
// `cmagnify_point` opened with `if (w <= 0.0f) return p;`, so a negative weight
// made it early out, and negative is what ease_in_back returns over the outer
// 94.9% of the ball: the deformer was inert almost everywhere it was asked to
// act, and only the inner d < 0.3704r moved at all.
//
// THAT BUG IS NOW FIXED — the guard reads `w == 0.0f`, in `cmagnify_point` and
// in `cblob_offset`, which had it too — so this set now straddles a sign change
// the deformer actually honours rather than one it swallowed. The row's moved
// count went 11 -> 23 of 32 when the guard was corrected, and the magnify row's
// literals moved with it. See `test_deform_zero_weight_guard.cpp`, which pins
// the operator itself so a "simplification" back to `<=` cannot pass.
//
// So the probes go where the behaviour is: twelve inside the undershoot
// boundary, six straddling it, and the rest carrying the rim and outside
// probes forward so the row still says what finite support does here. A row
// that pins a bug has to put its probes where the bug is.
std::vector<cfloat3> undershoot_probes(cfloat3 c) {
    const cfloat3 offsets[] = {
        // inside d = 0.3704: the only band where a negative-weight magnify
        // still moves anything
        cf3(0.000f, 0.000f, 0.000f),    cf3(0.125f, 0.000f, 0.000f),
        cf3(0.000f, 0.125f, 0.000f),    cf3(0.000f, 0.000f, 0.125f),
        cf3(-0.125f, 0.000f, 0.000f),   cf3(0.250f, 0.000f, 0.000f),
        cf3(0.000f, -0.250f, 0.000f),   cf3(0.000f, 0.000f, -0.250f),
        cf3(0.250f, 0.125f, 0.000f),    cf3(0.125f, -0.250f, 0.125f),
        cf3(-0.250f, 0.125f, -0.125f),  cf3(0.250f, 0.250f, 0.000f),
        // straddling it, d from 0.375 to 0.5
        cf3(0.375f, 0.000f, 0.000f),    cf3(0.000f, 0.375f, 0.000f),
        cf3(0.250f, 0.250f, 0.125f),    cf3(-0.375f, 0.125f, 0.000f),
        cf3(0.375f, 0.250f, 0.000f),    cf3(0.000f, 0.500f, 0.000f),
        // the body, where the undershoot is deepest
        cf3(0.625f, 0.000f, 0.000f),    cf3(0.000f, 0.000f, -0.625f),
        cf3(0.500f, 0.500f, 0.000f),    cf3(0.750f, 0.000f, 0.000f),
        cf3(0.000f, -0.750f, 0.000f),   cf3(0.375f, -0.375f, -0.375f),
        cf3(0.875f, 0.000f, 0.000f),    cf3(-0.500f, 0.500f, 0.250f),
        // just inside the rim, on the rim, and outside — as above
        cf3(0.000f, 0.750f, 0.625f),    cf3(0.750f, 0.625f, 0.000f),
        cf3(0.875f, 0.375f, 0.125f),    cf3(1.000f, 0.000f, 0.000f),
        cf3(1.125f, 0.000f, 0.000f),    cf3(1.250f, 0.750f, 0.000f),
    };
    std::vector<cfloat3> out;
    for (cfloat3 o : offsets) out.push_back(c + o);
    return out;
}

// -- fixtures the rows share --------------------------------------------------

constexpr float kRegionR = 1.0f;
// A pole of the unit sphere: half the region is inside the material and half
// is outside, so a row that only moved the far field would still fail (b).
const cfloat3 kSphereRegion = cf3(1.0f, 0.0f, 0.0f);
// The corresponding place on the capped cylinder below, whose half-height is 1.
const cfloat3 kColumnRegion = cf3(0.0f, 1.0f, 0.0f);

// A guide as HARD control points, so it compiles to itself and the row is
// pinned against the polyline it was built from rather than against whatever
// the tessellator does with it. Copied from test_deformers.cpp.
std::vector<scene::StrokePoint> polyline(const std::vector<cfloat3>& pts) {
    std::vector<scene::StrokePoint> out;
    for (cfloat3 p : pts) {
        scene::StrokePoint sp;
        sp.pos = p;
        sp.type = scene::StrokePointType::Hard;
        out.push_back(sp);
    }
    return out;
}

// A quarter circle in XY from (r,0,0) round to (0,r,0) — test_deformers.cpp's
// arc, at the same radius it uses to show a bend_curve is a `bend`.
std::vector<scene::StrokePoint> quarter_arc(float r, int segments) {
    std::vector<cfloat3> pts;
    for (int i = 0; i <= segments; ++i) {
        const float a = 1.5707963f * static_cast<float>(i) / static_cast<float>(segments);
        pts.push_back(cf3(r * std::cos(a), r * std::sin(a), 0.0f));
    }
    return polyline(pts);
}

// A 3x3x3 cage with four control points dragged in different directions. Not a
// uniform drag: that is a pure translation of the field (test_deformers.cpp
// asserts exactly that), which would make the row a translation test and pin
// none of the basis arithmetic.
Deformer varied_cage(std::uint8_t type) {
    Deformer d = Deformer::lattice(cf3(-1, -1, -1), cf3(1, 1, 1), 3, 3, 3);
    d.type = type;
    d.set_cage_offset(0, 0, 0, cf3(0.250f, 0.000f, 0.000f));
    d.set_cage_offset(2, 2, 2, cf3(0.000f, -0.375f, 0.125f));
    d.set_cage_offset(1, 2, 1, cf3(-0.125f, 0.250f, 0.000f));
    d.set_cage_offset(2, 0, 1, cf3(0.000f, 0.125f, -0.250f));
    return d;
}

// A stamp with structure in BOTH axes and a step running across it, so the row
// pins the bilinear lookup and the stamp's frame rather than one constant. A
// flat stamp would give a row that passes with the u/v lookup transposed, or
// dropped entirely. (The sample VALUES are not eighths and do not need to be:
// they are fixture data the kernel interpolates, not probe coordinates, and
// nothing here asserts on them directly.)
std::vector<float> ramp_stamp(int w, int h) {
    std::vector<float> s(static_cast<std::size_t>(w) * h, 0.0f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(w - 1);
            const float v = static_cast<float>(y) / static_cast<float>(h - 1);
            s[static_cast<std::size_t>(y) * w + x] = (x + y) % 3 == 0 ? 1.0f : u * v;
        }
    return s;
}

// -- the table ----------------------------------------------------------------

struct Case {
    // Logged through `std::string(c.name)` at every use, NOT bare. doctest
    // 2.5.3 stringifies a `const char*` in an INFO as "1", so the bare form
    // produces a failure message that names no row at all -- which was the
    // first thing this file got wrong and is worth not re-simplifying into.
    const char* name;
    // The enumerator this row covers, or -1 for the extra easing rows, which
    // repeat a deformer already covered and so are not part of the partition
    // the staleness guard checks.
    int covers;
    scene::Prim prim;
    Deformer def;
    std::vector<cfloat3> probes;
};

// ONE ROW PER ENUMERATOR, in enum order. The prim on each row is the one the
// discriminating half accepts: a sphere wherever the warp shows on a sphere,
// and something with a distinguishable axis wherever it does not (see the
// twist note in this file's header).
std::vector<Case> enum_cases() {
    std::vector<Case> c;
    c.push_back({"twist", cdeform_twist, scene::Prim::box(cf3(0.5f, 1.0f, 0.25f)),
                 Deformer::twist(1.5f), ball_probes()});
    c.push_back({"bend", cdeform_bend, scene::Prim::box(cf3(1.5f, 0.25f, 0.25f)),
                 Deformer::bend(0.75f), bar_probes()});
    c.push_back({"taper", cdeform_taper, scene::Prim::capped_cylinder(0.5f, 1.0f),
                 Deformer::taper(-1.0f, 1.0f, 1.0f, 0.25f, ease_smoothstep), column_probes()});
    c.push_back({"displace", cdeform_displace, scene::Prim::sphere(1.0f),
                 Deformer::displace(0.25f, 4.0f), ball_probes()});
    c.push_back({"wrap", cdeform_wrap, scene::Prim::box(cf3(1.5f, 0.25f, 0.25f)),
                 Deformer::wrap_around(-1.0f, 1.0f), bar_probes()});
    c.push_back({"elongate", cdeform_elongate, scene::Prim::sphere(1.0f),
                 Deformer::elongate(cf3(0.5f, 0.25f, 0.0f)), ball_probes()});
    c.push_back({"bend_linear", cdeform_bend_linear, scene::Prim::box(cf3(1.5f, 0.25f, 0.25f)),
                 Deformer::bend_linear(cf3(-1, 0, 0), cf3(1, 0, 0), cf3(0, 0.5f, 0),
                                       ease_smoothstep),
                 bar_probes()});
    c.push_back({"bend_radial", cdeform_bend_radial, scene::Prim::capped_cylinder(0.5f, 1.0f),
                 Deformer::bend_radial(0.0f, 0.5f, 0.5f, ease_smoothstep), column_probes()});
    c.push_back({"elongate_axis", cdeform_elongate_axis, scene::Prim::sphere(1.0f),
                 Deformer::elongate_axis(cf3(0.5f, 0.0f, 0.25f)), ball_probes()});
    c.push_back({"grab", cdeform_grab, scene::Prim::sphere(1.0f),
                 Deformer::grab(kSphereRegion, kRegionR, cf3(0.5f, 0.25f, 0.0f), ease_smoothstep,
                                false),
                 region_probes(kSphereRegion)});
    c.push_back({"pose", cdeform_pose, scene::Prim::capped_cylinder(0.5f, 1.0f),
                 Deformer::pose(kColumnRegion, kRegionR, cf3(0, 0, 1), 0.75f, ease_smoothstep),
                 region_probes(kColumnRegion)});
    c.push_back({"pose_line", cdeform_pose_line, scene::Prim::capped_cylinder(0.5f, 1.0f),
                 Deformer::pose_line(cf3(0, -1, 0), cf3(0, 1, 0), cf3(0, 0, 1), 0.75f,
                                     ease_smoothstep),
                 column_probes()});
    c.push_back({"magnify", cdeform_magnify, scene::Prim::sphere(1.0f),
                 Deformer::magnify(kSphereRegion, kRegionR, 0.5f, ease_smoothstep),
                 region_probes(kSphereRegion)});
    c.push_back({"noise", cdeform_noise, scene::Prim::sphere(1.0f),
                 Deformer::noise(0.25f, 3.0f, 3, 0.5f, 7u), ball_probes()});
    c.push_back({"twist_range", cdeform_twist_range, scene::Prim::box(cf3(0.5f, 1.5f, 0.25f)),
                 Deformer::twist_range(1.5f, -1.0f, 1.0f, ease_smoothstep), column_probes()});
    c.push_back({"bend_range", cdeform_bend_range, scene::Prim::box(cf3(1.5f, 0.25f, 0.25f)),
                 Deformer::bend_range(0.75f, -1.0f, 1.0f, ease_smoothstep), bar_probes()});
    c.push_back({"bend_curve", cdeform_bend_curve, scene::Prim::box(cf3(1.5f, 0.25f, 0.25f)),
                 Deformer::bend_curve(quarter_arc(1.5f, 12), -1.5f, 1.5f), bar_probes()});
    c.push_back({"lattice", cdeform_lattice, scene::Prim::sphere(1.0f),
                 varied_cage(cdeform_lattice), ball_probes()});
    Deformer through = varied_cage(cdeform_lattice_xform);
    // A rotated cage, which is the case an axis-aligned per-item box cannot
    // reproduce and therefore the only one that separates this row from the
    // one above it.
    through.cage_xform.rotation = math::Quat::from_axis_angle(cf3(0, 0, 1), 0.5f);
    c.push_back({"lattice_xform", cdeform_lattice_xform, scene::Prim::sphere(1.0f), through,
                 ball_probes()});
    c.push_back({"blob", cdeform_blob, scene::Prim::sphere(1.0f),
                 Deformer::blob(kSphereRegion, kRegionR, 0.5f, 3.0f, 3, 0.5f, 5u,
                                ease_smoothstep),
                 region_probes(kSphereRegion)});
    const std::vector<float> stamp = ramp_stamp(8, 8);
    c.push_back({"alpha", cdeform_alpha, scene::Prim::sphere(1.0f),
                 Deformer::alpha(cf3(0, 0, 1), cf3(0, 0, 1), cf3(1, 0, 0), stamp.data(), 8, 8,
                                 1.0f, kRegionR, 0.25f, ease_smoothstep),
                 region_probes(cf3(0, 0, 1))});
    return c;
}

// The four curves that LEAVE [0,1] on the way, on the region deformers that
// run their falloff through `cease`. See this file's header: today these carry
// a negative weight over most of the ball, so the region deformer runs
// BACKWARDS there, and these literals hold that until someone changes it on
// purpose.
std::vector<Case> undershoot_cases() {
    const std::uint8_t curves[] = {ease_in_back, ease_in_out_back, ease_in_elastic,
                                   ease_in_out_elastic};
    const char* names[] = {"grab/ease_in_back", "grab/ease_in_out_back", "grab/ease_in_elastic",
                           "grab/ease_in_out_elastic"};
    std::vector<Case> c;
    for (int i = 0; i < 4; ++i)
        c.push_back({names[i], -1, scene::Prim::sphere(1.0f),
                     Deformer::grab(kSphereRegion, kRegionR, cf3(0.5f, 0.25f, 0.0f), curves[i],
                                    false),
                     undershoot_probes(kSphereRegion)});
    c.push_back({"magnify/ease_in_back", -1, scene::Prim::sphere(1.0f),
                 Deformer::magnify(kSphereRegion, kRegionR, 0.5f, ease_in_back),
                 undershoot_probes(kSphereRegion)});
    c.push_back({"pose/ease_in_elastic", -1, scene::Prim::capped_cylinder(0.5f, 1.0f),
                 Deformer::pose(kColumnRegion, kRegionR, cf3(0, 0, 1), 0.75f, ease_in_elastic),
                 undershoot_probes(kColumnRegion)});
    return c;
}

std::vector<Case> all_cases() {
    std::vector<Case> c = enum_cases();
    for (const Case& e : undershoot_cases()) c.push_back(e);
    return c;
}

// One item, with the deformer or without it. The pair is what makes (b)
// possible: the same probes on the same prim, so any difference is the
// deformer and nothing else.
scene::Tape build(const Case& c, bool deformed, scene::Document& keep) {
    scene::Layer& l = keep.add_sdf_layer("l");
    scene::Node n;
    n.prim = c.prim;
    if (deformed) n.deformers.push_back(c.def);
    l.sdf->insert(n);
    return scene::compile_document(keep);
}

std::vector<float> evaluate(const Case& c, bool deformed) {
    scene::Document doc;
    const scene::Tape t = build(c, deformed, doc);
    std::vector<float> out;
    for (cfloat3 p : c.probes) out.push_back(t.eval(p).d);
    return out;
}

// Nine significant digits round-trip a float exactly, and the `.0` is not
// cosmetic: `%g` prints a whole number as "0", and "0f" is not a float literal
// in C++ — a regenerated table that skipped this would not compile. (The `n`
// and `i` in the search set spare "nan" and "inf" the suffix. Neither should
// ever appear here, and a table carrying one should fail to compile rather
// than be quietly accepted as a baseline.)
std::string literal(float v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(v));
    std::string s(buf);
    if (s.find_first_of(".eEni") == std::string::npos) s += ".0";
    return s + "f";
}

// Writes a regenerated table to stdout AND to a file at once. The file is not
// redundant, for the reason test_mesh_sculpt_parity.cpp gives: the ctest
// presets here set outputOnFailure, so a PASSING run's stdout never reaches a
// CI log, and a regeneration is a passing run by construction.
class TableWriter {
  public:
    TableWriter(bool enabled, const std::string& path)
        : out_(enabled ? std::fopen(path.c_str(), "w") : nullptr) {}
    ~TableWriter() {
        if (!out_) return;
        std::fputc('\n', out_);
        std::fclose(out_);
    }
    TableWriter(const TableWriter&) = delete;
    TableWriter& operator=(const TableWriter&) = delete;

    bool opened() const { return out_ != nullptr; }

    // Four to a line, each group labelled with the row and the index of its
    // first probe — so a failure message that names row and index points at a
    // findable place in the committed table rather than at an ordinal in 864.
    void emit(const char* row, std::size_t i, float v) {
        char head[128] = {0};
        if (i % 4 == 0) std::snprintf(head, sizeof head, "\n    // %s [%zu]\n   ", row, i);
        const std::string lit = literal(v);
        std::printf("%s %s,", head, lit.c_str());
        if (out_) std::fprintf(out_, "%s %s,", head, lit.c_str());
    }

  private:
    std::FILE* out_;
};

const float kGoldens[] = {
#include "deformer_goldens.inc"
};
constexpr std::size_t kGoldenCount = sizeof(kGoldens) / sizeof(kGoldens[0]);

}  // namespace

TEST_CASE("deformer goldens: the table covers every enumerator exactly once") {
    // PIN THE LAST ENUMERATOR, NOT A COUNT AND NOT A MIDDLE ONE. This is the
    // whole reason the guard in test_parity.cpp stopped working: it names
    // `cdeform_noise`, which is #13, so appending #14 through #20 changed
    // nothing it could see. Naming the LAST enumerator makes an append a
    // compile error here, and the loop below makes an INSERT one too.
    static_assert(cdeform_alpha == 20, "a deformer was added; add a row to enum_cases()");

    const std::vector<Case> cases = all_cases();
    std::vector<int> seen(cdeform_alpha + 1, 0);
    for (const Case& c : cases) {
        if (c.covers < 0) continue;
        REQUIRE(c.covers <= cdeform_alpha);
        ++seen[static_cast<std::size_t>(c.covers)];
    }
    for (int d = 0; d <= cdeform_alpha; ++d) {
        CAPTURE(d);
        // Exactly once: a second row for the same enumerator would let a
        // MISSING one hide behind a matching total.
        CHECK(seen[static_cast<std::size_t>(d)] == 1);
    }
}

TEST_CASE("deformer goldens: every row still evaluates to its committed value") {
    // (a). The probe coordinates ride the failure message, so a red build says
    // WHICH row and WHERE in its region moved rather than just that a number
    // did — which is the difference between "the rim early-outs too eagerly"
    // and "something changed".
    const bool regen = std::getenv("CLAY_DEFORM_GOLDEN_REGEN") != nullptr;
    const char* out_env = std::getenv("CLAY_DEFORM_GOLDEN_OUT");
    const std::string out_path = out_env ? out_env : "deformer_goldens.generated.inc";
    TableWriter writer(regen, out_path);
    // A table that cannot be written is worth saying out loud: the run still
    // passes, and someone would otherwise go looking for an artifact that was
    // never created.
    if (regen && !writer.opened())
        MESSAGE("could not open " << out_path << " to write a table");

    std::size_t index = 0;
    for (const Case& c : all_cases()) {
        INFO("row: " << std::string(c.name));
        const std::vector<float> got = evaluate(c, true);
        for (std::size_t i = 0; i < got.size(); ++i, ++index) {
            if (regen) {
                writer.emit(c.name, i, got[i]);
                continue;
            }
            CAPTURE(i);
            CAPTURE(c.probes[i].x);
            CAPTURE(c.probes[i].y);
            CAPTURE(c.probes[i].z);
            REQUIRE(index < kGoldenCount);
            CHECK(got[i] == doctest::Approx(kGoldens[index]).epsilon(1e-5));
        }
    }
    // A table with entries nobody reads is a table that has drifted from the
    // rows — a deleted row would otherwise leave its literals behind, unread
    // and unnoticed. So the count is asserted rather than assumed.
    if (!regen) CHECK(index == kGoldenCount);
}

TEST_CASE("deformer goldens: no row is a table of the item doing nothing") {
    // (b), THE HALF THAT MATTERS, modelled on test_c_measure_reach.cpp's
    // second case. A row whose deformer is inert on its fixture produces a
    // perfectly stable table that agrees with itself forever and gates
    // nothing — which is what the grab tests amount to today, and what the
    // twist row would have been on a sphere.
    //
    // EIGHT PROBES, not one. One probe clearing the bar can be a rounding
    // difference at a single sample; eight of thirty-two is a warp.
    //
    // MEASURED, so the next reader knows how much room each row has and does
    // not have to re-derive it (of 32 probes, at >= 1e-3):
    //
    //   bend_curve 31   lattice 29        lattice_xform 29   wrap 27
    //   elongate 27     elongate_axis 27  noise 27           pose_line 26
    //   bend 25         bend_linear 24    bend_range 23      grab 21
    //   magnify 20      alpha 20          twist_range 18     blob 16
    //   twist 15        taper 15          pose 15            bend_radial 14
    //   displace 10
    //
    //   grab/ease_in_back 27   grab/ease_in_out_back 27
    //   grab/ease_in_elastic 23   grab/ease_in_out_elastic 26
    //   pose/ease_in_elastic 14   magnify/ease_in_back 23
    //
    // DISPLACE IS THE TIGHTEST AND THAT IS ARITHMETIC, not a weak fixture: its
    // offset is amp*sin(f*x)*sin(f*y)*sin(f*z), which is EXACTLY ZERO wherever
    // any coordinate is, and 22 of the 32 ball probes sit on a coordinate
    // plane. The 10 that move are precisely the 10 with no zero component.
    // Raising the amplitude cannot help — a zero stays zero — so if this row
    // ever needs more room the answer is a probe set off the planes.
    //
    // MAGNIFY/EASE_IN_BACK used to be the other tight one, at 11, and that
    // number was the bug rather than a fixture problem: see `undershoot_probes`.
    // Correcting the guard to `w == 0.0f` took it to 23 without touching the
    // fixture, which is what a row reporting the kernel rather than itself
    // looks like.
    for (const Case& c : all_cases()) {
        INFO("row: " << std::string(c.name));
        const std::vector<float> deformed = evaluate(c, true);
        const std::vector<float> plain = evaluate(c, false);
        REQUIRE(deformed.size() == plain.size());
        int moved = 0;
        for (std::size_t i = 0; i < deformed.size(); ++i)
            if (std::fabs(deformed[i] - plain[i]) >= 1e-3f) ++moved;
        CAPTURE(moved);
        INFO("this row's deformer barely changes its fixture, so its goldens "
             "would keep passing after a kernel change that broke it -- pick a "
             "prim or a probe set the warp actually reaches");
        CHECK(moved >= 8);
    }
}
