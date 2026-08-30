#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "clay.h"

// The global voxel remesh across the ABI (c-abi spec: voxel remesh over the C
// ABI; add-voxel-remesher).
//
// The two things a C consumer can get wrong that C++ cannot: a descriptor
// written past the end of a shorter caller's buffer, and a failure it cannot
// tell apart from another failure. Both are checked here rather than assumed.

namespace {

struct MeshHandle {
    clay_mesh* m = nullptr;
    ~MeshHandle() { clay_mesh_destroy(m); }
    MeshHandle() = default;
    MeshHandle(const MeshHandle&) = delete;
    MeshHandle& operator=(const MeshHandle&) = delete;
};

// A UV sphere, built here rather than shared with the C++ fixtures: this file
// speaks only the C header, which is the point of it.
void sphere(std::vector<float>* positions, std::vector<std::uint32_t>* indices,
            float radius = 1.0f, float cx = 0.0f, int rings = 16, int segments = 32) {
    const float pi = 3.14159265358979323846f;
    auto push = [&](float x, float y, float z) {
        positions->push_back(x);
        positions->push_back(y);
        positions->push_back(z);
    };
    push(cx, radius, 0.0f);
    for (int r = 1; r < rings; ++r) {
        const float phi = pi * static_cast<float>(r) / static_cast<float>(rings);
        const float y = std::cos(phi) * radius, rr = std::sin(phi) * radius;
        for (int s = 0; s < segments; ++s) {
            const float th = 2.0f * pi * static_cast<float>(s) / static_cast<float>(segments);
            push(cx + rr * std::cos(th), y, rr * std::sin(th));
        }
    }
    push(cx, -radius, 0.0f);
    const std::uint32_t top = 0;
    const std::uint32_t bottom = static_cast<std::uint32_t>(positions->size() / 3 - 1);
    auto at = [&](int r, int s) {
        return static_cast<std::uint32_t>(1 + (r - 1) * segments + (s % segments));
    };
    for (int s = 0; s < segments; ++s) {
        indices->push_back(top);
        indices->push_back(at(1, s + 1));
        indices->push_back(at(1, s));
    }
    for (int r = 1; r < rings - 1; ++r)
        for (int s = 0; s < segments; ++s) {
            indices->push_back(at(r, s));
            indices->push_back(at(r, s + 1));
            indices->push_back(at(r + 1, s));
            indices->push_back(at(r, s + 1));
            indices->push_back(at(r + 1, s + 1));
            indices->push_back(at(r + 1, s));
        }
    for (int s = 0; s < segments; ++s) {
        indices->push_back(bottom);
        indices->push_back(at(rings - 1, s));
        indices->push_back(at(rings - 1, s + 1));
    }
}

clay_result build(MeshHandle* out, const std::vector<float>& p,
                  const std::vector<std::uint32_t>& i) {
    return clay_mesh_from_triangles(p.data(), p.size() / 3, i.data(), i.size(), &out->m);
}

clay_voxel_remesh_params defaults_at(std::uint32_t resolution) {
    clay_voxel_remesh_params p{};
    p.struct_size = sizeof(p);
    REQUIRE(clay_mesh_voxel_remesh_defaults(&p) == CLAY_OK);
    p.resolution_mode = CLAY_VOXEL_REMESH_LONGEST_AXIS;
    p.longest_axis_resolution = resolution;
    return p;
}

}  // namespace

TEST_CASE("c abi: the defaults are obtainable rather than transcribed") {
    clay_voxel_remesh_params p{};
    p.struct_size = sizeof(p);
    REQUIRE(clay_mesh_voxel_remesh_defaults(&p) == CLAY_OK);
    CHECK(p.resolution_mode == CLAY_VOXEL_REMESH_LONGEST_AXIS);
    CHECK(p.longest_axis_resolution == 256u);
    CHECK(p.surface_mode == CLAY_VOXEL_REMESH_SMOOTH);
    CHECK(p.open_surface_policy == CLAY_VOXEL_REMESH_OPEN_CLOSE);
    CHECK(p.small_component_policy == CLAY_VOXEL_REMESH_KEEP_COMPONENTS);
    CHECK(p.preserve_volume == 1);
    CHECK(p.project_to_source == 1);
    CHECK(p.preserve_colors == 1);
    CHECK(p.projection_strength == doctest::Approx(0.75f));
    CHECK(p.build_multires_levels == 0u);
    CHECK(p.memory_budget_bytes == 0u);
    // The caller keeps the size THEY declared.
    CHECK(p.struct_size == sizeof(p));
}

TEST_CASE("c abi: a remesh runs and reports") {
    std::vector<float> pos;
    std::vector<std::uint32_t> idx;
    sphere(&pos, &idx);
    MeshHandle src;
    REQUIRE(build(&src, pos, idx) == CLAY_OK);

    const clay_voxel_remesh_params p = defaults_at(48);
    clay_voxel_remesh_estimate estimate{};
    estimate.struct_size = sizeof(estimate);
    REQUIRE(clay_mesh_voxel_remesh_estimate(src.m, &p, &estimate) == CLAY_OK);
    CHECK(estimate.resolved_voxel_size > 0.0f);
    CHECK(estimate.estimated_active_samples > 0u);
    CHECK(estimate.estimated_memory_bytes > 0u);
    CHECK(estimate.component_count == 1u);
    CHECK(estimate.has_open_boundaries == 0);

    MeshHandle out;
    clay_voxel_remesh_report report{};
    report.struct_size = sizeof(report);
    REQUIRE(clay_mesh_voxel_remesh(src.m, &p, nullptr, &out.m, &report) == CLAY_OK);
    REQUIRE(out.m != nullptr);
    CHECK(clay_mesh_vertex_count(out.m) > 0);
    CHECK(report.voxel_size == estimate.resolved_voxel_size);
    CHECK(report.result_watertight == 1);
    CHECK(report.result_manifold == 1);
    CHECK(report.result_oriented == 1);
    CHECK(report.result_components == 1u);
    CHECK(report.active_samples > 0u);
    CHECK(report.active_samples <= estimate.estimated_active_samples);
    CHECK(report.source_vertices == clay_mesh_vertex_count(src.m));
    CHECK(report.result_vertices == clay_mesh_vertex_count(out.m));

    // The source is untouched: this is a mesh -> mesh operation, not an edit.
    CHECK(clay_mesh_vertex_count(src.m) == pos.size() / 3);
    CHECK(clay_mesh_index_count(src.m) == idx.size());
}

TEST_CASE("c abi: a descriptor is filled bounded by what the caller declared") {
    std::vector<float> pos;
    std::vector<std::uint32_t> idx;
    sphere(&pos, &idx);
    MeshHandle src;
    REQUIRE(build(&src, pos, idx) == CLAY_OK);
    const clay_voxel_remesh_params p = defaults_at(32);

    // A caller built against a NEWER header than this library: it declares a
    // size this build does not know. The fill is clamped to what this build
    // has, so the tail the caller knows about and this build does not is left
    // exactly as the caller left it — never filled with whatever this build
    // thought was there.
    struct Guarded {
        clay_voxel_remesh_report report;
        std::uint8_t tail[64];
    };
    Guarded g{};
    std::memset(&g, 0xAB, sizeof(g));
    g.report.struct_size = static_cast<std::uint32_t>(sizeof(clay_voxel_remesh_report) + 32);

    MeshHandle out;
    REQUIRE(clay_mesh_voxel_remesh(src.m, &p, nullptr, &out.m, &g.report) == CLAY_OK);
    CHECK(g.report.voxel_size > 0.0f);
    CHECK(g.report.source_triangles == idx.size() / 3);
    CHECK(g.report.result_triangles > 0u);
    // The caller keeps the size THEY declared: it describes their buffer.
    CHECK(g.report.struct_size == sizeof(clay_voxel_remesh_report) + 32);
    const std::uint8_t* bytes = reinterpret_cast<const std::uint8_t*>(&g);
    for (std::size_t i = sizeof(clay_voxel_remesh_report); i < sizeof(g); ++i)
        CHECK(bytes[i] == 0xAB);
}

TEST_CASE("c abi: a descriptor shorter than the original layout is refused, not filled") {
    // These three descriptors shipped whole in ABI 0.63.0, so their original
    // layout IS their current one and there is no shorter form to accept. A
    // caller declaring one is not naming an older layout — it is passing
    // something that was never a descriptor — and the answer is a refusal with
    // NOTHING written, rather than a partial fill of a struct nobody has.
    std::vector<float> pos;
    std::vector<std::uint32_t> idx;
    sphere(&pos, &idx);
    MeshHandle src;
    REQUIRE(build(&src, pos, idx) == CLAY_OK);
    const clay_voxel_remesh_params p = defaults_at(32);

    clay_voxel_remesh_report report;
    std::memset(&report, 0xCD, sizeof(report));
    report.struct_size = static_cast<std::uint32_t>(offsetof(clay_voxel_remesh_report, cancelled));
    clay_mesh* out = nullptr;
    CHECK(clay_mesh_voxel_remesh(src.m, &p, nullptr, &out, &report) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(out == nullptr);
    const std::uint8_t* bytes = reinterpret_cast<const std::uint8_t*>(&report);
    for (std::size_t i = sizeof(std::uint32_t); i < sizeof(report); ++i) CHECK(bytes[i] == 0xCD);

    clay_voxel_remesh_estimate estimate;
    std::memset(&estimate, 0xCD, sizeof(estimate));
    estimate.struct_size =
        static_cast<std::uint32_t>(offsetof(clay_voxel_remesh_estimate, exceeds_memory_budget));
    CHECK(clay_mesh_voxel_remesh_estimate(src.m, &p, &estimate) == CLAY_ERROR_INVALID_ARGUMENT);

    clay_voxel_remesh_params shortened;
    std::memset(&shortened, 0xCD, sizeof(shortened));
    shortened.struct_size =
        static_cast<std::uint32_t>(offsetof(clay_voxel_remesh_params, memory_budget_bytes));
    CHECK(clay_mesh_voxel_remesh(src.m, &shortened, nullptr, &out, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mesh_voxel_remesh_defaults(&shortened) == CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("c abi: failure kinds stay distinct") {
    std::vector<float> pos;
    std::vector<std::uint32_t> idx;
    sphere(&pos, &idx);
    MeshHandle src;
    REQUIRE(build(&src, pos, idx) == CLAY_OK);

    clay_mesh* out = nullptr;

    clay_voxel_remesh_params bad = defaults_at(48);
    bad.resolution_mode = CLAY_VOXEL_REMESH_VOXEL_SIZE;
    bad.voxel_size = 0.0f;
    CHECK(clay_mesh_voxel_remesh(src.m, &bad, nullptr, &out, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(out == nullptr);

    clay_voxel_remesh_params over = defaults_at(256);
    over.memory_budget_bytes = 1024;
    CHECK(clay_mesh_voxel_remesh(src.m, &over, nullptr, &out, nullptr) ==
          CLAY_ERROR_BUDGET_EXCEEDED);
    CHECK(out == nullptr);

    // An open source under the rejecting policy. Drop the last ring so the
    // sphere genuinely has a boundary.
    std::vector<std::uint32_t> holed(idx.begin(), idx.end() - 3 * 32);
    MeshHandle open_src;
    REQUIRE(build(&open_src, pos, holed) == CLAY_OK);
    clay_voxel_remesh_params reject = defaults_at(48);
    reject.open_surface_policy = CLAY_VOXEL_REMESH_OPEN_REJECT;
    clay_voxel_remesh_report report{};
    report.struct_size = sizeof(report);
    CHECK(clay_mesh_voxel_remesh(open_src.m, &reject, nullptr, &out, &report) ==
          CLAY_ERROR_UNSUPPORTED);
    CHECK(out == nullptr);
    // The report is filled for a REFUSAL too: the boundary count is exactly
    // what a host puts in front of a user.
    CHECK(report.source_was_open == 1);
    CHECK(report.source_boundary_edges > 0u);

    clay_voxel_remesh_params reserved = defaults_at(48);
    reserved.build_multires_levels = 1;
    CHECK(clay_mesh_voxel_remesh(src.m, &reserved, nullptr, &out, nullptr) ==
          CLAY_ERROR_UNSUPPORTED);

    clay_voxel_remesh_params unknown = defaults_at(48);
    unknown.surface_mode = 99;
    CHECK(clay_mesh_voxel_remesh(src.m, &unknown, nullptr, &out, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("c abi: a cancelled remesh is reported as cancelled") {
    std::vector<float> pos;
    std::vector<std::uint32_t> idx;
    sphere(&pos, &idx);
    MeshHandle src;
    REQUIRE(build(&src, pos, idx) == CLAY_OK);

    clay_cancel_token* token = clay_cancel_token_create();
    REQUIRE(token != nullptr);
    clay_cancel_token_cancel(token);

    clay_mesh* out = nullptr;
    clay_voxel_remesh_report report{};
    report.struct_size = sizeof(report);
    const clay_voxel_remesh_params p = defaults_at(96);
    CHECK(clay_mesh_voxel_remesh(src.m, &p, token, &out, &report) == CLAY_ERROR_CANCELLED);
    CHECK(out == nullptr);
    CHECK(report.cancelled == 1);
    clay_cancel_token_destroy(token);
}

TEST_CASE("c abi: a mask crosses a remesh, and a wrong length does not") {
    std::vector<float> pos;
    std::vector<std::uint32_t> idx;
    sphere(&pos, &idx);
    MeshHandle src;
    REQUIRE(build(&src, pos, idx) == CLAY_OK);
    const clay_voxel_remesh_params p = defaults_at(48);

    MeshHandle out;
    clay_voxel_remesh_report report{};
    report.struct_size = sizeof(report);
    REQUIRE(clay_mesh_voxel_remesh(src.m, &p, nullptr, &out.m, &report) == CLAY_OK);

    const std::size_t source_vertices = pos.size() / 3;
    std::vector<float> mask(source_vertices);
    for (std::size_t v = 0; v < source_vertices; ++v) mask[v] = pos[v * 3] > 0.0f ? 1.0f : 0.0f;

    std::vector<float> moved(clay_mesh_vertex_count(out.m), -1.0f);
    REQUIRE(clay_mesh_transfer_vertex_scalar(src.m, mask.data(), mask.size(), out.m,
                                             report.voxel_size * 2.0f, 0.0f, moved.data(),
                                             moved.size()) == CLAY_OK);
    const float* positions = clay_mesh_positions(out.m);
    std::size_t checked = 0, wrong = 0;
    for (std::size_t v = 0; v < moved.size(); ++v) {
        const float x = positions[v * 3];
        if (std::fabs(x) < 4.0f * report.voxel_size) continue;
        ++checked;
        if ((x > 0.0f) != (moved[v] > 0.5f)) ++wrong;
    }
    CHECK(checked > 100);
    CHECK(wrong == 0);

    // A length that does not match is refused rather than read.
    std::vector<float> short_mask(4, 1.0f);
    CHECK(clay_mesh_transfer_vertex_scalar(src.m, short_mask.data(), short_mask.size(), out.m,
                                           0.0f, 0.0f, moved.data(), moved.size()) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_mesh_transfer_vertex_scalar(src.m, mask.data(), mask.size(), out.m, 0.0f, 0.0f,
                                           moved.data(), moved.size() - 1) ==
          CLAY_ERROR_INVALID_ARGUMENT);
}

// -- the remesh THROUGH the document (add-voxel-remesh-document) --------------
//
// The pure operation above has no document, no undo and no state, which is what
// makes it testable. This is the other half: it has to land on a LAYER, replace
// what was there, be ONE step on the undo menu, and refuse to overwrite work
// that arrived while it was thinking.

namespace {

struct DocHandle {
    clay_document* doc = nullptr;
    ~DocHandle() { clay_document_destroy(doc); }
    DocHandle() = default;
    DocHandle(const DocHandle&) = delete;
    DocHandle& operator=(const DocHandle&) = delete;
};

// A document holding one mesh layer built from `positions`/`indices`.
clay_layer_id attach_layer(DocHandle* doc, const std::vector<float>& p,
                           const std::vector<std::uint32_t>& i, const char* name = "shell") {
    MeshHandle src;
    REQUIRE(clay_mesh_from_triangles(p.data(), p.size() / 3, i.data(), i.size(), &src.m) ==
            CLAY_OK);
    clay_mesh_layer_desc desc{};
    desc.struct_size = sizeof(desc);
    desc.name = name;
    clay_layer_id id = 0;
    REQUIRE(clay_document_add_mesh_layer(doc->doc, src.m, &desc, &id, nullptr) == CLAY_OK);
    return id;
}

std::size_t layer_triangles(clay_document* doc, clay_layer_id layer) {
    clay_mesh* borrowed = nullptr;
    REQUIRE(clay_document_mesh_layer_by_id(doc, layer, &borrowed) == CLAY_OK);
    return clay_mesh_index_count(borrowed) / 3;
}

}  // namespace

TEST_CASE("c abi: a layer remesh is one undo step that restores the old triangles") {
    std::vector<float> pos;
    std::vector<std::uint32_t> idx;
    sphere(&pos, &idx);

    DocHandle doc;
    doc.doc = clay_document_create();
    REQUIRE(doc.doc != nullptr);
    REQUIRE(clay_document_enable_undo(doc.doc) == CLAY_OK);
    const clay_layer_id layer = attach_layer(&doc, pos, idx);
    const std::size_t before = layer_triangles(doc.doc, layer);
    CHECK(before == idx.size() / 3);

    std::size_t depth_before = 0;
    REQUIRE(clay_document_undo_state(doc.doc, nullptr, &depth_before, nullptr) == CLAY_OK);

    const clay_voxel_remesh_params p = defaults_at(48);
    clay_voxel_remesh_report report{};
    report.struct_size = sizeof(report);
    REQUIRE(clay_document_voxel_remesh_layer(doc.doc, layer, &p, nullptr, &report) == CLAY_OK);

    const std::size_t after = layer_triangles(doc.doc, layer);
    CHECK(after != before);
    CHECK(after == report.result_triangles);
    CHECK(report.result_watertight == 1);

    // ONE step, not two and not none.
    std::size_t depth_after = 0;
    REQUIRE(clay_document_undo_state(doc.doc, nullptr, &depth_after, nullptr) == CLAY_OK);
    CHECK(depth_after == depth_before + 1);

    // ...and it puts the old triangles back, exactly.
    std::int32_t undone = 0;
    REQUIRE(clay_document_undo(doc.doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    CHECK(layer_triangles(doc.doc, layer) == before);

    std::int32_t redone = 0;
    REQUIRE(clay_document_redo(doc.doc, &redone) == CLAY_OK);
    CHECK(redone == 1);
    CHECK(layer_triangles(doc.doc, layer) == after);

    // Redo must not have emptied the step: a second undo has to work too.
    REQUIRE(clay_document_undo(doc.doc, &undone) == CLAY_OK);
    CHECK(layer_triangles(doc.doc, layer) == before);
}

TEST_CASE("c abi: a stale commit is refused rather than overwriting newer work") {
    std::vector<float> pos;
    std::vector<std::uint32_t> idx;
    sphere(&pos, &idx);

    DocHandle doc;
    doc.doc = clay_document_create();
    REQUIRE(doc.doc != nullptr);
    const clay_layer_id layer = attach_layer(&doc, pos, idx);

    std::uint64_t revision = 0;
    REQUIRE(clay_document_mesh_layer_revision(doc.doc, layer, &revision) == CLAY_OK);
    CHECK(revision > 0);

    // The host's own worker: remesh the layer's triangles with the PURE call,
    // which is the shape this exists to serve.
    clay_mesh* borrowed = nullptr;
    REQUIRE(clay_document_mesh_layer_by_id(doc.doc, layer, &borrowed) == CLAY_OK);
    MeshHandle rebuilt;
    const clay_voxel_remesh_params p = defaults_at(40);
    REQUIRE(clay_mesh_voxel_remesh(borrowed, &p, nullptr, &rebuilt.m, nullptr) == CLAY_OK);

    // ...and while it was thinking, something else rebuilt the layer.
    REQUIRE(clay_document_voxel_remesh_layer(doc.doc, layer, &p, nullptr, nullptr) == CLAY_OK);
    std::uint64_t moved = 0;
    REQUIRE(clay_document_mesh_layer_revision(doc.doc, layer, &moved) == CLAY_OK);
    CHECK(moved != revision);
    const std::size_t survivor = layer_triangles(doc.doc, layer);

    // The stale commit is REFUSED, and the newer work survives untouched.
    CHECK(clay_document_replace_mesh_layer(doc.doc, layer, rebuilt.m, revision) ==
          CLAY_ERROR_FORWARD_VERSION);
    CHECK(layer_triangles(doc.doc, layer) == survivor);

    // The same commit at the CURRENT revision is accepted.
    CHECK(clay_document_replace_mesh_layer(doc.doc, layer, rebuilt.m, moved) == CLAY_OK);
    CHECK(layer_triangles(doc.doc, layer) == clay_mesh_index_count(rebuilt.m) / 3);

    // ...and zero means "do not check", for a caller that knows nothing could
    // have moved.
    CHECK(clay_document_replace_mesh_layer(doc.doc, layer, rebuilt.m, 0) == CLAY_OK);
}

TEST_CASE("c abi: the geometry revision moves for a rebuild and not for a sculpt") {
    // The distinction the counter exists for. A sculpt moves vertices and
    // leaves the topology alone — that is the fixed-topology contract, and it
    // is exactly the change an adjacency, a BVH or a live sculptor SURVIVES. A
    // rebuild swaps every vertex and every index, and they do not.
    std::vector<float> pos;
    std::vector<std::uint32_t> idx;
    sphere(&pos, &idx);

    DocHandle doc;
    doc.doc = clay_document_create();
    REQUIRE(doc.doc != nullptr);
    const clay_layer_id layer = attach_layer(&doc, pos, idx);

    std::uint64_t start = 0;
    REQUIRE(clay_document_mesh_layer_revision(doc.doc, layer, &start) == CLAY_OK);

    clay_mesh* borrowed = nullptr;
    REQUIRE(clay_document_mesh_layer_by_id(doc.doc, layer, &borrowed) == CLAY_OK);
    clay_mesh_sculptor* sculptor = nullptr;
    REQUIRE(clay_mesh_sculptor_create(borrowed, -1.0f, &sculptor) == CLAY_OK);

    clay_mesh_brush_desc brush{};
    brush.struct_size = sizeof(brush);
    REQUIRE(clay_mesh_brush_defaults(&brush) == CLAY_OK);
    brush.verb = CLAY_MESH_BRUSH_DRAW;
    brush.center[0] = 0.0f;
    brush.center[1] = 1.0f;
    brush.center[2] = 0.0f;
    brush.radius = 0.4f;
    brush.strength = 0.05f;
    std::size_t moved = 0;
    REQUIRE(clay_mesh_sculptor_stamp(sculptor, &brush, nullptr, nullptr, &moved) == CLAY_OK);
    CHECK(moved > 0);

    std::uint64_t after_sculpt = 0;
    REQUIRE(clay_document_mesh_layer_revision(doc.doc, layer, &after_sculpt) == CLAY_OK);
    CHECK(after_sculpt == start);  // a sculpt is not a rebuild

    clay_mesh_sculptor_destroy(sculptor);

    const clay_voxel_remesh_params p = defaults_at(40);
    REQUIRE(clay_document_voxel_remesh_layer(doc.doc, layer, &p, nullptr, nullptr) == CLAY_OK);
    std::uint64_t after_remesh = 0;
    REQUIRE(clay_document_mesh_layer_revision(doc.doc, layer, &after_remesh) == CLAY_OK);
    CHECK(after_remesh > after_sculpt);
}

TEST_CASE("c abi: a layer remesh refuses what it should and leaves the layer alone") {
    std::vector<float> pos;
    std::vector<std::uint32_t> idx;
    sphere(&pos, &idx);

    DocHandle doc;
    doc.doc = clay_document_create();
    REQUIRE(doc.doc != nullptr);
    REQUIRE(clay_document_enable_undo(doc.doc) == CLAY_OK);
    const clay_layer_id layer = attach_layer(&doc, pos, idx);
    const std::size_t before = layer_triangles(doc.doc, layer);

    const clay_voxel_remesh_params p = defaults_at(48);
    CHECK(clay_document_voxel_remesh_layer(doc.doc, layer + 999, &p, nullptr, nullptr) ==
          CLAY_ERROR_NOT_FOUND);

    // A locked layer is refused BEFORE the rebuild, not after several seconds
    // of work.
    REQUIRE(clay_document_set_layer_protection(doc.doc, layer, 0, 1) == CLAY_OK);
    CHECK(clay_document_voxel_remesh_layer(doc.doc, layer, &p, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(layer_triangles(doc.doc, layer) == before);
    REQUIRE(clay_document_set_layer_protection(doc.doc, layer, 0, 0) == CLAY_OK);

    // A cancelled remesh changes nothing and adds no undo step.
    std::size_t depth = 0;
    REQUIRE(clay_document_undo_state(doc.doc, nullptr, &depth, nullptr) == CLAY_OK);
    clay_cancel_token* token = clay_cancel_token_create();
    clay_cancel_token_cancel(token);
    CHECK(clay_document_voxel_remesh_layer(doc.doc, layer, &p, token, nullptr) ==
          CLAY_ERROR_CANCELLED);
    clay_cancel_token_destroy(token);
    CHECK(layer_triangles(doc.doc, layer) == before);
    std::size_t depth_now = 0;
    REQUIRE(clay_document_undo_state(doc.doc, nullptr, &depth_now, nullptr) == CLAY_OK);
    CHECK(depth_now == depth);

    // An over-budget request likewise.
    clay_voxel_remesh_params over = defaults_at(256);
    over.memory_budget_bytes = 1024;
    CHECK(clay_document_voxel_remesh_layer(doc.doc, layer, &over, nullptr, nullptr) ==
          CLAY_ERROR_BUDGET_EXCEEDED);
    CHECK(layer_triangles(doc.doc, layer) == before);
    REQUIRE(clay_document_undo_state(doc.doc, nullptr, &depth_now, nullptr) == CLAY_OK);
    CHECK(depth_now == depth);
}

TEST_CASE("c abi: a live sculptor is refused after the layer it holds is rebuilt") {
    // THE TRAP THIS CLOSES. `resolve_sculptor` compared the layer's mesh
    // POINTER, which a std::map keeps stable across an assignment, and the
    // sculptor's own `valid()`, which compares vertex and index COUNTS. A
    // rebuild that happened to land on the same counts passed both and left
    // the sculptor's adjacency and BVH describing triangles that no longer
    // existed — every stamp after it moving the wrong vertices, silently.
    std::vector<float> pos;
    std::vector<std::uint32_t> idx;
    sphere(&pos, &idx);

    DocHandle doc;
    doc.doc = clay_document_create();
    REQUIRE(doc.doc != nullptr);
    const clay_layer_id layer = attach_layer(&doc, pos, idx);

    clay_mesh* borrowed = nullptr;
    REQUIRE(clay_document_mesh_layer_by_id(doc.doc, layer, &borrowed) == CLAY_OK);
    clay_mesh_sculptor* sculptor = nullptr;
    REQUIRE(clay_mesh_sculptor_create(borrowed, -1.0f, &sculptor) == CLAY_OK);

    clay_mesh_brush_desc brush{};
    brush.struct_size = sizeof(brush);
    REQUIRE(clay_mesh_brush_defaults(&brush) == CLAY_OK);
    brush.verb = CLAY_MESH_BRUSH_DRAW;
    brush.center[1] = 1.0f;
    brush.radius = 0.4f;
    brush.strength = 0.05f;

    // It works before the rebuild...
    std::size_t moved = 0;
    REQUIRE(clay_mesh_sculptor_stamp(sculptor, &brush, nullptr, nullptr, &moved) == CLAY_OK);
    CHECK(moved > 0);

    // THE REPLACEMENT THAT NEITHER OTHER CHECK CAN SEE: the same positions and
    // the same index COUNT, with the index array reversed — different
    // triangles, different adjacency, identical counts. A remesh normally
    // changes the counts and `valid()` catches it; this is the case that
    // slipped through, so this is the case the test has to build.
    std::vector<std::uint32_t> shuffled(idx.rbegin(), idx.rend());
    MeshHandle same_counts;
    REQUIRE(clay_mesh_from_triangles(pos.data(), pos.size() / 3, shuffled.data(),
                                     shuffled.size(), &same_counts.m) == CLAY_OK);
    CHECK(clay_mesh_vertex_count(same_counts.m) == clay_mesh_vertex_count(borrowed));
    CHECK(clay_mesh_index_count(same_counts.m) == clay_mesh_index_count(borrowed));
    REQUIRE(clay_document_replace_mesh_layer(doc.doc, layer, same_counts.m, 0) == CLAY_OK);

    // The counts still match, so the sculptor's own validity check is blind to
    // this — and the stamp is refused anyway.
    CHECK(clay_mesh_index_count(borrowed) == idx.size());
    CHECK(clay_mesh_sculptor_stamp(sculptor, &brush, nullptr, nullptr, &moved) ==
          CLAY_ERROR_NOT_FOUND);

    clay_mesh_sculptor_destroy(sculptor);
}

TEST_CASE("c abi: welding makes a marched mesh convertible, and reports what it did") {
    // The C side of add-mesh-weld. The mesh here is the one this ABI's own
    // mesher produces, so the fixture is the defect rather than a stand-in for
    // it: a marched sphere carries zero-area triangles that nothing downstream
    // had ever objected to.
    std::vector<float> pos;
    std::vector<std::uint32_t> idx;
    sphere(&pos, &idx, 1.0f, 0.0f, 24, 48);
    MeshHandle src;
    REQUIRE(build(&src, pos, idx) == CLAY_OK);

    // A UV sphere is clean, so remesh it first to get a MARCHED mesh.
    const clay_voxel_remesh_params p = defaults_at(48);
    MeshHandle marched;
    REQUIRE(clay_mesh_voxel_remesh(src.m, &p, nullptr, &marched.m, nullptr) == CLAY_OK);

    clay_weld_desc desc{};
    desc.struct_size = sizeof(desc);
    REQUIRE(clay_mesh_weld_defaults(&desc) == CLAY_OK);
    CHECK(desc.preserve_attribute_splits == 1);
    CHECK(desc.epsilon > 0.0f);

    const std::size_t before = clay_mesh_index_count(marched.m) / 3;
    clay_weld_report report{};
    report.struct_size = sizeof(report);
    REQUIRE(clay_mesh_weld(marched.m, &desc, &report) == CLAY_OK);
    CHECK(report.triangles_before == before);
    CHECK(report.triangles_after == clay_mesh_index_count(marched.m) / 3);
    CHECK(report.triangles_collapsed > 0);
    CHECK(report.triangles_invalid == 0);
    CHECK(report.epsilon > 0.0f);

    // Watertight before and after: a triangle whose corners coincide bounds
    // nothing, so removing it cannot open a hole.
    std::int32_t watertight = 0, manifold = 0;
    REQUIRE(clay_mesh_validate(marched.m, &watertight, &manifold) == CLAY_OK);
    CHECK(watertight == 1);
    CHECK(manifold == 1);

    // A second weld has nothing to do and says so.
    clay_weld_report again{};
    again.struct_size = sizeof(again);
    REQUIRE(clay_mesh_weld(marched.m, &desc, &again) == CLAY_OK);
    CHECK(again.vertices_merged == 0);
    CHECK(again.triangles_collapsed == 0);
    CHECK(again.triangles_after == report.triangles_after);
}

TEST_CASE("c abi: welding a layer bumps its geometry revision, and a no-op weld does not") {
    // A weld rewrites the triangles, so it is as invalidating as a rebuild —
    // and must say so through the same counter, or a live sculptor survives it.
    std::vector<float> pos;
    std::vector<std::uint32_t> idx;
    sphere(&pos, &idx);
    DocHandle doc;
    doc.doc = clay_document_create();
    REQUIRE(doc.doc != nullptr);
    const clay_layer_id layer = attach_layer(&doc, pos, idx);
    // Remesh so the layer holds a MARCHED mesh with something to weld.
    const clay_voxel_remesh_params p = defaults_at(40);
    REQUIRE(clay_document_voxel_remesh_layer(doc.doc, layer, &p, nullptr, nullptr) == CLAY_OK);

    std::uint64_t before = 0;
    REQUIRE(clay_document_mesh_layer_revision(doc.doc, layer, &before) == CLAY_OK);
    clay_mesh* borrowed = nullptr;
    REQUIRE(clay_document_mesh_layer_by_id(doc.doc, layer, &borrowed) == CLAY_OK);

    clay_weld_report r{};
    r.struct_size = sizeof(r);
    REQUIRE(clay_mesh_weld(borrowed, nullptr, &r) == CLAY_OK);
    REQUIRE(r.triangles_collapsed > 0);
    std::uint64_t after = 0;
    REQUIRE(clay_document_mesh_layer_revision(doc.doc, layer, &after) == CLAY_OK);
    CHECK(after > before);

    // A weld that changed nothing must NOT invalidate anything.
    clay_weld_report second{};
    second.struct_size = sizeof(second);
    REQUIRE(clay_mesh_weld(borrowed, nullptr, &second) == CLAY_OK);
    REQUIRE(second.vertices_merged == 0);
    std::uint64_t unchanged = 0;
    REQUIRE(clay_document_mesh_layer_revision(doc.doc, layer, &unchanged) == CLAY_OK);
    CHECK(unchanged == after);
}

TEST_CASE("c abi: welding refuses a protected layer and a bad epsilon") {
    std::vector<float> pos;
    std::vector<std::uint32_t> idx;
    sphere(&pos, &idx);
    DocHandle doc;
    doc.doc = clay_document_create();
    REQUIRE(doc.doc != nullptr);
    const clay_layer_id layer = attach_layer(&doc, pos, idx);
    clay_mesh* borrowed = nullptr;
    REQUIRE(clay_document_mesh_layer_by_id(doc.doc, layer, &borrowed) == CLAY_OK);

    clay_weld_desc bad{};
    bad.struct_size = sizeof(bad);
    REQUIRE(clay_mesh_weld_defaults(&bad) == CLAY_OK);
    bad.epsilon = -1.0f;
    CHECK(clay_mesh_weld(borrowed, &bad, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);

    REQUIRE(clay_document_set_layer_protection(doc.doc, layer, 0, 1) == CLAY_OK);
    CHECK(clay_mesh_weld(borrowed, nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
}
