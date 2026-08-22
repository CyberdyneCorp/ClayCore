// The blocked evaluator against the scalar one, on the standard parity corpus.
//
// The assertion is IDENTITY, not a tolerance. `eval_points_blocked` is a second
// implementation of `ctape_eval`'s walk, and the failure mode a tolerance would
// hide is exactly the one that matters: a slow divergence between two
// interpreters that are supposed to be the same. A blocked path that is merely
// "within 1e-6" of the scalar path has already stopped being the reference
// implementation every backend in the tree is compared against.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "clay/eval/backend.h"
#include "clay/field/volume.h"
#include "clay/io/parity_fixture.h"
#include "clay/scene/commands.h"

using namespace clay;

namespace {

std::vector<float> flatten(const std::vector<kernel::cfloat3>& pts) {
    std::vector<float> out(pts.size() * 3);
    for (std::size_t i = 0; i < pts.size(); ++i) {
        out[i * 3] = pts[i].x;
        out[i * 3 + 1] = pts[i].y;
        out[i * 3 + 2] = pts[i].z;
    }
    return out;
}

struct Run {
    std::vector<float> distances;
    std::vector<float> colors;
};

Run scalar(const scene::Tape& tape, const std::vector<float>& xyz) {
    const std::size_t n = xyz.size() / 3;
    Run r{std::vector<float>(n), std::vector<float>(n * 3)};
    eval::PointQuery q{xyz.data(), n, 1e-4f};
    eval::PointResults out{r.distances.data(), nullptr, r.colors.data()};
    eval::eval_points_reference(tape, q, out);
    return r;
}

Run blocked(const scene::Tape& tape, const std::vector<float>& xyz, std::size_t block) {
    const std::size_t n = xyz.size() / 3;
    Run r{std::vector<float>(n), std::vector<float>(n * 3)};
    eval::PointQuery q{xyz.data(), n, 1e-4f};
    eval::PointResults out{r.distances.data(), nullptr, r.colors.data()};
    eval::eval_points_blocked(tape, q, out, block);
    return r;
}

// Bit-for-bit, through the object representation: `==` on floats would call
// two NaNs different and -0.0 equal to 0.0, and neither is what "identical
// bytes" means. A tape can legitimately produce either.
bool identical(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// The parity corpus was built for what a hand-written GPU preview gets wrong,
// which is not the same set as what a second interpreter gets wrong. It contains
// NO gated item and no coloured volume, and mutation-testing showed both of the
// corresponding branches surviving a deliberate break. These documents exist to
// close that, and each one is verified to fail against the matching mutation.
std::vector<std::pair<std::string, scene::Tape>> extra_documents() {
    std::vector<std::pair<std::string, scene::Tape>> out;

    const auto sphere_at = [](kernel::cfloat3 c, float r) {
        return [c, r](kernel::cfloat3 p) {
            const kernel::cfloat3 d = p - c;
            return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) - r;
        };
    };
    const math::Aabb region(kernel::cf3(-1.2f, -1.2f, -1.2f), kernel::cf3(1.2f, 1.2f, 1.2f));

    // 1. A GATED item. The gate composes with every combine mode, and its
    // fully-protected end is a branch rather than a mix — the one place the
    // scalar walk deliberately does not use cmix, so a blocked path that
    // "simplified" it would drift by one ULP along every protected border.
    {
        auto mask = std::make_shared<const field::FieldVolume>(field::FieldVolume::sample(
            sphere_at(kernel::cf3(0.3f, 0.0f, 0.0f), 0.5f), region, 0.05f, 0.4f));
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("gated");
        scene::Node base;
        base.prim = scene::Prim::sphere(0.6f);
        base.op = scene::Op::Add;
        l.sdf->insert(base);
        scene::Node cut;
        cut.prim = scene::Prim::box(kernel::cf3(0.5f, 0.5f, 0.5f));
        cut.xform.position = kernel::cf3(0.35f, 0.1f, 0.0f);
        cut.op = scene::Op::Subtract;
        cut.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.08f};
        cut.gate = mask;
        cut.gate_width = 0.12f;
        l.sdf->insert(cut);
        out.emplace_back("gated_subtract", scene::compile_document(doc));
    }

    // 2. A COLOURED volume, so the out-parameter the volume opcode writes
    // through is actually written. The corpus's volumes carry no colour, so
    // that store was unexercised.
    {
        auto vol = std::make_shared<const field::FieldVolume>(field::FieldVolume::sample_colored(
            sphere_at(kernel::cf3(0.0f, 0.0f, 0.0f), 0.7f),
            [](kernel::cfloat3 p) {
                return kernel::cf3(0.5f + 0.5f * p.x, 0.5f + 0.5f * p.y, 0.5f + 0.5f * p.z);
            },
            region, 0.05f, 0.3f));
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("coloured volume");
        scene::Node n;
        n.prim = scene::Prim::volume();
        n.volume = vol;
        n.op = scene::Op::Add;
        l.sdf->insert(n);
        // A stamp over it, so the colour has to survive a combine rather than
        // being the only thing on the stack.
        scene::Node stamp;
        stamp.prim = scene::Prim::sphere(0.35f);
        stamp.xform.position = kernel::cf3(0.4f, 0.2f, 0.0f);
        stamp.op = scene::Op::Add;
        stamp.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.1f};
        l.sdf->insert(stamp);
        out.emplace_back("coloured_volume_and_stamp", scene::compile_document(doc));
    }

    // 3. A RADIAL array, the repeat path that evaluates the primitive twice and
    // takes a min — two calls whose colour output is deliberately discarded.
    {
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("radial");
        scene::Node n;
        n.prim = scene::Prim::capsule(kernel::cf3(0.0f, 0.0f, 0.0f), kernel::cf3(0.5f, 0.0f, 0.0f),
                                      0.12f);
        n.repeat = scene::Repeat::radial(7, 0.35f);
        n.op = scene::Op::Add;
        l.sdf->insert(n);
        out.emplace_back("radial_array", scene::compile_document(doc));
    }
    return out;
}

}  // namespace

// A dense lattice over the working region rather than the corpus's seam ring.
// The gate's fully-protected branch only fires where the mask's smoothstep
// SATURATES, which is deep inside the protected region — a ring straddling a
// seam never gets there, and a probe set that never gets there cannot tell a
// `>= 1.0f` boundary from a `> 1.0f` one. Verified: with the seam ring alone,
// mutating that comparison left every assertion passing.
std::vector<float> lattice_points() {
    std::vector<float> xyz;
    const int n = 17;
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const auto c = [n](int i) {
                    return -1.1f + 2.2f * (static_cast<float>(i) / static_cast<float>(n - 1));
                };
                xyz.push_back(c(x));
                xyz.push_back(c(y));
                xyz.push_back(c(z));
            }
    return xyz;
}

TEST_CASE("tape block: bit-identical on the paths the parity corpus does not reach") {
    const std::vector<float> xyz = lattice_points();
    for (const auto& doc : extra_documents()) {
        CAPTURE(doc.first);
        REQUIRE_FALSE(doc.second.empty());
        const Run want = scalar(doc.second, xyz);
        for (std::size_t block : {std::size_t(1), std::size_t(7), std::size_t(512)}) {
            CAPTURE(block);
            const Run got = blocked(doc.second, xyz, block);
            CHECK(identical(want.distances, got.distances));
            CHECK(identical(want.colors, got.colors));
        }
    }
}

TEST_CASE("tape block: bit-identical to the scalar walk on the parity corpus") {
    const std::vector<io::FixtureCase> cases = io::kernel_parity_cases();
    REQUIRE(cases.size() > 0);
    for (const io::FixtureCase& c : cases) {
        CAPTURE(c.name);
        const std::vector<float> xyz = flatten(c.points);
        const Run want = scalar(c.tape, xyz);
        const Run got = blocked(c.tape, xyz, 0);
        CHECK(identical(want.distances, got.distances));
        CHECK(identical(want.colors, got.colors));
    }
}

TEST_CASE("tape block: block size is not observable") {
    const std::vector<io::FixtureCase> cases = io::kernel_parity_cases();
    for (const io::FixtureCase& c : cases) {
        CAPTURE(c.name);
        const std::vector<float> xyz = flatten(c.points);
        const Run want = scalar(c.tape, xyz);
        // 1 is the degenerate block — it must still agree, and it is what a
        // caller reaches with a single-point query.
        for (std::size_t block : {std::size_t(1), std::size_t(2), std::size_t(7),
                                  std::size_t(64), std::size_t(512), std::size_t(4096)}) {
            CAPTURE(block);
            const Run got = blocked(c.tape, xyz, block);
            CHECK(identical(want.distances, got.distances));
            CHECK(identical(want.colors, got.colors));
        }
    }
}

TEST_CASE("tape block: a ragged batch matches an aligned one, for every remainder") {
    // Task 1.9. The corpus's composed case, which pushes and combines, so a
    // remainder that mishandled the stack would show up rather than cancel.
    const std::vector<io::FixtureCase> cases = io::kernel_parity_cases();
    const io::FixtureCase& c = cases.back();
    CAPTURE(c.name);
    const std::vector<float> full = flatten(c.points);
    const std::size_t block = 8;
    REQUIRE(c.points.size() > block * 2);

    for (std::size_t r = 1; r < block; ++r) {
        const std::size_t count = block * 2 + r;
        CAPTURE(count);
        const std::vector<float> xyz(full.begin(), full.begin() + count * 3);
        const Run want = scalar(c.tape, xyz);
        const Run got = blocked(c.tape, xyz, block);
        CHECK(identical(want.distances, got.distances));
        CHECK(identical(want.colors, got.colors));
    }
}

TEST_CASE("tape block: an empty tape is far outside, as the scalar walk says") {
    scene::Document doc;
    doc.add_sdf_layer("empty");
    const scene::Tape tape = scene::compile_document(doc);
    REQUIRE(tape.empty());

    const std::vector<float> xyz{0.0f, 0.0f, 0.0f, 1.0f, 2.0f, 3.0f, -4.0f, 0.5f, 0.25f};
    const Run want = scalar(tape, xyz);
    const Run got = blocked(tape, xyz, 0);
    CHECK(identical(want.distances, got.distances));
    CHECK(identical(want.colors, got.colors));
}

TEST_CASE("tape block: the stack depth is the tape's, not the maximum") {
    // What the blocked path allocates against. A flat chain of stamps holds two
    // values at a time however long it is, and allocating CLAY_TAPE_MAX_STACK
    // per block instead would be eight times the memory for no reason.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("flat");
    for (int i = 0; i < 32; ++i) {
        scene::Node n;
        n.prim = scene::Prim::sphere(0.3f);
        n.xform.position = kernel::cf3(static_cast<float>(i) * 0.1f, 0.0f, 0.0f);
        n.op = scene::Op::Add;
        n.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.05f};
        l.sdf->insert(n);
    }
    const scene::Tape tape = scene::compile_document(doc);
    REQUIRE(tape.instrs.size() > 32);
    const std::size_t depth = eval::tape_stack_depth(tape);
    CHECK(depth >= 1);
    CHECK(depth <= 2);
}
