#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "clay/eval/backend.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"
#include "scene_utils.h"

// Resident uploaded tapes and pooled transfer scratch (evaluation-backends
// spec: repeated evaluation reuses device-resident state). A backend may keep
// the uploaded form of recent tapes and its transfer buffers across calls,
// keyed on Tape::compile_id — a process-unique id the compiler stamps on
// every tape it returns, so an id hit IS a content hit. What this suite pins,
// on EVERY registered backend (the CPU backend included, where it pins the
// stateless behaviour the default paths already have):
//
//   1. compile_document/compile_layer stamp process-unique nonzero ids, and a
//      hand-assembled tape carries 0 (never cached);
//   2. re-evaluating one tape returns byte-identical values call after call —
//      residency may change speed, never bytes;
//   3. a recompiled document is never served stale bytes, even when the new
//      tape's sections are the SAME SIZES as the old one's — the id is
//      identity, not a size heuristic;
//   4. more distinct tapes than any residency holds, interleaved, still
//      evaluate correctly after eviction and re-upload;
//   5. the batch path's pooled scratch survives interleaved batch sizes —
//      grow, shrink, grow again — with byte-identical values.

using namespace clay;
using kernel::cf3;
using clay_test::item;

namespace {

// One sphere at x: recompiling with a different x moves the field by whole
// units, far above any backend's parity tolerance — a stale tape cannot pass
// as the fresh one.
scene::Document sphere_document(float x) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::sphere(1.0f), cf3(x, 0, 0)));
    return doc;
}

std::vector<float> eval_grid_values(eval::Backend* backend, const scene::Tape& tape,
                                    const eval::GridQuery& q) {
    std::vector<float> values(static_cast<std::size_t>(q.nx) * q.ny * q.nz);
    REQUIRE(backend->eval_grid(tape, q, values.data()) == eval::Status::Ok);
    return values;
}

eval::GridQuery unit_grid() {
    eval::GridQuery q;
    q.origin = cf3(-1.1f, -1.1f, -1.1f);
    q.spacing = 0.275f;
    q.nx = q.ny = q.nz = 9;
    return q;
}

}  // namespace

TEST_CASE("compiled tapes carry a process-unique identity") {
    scene::Document doc = sphere_document(0.0f);
    scene::Tape a = scene::compile_document(doc);
    scene::Tape b = scene::compile_document(doc);
    CHECK(a.compile_id != 0);
    CHECK(b.compile_id != 0);
    CHECK(a.compile_id != b.compile_id);  // unique per compile, even of one document
    scene::Tape layer_tape = scene::compile_layer(doc.layers.front());
    CHECK(layer_tape.compile_id != 0);
    scene::CullRegion cull{math::Aabb{cf3(-0.2f, -0.2f, -0.2f), cf3(0.2f, 0.2f, 0.2f)}};
    scene::Tape culled = scene::compile_document(doc, &cull);
    CHECK(culled.compile_id != 0);
    scene::Tape hand;  // assembled by hand: no identity, never cached
    CHECK(hand.compile_id == 0);
}

TEST_CASE("re-evaluating one tape is byte-identical call after call") {
    const scene::Document doc = clay_test::gnarly_document();
    const scene::Tape tape = scene::compile_document(doc);
    const eval::GridQuery q = unit_grid();
    for (eval::Backend* backend : eval::Registry::instance().all()) {
        CAPTURE(backend->name());
        const std::vector<float> first = eval_grid_values(backend, tape, q);
        for (int rep = 0; rep < 3; ++rep) {
            const std::vector<float> again = eval_grid_values(backend, tape, q);
            CHECK(std::memcmp(again.data(), first.data(),
                              first.size() * sizeof(float)) == 0);
        }
    }
}

TEST_CASE("a recompiled document with same-sized sections is never served stale") {
    // Same node count, same tape sizes — only the bytes differ. A residency
    // keyed on anything weaker than identity would hit and answer with the
    // old field.
    const scene::Document at_zero = sphere_document(0.0f);
    const scene::Document at_two = sphere_document(2.0f);
    const scene::Tape tape_zero = scene::compile_document(at_zero);
    const scene::Tape tape_two = scene::compile_document(at_two);
    REQUIRE(tape_zero.instrs.size() == tape_two.instrs.size());
    REQUIRE(tape_zero.params.size() == tape_two.params.size());
    const eval::GridQuery q = unit_grid();
    for (eval::Backend* backend : eval::Registry::instance().all()) {
        CAPTURE(backend->name());
        eval_grid_values(backend, tape_zero, q);  // make tape_zero resident
        const std::vector<float> values = eval_grid_values(backend, tape_two, q);
        std::size_t i = 0;
        for (int z = 0; z < q.nz; ++z)
            for (int y = 0; y < q.ny; ++y)
                for (int x = 0; x < q.nx; ++x, ++i) {
                    const kernel::cfloat3 p =
                        cf3(q.origin.x + q.spacing * static_cast<float>(x),
                            q.origin.y + q.spacing * static_cast<float>(y),
                            q.origin.z + q.spacing * static_cast<float>(z));
                    CHECK(values[i] == doctest::Approx(tape_two.eval(p).d).epsilon(1e-3));
                }
    }
}

TEST_CASE("evaluation stays correct through eviction and re-upload") {
    // More distinct tapes than any small residency holds, two rounds: the
    // second round re-uploads what the first round evicted.
    std::vector<scene::Document> docs;
    std::vector<scene::Tape> tapes;
    for (int i = 0; i < 6; ++i) {
        docs.push_back(sphere_document(static_cast<float>(i) * 0.5f));
        tapes.push_back(scene::compile_document(docs.back()));
    }
    const kernel::cfloat3 probe = cf3(0.3f, 0.2f, -0.4f);
    eval::GridQuery q;
    q.origin = probe;
    q.spacing = 0.1f;
    q.nx = q.ny = q.nz = 1;
    for (eval::Backend* backend : eval::Registry::instance().all()) {
        CAPTURE(backend->name());
        for (int round = 0; round < 2; ++round)
            for (const scene::Tape& tape : tapes) {
                const std::vector<float> v = eval_grid_values(backend, tape, q);
                CHECK(v[0] == doctest::Approx(tape.eval(probe).d).epsilon(1e-3));
            }
    }
}

TEST_CASE("pooled batch scratch survives interleaved batch sizes") {
    const scene::Document doc = clay_test::gnarly_document();
    // Per-brick culled tapes over a 3x3x3 block of lattice origins, the
    // refill shape.
    std::vector<scene::Tape> tapes;
    std::vector<kernel::cfloat3> origins;
    for (int z = 0; z < 3; ++z)
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x) {
                const kernel::cfloat3 lo = cf3(-0.6f + 0.4f * static_cast<float>(x),
                                               -0.6f + 0.4f * static_cast<float>(y),
                                               -0.6f + 0.4f * static_cast<float>(z));
                scene::CullRegion cull{
                    math::Aabb{lo, lo + cf3(0.55f, 0.55f, 0.55f)}};
                tapes.push_back(scene::compile_document(doc, &cull));
                origins.push_back(lo);
            }
    std::vector<const scene::Tape*> ptrs;
    for (const scene::Tape& t : tapes) ptrs.push_back(&t);
    for (eval::Backend* backend : eval::Registry::instance().all()) {
        CAPTURE(backend->name());
        auto batch_values = [&](std::size_t count) {
            eval::GridBatchQuery bq;
            bq.tapes = ptrs.data();
            bq.origins = origins.data();
            bq.spacing = 0.05f;
            bq.nx = bq.ny = bq.nz = 8;
            bq.count = count;
            std::vector<float> values(count * 8 * 8 * 8);
            REQUIRE(backend->eval_grid_batch(bq, values.data()) == eval::Status::Ok);
            return values;
        };
        const std::vector<float> full_first = batch_values(tapes.size());
        batch_values(1);  // shrink: the pool must not serve stale tail bytes
        const std::vector<float> full_again = batch_values(tapes.size());
        CHECK(std::memcmp(full_first.data(), full_again.data(),
                          full_first.size() * sizeof(float)) == 0);
    }
}
