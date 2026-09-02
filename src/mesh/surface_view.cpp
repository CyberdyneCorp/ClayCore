#include "clay/mesh/surface_view.h"

#include <algorithm>
#include <vector>

#include "clay/mesh/dynamic_sculpt.h"
#include "clay/mesh/dynamic_surface.h"
#include "clay/mesh/multires.h"
#include "clay/mesh/sculpt.h"

namespace clay {
namespace mesh {
namespace {

void write_f3(float* out, kernel::cfloat3 v) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}

}  // namespace

// -- construction -----------------------------------------------------------------

SurfaceView SurfaceView::over_mesh(const Mesh& mesh, const ChunkTable& chunks) {
    SurfaceView v;
    v.kind_ = SurfaceKind::Fixed;
    v.chunks_ = &chunks;
    v.corners_ = mesh.indices.data();
    v.positions_ = mesh.positions.data();
    v.normals_ = mesh.normals.empty() ? nullptr : mesh.normals.data();
    v.vertex_count_ = mesh.positions.size();
    return v;
}

SurfaceView SurfaceView::over_dynamic(const DynamicSurface& surface, const ChunkTable& chunks) {
    SurfaceView v;
    v.kind_ = SurfaceKind::Adaptive;
    v.chunks_ = &chunks;
    v.surface_ = &surface;
    v.vertex_count_ = surface.vertices().size();
    return v;
}

SurfaceView SurfaceView::over_level(MultiresSurface& surface, std::uint32_t level) {
    SurfaceView v;
    if (level >= surface.level_count()) return v;
    v.kind_ = SurfaceKind::Multires;
    // Order matters: the chunks are built from P(n), so this is what evaluates
    // the level, and the arrays below have to be taken after it.
    v.chunks_ = &surface.chunks_at(level);
    v.topology_ = &surface.topology_at(level);
    const std::vector<kernel::cfloat3>& positions = surface.positions_at(level);
    const std::vector<kernel::cfloat3>& normals = surface.normals_at(level);
    v.positions_ = positions.data();
    v.normals_ = normals.empty() ? nullptr : normals.data();
    v.vertex_count_ = positions.size();
    return v;
}

std::size_t SurfaceView::chunk_count() const {
    return chunks_ == nullptr ? 0 : chunks_->slot_count();
}

const std::vector<std::uint32_t>& SurfaceView::dirty_chunks() const {
    static const std::vector<std::uint32_t> kEmpty;
    return chunks_ == nullptr ? kEmpty : chunks_->dirty();
}

// -- the readback -------------------------------------------------------------------

ChunkReadback SurfaceView::copy_chunk(std::uint32_t chunk, const ChunkRevisions* expected,
                                      float* positions, std::size_t position_capacity,
                                      float* normals, std::size_t normal_capacity,
                                      std::uint32_t* indices, std::size_t index_capacity) const {
    ChunkReadback out;
    out.chunk = chunk;
    if (chunks_ == nullptr) return out;
    const SurfaceChunk* record = chunks_->chunk(chunk);
    if (record == nullptr) return out;
    out.ok = true;
    out.current = record->revisions;
    if (expected != nullptr) {
        out.requested = *expected;
        out.stale = *expected != record->revisions;
    } else {
        out.requested = record->revisions;
    }

    // The two shapes, and the note in the header says why there are two: a
    // representation with a stable per-chunk vertex list uploads welded, and one
    // whose topology moves under the stamp uploads unwelded rather than
    // rebuilding a vertex map per chunk per frame.
    const ChunkVertexSpan vertices = chunks_->vertices(chunk);
    const bool welded = kind_ != SurfaceKind::Adaptive && !vertices.empty();

    if (welded) {
        out.vertex_count = static_cast<std::uint32_t>(vertices.size());
        std::uint32_t triangles = 0;
        for (FaceId f : record->faces) {
            const std::uint32_t arity =
                topology_ != nullptr ? topology_->face_arity(f.slot) : 3u;
            if (arity >= 3) triangles += arity - 2;
        }
        out.index_count = triangles * 3;
    } else {
        std::uint32_t live = 0;
        for (FaceId f : record->faces) {
            if (surface_ != nullptr && !surface_->live(f)) continue;
            ++live;
        }
        out.vertex_count = live * 3;
        out.index_count = live * 3;
    }

    // THE CAPACITY QUERY: every buffer null reports what the chunk needs and
    // writes nothing, so a host sizes once and copies once.
    if (positions == nullptr && normals == nullptr && indices == nullptr) return out;

    const std::size_t needed_floats = static_cast<std::size_t>(out.vertex_count) * 3;
    if ((positions != nullptr && position_capacity < needed_floats) ||
        (normals != nullptr && normal_capacity < needed_floats) ||
        (indices != nullptr && index_capacity < out.index_count)) {
        // NOTHING IS WRITTEN into a buffer that is too small — not a partial
        // fill a caller might draw. The counts above say what it needed.
        out.truncated = true;
        return out;
    }

    if (welded) {
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            const std::uint32_t global = vertices[i];
            if (positions != nullptr && global < vertex_count_)
                write_f3(positions + i * 3, positions_[global]);
            if (normals != nullptr)
                write_f3(normals + i * 3,
                         normals_ != nullptr && global < vertex_count_ ? normals_[global]
                                                                      : kernel::cf3(0, 1, 0));
        }
        if (indices != nullptr) {
            std::size_t at = 0;
            for (FaceId f : record->faces) {
                std::uint32_t arity = 3;
                const std::uint32_t* corners =
                    topology_ != nullptr ? topology_->face(f.slot, &arity)
                                         : corners_ + static_cast<std::size_t>(f.slot) * 3;
                if (arity < 3) continue;
                // A FAN from the first corner. A quad's two triangles share its
                // diagonal, which is what every other triangulation in this
                // library does with a subdivision quad, so a host's silhouette
                // matches the level's own mesh.
                const std::uint32_t first = vertices.local_of(corners[0]);
                for (std::uint32_t k = 1; k + 1 < arity; ++k) {
                    indices[at++] = first;
                    indices[at++] = vertices.local_of(corners[k]);
                    indices[at++] = vertices.local_of(corners[k + 1]);
                }
            }
        }
        return out;
    }

    std::size_t vi = 0;
    for (FaceId f : record->faces) {
        if (surface_ == nullptr || !surface_->live(f)) continue;
        VertexId v[3];
        if (!surface_->face_vertices(f, v)) continue;
        for (int k = 0; k < 3; ++k) {
            const DynamicVertex* rec = surface_->vertex(v[k]);
            if (positions != nullptr)
                write_f3(positions + vi * 3, rec != nullptr ? rec->position : kernel::cf3(0, 0, 0));
            if (normals != nullptr)
                write_f3(normals + vi * 3, rec != nullptr ? rec->normal : kernel::cf3(0, 1, 0));
            if (indices != nullptr) indices[vi] = static_cast<std::uint32_t>(vi);
            ++vi;
        }
    }
    out.vertex_count = static_cast<std::uint32_t>(vi);
    out.index_count = out.vertex_count;
    return out;
}

// -- the fixed-mesh partitioner --------------------------------------------------------

void partition_mesh_chunks(const Mesh& mesh, const ChunkOptions& options, ChunkTable* out) {
    if (out == nullptr) return;
    const std::size_t triangles = mesh.indices.size() / 3;
    const std::size_t target = std::max<std::size_t>(options.target_faces, 1);
    out->reset(triangles / target + 1, triangles + target);
    out->set_options(options);
    if (triangles == 0) return;

    std::vector<std::uint32_t> order(triangles);
    for (std::uint32_t i = 0; i < triangles; ++i) order[i] = i;
    std::vector<kernel::cfloat3> centroids(triangles);
    for (std::uint32_t t = 0; t < triangles; ++t) {
        const kernel::cfloat3 a = mesh.positions[mesh.indices[t * 3 + 0]];
        const kernel::cfloat3 b = mesh.positions[mesh.indices[t * 3 + 1]];
        const kernel::cfloat3 c = mesh.positions[mesh.indices[t * 3 + 2]];
        centroids[t] = (a + b + c) * (1.0f / 3.0f);
    }

    std::vector<FaceId> block;
    std::vector<std::uint32_t> vertices;
    // A median split down to the target size, exactly as the adaptive
    // partitioner does, with the same INDEX tie-break so the partition is a
    // function of the mesh and not of the sort's stability. `nth_element` is not
    // stable and a tie decided differently on another standard library would
    // chunk the same mesh two ways.
    struct Chunker {
        const Mesh& mesh;
        ChunkTable& table;
        std::vector<std::uint32_t>& order;
        std::vector<kernel::cfloat3>& centroids;
        std::vector<FaceId>& block;
        std::vector<std::uint32_t>& vertices;
        std::size_t target;

        void run(std::size_t begin, std::size_t end) {
            const std::size_t count = end - begin;
            if (count <= target) {
                publish(begin, end);
                return;
            }
            math::Aabb spread;
            for (std::size_t i = begin; i < end; ++i) spread.expand(centroids[order[i]]);
            const kernel::cfloat3 ext = spread.extent();
            const int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : (ext.y >= ext.z ? 1 : 2);
            const std::size_t mid = begin + count / 2;
            std::nth_element(order.begin() + static_cast<std::ptrdiff_t>(begin),
                             order.begin() + static_cast<std::ptrdiff_t>(mid),
                             order.begin() + static_cast<std::ptrdiff_t>(end),
                             [&](std::uint32_t a, std::uint32_t b) {
                                 const kernel::cfloat3 ca = centroids[a], cb = centroids[b];
                                 const float va = axis == 0 ? ca.x : (axis == 1 ? ca.y : ca.z);
                                 const float vb = axis == 0 ? cb.x : (axis == 1 ? cb.y : cb.z);
                                 if (va != vb) return va < vb;
                                 return a < b;
                             });
            run(begin, mid);
            run(mid, end);
        }

        void publish(std::size_t begin, std::size_t end) {
            block.clear();
            vertices.clear();
            math::Aabb bounds;
            for (std::size_t i = begin; i < end; ++i) {
                const std::uint32_t t = order[i];
                block.push_back(FaceId{t, 0});
                for (int k = 0; k < 3; ++k) {
                    const std::uint32_t v = mesh.indices[t * 3 + static_cast<std::size_t>(k)];
                    vertices.push_back(v);
                    bounds.expand(mesh.positions[v]);
                }
            }
            std::sort(block.begin(), block.end(),
                      [](FaceId a, FaceId b) { return a.slot < b.slot; });
            std::sort(vertices.begin(), vertices.end());
            vertices.erase(std::unique(vertices.begin(), vertices.end()), vertices.end());
            const std::uint32_t chunk = table.create();
            table.assign_faces(chunk, block.data(), block.size());
            table.set_vertices(chunk, vertices.data(), vertices.size());
            table.set_bounds(chunk, bounds);
        }
    };

    Chunker{mesh, *out, order, centroids, block, vertices, target}.run(0, triangles);
    out->clear_dirty();
}

// -- the ledger ---------------------------------------------------------------------------

void report_surface_memory(const MeshSculptor& sculptor, memory::MemoryLedger* ledger) {
    if (ledger == nullptr) return;
    // The mesh and nothing else. The sculptor's ray tree is built LAZILY by a
    // non-const accessor, so a const report cannot ask for its size without
    // building one — and a memory report that allocates 1.3 s of BVH on a 2M
    // vertex mesh to answer "how much am I using" is a report no host would
    // call twice.
    ledger->add(memory::MemoryCategory::BaseGeometry, sculptor.mesh().bytes());
}

void report_surface_memory(const DynamicSculptor& sculptor, memory::MemoryLedger* ledger) {
    if (ledger == nullptr) return;
    // The half-edge structure is geometry and connectivity in one storage, and
    // splitting it would be arithmetic with no measurement behind it. It counts
    // as base geometry because that is what a host may never release.
    ledger->add(memory::MemoryCategory::BaseGeometry, sculptor.surface().bytes());
    ledger->add(memory::MemoryCategory::ChunkIndex, sculptor.bvh().bytes());
}

void report_surface_memory(const MultiresSurface& surface, memory::MemoryLedger* ledger) {
    if (ledger == nullptr) return;
    const MultiresMemory m = surface.memory();
    ledger->add(memory::MemoryCategory::BaseGeometry, m.base);
    ledger->add(memory::MemoryCategory::Topology, m.topology);
    ledger->add(memory::MemoryCategory::MultiresDetail, m.detail);
    ledger->add(memory::MemoryCategory::EvaluatedCache, m.evaluated);
    ledger->add(memory::MemoryCategory::LevelRuntimeCache, m.runtime_index);
    ledger->add(memory::MemoryCategory::ChunkIndex, m.chunk_index);
}

// -- the trim -----------------------------------------------------------------------------

memory::TrimReport trim_surface(MultiresSurface& surface, memory::Pressure pressure,
                                const memory::TrimGate* gate) {
    memory::TrimReport report;
    report.pressure = pressure;
    const MultiresMemory before = surface.memory();

    if (memory::trim_blocked(gate)) {
        // WHAT IT WOULD HAVE RELEASED, and this is a CEILING rather than a
        // promise at anything below critical pressure: computing the exact set
        // means walking the residency decision, and running that decision is
        // the thing the pin exists to prevent. At critical pressure the ceiling
        // is exact, because everything rebuildable goes.
        report.pinned = true;
        report.release(memory::MemoryCategory::EvaluatedCache, before.evaluated);
        report.release(memory::MemoryCategory::LevelRuntimeCache, before.runtime_index);
        report.release(memory::MemoryCategory::ChunkIndex, before.chunk_index);
        return report;
    }

    switch (pressure) {
        case memory::Pressure::None:
            // Nothing. A hierarchy holds no transient scratch of its own between
            // calls, and releasing a cache at no pressure is the engine
            // deciding for the host.
            break;
        case memory::Pressure::Warning:
            // The levels nobody is looking at, which on a deep hierarchy is
            // most of them.
            surface.drop_inactive_caches();
            break;
        case memory::Pressure::Urgent:
            // Everything except the sculpt and display levels — including the
            // levels BETWEEN them and the cage, which the next edit below the
            // active levels will pay to rebuild.
            surface.drop_intermediate_caches();
            break;
        case memory::Pressure::Critical:
            surface.drop_all_caches();
            break;
    }

    const MultiresMemory after = surface.memory();
    const auto released = [](std::size_t a, std::size_t b) { return a > b ? a - b : 0; };
    report.release(memory::MemoryCategory::EvaluatedCache,
                   released(before.evaluated, after.evaluated));
    report.release(memory::MemoryCategory::LevelRuntimeCache,
                   released(before.runtime_index, after.runtime_index));
    report.release(memory::MemoryCategory::ChunkIndex,
                   released(before.chunk_index, after.chunk_index));
    return report;
}

memory::TrimReport trim_surface(DynamicSculptor& sculptor, memory::Pressure pressure,
                                const memory::TrimGate* gate) {
    memory::TrimReport report;
    report.pressure = pressure;
    ChunkTable& table = sculptor.bvh().chunks_mutable();
    const std::size_t slack_bytes = table.arena_slack() * sizeof(FaceId);

    if (memory::trim_blocked(gate)) {
        report.pinned = true;
        report.release(memory::MemoryCategory::ChunkIndex, slack_bytes);
        return report;
    }
    if (pressure == memory::Pressure::None || pressure == memory::Pressure::Warning)
        return report;

    // THE ARENA'S SLACK, AND NOT THE INDEX ITSELF. An adaptive surface's tree is
    // its only means of asking a local question; releasing it does not save a
    // host memory so much as make the next dab a scan over every face, which is
    // the one thing this whole change exists to prevent. Nor is the SLOT POOL's
    // slack released here: it is rebuildable, but reconstructing it renumbers
    // slots, so the surface would come back identical as a surface and
    // different as a partition — and what a host has already uploaded would be
    // addressed by ids that no longer mean the same chunks.
    const std::size_t before = table.bytes();
    table.compact();
    const std::size_t after = table.bytes();
    report.release(memory::MemoryCategory::ChunkIndex, before > after ? before - after : 0);
    return report;
}

}  // namespace mesh
}  // namespace clay
