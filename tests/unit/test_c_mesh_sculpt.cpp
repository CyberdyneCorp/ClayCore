// The C ABI's fixed-topology mesh brushes (c-abi spec): the sculpting session,
// the verbs, strokes and masks, vertex-delta undo and mesh picking.
//
// What these defend is the boundary rather than the maths — the C++ suite owns
// the maths. Here: unknown enumerators are refused rather than mapped onto a
// default, cost knobs are bounded, a protected layer takes no edits, and a
// session whose layer disappeared says so instead of reading freed storage.

#include <doctest/doctest.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "clay.h"

namespace {

struct Doc {
    clay_document* doc = clay_document_create();
    Doc() = default;
    ~Doc() { clay_document_destroy(doc); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

// A grid on the XZ plane: enough vertices that a falloff has something to
// choose between, and flat, so a displacement is obvious.
struct Grid {
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
};

Grid grid(int n, float half) {
    Grid g;
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x) {
            g.positions.push_back(-half + step * static_cast<float>(x));
            g.positions.push_back(0.0f);
            g.positions.push_back(-half + step * static_cast<float>(z));
        }
    const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
    for (std::uint32_t z = 0; z < static_cast<std::uint32_t>(n); ++z)
        for (std::uint32_t x = 0; x < static_cast<std::uint32_t>(n); ++x) {
            const std::uint32_t a = z * stride + x, b = a + 1, c = a + stride, d = c + 1;
            for (std::uint32_t i : {a, c, b, b, c, d}) g.indices.push_back(i);
        }
    return g;
}

clay_mesh* grid_mesh(int n = 12, float half = 1.0f) {
    const Grid g = grid(n, half);
    clay_mesh* m = nullptr;
    REQUIRE(clay_mesh_from_triangles(g.positions.data(), g.positions.size() / 3, g.indices.data(),
                                     g.indices.size(), &m) == CLAY_OK);
    return m;
}

clay_mesh_brush_desc brush(int32_t verb, float radius, float strength) {
    clay_mesh_brush_desc d;
    d.struct_size = sizeof(d);
    REQUIRE(clay_mesh_brush_defaults(&d) == CLAY_OK);
    d.verb = verb;
    d.radius = radius;
    d.strength = strength;
    return d;
}

std::vector<float> positions_of(const clay_mesh* m) {
    const float* p = clay_mesh_positions(m);
    return std::vector<float>(p, p + clay_mesh_vertex_count(m) * 3);
}

std::vector<std::uint32_t> indices_of(const clay_mesh* m) {
    const std::uint32_t* i = clay_mesh_indices(m);
    return std::vector<std::uint32_t>(i, i + clay_mesh_index_count(m));
}

}  // namespace

TEST_CASE("c abi: a sculpting session stamps a mesh and leaves its topology alone") {
    clay_mesh* m = grid_mesh();
    const std::vector<float> before = positions_of(m);
    const std::vector<std::uint32_t> topology = indices_of(m);

    clay_mesh_sculptor* s = nullptr;
    REQUIRE(clay_mesh_sculptor_create(m, -1.0f, &s) == CLAY_OK);

    std::size_t vertices = 0, classes = 0;
    CHECK(clay_mesh_sculptor_vertex_count(s, &vertices) == CLAY_OK);
    CHECK(clay_mesh_sculptor_class_count(s, &classes) == CLAY_OK);
    CHECK(vertices == before.size() / 3);
    CHECK(classes == vertices);  // this grid has no seams

    const clay_mesh_brush_desc d = brush(CLAY_MESH_BRUSH_DRAW, 0.5f, 0.5f);
    std::size_t moved = 0;
    CHECK(clay_mesh_sculptor_stamp(s, &d, nullptr, nullptr, &moved) == CLAY_OK);
    CHECK(moved > 0);

    CHECK(indices_of(m) == topology);
    CHECK(positions_of(m) != before);

    clay_mesh_sculptor_destroy(s);
    clay_mesh_destroy(m);
}

TEST_CASE("c abi: every verb is reachable and none of them rewrites the index buffer") {
    for (int32_t verb = CLAY_MESH_BRUSH_GRAB; verb <= CLAY_MESH_BRUSH_SNAKEHOOK; ++verb) {
        clay_mesh* m = grid_mesh();
        const std::vector<std::uint32_t> topology = indices_of(m);
        clay_mesh_sculptor* s = nullptr;
        REQUIRE(clay_mesh_sculptor_create(m, -1.0f, &s) == CLAY_OK);

        clay_mesh_brush_desc d = brush(verb, 0.5f, 0.5f);
        d.direction[1] = 0.1f;  // grab and snakehook need a motion
        d.geodesic = verb == CLAY_MESH_BRUSH_FLATTEN || verb == CLAY_MESH_BRUSH_SCRAPE ? 0 : 1;
        std::size_t moved = 0;
        CAPTURE(verb);
        CHECK(clay_mesh_sculptor_stamp(s, &d, nullptr, nullptr, &moved) == CLAY_OK);
        CHECK(indices_of(m) == topology);

        clay_mesh_sculptor_destroy(s);
        clay_mesh_destroy(m);
    }
}

TEST_CASE("c abi: unknown enumerators and unmeanable knobs are refused, not defaulted") {
    clay_mesh* m = grid_mesh();
    clay_mesh_sculptor* s = nullptr;
    REQUIRE(clay_mesh_sculptor_create(m, -1.0f, &s) == CLAY_OK);
    const std::vector<float> before = positions_of(m);

    auto refused = [&](clay_mesh_brush_desc d) {
        return clay_mesh_sculptor_stamp(s, &d, nullptr, nullptr, nullptr) ==
               CLAY_ERROR_INVALID_ARGUMENT;
    };

    clay_mesh_brush_desc d = brush(CLAY_MESH_BRUSH_DRAW, 0.5f, 0.5f);
    d.verb = 99;
    CHECK(refused(d));

    d = brush(CLAY_MESH_BRUSH_DRAW, 0.5f, 0.5f);
    d.falloff = 7;
    CHECK(refused(d));

    d = brush(CLAY_MESH_BRUSH_FLATTEN, 0.5f, 0.5f);
    d.flatten_mode = 5;
    CHECK(refused(d));

    d = brush(CLAY_MESH_BRUSH_DRAW, 0.0f, 0.5f);
    CHECK(refused(d));

    d = brush(CLAY_MESH_BRUSH_SMOOTH, 0.5f, 0.5f);
    d.smooth_iterations = CLAY_MESH_MAX_SMOOTH_ITERATIONS + 1;
    CHECK(refused(d));

    d = brush(CLAY_MESH_BRUSH_DRAW, 0.5f, 0.5f);
    d.struct_size = 4;  // below the original layout
    CHECK(refused(d));

    // Not one of them touched the mesh.
    CHECK(positions_of(m) == before);

    // Zero iterations, which is what a host that declared only the original
    // layout sends, means one pass rather than a refusal.
    d = brush(CLAY_MESH_BRUSH_SMOOTH, 0.5f, 0.5f);
    d.smooth_iterations = 0;
    CHECK(clay_mesh_sculptor_stamp(s, &d, nullptr, nullptr, nullptr) == CLAY_OK);

    clay_mesh_sculptor_destroy(s);
    clay_mesh_destroy(m);
}

TEST_CASE("c abi: a stroke reaches a mesh, and one record undoes it bit-exactly") {
    clay_mesh* m = grid_mesh(16, 1.0f);
    const std::vector<float> before = positions_of(m);
    clay_mesh_sculptor* s = nullptr;
    REQUIRE(clay_mesh_sculptor_create(m, -1.0f, &s) == CLAY_OK);

    clay_stroke_preset preset;
    preset.struct_size = sizeof(preset);
    REQUIRE(clay_stroke_preset_defaults(&preset) == CLAY_OK);
    preset.radius = 0.25f;
    preset.strength = 0.8f;

    std::vector<float> samples;
    for (int i = 0; i < 8; ++i) {
        samples.push_back(-0.4f + 0.1f * static_cast<float>(i));
        samples.push_back(0.0f);
        samples.push_back(0.0f);
        samples.push_back(1.0f);  // pressure
        samples.push_back(0.0f);  // tilt
    }

    clay_mesh_deltas* deltas = clay_mesh_deltas_create();
    REQUIRE(deltas != nullptr);
    const clay_mesh_brush_desc d = brush(CLAY_MESH_BRUSH_DRAW, 0.25f, 1.0f);
    std::size_t applied = 0;
    CHECK(clay_mesh_sculptor_apply_stroke(s, samples.data(), samples.size() / 5, &preset, &d,
                                          nullptr, nullptr, 0, deltas, &applied) == CLAY_OK);
    CHECK(applied > 1);
    CHECK(positions_of(m) != before);

    std::size_t touched = 0;
    CHECK(clay_mesh_deltas_vertex_count(deltas, &touched) == CLAY_OK);
    CHECK(touched > 0);
    CHECK(touched <= clay_mesh_vertex_count(m));  // coalesced: one record per vertex

    CHECK(clay_mesh_deltas_revert(deltas, s) == CLAY_OK);
    CHECK(positions_of(m) == before);
    // Idempotent, and re-applicable.
    CHECK(clay_mesh_deltas_revert(deltas, s) == CLAY_OK);
    CHECK(positions_of(m) == before);
    CHECK(clay_mesh_deltas_apply(deltas, s) == CLAY_OK);
    CHECK(positions_of(m) != before);

    CHECK(clay_mesh_deltas_clear(deltas) == CLAY_OK);
    CHECK(clay_mesh_deltas_vertex_count(deltas, &touched) == CLAY_OK);
    CHECK(touched == 0);

    clay_mesh_deltas_destroy(deltas);
    clay_mesh_sculptor_destroy(s);
    clay_mesh_destroy(m);
}

TEST_CASE("c abi: a mask gates a mesh stroke") {
    clay_mesh* m = grid_mesh(16, 1.0f);
    const std::vector<float> before = positions_of(m);
    clay_mesh_sculptor* s = nullptr;
    REQUIRE(clay_mesh_sculptor_create(m, -1.0f, &s) == CLAY_OK);

    clay_mask* mask = clay_mask_create(0.05f);
    REQUIRE(mask != nullptr);
    const float centre[3] = {0.6f, 0.0f, 0.0f};
    clay_brush_params bp;
    std::memset(&bp, 0, sizeof bp);
    bp.struct_size = static_cast<std::uint32_t>(sizeof bp);
    bp.size = 40;
    bp.shape = CLAY_BRUSH_SHAPE_CUBE;
    bp.falloff = CLAY_BRUSH_FALLOFF_CONSTANT;
    bp.strength = 1.0f;
    REQUIRE(clay_mask_paint(mask, centre, &bp, 1.0f) == CLAY_OK);

    clay_stroke_preset preset;
    preset.struct_size = sizeof(preset);
    REQUIRE(clay_stroke_preset_defaults(&preset) == CLAY_OK);
    preset.radius = 0.25f;
    preset.strength = 1.0f;
    const float samples[10] = {-0.4f, 0, 0, 1, 0, 0.4f, 0, 0, 1, 0};

    const clay_mesh_brush_desc d = brush(CLAY_MESH_BRUSH_DRAW, 0.25f, 1.0f);
    std::size_t applied = 0;
    CHECK(clay_mesh_sculptor_apply_stroke(s, samples, 2, &preset, &d, mask, nullptr, 0, nullptr,
                                          &applied) == CLAY_OK);
    CHECK(applied > 0);

    const std::vector<float> after = positions_of(m);
    bool frozen_held = true, free_moved = false;
    for (std::size_t v = 0; v * 3 + 2 < after.size(); ++v) {
        const float x = before[v * 3], z = before[v * 3 + 2];
        if (x > 0.5f && std::fabs(z) < 0.4f) {
            if (after[v * 3 + 1] != before[v * 3 + 1]) frozen_held = false;
        } else if (after[v * 3 + 1] != before[v * 3 + 1]) {
            free_moved = true;
        }
    }
    CHECK(frozen_held);
    CHECK(free_moved);

    clay_mask_destroy(mask);
    clay_mesh_sculptor_destroy(s);
    clay_mesh_destroy(m);
}

TEST_CASE("c abi: a raycast turns a tap into a brush centre and a walk seed") {
    clay_mesh* m = grid_mesh(8, 1.0f);
    clay_mesh_sculptor* s = nullptr;
    REQUIRE(clay_mesh_sculptor_create(m, -1.0f, &s) == CLAY_OK);

    const float origin[3] = {0.21f, 1.0f, -0.13f};
    const float direction[3] = {0.0f, -1.0f, 0.0f};
    clay_mesh_hit hit;
    std::memset(&hit, 0, sizeof hit);
    hit.struct_size = static_cast<std::uint32_t>(sizeof hit);
    REQUIRE(clay_mesh_sculptor_raycast(s, origin, direction, nullptr, &hit) == CLAY_OK);
    CHECK(hit.hit == 1);
    CHECK(hit.t == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(hit.position[1] == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(hit.normal[1] == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(hit.seed_class != CLAY_MESH_NO_CLASS);

    // The hit feeds straight back into a stamp.
    clay_mesh_brush_desc d = brush(CLAY_MESH_BRUSH_DRAW, 0.3f, 0.5f);
    std::memcpy(d.center, hit.position, sizeof d.center);
    d.seed_class = hit.seed_class;
    std::size_t moved = 0;
    CHECK(clay_mesh_sculptor_stamp(s, &d, nullptr, nullptr, &moved) == CLAY_OK);
    CHECK(moved > 0);

    // A ray pointed away reports a miss rather than a hit at the origin.
    const float away[3] = {9.0f, 1.0f, 9.0f};
    REQUIRE(clay_mesh_sculptor_raycast(s, away, direction, nullptr, &hit) == CLAY_OK);
    CHECK(hit.hit == 0);

    // A direction with no length is a mistake, not a request.
    const float still[3] = {0.0f, 0.0f, 0.0f};
    CHECK(clay_mesh_sculptor_raycast(s, origin, still, nullptr, &hit) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    clay_mesh_sculptor_destroy(s);
    clay_mesh_destroy(m);
}

TEST_CASE("c abi: a raycast reads a frame, and refuses one that scales to nothing") {
    clay_mesh* m = grid_mesh(8, 1.0f);
    clay_mesh_sculptor* s = nullptr;
    REQUIRE(clay_mesh_sculptor_create(m, -1.0f, &s) == CLAY_OK);

    clay_mesh_frame frame;
    std::memset(&frame, 0, sizeof frame);
    frame.struct_size = static_cast<std::uint32_t>(sizeof frame);
    frame.position[1] = 2.0f;
    frame.scale = 0.0f;  // a zeroed frame reads as the identity, scale included

    const float origin[3] = {0.1f, 5.0f, 0.1f};
    const float direction[3] = {0.0f, -1.0f, 0.0f};
    clay_mesh_hit hit;
    std::memset(&hit, 0, sizeof hit);
    hit.struct_size = static_cast<std::uint32_t>(sizeof hit);
    REQUIRE(clay_mesh_sculptor_raycast(s, origin, direction, &frame, &hit) == CLAY_OK);
    CHECK(hit.hit == 1);
    CHECK(hit.position[1] == doctest::Approx(2.0f).epsilon(1e-4));

    frame.scale = -1.0f;
    CHECK(clay_mesh_sculptor_raycast(s, origin, direction, &frame, &hit) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    clay_mesh_sculptor_destroy(s);
    clay_mesh_destroy(m);
}

TEST_CASE("c abi: a mesh layer sculpts in place, and a locked one refuses") {
    Doc d;
    clay_mesh* source = grid_mesh(8, 1.0f);
    clay_mesh_layer_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.struct_size = static_cast<std::uint32_t>(sizeof desc);
    desc.name = "carried";
    clay_layer_id layer = 0;
    clay_mesh* borrowed = nullptr;
    REQUIRE(clay_document_add_mesh_layer(d.doc, source, &desc, &layer, &borrowed) == CLAY_OK);
    clay_mesh_destroy(source);

    const std::vector<float> before = positions_of(borrowed);
    clay_mesh_sculptor* s = nullptr;
    REQUIRE(clay_mesh_sculptor_create(borrowed, -1.0f, &s) == CLAY_OK);

    const clay_mesh_brush_desc brush_desc = brush(CLAY_MESH_BRUSH_DRAW, 0.5f, 0.5f);
    std::size_t moved = 0;
    CHECK(clay_mesh_sculptor_stamp(s, &brush_desc, nullptr, nullptr, &moved) == CLAY_OK);
    CHECK(moved > 0);
    // The DOCUMENT's own triangles changed, not a copy.
    CHECK(positions_of(borrowed) != before);

    // Lock it and the same stamp is refused.
    REQUIRE(clay_document_set_layer_protection(d.doc, layer, 0, 1) == CLAY_OK);
    const std::vector<float> locked_state = positions_of(borrowed);
    CHECK(clay_mesh_sculptor_stamp(s, &brush_desc, nullptr, nullptr, &moved) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(positions_of(borrowed) == locked_state);
    REQUIRE(clay_document_set_layer_protection(d.doc, layer, 0, 0) == CLAY_OK);

    // A ghosted layer says the same thing for its own reason.
    REQUIRE(clay_document_set_layer_protection(d.doc, layer, 1, 0) == CLAY_OK);
    CHECK(clay_mesh_sculptor_stamp(s, &brush_desc, nullptr, nullptr, &moved) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_document_set_layer_protection(d.doc, layer, 0, 0) == CLAY_OK);

    // And a session outliving its layer refuses instead of reading freed memory.
    REQUIRE(clay_document_remove_layer(d.doc, layer) == CLAY_OK);
    CHECK(clay_mesh_sculptor_stamp(s, &brush_desc, nullptr, nullptr, &moved) ==
          CLAY_ERROR_NOT_FOUND);

    clay_mesh_sculptor_destroy(s);
    clay_mesh_destroy(borrowed);
}

// Issue #368. The header now says a sculptor may be BUILT on any thread against
// one const document, which is what lets a host arm a mesh subtool without
// freezing on the weld and the tree. Nothing else in the ABI made that
// promise, so nothing else was holding it: this is the test the paragraph
// stands on.
TEST_CASE("c abi: sculptors build concurrently against one const document") {
    Doc d;
    // Two layers, so the workers cover both cases the contract covers: several
    // threads over ONE mesh, and threads over different meshes of the same
    // document at once.
    clay_layer_id layers[2] = {0, 0};
    clay_mesh* meshes[2] = {nullptr, nullptr};
    for (int i = 0; i < 2; ++i) {
        clay_mesh* source = grid_mesh(10 + i * 4, 1.0f);
        clay_mesh_layer_desc desc;
        std::memset(&desc, 0, sizeof desc);
        desc.struct_size = static_cast<std::uint32_t>(sizeof desc);
        desc.name = i == 0 ? "first" : "second";
        REQUIRE(clay_document_add_mesh_layer(d.doc, source, &desc, &layers[i], &meshes[i]) ==
                CLAY_OK);
        clay_mesh_destroy(source);
    }

    // What a serial build answers, which is what a concurrent one must answer.
    struct Reference {
        std::size_t vertices = 0;
        std::size_t classes = 0;
        clay_mesh_hit hit{};
    };
    const float origin[3] = {0.1f, 1.0f, -0.2f};
    const float direction[3] = {0.0f, -1.0f, 0.0f};
    Reference reference[2];
    for (int i = 0; i < 2; ++i) {
        clay_mesh_sculptor* s = nullptr;
        REQUIRE(clay_mesh_sculptor_create(meshes[i], -1.0f, &s) == CLAY_OK);
        REQUIRE(clay_mesh_sculptor_vertex_count(s, &reference[i].vertices) == CLAY_OK);
        REQUIRE(clay_mesh_sculptor_class_count(s, &reference[i].classes) == CLAY_OK);
        reference[i].hit.struct_size = static_cast<std::uint32_t>(sizeof(clay_mesh_hit));
        REQUIRE(clay_mesh_sculptor_raycast(s, origin, direction, nullptr, &reference[i].hit) ==
                CLAY_OK);
        REQUIRE(reference[i].hit.hit != 0);  // the ray must reach the grid, or this proves nothing
        clay_mesh_sculptor_destroy(s);
    }

    // The document is not touched from here on: no thread below mutates it, and
    // no thread here is the one the host would be editing from. That IS the
    // contract — free-threaded against a const document, not against an edit.
    std::atomic<int> mismatches{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&, t] {
            const int which = t % 2;
            for (int k = 0; k < 6; ++k) {
                clay_mesh_sculptor* s = nullptr;
                if (clay_mesh_sculptor_create(meshes[which], -1.0f, &s) != CLAY_OK) {
                    mismatches.fetch_add(1);
                    continue;
                }
                // What the header tells a host to do on the worker: build, then
                // warm the tree, so the interface thread's first pick pays for
                // neither.
                if (clay_mesh_sculptor_refresh(s) != CLAY_OK) mismatches.fetch_add(1);
                std::size_t vertices = 0, classes = 0;
                clay_mesh_hit hit{};
                hit.struct_size = static_cast<std::uint32_t>(sizeof hit);
                if (clay_mesh_sculptor_vertex_count(s, &vertices) != CLAY_OK ||
                    clay_mesh_sculptor_class_count(s, &classes) != CLAY_OK ||
                    clay_mesh_sculptor_raycast(s, origin, direction, nullptr, &hit) != CLAY_OK)
                    mismatches.fetch_add(1);
                if (vertices != reference[which].vertices ||
                    classes != reference[which].classes || hit.hit != reference[which].hit.hit ||
                    hit.triangle != reference[which].hit.triangle ||
                    hit.t != reference[which].hit.t)
                    mismatches.fetch_add(1);
                clay_mesh_sculptor_destroy(s);
            }
        });
    }
    for (std::thread& w : workers) w.join();
    CHECK(mismatches.load() == 0);

    for (clay_mesh* m : meshes) clay_mesh_destroy(m);
}

// The other half of the same paragraph: a worker that fails reads its OWN
// message. A shared error slot would make the contract unusable — a host could
// not tell which build failed, or read a message belonging to another thread.
TEST_CASE("c abi: a failed build on a worker leaves the calling thread's error alone") {
    clay_mesh* m = grid_mesh(6, 1.0f);
    clay_mesh_sculptor* ours = nullptr;
    REQUIRE(clay_mesh_sculptor_create(nullptr, -1.0f, &ours) == CLAY_ERROR_INVALID_ARGUMENT);
    const std::string here = clay_last_error() ? clay_last_error() : "";
    REQUIRE(!here.empty());

    std::string theirs;
    std::thread worker([&] {
        // A DIFFERENT failure, so the two messages cannot be confused for one
        // another: this one is refused on the out pointer, ours on the mesh.
        CHECK(clay_mesh_sculptor_create(m, -1.0f, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
        theirs = clay_last_error() ? clay_last_error() : "";
    });
    worker.join();

    CHECK(!theirs.empty());
    CHECK(theirs != here);
    // Ours survived the worker's, which is the whole point.
    CHECK(std::string(clay_last_error() ? clay_last_error() : "") == here);

    // A successful build on a worker is the case that actually matters, and it
    // must not disturb this thread's message either.
    std::atomic<clay_result> built{CLAY_ERROR_INVALID_ARGUMENT};
    std::thread ok([&] {
        clay_mesh_sculptor* s = nullptr;
        built.store(clay_mesh_sculptor_create(m, -1.0f, &s));
        clay_mesh_sculptor_destroy(s);
    });
    ok.join();
    CHECK(built.load() == CLAY_OK);
    CHECK(std::string(clay_last_error() ? clay_last_error() : "") == here);

    clay_mesh_destroy(m);
}

TEST_CASE("c abi: the null paths refuse rather than crash") {
    clay_mesh_sculptor* s = reinterpret_cast<clay_mesh_sculptor*>(1);
    CHECK(clay_mesh_sculptor_create(nullptr, -1.0f, &s) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(s == nullptr);

    clay_mesh* m = grid_mesh(4, 1.0f);
    CHECK(clay_mesh_sculptor_create(m, -1.0f, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_mesh_sculptor_create(m, -1.0f, &s) == CLAY_OK);

    const clay_mesh_brush_desc d = brush(CLAY_MESH_BRUSH_DRAW, 0.5f, 0.5f);
    CHECK(clay_mesh_sculptor_stamp(nullptr, &d, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mesh_sculptor_stamp(s, nullptr, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mesh_brush_defaults(nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mesh_deltas_vertex_count(nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mesh_deltas_revert(nullptr, s) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mesh_deltas_clear(nullptr) == CLAY_ERROR_INVALID_ARGUMENT);

    // Destroying a null handle is a no-op, as every destroy in this ABI is.
    clay_mesh_sculptor_destroy(nullptr);
    clay_mesh_deltas_destroy(nullptr);

    clay_mesh_sculptor_destroy(s);
    clay_mesh_destroy(m);
}

TEST_CASE("c abi: a lattice cage deforms the mesh and leaves its topology alone") {
    clay_mesh* m = grid_mesh();
    const std::vector<float> before = positions_of(m);
    const std::vector<std::uint32_t> topology = indices_of(m);

    clay_mesh_sculptor* s = nullptr;
    REQUIRE(clay_mesh_sculptor_create(m, -1.0f, &s) == CLAY_OK);

    // A cage over the grid, given some height so the Y axis is not degenerate.
    const float lo[3] = {-1.0f, -1.0f, -1.0f}, hi[3] = {1.0f, 1.0f, 1.0f};
    clay_mesh_lattice* cage = clay_mesh_lattice_create(lo, hi, 3, 3, 3);
    REQUIRE(cage != nullptr);

    int32_t nx = 0, ny = 0, nz = 0;
    CHECK(clay_mesh_lattice_divisions(cage, &nx, &ny, &nz) == CLAY_OK);
    CHECK((nx == 3 && ny == 3 && nz == 3));

    // Untouched: the identity, and applying it moves nothing.
    int32_t identity = 0;
    CHECK(clay_mesh_lattice_is_identity(cage, &identity) == CLAY_OK);
    CHECK(identity == 1);
    std::size_t moved = 99;
    CHECK(clay_mesh_sculptor_lattice(s, cage, nullptr, &moved) == CLAY_OK);
    CHECK(moved == 0);
    CHECK(positions_of(m) == before);

    // Drag the middle control point up.
    const float pull[3] = {0.0f, 0.8f, 0.0f};
    CHECK(clay_mesh_lattice_set_offset(cage, 1, 1, 1, pull) == CLAY_OK);
    CHECK(clay_mesh_lattice_is_identity(cage, &identity) == CLAY_OK);
    CHECK(identity == 0);

    float got[3] = {0, 0, 0};
    CHECK(clay_mesh_lattice_offset(cage, 1, 1, 1, got) == CLAY_OK);
    CHECK(got[1] == doctest::Approx(0.8f));
    // rest is the box's centre; position is rest plus the drag.
    CHECK(clay_mesh_lattice_rest(cage, 1, 1, 1, got) == CLAY_OK);
    CHECK(got[1] == doctest::Approx(0.0f));
    CHECK(clay_mesh_lattice_position(cage, 1, 1, 1, got) == CLAY_OK);
    CHECK(got[1] == doctest::Approx(0.8f));

    // The displacement is previewable without applying it.
    const float centre[3] = {0.0f, 0.0f, 0.0f};
    CHECK(clay_mesh_lattice_displacement(cage, centre, got) == CLAY_OK);
    CHECK(got[1] > 0.0f);

    clay_mesh_deltas* deltas = clay_mesh_deltas_create();
    REQUIRE(deltas != nullptr);
    CHECK(clay_mesh_sculptor_lattice(s, cage, deltas, &moved) == CLAY_OK);
    CHECK(moved > 0);
    CHECK(positions_of(m) != before);
    CHECK(indices_of(m) == topology);  // the contract, byte for byte

    // One undo step.
    CHECK(clay_mesh_deltas_revert(deltas, s) == CLAY_OK);
    CHECK(positions_of(m) == before);

    clay_mesh_deltas_destroy(deltas);
    clay_mesh_lattice_destroy(cage);
    clay_mesh_sculptor_destroy(s);
    clay_mesh_destroy(m);
}

TEST_CASE("c abi: a lattice refuses null arguments and clamps its divisions") {
    const float lo[3] = {0, 0, 0}, hi[3] = {1, 1, 1};
    CHECK(clay_mesh_lattice_create(nullptr, hi, 3, 3, 3) == nullptr);
    CHECK(clay_mesh_lattice_create(lo, nullptr, 3, 3, 3) == nullptr);

    // Divisions below two cannot span an axis; above the cap is a slider typo.
    clay_mesh_lattice* low = clay_mesh_lattice_create(lo, hi, 0, 1, -5);
    REQUIRE(low != nullptr);
    int32_t nx = 0, ny = 0, nz = 0;
    CHECK(clay_mesh_lattice_divisions(low, &nx, &ny, &nz) == CLAY_OK);
    CHECK((nx == 2 && ny == 2 && nz == 2));
    clay_mesh_lattice_destroy(low);

    int32_t identity = 0;
    CHECK(clay_mesh_lattice_is_identity(nullptr, &identity) == CLAY_ERROR_INVALID_ARGUMENT);
    float out[3];
    CHECK(clay_mesh_lattice_displacement(nullptr, lo, out) == CLAY_ERROR_INVALID_ARGUMENT);

    // Destroying null is a no-op, as everywhere else in this ABI.
    clay_mesh_lattice_destroy(nullptr);
}

// -- the colour pair across the boundary (add-mesh-colour-brushes) ------------

TEST_CASE("c abi: the colour verbs paint, refuse a colourless mesh, and move nothing") {
    clay_mesh* mesh = grid_mesh(16, 1.0f);
    clay_mesh_sculptor* sc = nullptr;
    REQUIRE(clay_mesh_sculptor_create(mesh, 0.0f, &sc) == CLAY_OK);

    // A mesh built from bare triangles has no colour attribute, which is the
    // case the verbs refuse rather than quietly allocating for.
    std::int32_t has = -1;
    REQUIRE(clay_mesh_sculptor_has_colors(sc, &has) == CLAY_OK);
    CHECK(has == 0);

    clay_mesh_brush_desc d = brush(CLAY_MESH_BRUSH_PAINT, 0.5f, 0.9f);
    // Red, in full: the defaults hand back WHITE, and ensure_colors fills
    // white, so setting only the red channel would paint white onto white and
    // correctly report that nothing changed.
    d.color[0] = 1.0f;
    d.color[1] = 0.0f;
    d.color[2] = 0.0f;
    std::size_t touched = 1;
    REQUIRE(clay_mesh_sculptor_stamp(sc, &d, nullptr, nullptr, &touched) == CLAY_OK);
    CHECK(touched == 0);
    REQUIRE(clay_mesh_sculptor_has_colors(sc, &has) == CLAY_OK);
    CHECK(has == 0);  // and it did not create one behind the stroke

    const float white[3] = {1, 1, 1};
    std::int32_t created = 0;
    REQUIRE(clay_mesh_sculptor_ensure_colors(sc, white, &created) == CLAY_OK);
    CHECK(created == 1);
    const float black[3] = {0, 0, 0};
    std::int32_t again = 1;
    REQUIRE(clay_mesh_sculptor_ensure_colors(sc, black, &again) == CLAY_OK);
    CHECK(again == 0);  // already there, left exactly as it is

    const std::size_t n = clay_mesh_vertex_count(mesh);
    std::vector<float> before_pos(n * 3), before_col(n * 3);
    std::memcpy(before_pos.data(), clay_mesh_positions(mesh), n * 3 * sizeof(float));
    std::memcpy(before_col.data(), clay_mesh_colors(mesh), n * 3 * sizeof(float));

    REQUIRE(clay_mesh_sculptor_stamp(sc, &d, nullptr, nullptr, &touched) == CLAY_OK);
    CHECK(touched > 0);
    CHECK(std::memcmp(before_col.data(), clay_mesh_colors(mesh), n * 3 * sizeof(float)) != 0);
    CHECK(std::memcmp(before_pos.data(), clay_mesh_positions(mesh), n * 3 * sizeof(float)) == 0);

    SUBCASE("smear is reachable and a zero drag does nothing") {
        clay_mesh_brush_desc sm = brush(CLAY_MESH_BRUSH_SMEAR, 0.5f, 1.0f);
        sm.direction[0] = 0.15f;
        std::size_t moved = 0;
        REQUIRE(clay_mesh_sculptor_stamp(sc, &sm, nullptr, nullptr, &moved) == CLAY_OK);
        CHECK(moved > 0);

        sm.direction[0] = 0.0f;
        std::size_t none = 1;
        REQUIRE(clay_mesh_sculptor_stamp(sc, &sm, nullptr, nullptr, &none) == CLAY_OK);
        CHECK(none == 0);
    }

    clay_mesh_sculptor_destroy(sc);
    clay_mesh_destroy(mesh);
}

TEST_CASE("c abi: a descriptor from before the colour field still stamps the old verbs") {
    // The versioned-descriptor pattern doing its job: `color` was appended
    // after the alpha block, so a caller compiled against the older layout
    // passes the shorter descriptor and gets exactly the verbs it had.
    clay_mesh* mesh = grid_mesh(16, 1.0f);
    clay_mesh_sculptor* sc = nullptr;
    REQUIRE(clay_mesh_sculptor_create(mesh, 0.0f, &sc) == CLAY_OK);

    clay_mesh_brush_desc d = brush(CLAY_MESH_BRUSH_DRAW, 0.5f, 0.3f);
    d.struct_size = offsetof(clay_mesh_brush_desc, alpha_extent) + sizeof(float);
    std::size_t moved = 0;
    REQUIRE(clay_mesh_sculptor_stamp(sc, &d, nullptr, nullptr, &moved) == CLAY_OK);
    CHECK(moved > 0);

    clay_mesh_sculptor_destroy(sc);
    clay_mesh_destroy(mesh);
}

// -- whole-form deformers across the boundary (add-mesh-deformers) ------------

TEST_CASE("c abi: a taper reaches a mesh layer and a mask holds part of it still") {
    clay_mesh* mesh = grid_mesh(16, 1.0f);
    clay_mesh_sculptor* sc = nullptr;
    REQUIRE(clay_mesh_sculptor_create(mesh, 0.0f, &sc) == CLAY_OK);

    clay_mesh_deform_desc d{};
    d.struct_size = sizeof(d);
    REQUIRE(clay_mesh_deform_defaults(&d) == CLAY_OK);
    CHECK(d.span > 0.0f);
    CHECK(d.scale_start == 1.0f);

    // The defaults are an identity, so they move nothing.
    std::size_t moved = 1;
    REQUIRE(clay_mesh_sculptor_deform(sc, &d, nullptr, nullptr, &moved) == CLAY_OK);
    CHECK(moved == 0);

    // The grid lies in the XZ plane, so a taper along X has a span to act on.
    d.verb = CLAY_MESH_DEFORM_TAPER;
    d.origin[0] = -1.0f;
    d.axis[0] = 1.0f;
    d.axis[1] = 0.0f;
    d.span = 2.0f;
    d.scale_end = 0.25f;
    REQUIRE(clay_mesh_sculptor_deform(sc, &d, nullptr, nullptr, &moved) == CLAY_OK);
    CHECK(moved > 0);

    SUBCASE("a preview does not apply anything") {
        const float p[3] = {0.0f, 0.0f, 1.0f};
        float out[3] = {0, 0, 0};
        REQUIRE(clay_mesh_deform_point(&d, p, out) == CLAY_OK);
        // Halfway along the span, the cross-section is scaled between the ends.
        CHECK(out[2] < p[2]);
        CHECK(out[0] == doctest::Approx(p[0]));
    }

    SUBCASE("unknown verbs and unmeanable knobs are refused") {
        clay_mesh_deform_desc bad = d;
        bad.verb = 99;
        CHECK(clay_mesh_sculptor_deform(sc, &bad, nullptr, nullptr, &moved) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        bad = d;
        bad.span = 0.0f;
        CHECK(clay_mesh_sculptor_deform(sc, &bad, nullptr, nullptr, &moved) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        bad = d;
        bad.axis[0] = bad.axis[1] = bad.axis[2] = 0.0f;
        CHECK(clay_mesh_sculptor_deform(sc, &bad, nullptr, nullptr, &moved) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        bad = d;
        bad.ease = CLAY_EASE_COUNT;
        CHECK(clay_mesh_sculptor_deform(sc, &bad, nullptr, nullptr, &moved) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }

    clay_mesh_sculptor_destroy(sc);
    clay_mesh_destroy(mesh);
}

TEST_CASE("c abi: a deform descriptor from before a field was appended still works") {
    clay_mesh* mesh = grid_mesh(12, 1.0f);
    clay_mesh_sculptor* sc = nullptr;
    REQUIRE(clay_mesh_sculptor_create(mesh, 0.0f, &sc) == CLAY_OK);

    clay_mesh_deform_desc d{};
    d.struct_size = sizeof(d);
    REQUIRE(clay_mesh_deform_defaults(&d) == CLAY_OK);
    d.verb = CLAY_MESH_DEFORM_TWIST;
    d.origin[0] = -1.0f;
    d.axis[0] = 1.0f;
    d.axis[1] = 0.0f;
    d.span = 2.0f;
    d.angle = 0.8f;
    // Declaring the original layout is what a host compiled against this
    // release will do for as long as the struct has not grown.
    d.struct_size = offsetof(clay_mesh_deform_desc, ease) + sizeof(std::int32_t);
    std::size_t moved = 0;
    REQUIRE(clay_mesh_sculptor_deform(sc, &d, nullptr, nullptr, &moved) == CLAY_OK);
    CHECK(moved > 0);

    clay_mesh_sculptor_destroy(sc);
    clay_mesh_destroy(mesh);
}

// -- the seed token and the peaks (add-extreme-poly-runtime) ------------------
//
// The C ABI's half of task 3.2 and task 7.7. What is defended here is the
// BOUNDARY: that a pick hands out the token beside the class it belongs to,
// that a token from a numbering that no longer exists is refused rather than
// spending the dab on an empty region, and that the peaks a host tunes a
// profile against are readable without owning anything.

TEST_CASE("c abi: a pick hands out the numbering its seed class was taken from") {
    clay_mesh* m = grid_mesh(16, 1.0f);
    clay_mesh_sculptor* s = nullptr;
    REQUIRE(clay_mesh_sculptor_create(m, -1.0f, &s) == CLAY_OK);

    std::uint64_t session = 0;
    REQUIRE(clay_mesh_sculptor_seed_revision(s, &session) == CLAY_OK);
    CHECK(session != 0);  // a real sculptor always claims a numbering

    const float origin[3] = {0.0f, 1.0f, 0.0f};
    const float down[3] = {0.0f, -1.0f, 0.0f};
    clay_mesh_hit hit{};
    hit.struct_size = sizeof(hit);
    REQUIRE(clay_mesh_sculptor_raycast(s, origin, down, nullptr, &hit) == CLAY_OK);
    REQUIRE(hit.hit == 1);
    CHECK(hit.seed_class != CLAY_MESH_NO_CLASS);
    // THE POINT: the class and the token come back together. A host that had to
    // fetch the token separately is a host that can forget to.
    CHECK(hit.seed_revision == session);

    // A miss carries neither, rather than a token beside no class at all.
    const float up[3] = {0.0f, 1.0f, 0.0f};
    clay_mesh_hit miss{};
    miss.struct_size = sizeof(miss);
    REQUIRE(clay_mesh_sculptor_raycast(s, origin, up, nullptr, &miss) == CLAY_OK);
    CHECK(miss.hit == 0);
    CHECK(miss.seed_class == CLAY_MESH_NO_CLASS);
    CHECK(miss.seed_revision == 0);

    clay_mesh_sculptor_destroy(s);
    clay_mesh_destroy(m);
}

TEST_CASE("c abi: a seed from a numbering that no longer exists is refused, not spent") {
    // Three sculptors over three identical grids. `picker` supplies a seed
    // numbered in ITS class space, which is genuinely not the other two's —
    // that is what a hierarchy produces on every rebind. `stale` and `honest`
    // then take the same dab, one with the wrong token and one with its own,
    // and each starts from an untouched grid so the two counts are comparable:
    // a stamp changes the surface the next stamp reaches across.
    clay_mesh* picked_mesh = grid_mesh(16, 1.0f);
    clay_mesh* stale_mesh = grid_mesh(16, 1.0f);
    clay_mesh* honest_mesh = grid_mesh(16, 1.0f);
    clay_mesh_sculptor* picker = nullptr;
    clay_mesh_sculptor* stale = nullptr;
    clay_mesh_sculptor* honest = nullptr;
    REQUIRE(clay_mesh_sculptor_create(picked_mesh, -1.0f, &picker) == CLAY_OK);
    REQUIRE(clay_mesh_sculptor_create(stale_mesh, -1.0f, &stale) == CLAY_OK);
    REQUIRE(clay_mesh_sculptor_create(honest_mesh, -1.0f, &honest) == CLAY_OK);

    const float origin[3] = {0.0f, 1.0f, 0.0f};
    const float down[3] = {0.0f, -1.0f, 0.0f};
    clay_mesh_hit hit{};
    hit.struct_size = sizeof(hit);
    REQUIRE(clay_mesh_sculptor_raycast(picker, origin, down, nullptr, &hit) == CLAY_OK);
    REQUIRE(hit.hit == 1);

    std::uint64_t theirs = 0;
    REQUIRE(clay_mesh_sculptor_seed_revision(honest, &theirs) == CLAY_OK);
    REQUIRE(theirs != hit.seed_revision);

    clay_mesh_brush_desc d = brush(CLAY_MESH_BRUSH_DRAW, 0.5f, 0.5f);
    d.seed_class = hit.seed_class;
    d.seed_revision = hit.seed_revision;  // the PICKER's token, spent elsewhere

    std::size_t rejected = 0;
    REQUIRE(clay_mesh_sculptor_stale_seeds_rejected(stale, &rejected) == CLAY_OK);
    CHECK(rejected == 0);

    std::size_t stale_moved = 0;
    REQUIRE(clay_mesh_sculptor_stamp(stale, &d, nullptr, nullptr, &stale_moved) == CLAY_OK);
    // The dab still lands, through the scan the refusal fell back to. That is
    // the shape of the fix: a rejected seed costs one query, an accepted stale
    // one costs the whole stamp.
    CHECK(stale_moved > 0);
    REQUIRE(clay_mesh_sculptor_stale_seeds_rejected(stale, &rejected) == CLAY_OK);
    CHECK(rejected == 1);

    clay_mesh_brush_desc own = d;
    own.seed_revision = theirs;
    std::size_t honest_moved = 0;
    REQUIRE(clay_mesh_sculptor_stamp(honest, &own, nullptr, nullptr, &honest_moved) == CLAY_OK);
    // THE CLAIM: refusing the seed changed what the stamp COST, not what it
    // did. Both dabs moved the same region of the same grid.
    CHECK(honest_moved == stale_moved);
    REQUIRE(clay_mesh_sculptor_stale_seeds_rejected(honest, &rejected) == CLAY_OK);
    CHECK(rejected == 0);  // this one was accepted

    clay_mesh_sculptor_destroy(picker);
    clay_mesh_sculptor_destroy(stale);
    clay_mesh_sculptor_destroy(honest);
    clay_mesh_destroy(picked_mesh);
    clay_mesh_destroy(stale_mesh);
    clay_mesh_destroy(honest_mesh);
}

TEST_CASE("c abi: a brush descriptor from before seed_revision was appended still stamps") {
    clay_mesh* m = grid_mesh(16, 1.0f);
    clay_mesh_sculptor* s = nullptr;
    REQUIRE(clay_mesh_sculptor_create(m, -1.0f, &s) == CLAY_OK);

    clay_mesh_brush_desc d = brush(CLAY_MESH_BRUSH_DRAW, 0.5f, 0.5f);
    // What a host compiled against the previous header sends: the layout
    // through automask_cavity_strength, with nothing after it. The appended
    // field then reads as zero, which is the value that claims no numbering.
    d.struct_size = static_cast<std::uint32_t>(offsetof(clay_mesh_brush_desc, seed_revision));
    std::size_t moved = 0;
    REQUIRE(clay_mesh_sculptor_stamp(s, &d, nullptr, nullptr, &moved) == CLAY_OK);
    CHECK(moved > 0);
    std::size_t rejected = 0;
    REQUIRE(clay_mesh_sculptor_stale_seeds_rejected(s, &rejected) == CLAY_OK);
    CHECK(rejected == 0);  // claiming nothing is not a stale claim

    clay_mesh_sculptor_destroy(s);
    clay_mesh_destroy(m);
}

TEST_CASE("c abi: the peaks are high-water marks a host reads without owning anything") {
    clay_mesh* m = grid_mesh(24, 1.0f);
    clay_mesh_sculptor* s = nullptr;
    REQUIRE(clay_mesh_sculptor_create(m, -1.0f, &s) == CLAY_OK);

    clay_peak_telemetry t{};
    t.struct_size = sizeof(t);
    REQUIRE(clay_mesh_sculptor_peak_telemetry(s, &t) == CLAY_OK);
    CHECK(t.workset_vertices == 0);  // nothing has been stamped

    const clay_mesh_brush_desc wide = brush(CLAY_MESH_BRUSH_DRAW, 0.8f, 0.5f);
    std::size_t moved = 0;
    REQUIRE(clay_mesh_sculptor_stamp(s, &wide, nullptr, nullptr, &moved) == CLAY_OK);
    REQUIRE(moved > 0);
    REQUIRE(clay_mesh_sculptor_peak_telemetry(s, &t) == CLAY_OK);
    const std::uint64_t widest = t.workset_vertices;
    CHECK(widest > 0);

    // A HIGH-WATER MARK, not the last value: a smaller stamp afterwards does
    // not pull the peak down, because what a host has to size for is the
    // largest thing that happened rather than the most recent one.
    //
    // Placed in a CORNER the wide stamp did not reach. A second stamp at the
    // same centre would find the surface already displaced out from under the
    // brush and gather nothing, which would make this assert on an empty
    // region rather than on a small one.
    clay_mesh_brush_desc narrow = brush(CLAY_MESH_BRUSH_DRAW, 0.15f, 0.5f);
    narrow.center[0] = -0.9f;
    narrow.center[2] = -0.9f;
    REQUIRE(clay_mesh_sculptor_stamp(s, &narrow, nullptr, nullptr, &moved) == CLAY_OK);
    REQUIRE(moved > 0);
    REQUIRE(clay_mesh_sculptor_peak_telemetry(s, &t) == CLAY_OK);
    CHECK(t.workset_vertices == widest);

    REQUIRE(clay_mesh_sculptor_reset_peak_telemetry(s) == CLAY_OK);
    REQUIRE(clay_mesh_sculptor_peak_telemetry(s, &t) == CLAY_OK);
    CHECK(t.workset_vertices == 0);
    REQUIRE(clay_mesh_sculptor_stamp(s, &narrow, nullptr, nullptr, &moved) == CLAY_OK);
    REQUIRE(clay_mesh_sculptor_peak_telemetry(s, &t) == CLAY_OK);
    CHECK(t.workset_vertices > 0);
    CHECK(t.workset_vertices < widest);  // the narrow stamp gathers less

    // The descriptor rules, on an out parameter like any other.
    CHECK(clay_mesh_sculptor_peak_telemetry(s, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    clay_peak_telemetry stunted{};
    stunted.struct_size = 4;
    CHECK(clay_mesh_sculptor_peak_telemetry(s, &stunted) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mesh_sculptor_peak_telemetry(nullptr, &t) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mesh_sculptor_seed_revision(nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);

    clay_mesh_sculptor_destroy(s);
    clay_mesh_destroy(m);
}
