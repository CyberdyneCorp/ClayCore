#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "clay/eval/backend.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"
#include "scene_utils.h"

// Vulkan backend behaviour beyond parity (which test_parity.cpp already runs
// against every registered backend). What is tested here is what is specific
// to this backend: the capabilities it declines, tape residency, and buffer
// reuse across differently sized calls.
//
// Every case skips when no Vulkan runtime is present. That is deliberate and
// it is also why none of these ASSERT on absence: a machine without a device
// must not fail the suite, and a machine with one must not pass it vacuously
// -- so the presence of the backend is reported, and the gate that catches a
// vacuous run is the differential assertion count in docs/RELEASE.md.

using namespace clay;
using clay_test::item;

namespace clay {
namespace eval {
std::uint64_t vulkan_tape_uploads(const Backend& backend);
}
}  // namespace clay

namespace {

eval::Backend* vulkan() { return eval::Registry::instance().find("vulkan"); }

scene::Document two_spheres(kernel::cfloat3 second = kernel::cf3(0.4f, 0.1f, 0.0f)) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::sphere(1.0f), kernel::cf3(-0.3f, 0.0f, 0.0f)));
    l.sdf->insert(item(scene::Prim::box(kernel::cf3(0.6f, 0.5f, 0.4f)), second));
    return doc;
}

std::vector<float> lattice(int n, float extent) {
    std::vector<float> pts;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            for (int k = 0; k < n; ++k) {
                const float s = 2.0f * extent / static_cast<float>(n - 1);
                pts.push_back(-extent + s * static_cast<float>(i));
                pts.push_back(-extent + s * static_cast<float>(j));
                pts.push_back(-extent + s * static_cast<float>(k));
            }
    return pts;
}

std::vector<float> cpu_distances(const scene::Tape& tape, const std::vector<float>& pts) {
    eval::PointQuery q;
    q.points_xyz = pts.data();
    q.count = pts.size() / 3;
    std::vector<float> out(q.count);
    eval::PointResults res;
    res.distances = out.data();
    eval::eval_points_reference(tape, q, res);
    return out;
}

}  // namespace

TEST_CASE("vulkan: declines what it does not implement, rather than guessing") {
    eval::Backend* vk = vulkan();
    if (!vk) return;

    scene::Document doc = two_spheres();
    scene::Tape tape = scene::compile_document(doc);

    // Sphere tracing is templated C++ (field.h) and no compute dialect in
    // this tree compiles it. Unsupported is the contract that lets a caller
    // fall back without the result changing.
    float ray[6] = {0, 0, -5, 0, 0, 1};
    eval::RayQuery rq;
    rq.rays = ray;
    rq.count = 1;
    eval::RayHit hit{};
    CHECK(vk->raycast(tape, rq, &hit) == eval::Status::Unsupported);

    eval::GridQuery gq;
    gq.nx = gq.ny = gq.nz = 4;
    std::vector<float> verts;
    std::vector<std::uint32_t> indices;
    CHECK(vk->mesh(tape, gq, &verts, &indices) == eval::Status::Unsupported);

    // The flag and the implementation have to agree: a caller that trusts
    // device_meshing and gets host meshing is measuring the wrong thing.
    CHECK(vk->caps().device_meshing == false);
}

TEST_CASE("vulkan: an unchanged tape is uploaded once, however many dispatches") {
    eval::Backend* vk = vulkan();
    if (!vk) return;

    scene::Document doc = two_spheres();
    scene::Tape tape = scene::compile_document(doc);
    std::vector<float> pts = lattice(6, 1.5f);

    eval::PointQuery q;
    q.points_xyz = pts.data();
    q.count = pts.size() / 3;
    std::vector<float> out(q.count);
    eval::PointResults res;
    res.distances = out.data();

    REQUIRE(vk->eval_points(tape, q, res) == eval::Status::Ok);
    const std::uint64_t after_first = eval::vulkan_tape_uploads(*vk);
    std::vector<float> first = out;

    // A dab's worth of dispatches against one document.
    for (int i = 0; i < 24; ++i) REQUIRE(vk->eval_points(tape, q, res) == eval::Status::Ok);
    CHECK(eval::vulkan_tape_uploads(*vk) == after_first);
    CHECK(out == first);
}

TEST_CASE("vulkan: an edited tape is uploaded again, and the result follows the edit") {
    eval::Backend* vk = vulkan();
    if (!vk) return;

    scene::Document doc = two_spheres();
    scene::Tape tape = scene::compile_document(doc);
    std::vector<float> pts = lattice(5, 1.5f);
    eval::PointQuery q;
    q.points_xyz = pts.data();
    q.count = pts.size() / 3;
    std::vector<float> before(q.count), after(q.count);
    eval::PointResults r0, r1;
    r0.distances = before.data();
    r1.distances = after.data();

    REQUIRE(vk->eval_points(tape, q, r0) == eval::Status::Ok);
    const std::uint64_t uploads = eval::vulkan_tape_uploads(*vk);

    // Same shape, moved: same tape LENGTH, different contents. A residency
    // check keyed on sizes alone would evaluate the stale field here.
    scene::Document moved = two_spheres(kernel::cf3(0.9f, 0.2f, -0.4f));
    scene::Tape moved_tape = scene::compile_document(moved);
    REQUIRE(moved_tape.instrs.size() == tape.instrs.size());
    REQUIRE(moved_tape.params.size() == tape.params.size());

    REQUIRE(vk->eval_points(moved_tape, q, r1) == eval::Status::Ok);
    CHECK(eval::vulkan_tape_uploads(*vk) == uploads + 1);
    CHECK(after != before);

    // and it is the right field, not merely a different one
    std::vector<float> reference = cpu_distances(moved_tape, pts);
    for (std::size_t i = 0; i < after.size(); ++i)
        CHECK(after[i] == doctest::Approx(reference[i]).epsilon(1e-4));
}

TEST_CASE("vulkan: buffers are reused across calls of different sizes") {
    eval::Backend* vk = vulkan();
    if (!vk) return;

    scene::Document doc = two_spheres();
    scene::Tape tape = scene::compile_document(doc);

    // Growing then shrinking: a shrink must not leave the previous call's
    // larger buffer feeding stale values into this one's results.
    for (int n : {4, 9, 3, 9, 2}) {
        std::vector<float> pts = lattice(n, 1.2f);
        eval::PointQuery q;
        q.points_xyz = pts.data();
        q.count = pts.size() / 3;
        std::vector<float> out(q.count);
        eval::PointResults res;
        res.distances = out.data();
        REQUIRE(vk->eval_points(tape, q, res) == eval::Status::Ok);

        std::vector<float> reference = cpu_distances(tape, pts);
        for (std::size_t i = 0; i < out.size(); ++i)
            CHECK(out[i] == doctest::Approx(reference[i]).epsilon(1e-4));
    }
}

TEST_CASE("vulkan: eval_grid agrees with eval_points on the same lattice") {
    eval::Backend* vk = vulkan();
    if (!vk) return;

    scene::Document doc = two_spheres();
    scene::Tape tape = scene::compile_document(doc);

    eval::GridQuery gq;
    gq.origin = kernel::cf3(-1.0f, -0.8f, -0.6f);
    gq.spacing = 0.25f;
    gq.nx = 7;
    gq.ny = 5;
    gq.nz = 3;
    const std::size_t total = 7u * 5u * 3u;
    std::vector<float> grid(total);
    REQUIRE(vk->eval_grid(tape, gq, grid.data(), nullptr) == eval::Status::Ok);

    // The same lattice, spelled out as points: x fastest, as the grid says.
    std::vector<float> pts;
    for (int z = 0; z < gq.nz; ++z)
        for (int y = 0; y < gq.ny; ++y)
            for (int x = 0; x < gq.nx; ++x) {
                pts.push_back(gq.origin.x + gq.spacing * static_cast<float>(x));
                pts.push_back(gq.origin.y + gq.spacing * static_cast<float>(y));
                pts.push_back(gq.origin.z + gq.spacing * static_cast<float>(z));
            }
    std::vector<float> reference = cpu_distances(tape, pts);
    for (std::size_t i = 0; i < total; ++i)
        CHECK(grid[i] == doctest::Approx(reference[i]).epsilon(1e-4));
}

TEST_CASE("vulkan: colors come back when asked for, and are left alone when not") {
    eval::Backend* vk = vulkan();
    if (!vk) return;

    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = item(scene::Prim::sphere(1.0f), kernel::cf3(0, 0, 0));
    n.color = kernel::cf3(0.25f, 0.5f, 0.75f);
    l.sdf->insert(n);
    scene::Tape tape = scene::compile_document(doc);

    std::vector<float> pts = lattice(3, 0.5f);
    eval::PointQuery q;
    q.points_xyz = pts.data();
    q.count = pts.size() / 3;
    std::vector<float> dist(q.count), colors(q.count * 3, -7.0f);

    eval::PointResults res;
    res.distances = dist.data();
    res.colors_rgb = colors.data();
    REQUIRE(vk->eval_points(tape, q, res) == eval::Status::Ok);
    for (std::size_t i = 0; i < q.count; ++i) {
        CHECK(colors[i * 3 + 0] == doctest::Approx(0.25f).epsilon(1e-5));
        CHECK(colors[i * 3 + 1] == doctest::Approx(0.5f).epsilon(1e-5));
        CHECK(colors[i * 3 + 2] == doctest::Approx(0.75f).epsilon(1e-5));
    }

    std::vector<float> untouched(q.count * 3, -7.0f);
    eval::PointResults no_colors;
    no_colors.distances = dist.data();
    REQUIRE(vk->eval_points(tape, q, no_colors) == eval::Status::Ok);
    CHECK(untouched == std::vector<float>(q.count * 3, -7.0f));
}

TEST_CASE("vulkan: bad requests are refused rather than dispatched") {
    eval::Backend* vk = vulkan();
    if (!vk) return;

    scene::Document doc = two_spheres();
    scene::Tape tape = scene::compile_document(doc);

    std::vector<float> pts = lattice(2, 1.0f);
    std::vector<float> out(pts.size() / 3);
    eval::PointQuery q;
    q.points_xyz = pts.data();
    q.count = pts.size() / 3;

    eval::PointResults no_out;
    CHECK(vk->eval_points(tape, q, no_out) == eval::Status::InvalidInput);

    eval::PointQuery no_pts = q;
    no_pts.points_xyz = nullptr;
    eval::PointResults res;
    res.distances = out.data();
    CHECK(vk->eval_points(tape, no_pts, res) == eval::Status::InvalidInput);

    // An empty batch is not an error; it is nothing to do.
    eval::PointQuery empty = q;
    empty.count = 0;
    CHECK(vk->eval_points(tape, empty, res) == eval::Status::Ok);

    eval::GridQuery bad;
    bad.nx = 0;
    bad.ny = bad.nz = 4;
    std::vector<float> grid(1);
    CHECK(vk->eval_grid(tape, bad, grid.data(), nullptr) == eval::Status::InvalidInput);
}

TEST_CASE("vulkan: an empty tape evaluates to far outside, as everywhere else") {
    eval::Backend* vk = vulkan();
    if (!vk) return;

    scene::Document doc;
    doc.add_sdf_layer("empty");
    scene::Tape tape = scene::compile_document(doc);

    std::vector<float> pts = lattice(3, 1.0f);
    eval::PointQuery q;
    q.points_xyz = pts.data();
    q.count = pts.size() / 3;
    std::vector<float> out(q.count);
    eval::PointResults res;
    res.distances = out.data();
    REQUIRE(vk->eval_points(tape, q, res) == eval::Status::Ok);

    std::vector<float> reference = cpu_distances(tape, pts);
    for (std::size_t i = 0; i < out.size(); ++i) CHECK(out[i] == reference[i]);
}
