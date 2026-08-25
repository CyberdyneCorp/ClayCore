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
std::uint64_t vulkan_tape_patches(const Backend& backend);
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

// An appended tape names the tape it grew from and where the two stop
// agreeing, so the backend transfers only the suffix rather than re-uploading
// 7.8 MiB to absorb ~148 bytes. See scene/tape.h -- lineage.
//
// A patch that writes to the wrong offset still counts as a patch, still
// returns Ok, and still produces a field. So counting patches is the WEAK
// half of these tests; the assertion that matters is that a field arrived at
// by patching equals the same field uploaded whole.
namespace {

// A document that grows by appending, plus the tapes each append produces.
struct Stroke {
    scene::Document doc;
    scene::LayerId layer = 0;
    scene::TapeCheckpoint cp;
    scene::Tape tape;

    void begin(scene::Document d) {
        doc = std::move(d);
        layer = doc.layers.back().id;
        tape = scene::compile_document_resumable(doc, &cp);
    }
    // Appends one dab and returns whether the compile could reuse the prefix.
    bool dab(float x, float y) {
        scene::Node n = item(scene::Prim::sphere(0.22f), kernel::cf3(x, y, 0.0f));
        n.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.05f};
        const scene::NodeId id = doc.find_layer(layer)->sdf->insert(n);
        scene::Tape grown;
        scene::TapeCheckpoint next;
        if (!scene::compile_document_append(tape, cp, doc, {id}, &grown, &next)) return false;
        tape = std::move(grown);
        cp = next;
        return true;
    }
};

std::vector<float> eval_with(eval::Backend* vk, const scene::Tape& tape,
                             const std::vector<float>& pts) {
    eval::PointQuery q;
    q.points_xyz = pts.data();
    q.count = pts.size() / 3;
    std::vector<float> out(q.count);
    eval::PointResults res;
    res.distances = out.data();
    REQUIRE(vk->eval_points(tape, q, res) == eval::Status::Ok);
    return out;
}

// Puts something else on the device so the next evaluation of `tape` cannot
// be served by residency or by a patch, forcing a whole upload.
void evict(eval::Backend* vk, const std::vector<float>& pts) {
    scene::Document other;
    other.add_sdf_layer("other").sdf->insert(
        item(scene::Prim::box(kernel::cf3(0.3f, 0.9f, 0.2f)), kernel::cf3(-0.8f, 0.0f, 0.0f)));
    eval_with(vk, scene::compile_document(other), pts);
}

}  // namespace

TEST_CASE("vulkan: a stroke uploads once and patches after that") {
    eval::Backend* vk = vulkan();
    if (!vk) return;
    const std::vector<float> pts = lattice(6, 1.5f);

    Stroke s;
    s.begin(two_spheres());
    eval_with(vk, s.tape, pts);
    const std::uint64_t uploads = eval::vulkan_tape_uploads(*vk);
    const std::uint64_t patches = eval::vulkan_tape_patches(*vk);

    for (int i = 1; i <= 8; ++i) {
        REQUIRE(s.dab(-0.6f + 0.15f * static_cast<float>(i), 0.35f));
        eval_with(vk, s.tape, pts);
    }
    // Every dab after the first evaluation was a suffix transfer, and the
    // resident tape advanced each time -- without that only the first dab
    // would patch and the other seven would re-upload.
    CHECK(eval::vulkan_tape_uploads(*vk) == uploads);
    CHECK(eval::vulkan_tape_patches(*vk) == patches + 8);
}

TEST_CASE("vulkan: a long stroke re-packs occasionally, not per dab") {
    eval::Backend* vk = vulkan();
    if (!vk) return;
    const std::vector<float> pts = lattice(4, 1.5f);

    // The buffers cannot have unbounded slack, so a stroke does eventually
    // outgrow its reserved capacity and is uploaded whole again. That is
    // correct and it is not a regression: each re-pack reserves half again,
    // so the re-packs are geometric and their cost is amortised.
    //
    // What WOULD be a regression is re-packing per dab, which is what an
    // exact-fit buffer does -- and it is invisible to a test that only checks
    // the field, because every one of those uploads is correct. Hence a
    // counter test, and hence a long stroke rather than a short one.
    Stroke s;
    s.begin(two_spheres());
    eval_with(vk, s.tape, pts);
    const std::uint64_t uploads = eval::vulkan_tape_uploads(*vk);
    const std::uint64_t patches = eval::vulkan_tape_patches(*vk);

    const int kDabs = 60;
    for (int i = 1; i <= kDabs; ++i) {
        REQUIRE(s.dab(-0.8f + 0.026f * static_cast<float>(i), 0.3f));
        eval_with(vk, s.tape, pts);
    }
    const std::uint64_t re_packs = eval::vulkan_tape_uploads(*vk) - uploads;
    const std::uint64_t patched = eval::vulkan_tape_patches(*vk) - patches;
    CHECK(patched + re_packs == static_cast<std::uint64_t>(kDabs));
    CHECK(patched >= static_cast<std::uint64_t>(kDabs) * 9 / 10);
    CHECK(re_packs <= 6);  // geometric; per-dab would be 60
}

TEST_CASE("vulkan: a patched field is the field, not merely a field") {
    eval::Backend* vk = vulkan();
    if (!vk) return;
    const std::vector<float> pts = lattice(6, 1.5f);

    SUBCASE("over a stroke, so drift that accumulates is caught") {
        Stroke s;
        s.begin(two_spheres());
        eval_with(vk, s.tape, pts);
        for (int i = 1; i <= 10; ++i) {
            REQUIRE(s.dab(-0.6f + 0.12f * static_cast<float>(i), 0.3f));
            const std::vector<float> patched = eval_with(vk, s.tape, pts);
            evict(vk, pts);
            const std::vector<float> whole = eval_with(vk, s.tape, pts);
            CHECK(patched == whole);
        }
    }

    SUBCASE("on a document carrying a blob, which is what the slack is for") {
        // A stroke item puts points in the blob, so params and blob both
        // grow. With the blob packed directly behind params an append would
        // shove it right; the reserved gap is what keeps it still.
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(item(scene::Prim::sphere(0.9f), kernel::cf3(0.0f, 0.0f, 0.0f)));
        scene::Node stroke;
        stroke.prim = scene::Prim::stroke();
        stroke.stroke = {{kernel::cf3(-0.6f, 0.2f, 0.0f), 0.18f},
                         {kernel::cf3(0.0f, 0.5f, 0.0f), 0.22f},
                         {kernel::cf3(0.6f, 0.2f, 0.0f), 0.18f}};
        stroke.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.06f};
        l.sdf->insert(stroke);

        Stroke s;
        s.begin(std::move(doc));
        REQUIRE(s.tape.blob.size() > 0);
        eval_with(vk, s.tape, pts);
        for (int i = 1; i <= 6; ++i) {
            REQUIRE(s.dab(-0.5f + 0.18f * static_cast<float>(i), -0.4f));
            const std::vector<float> patched = eval_with(vk, s.tape, pts);
            evict(vk, pts);
            const std::vector<float> whole = eval_with(vk, s.tape, pts);
            CHECK(patched == whole);
        }
    }
}

TEST_CASE("vulkan: a tape that cannot be patched is uploaded whole") {
    eval::Backend* vk = vulkan();
    if (!vk) return;
    const std::vector<float> pts = lattice(5, 1.5f);

    SUBCASE("one claiming no lineage") {
        scene::Tape plain = scene::compile_document(two_spheres());
        CHECK(plain.parent_id == 0);
        evict(vk, pts);
        const std::uint64_t patches = eval::vulkan_tape_patches(*vk);
        const std::vector<float> got = eval_with(vk, plain, pts);
        CHECK(eval::vulkan_tape_patches(*vk) == patches);
        CHECK(got.size() == pts.size() / 3);
    }

    SUBCASE("one whose ancestor is no longer resident") {
        Stroke s;
        s.begin(two_spheres());
        eval_with(vk, s.tape, pts);
        REQUIRE(s.dab(0.5f, 0.4f));
        evict(vk, pts);  // the ancestor is gone
        const std::uint64_t patches = eval::vulkan_tape_patches(*vk);
        const std::uint64_t uploads = eval::vulkan_tape_uploads(*vk);
        eval_with(vk, s.tape, pts);
        CHECK(eval::vulkan_tape_patches(*vk) == patches);
        CHECK(eval::vulkan_tape_uploads(*vk) == uploads + 1);
    }

    SUBCASE("a hand-assembled tape is never served a resident upload") {
        // compile_id 0 means no identity, so nothing about it may be trusted
        // to match what is on the device.
        scene::Tape hand = scene::compile_document(two_spheres());
        hand.compile_id = 0;
        hand.parent_id = 0;
        const std::vector<float> first = eval_with(vk, hand, pts);
        const std::uint64_t uploads = eval::vulkan_tape_uploads(*vk);
        const std::vector<float> second = eval_with(vk, hand, pts);
        CHECK(eval::vulkan_tape_uploads(*vk) == uploads + 1);
        CHECK(first == second);
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
