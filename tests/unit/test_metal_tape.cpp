#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "clay/eval/backend.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"
#include "scene_utils.h"

// Metal tape residency beyond parity (which test_parity.cpp already runs
// against every registered backend) and beyond the id-keyed cache that
// test_backend_residency.cpp pins on every backend. What is tested here is
// what is specific to THIS backend: an appended tape is served by copying the
// suffix it does not share with a resident one, rather than by allocating and
// re-copying all three buffers.
//
// Every case skips when no Metal device is present. That is deliberate and it
// is also why none of these ASSERT on absence: a machine without a device must
// not fail the suite, and a machine with one must not pass it vacuously -- so
// the gate that catches a vacuous run is the differential assertion count in
// docs/RELEASE.md.
//
// A patch that writes to the wrong offset still counts as a patch, still
// returns Ok, and still produces a field. So counting patches is the WEAK half
// of these tests; the assertion that matters is that a field arrived at by
// patching equals the same field uploaded whole.

using namespace clay;
using clay_test::item;

namespace clay {
namespace eval {
std::uint64_t metal_tape_uploads(const Backend& backend);
std::uint64_t metal_tape_patches(const Backend& backend);
}  // namespace eval
}  // namespace clay

namespace {

eval::Backend* metal() { return eval::Registry::instance().find("metal"); }

scene::Document two_spheres() {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::sphere(1.0f), kernel::cf3(-0.3f, 0.0f, 0.0f)));
    l.sdf->insert(item(scene::Prim::box(kernel::cf3(0.6f, 0.5f, 0.4f)),
                       kernel::cf3(0.4f, 0.1f, 0.0f)));
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

std::vector<float> eval_with(eval::Backend* mtl, const scene::Tape& tape,
                             const std::vector<float>& pts) {
    eval::PointQuery q;
    q.points_xyz = pts.data();
    q.count = pts.size() / 3;
    std::vector<float> out(q.count);
    eval::PointResults res;
    res.distances = out.data();
    REQUIRE(mtl->eval_points(tape, q, res) == eval::Status::Ok);
    return out;
}

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

// Puts enough else on the device that nothing of an earlier tape survives.
// Metal holds several tapes resident rather than one, so evicting takes more
// than a single unrelated upload -- and a test that assumed one would silently
// stop testing eviction the day the residency grew a slot.
void evict(eval::Backend* mtl, const std::vector<float>& pts) {
    for (int i = 0; i < 8; ++i) {
        scene::Document other;
        const float x = -1.0f + 0.21f * static_cast<float>(i);
        other.add_sdf_layer("other").sdf->insert(
            item(scene::Prim::box(kernel::cf3(0.3f, 0.9f, 0.2f)), kernel::cf3(x, 0.0f, 0.0f)));
        eval_with(mtl, scene::compile_document(other), pts);
    }
}

}  // namespace

TEST_CASE("metal: a stroke uploads once and patches after that") {
    eval::Backend* mtl = metal();
    if (!mtl) return;
    const std::vector<float> pts = lattice(6, 1.5f);

    Stroke s;
    s.begin(two_spheres());
    eval_with(mtl, s.tape, pts);
    const std::uint64_t uploads = eval::metal_tape_uploads(*mtl);
    const std::uint64_t patches = eval::metal_tape_patches(*mtl);

    for (int i = 1; i <= 8; ++i) {
        REQUIRE(s.dab(-0.6f + 0.15f * static_cast<float>(i), 0.35f));
        eval_with(mtl, s.tape, pts);
    }
    // Every dab after the first evaluation was a suffix copy, and the resident
    // tape advanced each time -- without that only the first dab would patch
    // and the other seven would re-upload.
    CHECK(eval::metal_tape_uploads(*mtl) == uploads);
    CHECK(eval::metal_tape_patches(*mtl) == patches + 8);
}

TEST_CASE("metal: a long stroke re-packs occasionally, not per dab") {
    eval::Backend* mtl = metal();
    if (!mtl) return;
    const std::vector<float> pts = lattice(4, 1.5f);

    // The buffers cannot have unbounded slack, and an MTL::Buffer cannot be
    // resized, so a stroke does eventually outgrow its reserved capacity and
    // is allocated afresh. That is correct and it is not a regression: each
    // re-pack reserves half again, so the re-packs are geometric and their
    // cost is amortised.
    //
    // What WOULD be a regression is re-packing per dab, which is what an
    // exact-fit buffer does -- and it is invisible to a test that only checks
    // the field, because every one of those uploads is correct. Hence a
    // counter test, and hence a long stroke rather than a short one.
    Stroke s;
    s.begin(two_spheres());
    eval_with(mtl, s.tape, pts);
    const std::uint64_t uploads = eval::metal_tape_uploads(*mtl);
    const std::uint64_t patches = eval::metal_tape_patches(*mtl);

    const int kDabs = 60;
    for (int i = 1; i <= kDabs; ++i) {
        REQUIRE(s.dab(-0.8f + 0.026f * static_cast<float>(i), 0.3f));
        eval_with(mtl, s.tape, pts);
    }
    const std::uint64_t re_packs = eval::metal_tape_uploads(*mtl) - uploads;
    const std::uint64_t patched = eval::metal_tape_patches(*mtl) - patches;
    CHECK(patched + re_packs == static_cast<std::uint64_t>(kDabs));
    CHECK(patched >= static_cast<std::uint64_t>(kDabs) * 9 / 10);
    CHECK(re_packs <= 6);  // geometric; per-dab would be 60
}

TEST_CASE("metal: a patched field is the field, not merely a field") {
    eval::Backend* mtl = metal();
    if (!mtl) return;
    const std::vector<float> pts = lattice(6, 1.5f);

    SUBCASE("over a stroke, so drift that accumulates is caught") {
        Stroke s;
        s.begin(two_spheres());
        eval_with(mtl, s.tape, pts);
        for (int i = 1; i <= 10; ++i) {
            REQUIRE(s.dab(-0.6f + 0.12f * static_cast<float>(i), 0.3f));
            const std::vector<float> patched = eval_with(mtl, s.tape, pts);
            evict(mtl, pts);
            const std::vector<float> whole = eval_with(mtl, s.tape, pts);
            CHECK(patched == whole);
        }
    }

    SUBCASE("on a document carrying a blob, which grows a second section") {
        // A stroke item puts points in the blob, so params and blob both grow
        // and the patch has to land in two buffers at two different offsets.
        // Swapping them writes a plausible field and fails only here.
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
        eval_with(mtl, s.tape, pts);
        for (int i = 1; i <= 6; ++i) {
            REQUIRE(s.dab(-0.5f + 0.18f * static_cast<float>(i), -0.4f));
            const std::vector<float> patched = eval_with(mtl, s.tape, pts);
            evict(mtl, pts);
            const std::vector<float> whole = eval_with(mtl, s.tape, pts);
            CHECK(patched == whole);
        }
    }
}

// Metal keeps SEVERAL tapes resident, which Vulkan does not, so it has a
// hazard Vulkan cannot have: the entry a patch lands in is found by ancestor
// id while the entry a whole upload evicts is found by least-recent use. Patch
// into the wrong slot and an unrelated tape -- a pick tape, a layer tape --
// silently becomes the stroke.
TEST_CASE("metal: patching a stroke leaves the other resident tapes alone") {
    eval::Backend* mtl = metal();
    if (!mtl) return;
    const std::vector<float> pts = lattice(5, 1.5f);

    scene::Document other;
    other.add_sdf_layer("pick").sdf->insert(
        item(scene::Prim::sphere(0.5f), kernel::cf3(0.0f, -0.9f, 0.0f)));
    const scene::Tape pick = scene::compile_document(other);
    const std::vector<float> pick_before = eval_with(mtl, pick, pts);

    Stroke s;
    s.begin(two_spheres());
    eval_with(mtl, s.tape, pts);
    for (int i = 1; i <= 6; ++i) {
        REQUIRE(s.dab(-0.5f + 0.16f * static_cast<float>(i), 0.4f));
        eval_with(mtl, s.tape, pts);
        // Still resident, still itself: an id hit that returns the stroke's
        // buffers would fail here and nowhere else.
        CHECK(eval_with(mtl, pick, pts) == pick_before);
    }
}

TEST_CASE("metal: a tape that cannot be patched is uploaded whole") {
    eval::Backend* mtl = metal();
    if (!mtl) return;
    const std::vector<float> pts = lattice(5, 1.5f);

    SUBCASE("one claiming no lineage") {
        scene::Tape plain = scene::compile_document(two_spheres());
        CHECK(plain.parent_id == 0);
        evict(mtl, pts);
        const std::uint64_t patches = eval::metal_tape_patches(*mtl);
        const std::vector<float> got = eval_with(mtl, plain, pts);
        CHECK(eval::metal_tape_patches(*mtl) == patches);
        CHECK(got.size() == pts.size() / 3);
    }

    SUBCASE("one whose ancestor is no longer resident") {
        Stroke s;
        s.begin(two_spheres());
        eval_with(mtl, s.tape, pts);
        REQUIRE(s.dab(0.5f, 0.4f));
        evict(mtl, pts);  // the ancestor is gone
        const std::uint64_t patches = eval::metal_tape_patches(*mtl);
        const std::uint64_t uploads = eval::metal_tape_uploads(*mtl);
        eval_with(mtl, s.tape, pts);
        CHECK(eval::metal_tape_patches(*mtl) == patches);
        CHECK(eval::metal_tape_uploads(*mtl) == uploads + 1);
    }

    SUBCASE("a hand-assembled tape is never served a resident upload") {
        // compile_id 0 means no identity, so nothing about it may be trusted
        // to match what is on the device: it goes through the scratch slots
        // and is re-copied per call, which is neither an upload nor a patch.
        scene::Tape hand = scene::compile_document(two_spheres());
        hand.compile_id = 0;
        hand.parent_id = 0;
        const std::vector<float> first = eval_with(mtl, hand, pts);
        const std::uint64_t uploads = eval::metal_tape_uploads(*mtl);
        const std::uint64_t patches = eval::metal_tape_patches(*mtl);
        const std::vector<float> second = eval_with(mtl, hand, pts);
        CHECK(eval::metal_tape_uploads(*mtl) == uploads);
        CHECK(eval::metal_tape_patches(*mtl) == patches);
        CHECK(first == second);
    }
}
