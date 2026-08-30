// Global voxel remeshing (meshing spec, add-voxel-remesher).
//
// The properties the operation is defined by rather than the code paths it
// happens to have: overlaps fuse, resolution predicts detail, the sparse domain
// stores the same field a dense one would, open surfaces do what policy says,
// resources are refused before they are spent, and the same input twice is the
// same bytes twice.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "clay/mesh/bvh.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/to_field.h"
#include "clay/mesh/transfer.h"
#include "clay/mesh/validate.h"
#include "clay/mesh/voxel_remesh.h"
#include "../../src/mesh/voxel_remesh_internal.h"
#include "voxel_remesh_fixtures.h"

using namespace clay;
using namespace clay::mesh;
using clay_test::sphere;
using kernel::cf3;

namespace {

VoxelRemeshParams at_resolution(std::uint32_t n) {
    VoxelRemeshParams p;
    p.resolution_mode = VoxelRemeshResolutionMode::LongestAxisResolution;
    p.longest_axis_resolution = n;
    return p;
}

VoxelRemeshParams at_voxel_size(float v) {
    VoxelRemeshParams p;
    p.resolution_mode = VoxelRemeshResolutionMode::VoxelSize;
    p.voxel_size = v;
    return p;
}

// The symmetric surface distance, sampled deterministically: every vertex of
// each mesh against the other's tree, and the worse of the two RMS readings.
// Deterministic because it walks vertices in order and does no sampling of its
// own.
double surface_error(const Mesh& a, const Mesh& b) {
    const Bvh ta = Bvh::build(a);
    const Bvh tb = Bvh::build(b);
    auto rms = [](const Mesh& m, const Bvh& against) {
        double sum = 0.0;
        for (const kernel::cfloat3& p : m.positions) {
            const double d = against.unsigned_distance(p);
            sum += d * d;
        }
        return m.positions.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(m.positions.size()));
    };
    return std::max(rms(a, tb), rms(b, ta));
}

std::uint32_t component_count(const Mesh& m) {
    // Union-find over shared vertex indices, the same relation the report uses.
    std::vector<std::uint32_t> parent(m.positions.size());
    for (std::size_t i = 0; i < parent.size(); ++i) parent[i] = static_cast<std::uint32_t>(i);
    std::function<std::uint32_t(std::uint32_t)> find = [&](std::uint32_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    std::vector<std::uint8_t> used(m.positions.size(), 0);
    for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        const std::uint32_t a = m.indices[t], b = m.indices[t + 1], c = m.indices[t + 2];
        used[a] = used[b] = used[c] = 1;
        parent[find(a)] = find(b);
        parent[find(find(a))] = find(c);
    }
    std::vector<std::uint32_t> roots;
    for (std::size_t i = 0; i < parent.size(); ++i)
        if (used[i]) roots.push_back(find(static_cast<std::uint32_t>(i)));
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return static_cast<std::uint32_t>(roots.size());
}

bool all_finite(const Mesh& m) {
    for (const kernel::cfloat3& p : m.positions)
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
    return true;
}

// March a FieldVolume by the rule `voxel_remesh` extracts by: the stored sample
// where there is one, the volume's own far bound elsewhere, and one ring of
// positive samples around the lattice. Written out here rather than reached
// into, so the equivalence test is comparing two independent paths to the same
// answer instead of calling the thing it is checking.
Mesh march_volume(const field::FieldVolume& v) {
    const float spacing = v.cell_size();
    const kernel::cfloat3 origin = v.origin();
    const kernel::cfloat3 span = v.bounds().extent();
    const int n[3] = {static_cast<int>(std::lround(span.x / spacing)),
                      static_cast<int>(std::lround(span.y / spacing)),
                      static_cast<int>(std::lround(span.z / spacing))};
    const float outside = v.band() * 4.0f;
    auto sample = [&](int i, int j, int k) -> float {
        if (i < 0 || j < 0 || k < 0 || i > n[0] || j > n[1] || k > n[2]) return outside;
        if (const std::optional<float> s = v.sample_at(i, j, k)) return *s;
        return v.eval(origin + kernel::cf3(static_cast<float>(i), static_cast<float>(j),
                                           static_cast<float>(k)) *
                                   spacing);
    };
    const int cell_min[3] = {-1, -1, -1};
    const int cell_max[3] = {n[0] + 1, n[1] + 1, n[2] + 1};
    return mesh_lattice_parallel(sample, cell_min, cell_max, origin, spacing);
}

void check_clean(const Mesh& m) {
    const ValidationReport r = validate(m);
    CHECK(r.watertight);
    CHECK(r.manifold);
    CHECK(r.oriented);
    CHECK(r.degenerate_triangles == 0);
    CHECK(all_finite(m));
}

}  // namespace

TEST_CASE("the fixtures are what they claim to be") {
    // A guard, and it earned its place: the box fixture shipped with two of its
    // six faces wound inward, which made the remesher look like it was
    // shattering a two-slab model into eleven components. A fixture that is
    // wrong makes the code under test look wrong, so the fixtures are checked
    // before anything is concluded from them.
    const Mesh c = clay_test::cube(0.5f);
    CHECK(signed_volume(c) == doctest::Approx(1.0));
    CHECK(surface_area(c) == doctest::Approx(6.0));
    CHECK(validate(c).clean());

    const Mesh s = sphere(1.0f, cf3(0, 0, 0), 24, 48);
    CHECK(validate(s).clean());
    CHECK(signed_volume(s) == doctest::Approx(4.18879).epsilon(0.02));  // 4/3 pi

    const Mesh t = clay_test::torus();
    CHECK(validate(t).watertight);
    CHECK(validate(t).oriented);
    CHECK(validate(t).euler_characteristic == 0);

    CHECK(validate(clay_test::narrow_gap()).clean());
    CHECK(validate(clay_test::open_sphere()).boundary_edges > 0);
    CHECK(component_count(clay_test::body_and_island()) == 2u);
}

TEST_CASE("voxel remesh rebuilds a sphere into clean topology") {
    const Mesh src = sphere(1.0f, cf3(0, 0, 0), 20, 40);
    const VoxelRemeshResult r = voxel_remesh(src, at_resolution(64));
    REQUIRE(r.ok());
    check_clean(r.mesh);
    CHECK(r.report.result_triangles > 0);
    // Identity is not preserved, and the report is where that shows.
    CHECK(r.report.source_triangles == src.triangle_count());
    CHECK(r.report.result_triangles != r.report.source_triangles);
    CHECK(r.report.voxel_size == doctest::Approx(2.0f / 64.0f).epsilon(0.01));
    // Within a voxel of the sphere it came from.
    CHECK(surface_error(src, r.mesh) < 2.0 * r.report.voxel_size);
}

TEST_CASE("voxel remesh fuses overlapping shells into one body") {
    const Mesh src = clay_test::overlapping_spheres();
    CHECK(component_count(src) == 2u);
    const VoxelRemeshResult r = voxel_remesh(src, at_resolution(64));
    REQUIRE(r.ok());
    check_clean(r.mesh);
    CHECK(component_count(r.mesh) == 1u);
    CHECK(r.report.source_components == 2u);
    CHECK(r.report.result_components == 1u);
}

TEST_CASE("voxel remesh fuses two different tessellations") {
    const VoxelRemeshResult r = voxel_remesh(clay_test::sphere_and_cube(), at_resolution(64));
    REQUIRE(r.ok());
    check_clean(r.mesh);
    CHECK(component_count(r.mesh) == 1u);
}

TEST_CASE("voxel remesh gives a nested shell one exterior") {
    // The winding number counts the inner sphere's interior twice; what comes
    // out must still be one surface, not one with a second sphere inside it.
    const VoxelRemeshResult r = voxel_remesh(clay_test::nested_shells(), at_resolution(64));
    REQUIRE(r.ok());
    check_clean(r.mesh);
    CHECK(component_count(r.mesh) == 1u);
}

TEST_CASE("voxel remesh survives an inconsistent winding") {
    // Triangles flipped here and there through an otherwise closed sphere —
    // the state an import or a careless boolean leaves. A closest-triangle
    // pseudonormal sign is meaningless at each flipped face; the generalized
    // winding number only bends around them, which is why this is expected to
    // come out whole.
    Mesh src = sphere(0.8f, cf3(0, 0, 0), 20, 40);
    for (std::size_t t = 0; t + 2 < src.indices.size(); t += 3 * 17)
        std::swap(src.indices[t + 1], src.indices[t + 2]);

    const VoxelRemeshResult r = voxel_remesh(src, at_resolution(48));
    REQUIRE(r.ok());
    check_clean(r.mesh);
    // Positive: the result is outward-oriented whatever the source was.
    CHECK(r.report.result_volume > 0.0);
    CHECK(surface_error(sphere(0.8f, cf3(0, 0, 0), 20, 40), r.mesh) <
          3.0 * r.report.voxel_size);
}

TEST_CASE("a wholly inside-out source fails rather than producing nothing quietly") {
    // Every triangle reversed is not a defect the winding number can repair: it
    // is a surface that says the material is everywhere EXCEPT where the model
    // is, and inside the sampled region there is then no material at all. The
    // honest answer is a typed failure, and the report's negative source volume
    // is what names the cause.
    const Mesh flipped = clay_test::reversed_winding(sphere(0.8f, cf3(0, 0, 0), 20, 40));
    const VoxelRemeshResult r = voxel_remesh(flipped, at_resolution(48));
    CHECK(r.status == VoxelRemeshStatus::ExtractionFailed);
    CHECK(r.mesh.empty());
    CHECK(r.report.source_volume < 0.0);
}

TEST_CASE("voxel remesh rebuilds stretched topology at uniform density") {
    const Mesh src = clay_test::stretched_sphere();
    const VoxelRemeshResult r = voxel_remesh(src, at_resolution(96));
    REQUIRE(r.ok());
    check_clean(r.mesh);

    // The claim is that edge lengths cluster around the RESOLUTION, so the
    // measurement is the longest edge against the voxel size — not the spread
    // about the mean, which a marched lattice loses on by construction: its
    // vertices sit on lattice edges, so slivers beside near-diagonal edges are
    // how marching tetrahedra works and say nothing about uniformity.
    auto longest_edge = [](const Mesh& m) {
        double worst = 0.0;
        for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3)
            for (int e = 0; e < 3; ++e) {
                const kernel::cfloat3 a = m.positions[m.indices[t + e]];
                const kernel::cfloat3 b = m.positions[m.indices[t + (e + 1) % 3]];
                worst = std::max(worst, static_cast<double>(kernel::clength(b - a)));
            }
        return worst;
    };
    // Nothing longer than a cell's diagonal: an isosurface vertex lies on a
    // lattice edge, so no edge of the result can span more than one cell.
    CHECK(longest_edge(r.mesh) <= std::sqrt(3.0) * r.report.voxel_size + 1e-5);
    // ...and the source's longest edge is many times that, which is the state
    // this operation exists to leave behind.
    CHECK(longest_edge(src) > 5.0 * r.report.voxel_size);
}

TEST_CASE("finer voxels keep more of the source") {
    const Mesh src = clay_test::stretched_sphere();
    const VoxelRemeshResult coarse = voxel_remesh(src, at_resolution(32));
    const VoxelRemeshResult fine = voxel_remesh(src, at_resolution(96));
    REQUIRE(coarse.ok());
    REQUIRE(fine.ok());
    CHECK(surface_error(src, fine.mesh) < surface_error(src, coarse.mesh));
    CHECK(fine.report.result_triangles > coarse.report.result_triangles);
}

TEST_CASE("the two spellings of one resolution agree exactly") {
    const Mesh src = sphere(1.0f, cf3(0, 0, 0), 16, 32);
    const VoxelRemeshResult by_resolution = voxel_remesh(src, at_resolution(48));
    REQUIRE(by_resolution.ok());
    const VoxelRemeshResult by_size =
        voxel_remesh(src, at_voxel_size(by_resolution.report.voxel_size));
    REQUIRE(by_size.ok());
    CHECK(by_size.report.voxel_size == by_resolution.report.voxel_size);
    REQUIRE(by_size.mesh.positions.size() == by_resolution.mesh.positions.size());
    CHECK(std::memcmp(by_size.mesh.positions.data(), by_resolution.mesh.positions.data(),
                      by_size.mesh.positions.size() * sizeof(kernel::cfloat3)) == 0);
    CHECK(by_size.mesh.indices == by_resolution.mesh.indices);
}

TEST_CASE("repeated remeshes are bit-identical") {
    const Mesh src = clay_test::overlapping_spheres();
    const VoxelRemeshResult a = voxel_remesh(src, at_resolution(56));
    const VoxelRemeshResult b = voxel_remesh(src, at_resolution(56));
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    REQUIRE(a.mesh.positions.size() == b.mesh.positions.size());
    CHECK(std::memcmp(a.mesh.positions.data(), b.mesh.positions.data(),
                      a.mesh.positions.size() * sizeof(kernel::cfloat3)) == 0);
    CHECK(std::memcmp(a.mesh.normals.data(), b.mesh.normals.data(),
                      a.mesh.normals.size() * sizeof(kernel::cfloat3)) == 0);
    CHECK(a.mesh.indices == b.mesh.indices);
}

TEST_CASE("an invalid resolution is refused") {
    const Mesh src = sphere(1.0f, cf3(0, 0, 0), 12, 24);
    for (float bad : {0.0f, -0.1f, std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::quiet_NaN()}) {
        const VoxelRemeshResult r = voxel_remesh(src, at_voxel_size(bad));
        CHECK(r.status == VoxelRemeshStatus::InvalidResolution);
        CHECK(r.mesh.empty());
    }
    VoxelRemeshParams zero = at_resolution(0);
    const VoxelRemeshResult r = voxel_remesh(src, zero);
    CHECK(r.status == VoxelRemeshStatus::InvalidResolution);
    CHECK(r.mesh.empty());
}

TEST_CASE("an empty source is refused rather than meshed") {
    const VoxelRemeshResult r = voxel_remesh(Mesh{}, at_resolution(32));
    CHECK(r.status == VoxelRemeshStatus::EmptySource);
    CHECK(r.mesh.empty());
    const VoxelRemeshEstimate e = voxel_remesh_estimate(Mesh{}, at_resolution(32));
    CHECK(e.status == VoxelRemeshStatus::EmptySource);
}

TEST_CASE("a multires request is refused rather than ignored") {
    VoxelRemeshParams p = at_resolution(32);
    p.build_multires_levels = 2;
    const VoxelRemeshResult r = voxel_remesh(sphere(1.0f, cf3(0, 0, 0), 12, 24), p);
    CHECK(r.status == VoxelRemeshStatus::Unsupported);
}

TEST_CASE("an oversized request is refused before it allocates") {
    const Mesh src = sphere(1.0f, cf3(0, 0, 0), 20, 40);
    VoxelRemeshParams p = at_resolution(256);
    const VoxelRemeshEstimate estimate = voxel_remesh_estimate(src, p);
    REQUIRE(estimate.estimated_memory_bytes > 0);

    p.memory_budget_bytes = estimate.estimated_memory_bytes / 4;
    const VoxelRemeshEstimate refused = voxel_remesh_estimate(src, p);
    CHECK(refused.exceeds_memory_budget);
    CHECK(refused.status == VoxelRemeshStatus::ExceedsBudget);

    const VoxelRemeshResult r = voxel_remesh(src, p);
    CHECK(r.status == VoxelRemeshStatus::ExceedsBudget);
    CHECK(r.mesh.empty());
    // Refused, not quietly lowered: nothing came back at any other resolution.
    CHECK(r.report.result_triangles == 0);
}

TEST_CASE("the estimate predicts the run") {
    const Mesh src = clay_test::overlapping_spheres();
    const VoxelRemeshParams p = at_resolution(64);
    const VoxelRemeshEstimate e = voxel_remesh_estimate(src, p);
    const VoxelRemeshResult r = voxel_remesh(src, p);
    REQUIRE(r.ok());
    CHECK(e.resolved_voxel_size == r.report.voxel_size);
    // A bound, not a prediction: the marking keeps bricks that turn out to hold
    // nothing near enough to store.
    CHECK(e.estimated_active_samples >= r.report.active_samples);
    CHECK(r.report.active_samples > 0);
    CHECK(r.report.result_triangles >= e.estimated_triangle_min);
    CHECK(r.report.result_triangles <= e.estimated_triangle_max);
    CHECK(e.component_count == 2u);
    CHECK_FALSE(e.has_open_boundaries);
}

TEST_CASE("the estimate sees an open boundary and a thin feature") {
    const VoxelRemeshEstimate open =
        voxel_remesh_estimate(clay_test::open_sphere(), at_resolution(48));
    CHECK(open.has_open_boundaries);
    CHECK(open.boundary_edge_count > 0);

    // A plate two voxels thick at this resolution: material the resolution can
    // just about see, and the warning exists to say so before it is lost.
    const Mesh thin = clay_test::plate(0.02f);
    const VoxelRemeshEstimate warn = voxel_remesh_estimate(thin, at_resolution(32));
    CHECK(warn.thin_feature_warning);
    const VoxelRemeshEstimate fine = voxel_remesh_estimate(thin, at_resolution(400));
    CHECK_FALSE(fine.thin_feature_warning);
}

TEST_CASE("open surface policy is explicit") {
    const Mesh src = clay_test::open_sphere();

    VoxelRemeshParams reject = at_resolution(48);
    reject.open_surface_policy = VoxelRemeshOpenSurfacePolicy::Reject;
    const VoxelRemeshResult rejected = voxel_remesh(src, reject);
    CHECK(rejected.status == VoxelRemeshStatus::OpenSurfaceRejected);
    CHECK(rejected.mesh.empty());
    CHECK(rejected.report.source_boundary_edges > 0);
    CHECK(rejected.report.source_was_open);

    VoxelRemeshParams close = at_resolution(48);
    close.open_surface_policy = VoxelRemeshOpenSurfacePolicy::Close;
    const VoxelRemeshResult closed = voxel_remesh(src, close);
    REQUIRE(closed.ok());
    check_clean(closed.mesh);
    CHECK(closed.report.source_was_open);
    CHECK(closed.report.result_boundary_edges == 0);

    VoxelRemeshParams best = at_resolution(48);
    best.open_surface_policy = VoxelRemeshOpenSurfacePolicy::BestEffort;
    const VoxelRemeshResult effort = voxel_remesh(src, best);
    REQUIRE(effort.ok());
    CHECK(effort.report.source_was_open);
}

TEST_CASE("floating components are kept unless removal is asked for") {
    const Mesh src = clay_test::body_and_island();
    const VoxelRemeshResult kept = voxel_remesh(src, at_resolution(64));
    REQUIRE(kept.ok());
    CHECK(kept.report.result_components == 2u);
    CHECK(kept.report.removed_components == 0u);

    VoxelRemeshParams cull = at_resolution(64);
    cull.small_component_policy = VoxelRemeshSmallComponentPolicy::RemoveBelowVolume;
    // The island is a 0.09 sphere: about 0.003 cubic units. The body is 0.7:
    // about 1.4. A threshold between them names the island and nothing else.
    cull.minimum_component_volume = 0.05f;
    const VoxelRemeshResult culled = voxel_remesh(src, cull);
    REQUIRE(culled.ok());
    CHECK(culled.report.removed_components == 1u);
    CHECK(culled.report.result_components == 1u);
    check_clean(culled.mesh);
}

TEST_CASE("colour transfers spatially and absent attributes stay absent") {
    const Mesh coloured = clay_test::with_color_ramp(sphere(1.0f, cf3(0, 0, 0), 20, 40));
    const VoxelRemeshResult r = voxel_remesh(coloured, at_resolution(48));
    REQUIRE(r.ok());
    CHECK(r.report.colors_transferred);
    REQUIRE(r.mesh.colors.size() == r.mesh.positions.size());
    double worst = 0.0;
    for (std::size_t i = 0; i < r.mesh.positions.size(); ++i) {
        const float expect = std::clamp(r.mesh.positions[i].x * 0.5f + 0.5f, 0.0f, 1.0f);
        worst = std::max(worst, static_cast<double>(std::fabs(r.mesh.colors[i].x - expect)));
    }
    CHECK(worst < 0.1);

    const VoxelRemeshResult plain =
        voxel_remesh(sphere(1.0f, cf3(0, 0, 0), 20, 40), at_resolution(48));
    REQUIRE(plain.ok());
    CHECK(plain.mesh.colors.empty());
    CHECK_FALSE(plain.report.colors_transferred);
    CHECK_FALSE(plain.report.uvs_dropped);
}

TEST_CASE("a malformed colour array is treated as absent") {
    Mesh bad = sphere(1.0f, cf3(0, 0, 0), 16, 32);
    bad.colors.assign(7, cf3(1, 0, 0));  // neither empty nor the vertex count
    const VoxelRemeshResult r = voxel_remesh(bad, at_resolution(40));
    REQUIRE(r.ok());
    CHECK(r.mesh.colors.empty());
    CHECK_FALSE(r.report.colors_transferred);
}

TEST_CASE("uvs are dropped and the report says so") {
    const Mesh uved = clay_test::with_uvs(sphere(1.0f, cf3(0, 0, 0), 16, 32));
    const VoxelRemeshResult r = voxel_remesh(uved, at_resolution(40));
    REQUIRE(r.ok());
    CHECK(r.mesh.uvs.empty());
    CHECK(r.report.uvs_dropped);
}

TEST_CASE("projection recovers detail and honours its clamp") {
    const Mesh src = clay_test::stretched_sphere();

    VoxelRemeshParams off = at_resolution(48);
    off.project_to_source = false;
    VoxelRemeshParams on = at_resolution(48);
    on.project_to_source = true;

    const VoxelRemeshResult a = voxel_remesh(src, off);
    const VoxelRemeshResult b = voxel_remesh(src, on);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK(surface_error(src, b.mesh) < surface_error(src, a.mesh));
    CHECK(b.report.projected_to_source);
    CHECK(b.report.projected_vertices > 0);
    check_clean(b.mesh);

    // The same topology either way, so a vertex-by-vertex comparison is the
    // clamp itself: no vertex moved further than strength * limit.
    REQUIRE(a.mesh.positions.size() == b.mesh.positions.size());
    const float limit = on.projection_strength * on.max_projection_distance_voxels *
                        b.report.voxel_size;
    for (std::size_t i = 0; i < a.mesh.positions.size(); ++i)
        CHECK(kernel::clength(b.mesh.positions[i] - a.mesh.positions[i]) <= limit + 1e-5f);
}

TEST_CASE("projection does not pull a vertex across a narrow gap") {
    // Two slabs with a gap between them narrower than the projection clamp
    // reaches. A nearest-point projection with no sheet test would pull the
    // vertices lining one side of the gap onto the other side of it.
    const Mesh src = clay_test::narrow_gap();
    VoxelRemeshParams off = at_resolution(128);
    off.project_to_source = false;
    off.max_projection_distance_voxels = 6.0f;
    VoxelRemeshParams on = off;
    on.project_to_source = true;

    const VoxelRemeshResult a = voxel_remesh(src, off);
    const VoxelRemeshResult b = voxel_remesh(src, on);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    check_clean(b.mesh);
    // The gap survived: two slabs in, two bodies out.
    CHECK(b.report.result_components == 2u);

    // Extraction and projection share a topology, so this is vertex for
    // vertex: no vertex clearly on one side of the gap's mid-plane ended up on
    // the other.
    REQUIRE(a.mesh.positions.size() == b.mesh.positions.size());
    std::size_t crossed = 0, considered = 0;
    for (std::size_t i = 0; i < a.mesh.positions.size(); ++i) {
        const float before = a.mesh.positions[i].z;
        if (std::fabs(before) < b.report.voxel_size) continue;  // on the plane itself
        ++considered;
        if ((before > 0.0f) != (b.mesh.positions[i].z > 0.0f)) ++crossed;
    }
    CHECK(considered > 1000);
    CHECK(crossed == 0);
}

TEST_CASE("volume is measured, and correction is clamped and skipped when meaningless") {
    const Mesh src = sphere(1.0f, cf3(0, 0, 0), 24, 48);
    VoxelRemeshParams p = at_resolution(64);
    p.preserve_volume = true;
    const VoxelRemeshResult r = voxel_remesh(src, p);
    REQUIRE(r.ok());
    CHECK(r.report.source_volume > 0.0);
    CHECK(r.report.result_volume > 0.0);
    CHECK(r.report.relative_volume_error < 0.05);

    // An open source closed under policy adds material the source never had, so
    // scaling toward its volume would chase a number that is no longer about
    // the same solid.
    VoxelRemeshParams open = at_resolution(64);
    open.preserve_volume = true;
    const VoxelRemeshResult opened = voxel_remesh(clay_test::open_sphere(), open);
    REQUIRE(opened.ok());
    CHECK_FALSE(opened.report.volume_corrected);
}

TEST_CASE("cancellation is non-destructive") {
    Mesh src = clay_test::overlapping_spheres();
    const Mesh before = src;

    parallel::CancelToken token;
    token.cancel();
    const VoxelRemeshResult r = voxel_remesh(src, at_resolution(128), &token);
    CHECK(r.status == VoxelRemeshStatus::Cancelled);
    CHECK(r.report.cancelled);
    CHECK(r.mesh.empty());

    REQUIRE(src.positions.size() == before.positions.size());
    CHECK(std::memcmp(src.positions.data(), before.positions.data(),
                      src.positions.size() * sizeof(kernel::cfloat3)) == 0);
    CHECK(src.indices == before.indices);
}

TEST_CASE("the sparse domain is the field a dense sampling would produce") {
    // The claim the whole sampling design rests on: sparsity is an optimisation
    // of ONE field, not a second field. A closed source, so the far-brick sign
    // is well defined for both paths.
    //
    // The remesh does not hand its field back — nor should it — so the
    // comparison goes through the one artifact that depends on every stored
    // sample. The dense field is marched on the same lattice by the same rule,
    // and the two meshes have to agree BYTE FOR BYTE. Projection and the volume
    // correction are off, because both move vertices after extraction and this
    // is a question about the field.
    const Mesh src = sphere(0.8f, cf3(0, 0, 0), 20, 40);
    VoxelRemeshParams p = at_resolution(48);
    p.project_to_source = false;
    p.preserve_volume = false;
    const VoxelRemeshResult r = voxel_remesh(src, p);
    REQUIRE(r.ok());

    const float voxel = r.report.voxel_size;
    ImportSettings dense;
    dense.cell_size = voxel;
    dense.band = voxel * kVoxelRemeshBandVoxels;
    dense.padding = voxel * kVoxelRemeshPaddingVoxels;
    const std::optional<field::FieldVolume> reference = to_field(src, dense);
    REQUIRE(reference.has_value());
    CHECK(reference->cell_size() == voxel);
    // Same lattice, and the same bricks stored on it.
    CHECK(reference->sample_count() == r.report.active_samples);

    const Mesh dense_mesh = march_volume(*reference);
    REQUIRE(dense_mesh.positions.size() == r.mesh.positions.size());
    CHECK(std::memcmp(dense_mesh.positions.data(), r.mesh.positions.data(),
                      dense_mesh.positions.size() * sizeof(kernel::cfloat3)) == 0);
    CHECK(dense_mesh.indices == r.mesh.indices);
}

TEST_CASE("sampling follows the surface, not the bounding box") {
    // Doubling a sphere's radius at a fixed voxel size multiplies its SURFACE
    // by four and its bounding volume by eight. A domain that followed the box
    // would grow by eight; this has to grow like the surface.
    const float voxel = 0.004f;
    const VoxelRemeshEstimate small =
        voxel_remesh_estimate(sphere(0.5f, cf3(0, 0, 0), 32, 64), at_voxel_size(voxel));
    const VoxelRemeshEstimate large =
        voxel_remesh_estimate(sphere(1.0f, cf3(0, 0, 0), 32, 64), at_voxel_size(voxel));
    REQUIRE(small.estimated_active_samples > 0);
    const double ratio = static_cast<double>(large.estimated_active_samples) /
                         static_cast<double>(small.estimated_active_samples);
    CHECK(ratio < 5.5);  // four-ish, and nowhere near eight

    // ...and the marked band is a fraction of what a dense evaluation of the
    // same lattice would have cost. The comparison is against the dense
    // alternative directly: every brick of the lattice, at the same samples per
    // brick this marking counts.
    //
    // The margin WIDENS with resolution, which is the point rather than an
    // artefact — the band is a surface and the lattice is a volume — so this is
    // measured at a fine voxel size. At a coarse one the brick granularity
    // dominates and sparsity buys much less, which is honest and is why the
    // feature is defined against high resolution.
    const double bricks = std::ceil(large.grid_dimensions[0] / 8.0) *
                          std::ceil(large.grid_dimensions[1] / 8.0) *
                          std::ceil(large.grid_dimensions[2] / 8.0);
    const double dense_samples = bricks * 9.0 * 9.0 * 9.0;
    CHECK(static_cast<double>(large.estimated_active_samples) < dense_samples * 0.35);
}

TEST_CASE("a long thin body's band grows with its length") {
    // The other half of the same property, on the shape whose band fills its
    // own cross-section: doubling the length doubles the surface and the work,
    // and nothing here grows faster than that.
    const float voxel = 0.02f;
    const VoxelRemeshEstimate shorter =
        voxel_remesh_estimate(clay_test::long_bar(1.0f), at_voxel_size(voxel));
    const VoxelRemeshEstimate longer =
        voxel_remesh_estimate(clay_test::long_bar(2.0f), at_voxel_size(voxel));
    REQUIRE(shorter.estimated_active_samples > 0);
    const double ratio = static_cast<double>(longer.estimated_active_samples) /
                         static_cast<double>(shorter.estimated_active_samples);
    CHECK(ratio < 2.6);

    const VoxelRemeshResult r = voxel_remesh(clay_test::long_bar(2.0f), at_voxel_size(voxel));
    REQUIRE(r.ok());
    check_clean(r.mesh);
}

TEST_CASE("a torus keeps its hole") {
    const Mesh src = clay_test::torus();
    const VoxelRemeshResult r = voxel_remesh(src, at_resolution(96));
    REQUIRE(r.ok());
    check_clean(r.mesh);
    // Euler characteristic 0 for a genus-1 surface. A remesh that filled the
    // hole would report 2.
    CHECK(validate(r.mesh).euler_characteristic == 0);
}

TEST_CASE("a mask is resampled onto the new topology") {
    const Mesh src = sphere(1.0f, cf3(0, 0, 0), 20, 40);
    // Masked where x > 0, sharply.
    std::vector<float> mask(src.positions.size());
    for (std::size_t i = 0; i < mask.size(); ++i)
        mask[i] = src.positions[i].x > 0.0f ? 1.0f : 0.0f;

    const VoxelRemeshResult r = voxel_remesh(src, at_resolution(64));
    REQUIRE(r.ok());
    const std::vector<float> moved =
        transfer_vertex_scalar(src, mask, r.mesh, r.report.voxel_size * 2.0f);
    REQUIRE(moved.size() == r.mesh.positions.size());

    // Away from the boundary the mask has to agree with the region it names.
    std::size_t checked = 0, wrong = 0;
    for (std::size_t i = 0; i < moved.size(); ++i) {
        const float x = r.mesh.positions[i].x;
        if (std::fabs(x) < 4.0f * r.report.voxel_size) continue;
        ++checked;
        if ((x > 0.0f) != (moved[i] > 0.5f)) ++wrong;
    }
    CHECK(checked > 100);
    CHECK(wrong == 0);
}

TEST_CASE("a scalar transfer refuses a malformed array") {
    const Mesh src = sphere(1.0f, cf3(0, 0, 0), 12, 24);
    const Mesh dst = sphere(1.0f, cf3(0, 0, 0), 10, 20);
    const std::vector<float> wrong_length(5, 1.0f);
    const std::vector<float> out = transfer_vertex_scalar(src, wrong_length, dst, 0.0f, -1.0f);
    REQUIRE(out.size() == dst.positions.size());
    for (float v : out) CHECK(v == -1.0f);
}

TEST_CASE("an absurdly fine voxel size is refused, not cast into undefined behaviour") {
    // The lattice is O(bounding box), so its size has to be checked — and the
    // check has to happen in double BEFORE the cell counts are cast to int. A
    // 1e-7 voxel on a two-unit model is 2e7 cells on a side and 8e21 in total;
    // computed as ints that is signed overflow, which is undefined and would
    // reach the ceiling as whatever the wrap happened to produce.
    const Mesh src = sphere(1.0f, cf3(0, 0, 0), 12, 24);
    for (float tiny : {1e-7f, 1e-9f, 1e-20f}) {
        const VoxelRemeshResult r = voxel_remesh(src, at_voxel_size(tiny));
        CHECK(r.status == VoxelRemeshStatus::ExceedsBudget);
        CHECK(r.mesh.empty());
        const VoxelRemeshEstimate e = voxel_remesh_estimate(src, at_voxel_size(tiny));
        CHECK(e.status == VoxelRemeshStatus::ExceedsBudget);
        CHECK(e.exceeds_memory_budget == false);  // the LIBRARY's ceiling, not a caller budget
    }
}

TEST_CASE("a component threshold above the whole model gives the model back") {
    // A threshold larger than everything is a caller misjudging the scale. The
    // useful answer is the model — not an empty mesh, which would then fail the
    // watertight contract and report a validation failure for what was really a
    // bad number.
    const Mesh src = clay_test::body_and_island();
    VoxelRemeshParams p = at_resolution(48);
    p.small_component_policy = VoxelRemeshSmallComponentPolicy::RemoveBelowVolume;
    p.minimum_component_volume = 1e6f;
    const VoxelRemeshResult r = voxel_remesh(src, p);
    REQUIRE(r.ok());
    CHECK(r.report.removed_components == 0u);
    CHECK(r.report.result_components == 2u);
    CHECK(r.mesh.triangle_count() > 0);
    check_clean(r.mesh);
}

TEST_CASE("small-component removal survives an out-of-range index") {
    // Reachable from a caller's own mesh through the C ABI, not from the
    // marcher. Two of the three per-triangle vertex reads used to be unguarded,
    // so a triangle naming a vertex past the array read past it.
    Mesh bad = sphere(0.5f, cf3(0, 0, 0), 8, 16);
    const std::uint32_t past = static_cast<std::uint32_t>(bad.positions.size() + 5);
    bad.indices.push_back(0);
    bad.indices.push_back(1);
    bad.indices.push_back(past);
    std::vector<std::uint32_t> label;
    CHECK(remesh_detail::label_components(bad, &label) >= 1u);
    // The point is that this returns rather than reading past the array; the
    // sanitizer build is what makes the check meaningful.
    const std::uint32_t removed = remesh_detail::remove_small_components(&bad, 1e-6);
    CHECK(removed <= 1u);
}

TEST_CASE("sharp mode runs and is not held to the watertight contract") {
    // Dual contouring is flagged in the meshing spec and is not guaranteed
    // manifold, so Sharp returns what it produced with the report saying what
    // it is — rather than failing the validation the DEFAULT mode promises.
    const Mesh src = clay_test::cube(0.5f);
    VoxelRemeshParams p = at_resolution(48);
    p.surface_mode = VoxelRemeshSurfaceMode::Sharp;
    const VoxelRemeshResult r = voxel_remesh(src, p);
    REQUIRE(r.ok());
    CHECK(r.mesh.triangle_count() > 0);
    CHECK(all_finite(r.mesh));
    // The report says what it actually got, whatever that is.
    const ValidationReport check = validate(r.mesh);
    CHECK(r.report.result_watertight == check.watertight);
    CHECK(r.report.result_manifold == check.manifold);

    // ...and the DEFAULT mode on the same fixture is held to it.
    const VoxelRemeshResult smooth = voxel_remesh(src, at_resolution(48));
    REQUIRE(smooth.ok());
    check_clean(smooth.mesh);
}
