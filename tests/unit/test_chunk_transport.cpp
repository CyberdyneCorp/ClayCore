// THE DIRTY STREAM IS THE WHOLE SURFACE, ARRIVING A PIECE AT A TIME
// (sculpt-runtime and c-abi specs, add-extreme-poly-runtime 6.5 and 7.5).
//
// The whole-surface path stays in the library for correctness — `to_mesh`,
// `mesh_at_level`, the shipped per-block copies — and the incremental path is
// what a host at twenty million vertices actually uses. That arrangement is
// only worth having if the two agree, and NOTHING IN THE PIXELS SAYS WHEN THEY
// STOP. A host drawing from the dirty stream draws something the engine does
// not think it made, and it looks like geometry, so it survives review.
//
// So this file reconstructs the surface the way a host does — upload every
// chunk once, then apply only what the dirty set names after each edit — and
// compares the reconstruction against the whole-surface path the library still
// ships. Every triangle, by the bits of its nine floats.
//
// WHY TRIANGLES AND NOT INDEX BUFFERS. The two paths number their vertices
// differently on purpose: the whole-surface path emits the level's own global
// ids and a chunk emits ids local to itself, which is what lets a host upload a
// chunk as a standalone draw. Comparing index buffers would therefore compare
// the numbering rather than the surface. A triangle is canonicalised by
// ROTATING it so its smallest corner is first — a rotation and not a sort,
// because a sort loses the winding, and a surface whose triangles are all
// inside out is a different surface that a sorted comparison would call equal.
//
// THE PREVIEW GATE (7.5) IS THE SAME MEASUREMENT, COUNTED. The bytes a host is
// handed per stamp have to follow the dirty chunks and not the model, so the
// fixture here is FIXED-SPACING WITH A GROWING EXTENT — more of the same
// geometry at the same detail. A more finely subdivided cage would not hold the
// footprint constant and the gate would be measuring the fixture.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "clay/mesh/dynamic_sculpt.h"
#include "clay/mesh/dynamic_surface.h"
#include "clay/mesh/multires.h"
#include "clay/mesh/sculpt.h"
#include "clay/mesh/surface_view.h"

using namespace clay;
using namespace clay::kernel;
using mesh::ChunkOptions;
using mesh::ChunkReadback;
using mesh::ChunkRevisions;
using mesh::ChunkTable;
using mesh::DynamicSculptor;
using mesh::DynamicSurface;
using mesh::Mesh;
using mesh::MultiresSurface;
using mesh::SurfaceView;

namespace {

std::uint32_t bits_of(float f) {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

// One triangle, as the nine floats a host would upload, in a form two paths
// that number their vertices differently can still be compared by.
struct Tri {
    std::array<std::uint32_t, 9> bits{};
    bool operator<(const Tri& o) const { return bits < o.bits; }
    bool operator==(const Tri& o) const { return bits == o.bits; }
};

Tri make_tri(cfloat3 a, cfloat3 b, cfloat3 c) {
    const std::array<std::array<std::uint32_t, 3>, 3> corner = {
        {{bits_of(a.x), bits_of(a.y), bits_of(a.z)},
         {bits_of(b.x), bits_of(b.y), bits_of(b.z)},
         {bits_of(c.x), bits_of(c.y), bits_of(c.z)}}};
    std::size_t first = 0;
    for (std::size_t i = 1; i < 3; ++i)
        if (corner[i] < corner[first]) first = i;
    Tri t;
    for (std::size_t k = 0; k < 3; ++k) {
        const std::array<std::uint32_t, 3>& p = corner[(first + k) % 3];
        for (std::size_t j = 0; j < 3; ++j) t.bits[k * 3 + j] = p[j];
    }
    return t;
}

std::vector<Tri> triangles_of(const Mesh& m) {
    std::vector<Tri> out;
    out.reserve(m.indices.size() / 3);
    for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3)
        out.push_back(make_tri(m.positions[m.indices[i + 0]], m.positions[m.indices[i + 1]],
                               m.positions[m.indices[i + 2]]));
    std::sort(out.begin(), out.end());
    return out;
}

// One chunk, copied exactly as a host copies it: size it, then fill it.
struct ChunkUpload {
    std::vector<Tri> triangles;
    ChunkRevisions revisions;
    std::size_t bytes = 0;
    bool ok = false;
};

ChunkUpload upload(const SurfaceView& view, std::uint32_t chunk, const ChunkRevisions* expected) {
    ChunkUpload up;
    const ChunkReadback sized = view.copy_chunk(chunk, nullptr, nullptr, 0, nullptr, 0, nullptr, 0);
    if (!sized.ok) return up;
    std::vector<float> positions(static_cast<std::size_t>(sized.vertex_count) * 3, 0.0f);
    std::vector<std::uint32_t> indices(sized.index_count, 0u);
    const ChunkReadback got =
        view.copy_chunk(chunk, expected, positions.empty() ? nullptr : positions.data(),
                        positions.size(), nullptr, 0, indices.empty() ? nullptr : indices.data(),
                        indices.size());
    if (!got.ok || got.truncated) return up;
    const auto at = [&](std::uint32_t i) {
        return cf3(positions[i * 3 + 0], positions[i * 3 + 1], positions[i * 3 + 2]);
    };
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
        up.triangles.push_back(make_tri(at(indices[i + 0]), at(indices[i + 1]), at(indices[i + 2])));
    up.revisions = got.current;
    // WHAT THE HOST WAS ACTUALLY HANDED, which is the figure the preview gate
    // is about: three floats per vertex plus one index per corner.
    up.bytes = positions.size() * sizeof(float) + indices.size() * sizeof(std::uint32_t);
    up.ok = true;
    return up;
}

// A host's own copy of the surface, keyed by chunk, patched from a dirty set.
class Reconstruction {
  public:
    // The first upload: every live chunk, which is what a host does when it
    // first sees a document.
    std::size_t upload_all(const SurfaceView& view) {
        std::size_t bytes = 0;
        for (std::uint32_t i = 0; i < view.chunk_count(); ++i) {
            if (view.chunks().chunk(i) == nullptr) continue;
            const ChunkUpload up = upload(view, i, nullptr);
            if (!up.ok) continue;
            chunks_[i] = up;
            bytes += up.bytes;
        }
        return bytes;
    }

    // The drain: only what the dirty set names, and a released chunk is SKIPPED
    // rather than treated as an error — a partitioner that retires a chunk does
    // not walk the dirty list to remove it.
    std::size_t drain(const SurfaceView& view, std::size_t* chunks_drained) {
        std::size_t bytes = 0;
        const std::vector<std::uint32_t> dirty = view.dirty_chunks();
        for (std::uint32_t id : dirty) {
            if (view.chunks().chunk(id) == nullptr) {
                chunks_.erase(id);
                continue;
            }
            const ChunkUpload up = upload(view, id, nullptr);
            if (!up.ok) continue;
            chunks_[id] = up;
            bytes += up.bytes;
        }
        if (chunks_drained != nullptr) *chunks_drained = dirty.size();
        return bytes;
    }

    std::vector<Tri> surface() const {
        std::vector<Tri> out;
        for (const auto& entry : chunks_)
            out.insert(out.end(), entry.second.triangles.begin(), entry.second.triangles.end());
        std::sort(out.begin(), out.end());
        return out;
    }

  private:
    std::map<std::uint32_t, ChunkUpload> chunks_;
};

// A cage at FIXED SPACING whose extent grows with `n`. "A bigger model" here is
// more of the same geometry, never a more finely subdivided one, or the
// footprint would not be held constant.
Mesh quad_field(int n, float spacing) {
    Mesh m;
    const float half = spacing * static_cast<float>(n) * 0.5f;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            m.positions.push_back(cf3(-half + spacing * static_cast<float>(x), 0.0f,
                                      -half + spacing * static_cast<float>(z)));
    const auto at = [&](int x, int z) { return static_cast<std::uint32_t>(z * (n + 1) + x); };
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            m.quads.push_back(at(x, z));
            m.quads.push_back(at(x + 1, z));
            m.quads.push_back(at(x + 1, z + 1));
            m.quads.push_back(at(x, z + 1));
            m.indices.insert(m.indices.end(), {at(x, z), at(x + 1, z), at(x + 1, z + 1), at(x, z),
                                               at(x + 1, z + 1), at(x, z + 1)});
        }
    return m;
}

// An edit with a WORLD-SPACE footprint, applied the way the sculptor applies
// one: write into the level mesh, then tell the hierarchy which vertices moved.
// A world-space ball is what holds the footprint constant while the model grows.
std::size_t move_ball(MultiresSurface& surface, std::uint32_t level, cfloat3 centre, float radius,
                      float lift) {
    const std::vector<cfloat3>& positions = surface.positions_at(level);
    std::vector<std::uint32_t> moved;
    for (std::uint32_t i = 0; i < positions.size(); ++i)
        if (clength(positions[i] - centre) <= radius) moved.push_back(i);
    if (moved.empty()) return 0;
    Mesh& level_mesh = surface.level_mesh(level);
    for (std::uint32_t v : moved) level_mesh.positions[v].y += lift;
    surface.absorb_level_edit(level, moved);
    surface.positions_at(level);
    return moved.size();
}

}  // namespace

TEST_CASE("chunk transport: a hierarchy reconstructed from the dirty stream IS the level mesh") {
    // Ten base quads a side at three levels is 6400 faces and about
    // twenty-five chunks — enough that "a local edit drained a fraction of
    // them" is a statement about locality rather than about a fixture too
    // small to have a fraction.
    auto surface = MultiresSurface::from_mesh(quad_field(10, 0.25f));
    REQUIRE(surface.has_value());
    REQUIRE(surface->add_level());
    REQUIRE(surface->add_level());
    REQUIRE(surface->add_level());
    const std::uint32_t level = surface->max_level();

    Reconstruction host;
    {
        SurfaceView view = SurfaceView::over_level(*surface, level);
        REQUIRE(view.valid());
        REQUIRE(host.upload_all(view) > 0);
    }
    // THE FIRST COMPARISON: the initial upload against the whole-surface path
    // the library still ships. If these differ, nothing after this means
    // anything.
    const Mesh whole_before = surface->mesh_at_level(level);
    REQUIRE_FALSE(whole_before.indices.empty());
    CHECK(host.surface() == triangles_of(whole_before));

    // Now an edit, and the DIRTY SET ALONE is what the host is told about.
    const std::size_t moved = move_ball(*surface, level, cf3(0, 0, 0), 0.30f, 0.12f);
    REQUIRE(moved > 0);

    std::size_t drained = 0;
    std::size_t all_chunks = 0;
    {
        SurfaceView view = SurfaceView::over_level(*surface, level);
        all_chunks = view.chunks().live_count();
        REQUIRE(host.drain(view, &drained) > 0);
    }
    REQUIRE(drained > 0);
    // A LOCAL EDIT IS A LOCAL DRAIN. Reconstructing correctly by re-uploading
    // everything is not the claim; this is what makes the comparison below
    // about the incremental path rather than about a second full copy.
    CAPTURE(drained);
    CAPTURE(all_chunks);
    CHECK(drained * 2 < all_chunks);

    const Mesh whole_after = surface->mesh_at_level(level);
    // AND THE EDIT ACTUALLY LANDED, or the two paths would agree about a
    // surface neither of them changed.
    CHECK(triangles_of(whole_after) != triangles_of(whole_before));
    CHECK(host.surface() == triangles_of(whole_after));
}

TEST_CASE("chunk transport: a second edit's stream is applied on top of the first") {
    auto surface = MultiresSurface::from_mesh(quad_field(10, 0.25f));
    REQUIRE(surface.has_value());
    REQUIRE(surface->add_level());
    REQUIRE(surface->add_level());
    REQUIRE(surface->add_level());
    const std::uint32_t level = surface->max_level();

    Reconstruction host;
    {
        SurfaceView view = SurfaceView::over_level(*surface, level);
        host.upload_all(view);
    }

    // Two edits in DIFFERENT places, drained separately. The second drain must
    // not undo the first: a reconstruction that rebuilt from the newest stream
    // alone would pass a single-edit test and fail this one.
    const cfloat3 places[2] = {cf3(-0.4f, 0.0f, -0.4f), cf3(0.4f, 0.0f, 0.4f)};
    for (int i = 0; i < 2; ++i) {
        REQUIRE(move_ball(*surface, level, places[i], 0.22f, 0.09f) > 0);
        SurfaceView view = SurfaceView::over_level(*surface, level);
        std::size_t drained = 0;
        host.drain(view, &drained);
        CHECK(drained > 0);
        surface->clear_dirty_chunks(level);
    }

    const Mesh whole = surface->mesh_at_level(level);
    CHECK(host.surface() == triangles_of(whole));
}

TEST_CASE("chunk transport: an adaptive surface reconstructs against to_mesh") {
    auto surface = DynamicSurface::from_mesh(quad_field(10, 0.2f));
    REQUIRE(surface.has_value());
    DynamicSculptor sculptor(*surface);

    Reconstruction host;
    {
        SurfaceView view = SurfaceView::over_dynamic(sculptor.surface(), sculptor.bvh().chunks());
        REQUIRE(view.valid());
        REQUIRE(host.upload_all(view) > 0);
    }
    // The adaptive surface's chunks upload UNWELDED — its topology moves under
    // the stamp being uploaded, so a per-chunk vertex map would be the heap
    // object per chunk per frame the transport requirement forbids. The
    // triangle comparison is indifferent to that, which is the point of
    // comparing triangles.
    mesh::DynamicSurfaceExportOptions export_options;
    export_options.normals = false;
    export_options.colors = false;
    export_options.uvs = false;
    CHECK(host.surface() == triangles_of(surface->to_mesh(export_options)));

    // A stamp WITH topology changes, which is the case a chunk table has to
    // survive: faces are created and destroyed under the chunk that owns them.
    mesh::MeshBrushSettings brush;
    brush.center = cf3(0, 0, 0);
    brush.radius = 0.35f;
    brush.strength = 0.5f;
    mesh::DynamicTopologySettings topology;
    topology.enabled = true;
    topology.detail_mode = mesh::DynamicDetailMode::BrushRelative;
    const mesh::DynamicStampResult r =
        sculptor.stamp(mesh::MeshBrush::Draw, brush, topology);
    REQUIRE(r.changed());

    {
        SurfaceView view = SurfaceView::over_dynamic(sculptor.surface(), sculptor.bvh().chunks());
        std::size_t drained = 0;
        host.drain(view, &drained);
        CHECK(drained > 0);
    }
    CHECK(host.surface() == triangles_of(surface->to_mesh(export_options)));
}

TEST_CASE("chunk transport: a fixed mesh's chunks cover its whole-surface triangles") {
    Mesh mesh = quad_field(12, 0.2f);
    ChunkOptions options;
    options.target_faces = 64;
    ChunkTable table;
    mesh::partition_mesh_chunks(mesh, options, &table);

    Reconstruction host;
    SurfaceView view = SurfaceView::over_mesh(mesh, table);
    REQUIRE(view.valid());
    REQUIRE(host.upload_all(view) > 0);
    CHECK(host.surface() == triangles_of(mesh));

    // And after the sculptor has moved the mesh underneath it, a fresh upload
    // still is the mesh. The fixed sculptor does not yet mark chunks — see
    // `partition_mesh_chunks` — so what is asserted here is the transport, not
    // a dirty set it does not have.
    mesh::MeshSculptor sculptor(mesh);
    mesh::MeshBrushSettings brush;
    brush.center = cf3(0, 0, 0);
    brush.radius = 0.4f;
    brush.strength = 0.4f;
    REQUIRE(sculptor.stamp(mesh::MeshBrush::Draw, brush) > 0);

    Reconstruction again;
    SurfaceView after = SurfaceView::over_mesh(mesh, table);
    again.upload_all(after);
    CHECK(again.surface() == triangles_of(mesh));
}

TEST_CASE("preview gate: the bytes a stamp hands a host follow the dirty chunks, not the model") {
    // FIXED SPACING, GROWING EXTENT, and the same world-space footprint on
    // both. The small cage is 10x10 base quads and the large one 40x40 —
    // sixteen times the faces at the same detail.
    //
    // BOTH MODELS ARE WIDER THAN THE FOOTPRINT, deliberately. A first version
    // of this used a cage barely larger than the brush, so the small model's
    // dab hit two of its three chunks and the ratio measured the boundary
    // rather than the transport.
    struct Row {
        std::size_t model_triangles = 0;
        std::size_t stamp_bytes = 0;
        std::size_t initial_bytes = 0;
        std::size_t drained = 0;
    };
    Row rows[2];
    const int sizes[2] = {10, 40};

    for (int i = 0; i < 2; ++i) {
        auto surface = MultiresSurface::from_mesh(quad_field(sizes[i], 0.25f));
        REQUIRE(surface.has_value());
        REQUIRE(surface->add_level());
        REQUIRE(surface->add_level());
        const std::uint32_t level = surface->max_level();

        Reconstruction host;
        {
            SurfaceView view = SurfaceView::over_level(*surface, level);
            rows[i].initial_bytes = host.upload_all(view);
        }
        surface->clear_dirty_chunks(level);
        rows[i].model_triangles = surface->mesh_at_level(level).indices.size() / 3;

        REQUIRE(move_ball(*surface, level, cf3(0, 0, 0), 0.30f, 0.10f) > 0);
        SurfaceView view = SurfaceView::over_level(*surface, level);
        rows[i].stamp_bytes = host.drain(view, &rows[i].drained);
    }

    CAPTURE(rows[0].model_triangles);
    CAPTURE(rows[1].model_triangles);
    CAPTURE(rows[0].stamp_bytes);
    CAPTURE(rows[1].stamp_bytes);
    REQUIRE(rows[0].stamp_bytes > 0);
    REQUIRE(rows[1].stamp_bytes > 0);

    // The model grew by this much...
    const double model_ratio = static_cast<double>(rows[1].model_triangles) /
                               static_cast<double>(rows[0].model_triangles);
    REQUIRE(model_ratio > 8.0);
    // ...and a FULL upload grew with it, which is what makes the next line a
    // claim rather than an accident of a small fixture.
    CHECK(static_cast<double>(rows[1].initial_bytes) >
          4.0 * static_cast<double>(rows[0].initial_bytes));

    // THE GATE. A stamp of the same world footprint hands the host the same
    // bytes at sixteen times the model. The band is generous — a chunk is a
    // fixed face count and a footprint that straddles a chunk boundary
    // differently costs one more chunk — and an O(model) transport would be
    // off by the model ratio, not by a chunk.
    const double byte_ratio =
        static_cast<double>(rows[1].stamp_bytes) / static_cast<double>(rows[0].stamp_bytes);
    CAPTURE(byte_ratio);
    CAPTURE(model_ratio);
    CHECK(byte_ratio < 2.0);
    CHECK(byte_ratio * 4.0 < model_ratio);
}
