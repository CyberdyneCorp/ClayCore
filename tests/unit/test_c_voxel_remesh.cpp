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
