#include "clay/mesh/sculpt.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>

#include "clay/kernel/deform.h"  // calpha_sample, calpha_frame
#include "clay/mesh/automask.h"

namespace clay {
namespace mesh {
namespace {

// The three geometric helpers that genuinely READ A MESH, and so are the three
// that could not move to `sculpt_kernels.cpp` with the rest. Everything else
// this file used to define — the falloff, the alpha frame, the plane offset,
// the Laplacian, polish's gate and every verb — is now shared math and lives
// there.

// Unnormalized face normal — its length is twice the triangle's area, which is
// exactly the weight an area-weighted vertex normal wants.
kernel::cfloat3 face_normal(const Mesh& m, std::uint32_t tri) {
    const kernel::cfloat3& a = m.positions[m.indices[tri * 3]];
    const kernel::cfloat3& b = m.positions[m.indices[tri * 3 + 1]];
    const kernel::cfloat3& c = m.positions[m.indices[tri * 3 + 2]];
    return kernel::ccross(b - a, c - a);
}

// The interior angle a triangle subtends at one of its corners.
float corner_angle(const Mesh& m, std::uint32_t tri, int corner) {
    const kernel::cfloat3& p = m.positions[m.indices[tri * 3 + corner]];
    const kernel::cfloat3& a = m.positions[m.indices[tri * 3 + (corner + 1) % 3]];
    const kernel::cfloat3& b = m.positions[m.indices[tri * 3 + (corner + 2) % 3]];
    const kernel::cfloat3 u = a - p, v = b - p;
    const float lu = kernel::clength(u), lv = kernel::clength(v);
    if (lu < 1e-20f || lv < 1e-20f) return 0.0f;
    return std::acos(std::clamp(kernel::cdot(u, v) / (lu * lv), -1.0f, 1.0f));
}

// A weld class's geometric normal, ANGLE-weighted over its incident triangles.
//
// GEOMETRIC rather than the mesh's stored normals on purpose: displacement is
// about where the surface is, not about how it shades, and this way a mesh
// imported without normals sculpts exactly like one that has them.
//
// Angle-weighted rather than area-weighted, which is the more usual choice and
// is wrong here. A lattice-derived mesh — anything marching cubes or surface
// nets produced, which is most of what this library hands back — has triangles
// of wildly uneven area, so an area-weighted normal leans toward whichever
// neighbour happens to be large and varies at the LATTICE's frequency rather
// than the surface's. `inflate`, which displaces each vertex along its own
// normal, turns that straight into a golf-ball dimple across the stamp. The
// angle at the corner is a property of the surface rather than of how it was
// triangulated, and the dimple goes away.
kernel::cfloat3 class_normal(const Mesh& m, const Adjacency& adj, std::uint32_t cls) {
    std::size_t n = 0;
    const std::uint32_t* tris = adj.triangles_of(cls, &n);
    kernel::cfloat3 sum = kernel::cf3(0, 0, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const kernel::cfloat3 face = safe_normalize(face_normal(m, tris[i]), kernel::cf3(0, 0, 0));
        for (int corner = 0; corner < 3; ++corner)
            if (adj.class_of(m.indices[tris[i] * 3 + corner]) == cls)
                sum = sum + face * corner_angle(m, tris[i], corner);
    }
    return safe_normalize(sum, kernel::cf3(0, 1, 0));
}

}  // namespace

bool writes_color(MeshBrush verb) {
    return verb == MeshBrush::Paint || verb == MeshBrush::Smear;
}

const char* StageTelemetry::name(SculptStage stage) {
    switch (stage) {
        case SculptStage::SeedResolve: return "seed";
        case SculptStage::SpatialQuery: return "query";
        case SculptStage::Weight: return "weight";
        case SculptStage::Alpha: return "alpha";
        case SculptStage::Automask: return "automask";
        case SculptStage::Snapshot: return "snapshot";
        case SculptStage::NeighborBuild: return "neighbors";
        case SculptStage::Kernel: return "kernel";
        case SculptStage::Writeback: return "writeback";
        case SculptStage::NormalRefresh: return "normals";
        case SculptStage::Topology: return "topology";
        case SculptStage::ChunkMark: return "chunkmark";
        case SculptStage::BvhUpdate: return "index";
        case SculptStage::Count: break;
    }
    return "?";
}

bool default_geodesic(MeshBrush verb) {
    // Flatten and Scrape mean "everything under this disc". A surface walk
    // would refuse to flatten across a groove, which is where a flatten is
    // most wanted.
    return verb != MeshBrush::Flatten && verb != MeshBrush::Scrape;
}

// -- VertexDeltas -------------------------------------------------------------

void VertexDeltas::clear() {
    vertices_.clear();
    before_position_.clear();
    after_position_.clear();
    before_normal_.clear();
    after_normal_.clear();
    before_color_.clear();
    after_color_.clear();
    slot_.clear();
    normals_ = false;
    colors_ = false;
}

std::optional<kernel::cfloat3> VertexDeltas::origin_of(std::uint32_t v) const {
    const auto it = slot_.find(v);
    if (it == slot_.end()) return std::nullopt;
    return before_position_[it->second];
}

void VertexDeltas::note(std::uint32_t v, const Mesh& m) {
    if (slot_.find(v) != slot_.end()) return;
    const bool has_normals = m.normals.size() == m.positions.size();
    const bool has_colors = m.colors.size() == m.positions.size();
    if (vertices_.empty()) {
        normals_ = has_normals;
        colors_ = has_colors;
    }
    slot_.emplace(v, static_cast<std::uint32_t>(vertices_.size()));
    vertices_.push_back(v);
    before_position_.push_back(m.positions[v]);
    after_position_.push_back(m.positions[v]);
    if (normals_ && has_normals) {
        before_normal_.push_back(m.normals[v]);
        after_normal_.push_back(m.normals[v]);
    }
    if (colors_ && has_colors) {
        before_color_.push_back(m.colors[v]);
        after_color_.push_back(m.colors[v]);
    }
}

void VertexDeltas::sync_after(std::uint32_t v, const Mesh& m) {
    auto it = slot_.find(v);
    if (it == slot_.end()) return;
    after_position_[it->second] = m.positions[v];
    if (!after_normal_.empty() && m.normals.size() == m.positions.size())
        after_normal_[it->second] = m.normals[v];
    if (!after_color_.empty() && m.colors.size() == m.positions.size())
        after_color_[it->second] = m.colors[v];
}

bool VertexDeltas::revert(Mesh& m) const {
    if (vertices_.empty()) return true;
    const bool normals = !before_normal_.empty() && m.normals.size() == m.positions.size();
    const bool colors = !before_color_.empty() && m.colors.size() == m.positions.size();
    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        const std::uint32_t v = vertices_[i];
        if (v >= m.positions.size()) return false;
    }
    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        m.positions[vertices_[i]] = before_position_[i];
        if (normals) m.normals[vertices_[i]] = before_normal_[i];
        if (colors) m.colors[vertices_[i]] = before_color_[i];
    }
    return true;
}

bool VertexDeltas::apply(Mesh& m) const {
    if (vertices_.empty()) return true;
    const bool normals = !after_normal_.empty() && m.normals.size() == m.positions.size();
    const bool colors = !after_color_.empty() && m.colors.size() == m.positions.size();
    for (std::size_t i = 0; i < vertices_.size(); ++i)
        if (vertices_[i] >= m.positions.size()) return false;
    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        m.positions[vertices_[i]] = after_position_[i];
        if (normals) m.normals[vertices_[i]] = after_normal_[i];
        if (colors) m.colors[vertices_[i]] = after_color_[i];
    }
    return true;
}

// -- MeshSculptor -------------------------------------------------------------

namespace {

// The seed-token counter. Process-wide and monotonic, so no two live sculptors
// share a token and a token is never reused after one is destroyed — a
// hierarchy destroys and rebuilds its level sculptor on every rebind, and a
// counter that restarted would hand the new one the retired one's identity,
// which is precisely the confusion the token exists to catch.
//
// Relaxed ordering is enough: the only thing required of the value is that it
// differs between sculptors, never that it orders anything against other
// memory.
std::atomic<std::uint64_t> g_seed_revision{kNoSeedRevision};

std::uint64_t next_seed_revision() {
    return g_seed_revision.fetch_add(1, std::memory_order_relaxed) + 1;
}

}  // namespace

MeshSculptor::MeshSculptor(Mesh& m, float weld_epsilon)
    : mesh_(m), adjacency_(Adjacency::build(m, weld_epsilon)),
      seed_revision_(next_seed_revision()) {}

MeshSculptor::MeshSculptor(Mesh& m, Adjacency adjacency)
    : mesh_(m), adjacency_(std::move(adjacency)), seed_revision_(next_seed_revision()) {}

const Bvh& MeshSculptor::bvh() {
    if (!bvh_) bvh_ = std::make_unique<Bvh>(Bvh::build(mesh_));
    return *bvh_;
}

void MeshSculptor::refresh_bvh() {
    bvh_ = std::make_unique<Bvh>(Bvh::build(mesh_));
    clear_bvh_dirty();  // a rebuild covers everything, so nothing is owed
}

// Record that `cls` moved, for the next refit. Marked through `class_dirty_` so
// a stroke that stamps the same classes hundreds of times keeps ONE entry each.
void MeshSculptor::mark_bvh_dirty(std::uint32_t cls) {
    if (bvh_all_dirty_) return;  // already owed everything
    if (class_dirty_.size() != adjacency_.class_count()) {
        class_dirty_.assign(adjacency_.class_count(), 0);
        dirty_classes_.clear();
    }
    if (cls >= class_dirty_.size() || class_dirty_[cls]) return;
    class_dirty_[cls] = 1;
    dirty_classes_.push_back(cls);
}

void MeshSculptor::clear_bvh_dirty() {
    // Through the list, so the reset costs what was touched. Clearing the mark
    // array would be O(classes), which is O(mesh) on the per-stamp path this
    // whole change exists to keep proportional to the brush.
    for (std::uint32_t c : dirty_classes_)
        if (c < class_dirty_.size()) class_dirty_[c] = 0;
    dirty_classes_.clear();
    bvh_all_dirty_ = false;
}

void MeshSculptor::refit_bvh() {
    if (!bvh_) return;  // nothing built yet; the next bvh() builds a correct one

    // EVERYTHING moved since the last fit, not everything the last stamp moved.
    // `apply_stroke` runs many stamps and `region_` holds only the final one,
    // so deriving the set from it would name a SUBSET — and a subset leaves the
    // unnamed triangles' ancestors holding pre-stroke bounds, which is a query
    // that quietly answers for geometry that has moved.
    if (bvh_all_dirty_) {
        if (!bvh_->refit(mesh_)) refresh_bvh();
        clear_bvh_dirty();
        return;
    }
    if (dirty_classes_.empty()) return;

    // A triangle changes when any of its corners does, so the triangles of the
    // moved classes are exactly the changed set — not merely a superset.
    refit_tris_.clear();
    for (std::uint32_t c : dirty_classes_) {
        std::size_t n = 0;
        const std::uint32_t* tris = adjacency_.triangles_of(c, &n);
        for (std::size_t k = 0; k < n; ++k) refit_tris_.push_back(tris[k]);
    }
    if (refit_tris_.empty()) {
        clear_bvh_dirty();
        return;
    }
    if (!bvh_->refit(mesh_, refit_tris_.data(), refit_tris_.size()))
        refresh_bvh();
    else
        clear_bvh_dirty();
}

// The BVH, refitted to the mesh as it is now.
//
// The brushes needed a spatial index over the surface and this is already one,
// built over the same triangles and — since `refit` — cheap to keep current
// under a stroke. A second structure would be a second thing to keep in step
// with the vertices, and the two would disagree the first time one was missed.
//
// The first call on a large mesh pays a build, exactly as a host's first pick
// does today. Every call after that pays a refit of what moved.
const Bvh* MeshSculptor::surface_index() {
    // NEVER BUILDS ONE. Measured on a million-vertex grid: a build is 689 ms
    // and it saves 1.24 ms per stamp, so building it here would need ~550
    // stamps to break even and every shorter session would be worse off. A
    // brush is not the right caller to decide a host should own a ray tree.
    //
    // Any host that places a brush by picking already has one, because
    // `raycast` builds it on the first pick — and since `refit` it is cheap to
    // keep current. So the common case gets the index for free, and a host that
    // never picks keeps exactly the behaviour it had, by the scans below.
    if (!bvh_) return nullptr;
    refit_bvh();
    return bvh_.get();
}

// Every class with a vertex inside the ball. Exact: a class sits on a vertex,
// a vertex is a corner of every triangle that references it, and the tree
// returns every triangle reaching the ball — so no class inside it is missed.
bool MeshSculptor::classes_in_ball(kernel::cfloat3 centre, float radius,
                                   std::vector<std::uint32_t>* out) {
    const Bvh* tree = surface_index();
    // The ray tree first where there is one, so a host that picks keeps exactly
    // the path it had; the chunk tree is what a sculptor NOTHING picks against
    // has instead of a scan, which is the multires level.
    if (!tree) return classes_in_ball_chunked(centre, radius, out);
    out->clear();
    tree->triangles_in_ball(centre, radius, &ball_tris_);
    const float r2 = radius * radius;
    if (ball_mark_.size() != adjacency_.class_count()) ball_mark_.assign(adjacency_.class_count(), 0);
    for (std::uint32_t t : ball_tris_) {
        const std::size_t base = static_cast<std::size_t>(t) * 3;
        if (base + 2 >= mesh_.indices.size()) continue;
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t v = mesh_.indices[base + k];
            if (v >= mesh_.positions.size()) continue;
            const std::uint32_t c = adjacency_.class_of(v);
            if (c >= ball_mark_.size() || ball_mark_[c]) continue;
            // The tree admits a triangle that merely REACHES the ball, so the
            // corner still has to be tested. Over-admitting there is what makes
            // this exact here.
            if (kernel::cdot2(mesh_.positions[v] - centre) > r2) continue;
            ball_mark_[c] = 1;
            out->push_back(c);
        }
    }
    for (std::uint32_t c : *out) ball_mark_[c] = 0;  // retire what we set
    return true;
}

kernel::cfloat3 MeshSculptor::class_position(std::uint32_t cls) const {
    std::size_t n = 0;
    return mesh_.positions[adjacency_.members(cls, &n)[0]];
}

std::uint32_t MeshSculptor::nearest_class(kernel::cfloat3 p) {
    // Through the tree, which finds the nearest triangle CORNER — and a class
    // sits on a vertex, so the nearest corner names the nearest class. O(log N)
    // where the scan this replaced was O(classes) and, being on the seed path
    // of every unseeded stroke, was the largest single term in a dab on a big
    // mesh (measured 1.34 ms of a 1.34 ms stamp at a million vertices).
    //
    // A class NO TRIANGLE REFERENCES cannot be found this way. Such a vertex is
    // unreachable by a brush anyway — a region is a walk over triangles — so
    // the two answers differ only where the old one was useless.
    const Bvh* tree = surface_index();
    if (!tree) {
        // The chunk tree, when this sculptor was given a table: a descent over
        // chunk bounds returning the same class the scan below returns, tie for
        // tie. See `nearest_class_chunked`.
        std::uint32_t chunked = kNoClass;
        if (nearest_class_chunked(p, &chunked)) return chunked;
        // No ray tree to ask, so the scan this replaced is still the answer.
        const std::uint32_t classes = static_cast<std::uint32_t>(adjacency_.class_count());
        anchor_measurements_ += classes;
        std::uint32_t best = kNoClass;
        float best_d2 = 0.0f;
        for (std::uint32_t c = 0; c < classes; ++c) {
            const float d2 = kernel::cdot2(class_position(c) - p);
            if (best == kNoClass || d2 < best_d2) {
                best = c;
                best_d2 = d2;
            }
        }
        return best;
    }
    const Bvh::NearestVertex hit = tree->nearest_vertex(p);
    if (!hit.found) return kNoClass;
    const std::size_t base = static_cast<std::size_t>(hit.triangle) * 3 +
                             static_cast<std::size_t>(hit.corner);
    if (base >= mesh_.indices.size()) return kNoClass;
    const std::uint32_t v = mesh_.indices[base];
    if (v >= mesh_.positions.size()) return kNoClass;
    return adjacency_.class_of(v);
}

// The brush's OWN facing, for the normal-angle automask. Fixed for the stamp
// and never taken from the region: the region's average normal is weighted by
// the very weights the automask is shaping, so reading it here would be
// circular.
kernel::cfloat3 MeshSculptor::automask_reference(const MeshBrushSettings& settings) {
    if (!is_zero(settings.deposit_normal))
        return safe_normalize(settings.deposit_normal, kernel::cf3(0, 1, 0));
    const std::uint32_t near = automask_seed_ != kNoClass ? automask_seed_
                                                          : nearest_class(settings.center);
    return near != kNoClass ? class_normal(mesh_, adjacency_, near) : kernel::cf3(0, 1, 0);
}

std::uint32_t MeshSculptor::accepted_seed(const MeshBrushSettings& settings) {
    if (settings.seed_class >= adjacency_.class_count()) return kNoClass;
    // A caller that claims nothing gets what it has always got: the bounds
    // check above and nothing more. Silently REFUSING an unrevisioned seed
    // would have been the stricter design and was rejected — it turns every
    // shipped caller's fast path into a full scan, which is a performance
    // regression delivered as a correctness fix.
    if (settings.seed_revision == kNoSeedRevision) return settings.seed_class;
    if (settings.seed_revision == seed_revision_) return settings.seed_class;
    // In bounds, and from somebody else's numbering. Falling back to the scan
    // costs one query; honouring it costs the stamp, because a seed outside the
    // radius makes `geodesic_region` return an empty region.
    ++stale_seeds_rejected_;
    return kNoClass;
}

// -- the chunk query path -----------------------------------------------------

void MeshSculptor::set_chunks(ChunkTable* chunks) {
    if (chunks_ == chunks) return;
    chunks_ = chunks;
    chunk_tree_.clear();
    chunk_tree_built_ = false;
    chunk_of_.clear();
}

void MeshSculptor::rebuild_chunk_of() {
    chunk_of_.assign(mesh_.positions.size(), ChunkTable::kNoChunk);
    if (chunks_ == nullptr) return;
    for (std::uint32_t id = 0; id < chunks_->slot_count(); ++id) {
        if (chunks_->chunk(id) == nullptr) continue;
        const ChunkVertexSpan span = chunks_->vertices(id);
        for (std::size_t i = 0; i < span.size(); ++i) {
            const std::uint32_t v = span[i];
            // A vertex on a chunk boundary is listed by every chunk whose faces
            // reference it. The FIRST wins, so the map is a function and a
            // write region turns into one dirty id per vertex rather than a
            // set — the other chunks holding it are reached anyway, because
            // this stamp wrote their faces' corners too.
            if (v < chunk_of_.size() && chunk_of_[v] == ChunkTable::kNoChunk) chunk_of_[v] = id;
        }
    }
}

const ChunkTree* MeshSculptor::chunk_index() {
    if (chunks_ == nullptr) return nullptr;
    if (!chunk_tree_built_) {
        chunk_tree_.build(*chunks_);
        chunk_tree_built_ = true;
        rebuild_chunk_of();
    }
    return chunk_tree_.empty() ? nullptr : &chunk_tree_;
}

// Every class with a vertex inside the ball, from the chunk tree. EXACT and not
// merely close: the tree admits a chunk whose BOX meets the ball, and then every
// vertex of every admitted chunk is tested against the radius itself — so the
// set is the one the scan produces, and the sort below makes the ORDER the same
// too. That is what lets this replace the scan without moving a single result.
bool MeshSculptor::classes_in_ball_chunked(kernel::cfloat3 centre, float radius,
                                           std::vector<std::uint32_t>* out) {
    const ChunkTree* tree = chunk_index();
    if (tree == nullptr) return false;
    const math::Aabb ball{centre - kernel::cf3(radius, radius, radius),
                          centre + kernel::cf3(radius, radius, radius)};
    tree->query(ball, &chunk_hits_);
    out->clear();
    const float r2 = radius * radius;
    if (ball_mark_.size() != adjacency_.class_count())
        ball_mark_.assign(adjacency_.class_count(), 0);
    for (std::uint32_t id : chunk_hits_) {
        const ChunkVertexSpan span = chunks_->vertices(id);
        for (std::size_t i = 0; i < span.size(); ++i) {
            const std::uint32_t v = span[i];
            if (v >= mesh_.positions.size()) continue;
            const std::uint32_t c = adjacency_.class_of(v);
            if (c >= ball_mark_.size() || ball_mark_[c]) continue;
            if (kernel::cdot2(mesh_.positions[v] - centre) > r2) continue;
            ball_mark_[c] = 1;
            out->push_back(c);
        }
    }
    for (std::uint32_t c : *out) ball_mark_[c] = 0;
    return true;
}

// The class nearest `p`, from the chunk tree, and EXACTLY the one the scan
// returns — including its tie-break.
//
// Two-pass rather than a best-first descent with a running bound: the first
// pass takes the nearest CHUNK's nearest vertex to get a radius, the second is
// an ordinary ball query at that radius. It is two descents instead of one, and
// it is the version whose answer is provably the scan's: a priority descent
// that prunes on the bound it is still refining has to get the >= / > right at
// every step to stay exact, and the two-pass form has nowhere to put that
// mistake.
bool MeshSculptor::nearest_class_chunked(kernel::cfloat3 p, std::uint32_t* out) {
    const ChunkTree* tree = chunk_index();
    if (tree == nullptr) return false;
    // The starting radius comes from the NEAREST CHUNK, not from the model.
    // Seeding it from the table's extent was the first version and it was
    // O(model) by construction: a sixteen-times-bigger plane has a four-times
    // wider box, so the first query admitted sixteen times the chunks and the
    // locality gate read 2.28x instead of 1.00x. The nearest chunk is a
    // branch-and-bound descent and its answer is a property of the surface
    // near `p`.
    float radius = 0.0f;
    {
        float chunk_distance = 0.0f;
        const std::uint32_t near = tree->nearest_chunk(p, &chunk_distance);
        if (near == ChunkTable::kNoChunk) return false;
        // The nearest vertex OF that chunk bounds the answer from above: the
        // true nearest vertex is no further than this one, so a ball of this
        // radius certainly contains it.
        const ChunkVertexSpan span = chunks_->vertices(near);
        float bound = 0.0f;
        bool have = false;
        anchor_measurements_ += span.size();
        for (std::size_t i = 0; i < span.size(); ++i) {
            const std::uint32_t v = span[i];
            if (v >= mesh_.positions.size()) continue;
            const float d2 = kernel::cdot2(mesh_.positions[v] - p);
            if (!have || d2 < bound) {
                bound = d2;
                have = true;
            }
        }
        if (!have) return false;
        radius = std::sqrt(bound);
        // A degenerate chunk of coincident vertices can put `p` exactly on one,
        // and a zero-radius box admits only what touches the point. The chunk's
        // own distance is the smallest step that certainly widens it.
        if (!(radius > 0.0f)) radius = chunk_distance > 0.0f ? chunk_distance : 1e-6f;
    }
    std::uint32_t best = kNoClass;
    float best_d2 = 0.0f;
    for (int attempt = 0; attempt < 24; ++attempt) {
        const math::Aabb box{p - kernel::cf3(radius, radius, radius),
                             p + kernel::cf3(radius, radius, radius)};
        tree->query(box, &chunk_hits_);
        best = kNoClass;
        for (std::uint32_t id : chunk_hits_) {
            const ChunkVertexSpan span = chunks_->vertices(id);
            anchor_measurements_ += span.size();
            for (std::size_t i = 0; i < span.size(); ++i) {
                const std::uint32_t v = span[i];
                if (v >= mesh_.positions.size()) continue;
                const std::uint32_t c = adjacency_.class_of(v);
                // `class_position` is the class's FIRST member, and a class is
                // a set of coincident vertices, so any member's position is the
                // class's. Reading `v` rather than the first member is the same
                // number and one fewer indirection.
                const float d2 = kernel::cdot2(mesh_.positions[v] - p);
                // Strictly closer, or exactly as close and lower-numbered:
                // the ascending scan keeps the first of a tie and so does this.
                if (best == kNoClass || d2 < best_d2 || (d2 == best_d2 && c < best)) {
                    best = c;
                    best_d2 = d2;
                }
            }
        }
        // A vertex inside the BOX may still be further than any vertex outside
        // it, because the box is a cube and the answer is a sphere. The result
        // stands only once the nearest found sits within the half-extent, which
        // is the largest sphere the box certainly covered.
        if (best != kNoClass && std::sqrt(best_d2) <= radius) {
            *out = best;
            return true;
        }
        radius *= 2.0f;
    }
    // Twenty-four doublings from a fraction of the model without covering it
    // means the table's bounds do not describe this mesh. Refusing hands the
    // caller back to the scan, which cannot be wrong.
    return false;
}

// Refit the bounds of the chunks this stamp wrote and mark them, so a host's
// dirty stream is a property of the sculptor rather than of the host.
void MeshSculptor::publish_chunks(bool normals_changed, bool attributes_changed) {
    dirty_chunks_.clear();
    if (chunks_ == nullptr) return;
    // The MAP is what this needs, not the tree. Requiring the tree tied the
    // dirty stream to the query path, so a caller that supplied a table purely
    // for the transport — and passed its own `seed_class`, which is what a host
    // that picks does — got an empty stream and a silent zero-byte upload.
    if (chunk_of_.size() != mesh_.positions.size()) rebuild_chunk_of();
    if (dirty_mark_.size() != chunks_->slot_count()) dirty_mark_.assign(chunks_->slot_count(), 0);
    for (WorkItemId item : region_.write_region) {
        const std::uint32_t c = item.as_weld_class();
        if (c >= adjacency_.class_count()) continue;
        std::size_t n = 0;
        const std::uint32_t* members = adjacency_.members(c, &n);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t v = members[i];
            if (v >= chunk_of_.size()) continue;
            const std::uint32_t id = chunk_of_[v];
            if (id == ChunkTable::kNoChunk || id >= dirty_mark_.size() || dirty_mark_[id]) continue;
            dirty_mark_[id] = 1;
            dirty_chunks_.push_back(id);
        }
    }
    for (std::uint32_t id : dirty_chunks_) {
        dirty_mark_[id] = 0;
        // The bounds are recomputed from the chunk's own vertices rather than
        // expanded. A brush that pulls a vertex IN would leave an expanded box
        // permanently too large, and a query's cost follows the boxes.
        math::Aabb bounds;
        const ChunkVertexSpan span = chunks_->vertices(id);
        for (std::size_t i = 0; i < span.size(); ++i) {
            const std::uint32_t v = span[i];
            if (v < mesh_.positions.size()) bounds.expand(mesh_.positions[v]);
        }
        chunks_->set_bounds(id, bounds);
        // Geometry, always: this is the write region. NEVER topology — the
        // fixed-topology contract is that `indices` and `quads` come back
        // byte-identical, so marking it would tell a host to re-upload an index
        // buffer that cannot have changed.
        chunks_->mark(id, ChunkDirty::Geometry);
        if (normals_changed) chunks_->mark(id, ChunkDirty::Normals);
        if (attributes_changed) chunks_->mark(id, ChunkDirty::Attributes);
    }
    // Only when there IS a tree: a table supplied for the transport alone has
    // no index to keep current.
    if (chunk_tree_built_ && !dirty_chunks_.empty())
        chunk_tree_.refit(*chunks_, dirty_chunks_.data(), dirty_chunks_.size());
}

// The fixed mesh's answers to the two questions `compose_workset` cannot
// answer for itself. Free functions with a `this` context rather than lambdas,
// because `WorkItemReader` holds function pointers — see the note there on why
// it is not a `std::function`.
kernel::cfloat3 MeshSculptor::normal_of_item(const void* context, WorkItemId item) {
    const MeshSculptor* self = static_cast<const MeshSculptor*>(context);
    return class_normal(self->mesh_, self->adjacency_, item.as_weld_class());
}

// THE WALK: everything the brush REACHES, with the distance each was reached
// at. It fills `candidates_` and `distance_` and nothing else, because what
// happens next — the weights, the drops, the automask, the frame — is the same
// on all three representations and lives in `compose_workset`.
void MeshSculptor::build_fixed_mesh_workset(const MeshBrushSettings& settings) {
    BrushRegion& r = region_;
    // Retire the LAST stamp's slots before `r.items` is overwritten, so the
    // reset costs what that stamp touched. `adjacency.h` states the rule this
    // follows and the reason: "allocating a per-class array per stamp is the
    // entire cost of the stroke". `slot` has to stay a full per-class array
    // because the verbs index it by arbitrary ring neighbours — what changes is
    // that it is sized ONCE and never cleared wholesale again.
    if (r.slot.size() != adjacency_.class_count()) {
        r.slot.assign(adjacency_.class_count(), kNoClass);
        r.items.clear();
    } else {
        for (WorkItemId item : r.items) {
            const std::uint32_t c = item.as_weld_class();
            if (c < r.slot.size()) r.slot[c] = kNoClass;
        }
    }
    // Resolved ONCE, before the two region shapes divide, because both of them
    // read the caller's seed and a seed rejected on one path must be rejected
    // on the other. `kNoClass` from here means "no usable seed was given",
    // which is the state each branch below already knows how to handle.
    const std::uint32_t given_seed = accepted_seed(settings);
    StageTimer seed_timer(stages_, SculptStage::SeedResolve);
    if (settings.geodesic) {
        // Seeded from the index when there is one. `geodesic_region` scans
        // every class for a seed when it is given none, which put an O(mesh)
        // term on the DEFAULT path — `seed_class` was the only way out and few
        // callers have one.
        //
        // Without an index the seed is left unset, so that scan still happens
        // — inside `geodesic_region`, exactly as before. Doing it here instead
        // measured 1.30 -> 1.98 ms at a million classes, and the cause was one
        // extra branch per iteration in a differently-shaped copy of the same
        // loop. Two copies of a hot scan is a defect whichever is faster.
        std::uint32_t seed = given_seed;
        // Resolved HERE only when there is an index to resolve it with — a ray
        // tree, or the chunk tree a multires level supplies. With neither, the
        // seed is deliberately left unset and `geodesic_region` finds it by the
        // scan it already contains: doing that scan here instead measured
        // 1.30 -> 1.98 ms at a million classes, because it is one extra branch
        // per iteration in a differently-shaped copy of the same loop. Two
        // copies of a hot scan is a defect whichever is faster.
        if (seed >= adjacency_.class_count() &&
            (surface_index() != nullptr || chunk_index() != nullptr))
            seed = nearest_class(settings.center);
        automask_seed_ = seed < adjacency_.class_count() ? seed : kNoClass;
        seed_timer.stop();
        StageTimer query_timer(stages_, SculptStage::SpatialQuery);
        geodesic_region(mesh_, adjacency_, settings.center, settings.radius, walk_, &candidates_,
                        &distance_, seed);
    } else {
        // The euclidean region IS a ball query, and used to be written as a
        // scan over every class. Flatten and Scrape are the two verbs that take
        // this path (`default_geodesic`), and they are the two that most want a
        // large radius.
        // The connectivity automask needs an anchor and a ball query has no
        // walk to have chosen one, so it is resolved here — and ONLY when a
        // factor actually wants it, because it costs a query.
        automask_seed_ = kNoClass;
        if (has_factor(settings.automask.factors, AutomaskFactor::TopologyConnected)) {
            automask_seed_ =
                given_seed < adjacency_.class_count() ? given_seed : nearest_class(settings.center);
        }
        seed_timer.stop();
        StageTimer query_timer(stages_, SculptStage::SpatialQuery);
        if (classes_in_ball(settings.center, settings.radius, &candidates_)) {
            // SORTED, so the region is the same list whether it came from the
            // tree or from the scan below — and so it does not depend on the
            // tree's shape, which a rebuild changes. The verbs accumulate a
            // weighted normal and a plane over this list, and float addition is
            // not associative, so an order that varied would make a brush's
            // result depend on whether the host happened to have picked.
            std::sort(candidates_.begin(), candidates_.end());
            distance_.resize(candidates_.size());
            for (std::size_t i = 0; i < candidates_.size(); ++i)
                distance_[i] = kernel::clength(class_position(candidates_[i]) - settings.center);
        } else {
            euclidean_region(mesh_, adjacency_, settings.center, settings.radius, &candidates_,
                             &distance_);
        }
    }

    // The walk speaks weld classes; the workset speaks `WorkItemId`. The
    // positions are lifted here rather than inside the weight loop because the
    // neutral composition takes a candidate's position as given — it is the one
    // thing every representation can hand over without being asked a question.
    r.items.resize(candidates_.size());
    r.positions.resize(candidates_.size());
    for (std::size_t i = 0; i < candidates_.size(); ++i) {
        const std::uint32_t c = candidates_[i];
        std::size_t mc = 0;
        r.items[i] = WorkItemId::weld_class(c);
        r.positions[i] = mesh_.positions[adjacency_.members(c, &mc)[0]];
    }
}

void MeshSculptor::gather(const MeshBrushSettings& settings, const field::MaskGate& gate) {
    // Every transient this stamp asks the arena for is dead when the stamp
    // ends, so the arena starts each one at zero and keeps its storage.
    arena_.reset();
    build_fixed_mesh_workset(settings);

    // The alpha's frame, once for the whole stamp rather than per vertex. Its
    // direction defaults to the brush's own averaged normal — which is not yet
    // computed here, so an unset direction uses the mesh normal nearest the
    // centre. Good enough, and cheaper than a second pass: the stamp is a disc
    // on a surface, and its normal barely varies across one brush radius.
    AlphaFrame alpha_frame;
    {
        StageTimer alpha_timer(stages_, SculptStage::Alpha);
    if (settings.has_alpha()) {
        // The fallback is resolved LAZILY, only when the caller supplied no
        // direction: it costs a nearest-class query, and a caller that named a
        // direction must not pay for one.
        kernel::cfloat3 fallback = kernel::cf3(0, 1, 0);
        if (kernel::clength(settings.alpha_direction) < 1e-9f) {
            const std::uint32_t near = nearest_class(settings.center);
            if (near != kNoClass) fallback = class_normal(mesh_, adjacency_, near);
        }
        alpha_frame = alpha_frame_for(settings, fallback);
    }
    }

    const MeshWorkItemTopology topology(mesh_, adjacency_, region_);
    const WorkItemId seed_item = WorkItemId::weld_class(automask_seed_);

    WorkComposeInputs in;
    in.settings = &settings;
    in.gate = &gate;
    in.alpha = &alpha_frame;
    in.path_distance = distance_.data();
    in.geodesic = settings.geodesic;
    in.taper_start = kPathTaperStart;
    in.path_budget = kDefaultPathBudget;
    in.topology = &topology;
    in.automask_inputs = &automask_inputs_;
    // Resolved ONLY when a factor wants it: the reference costs a nearest-class
    // query on a brush that named no deposit direction, and a stamp with no
    // automask must not pay for one.
    if (settings.automask.any()) in.automask_reference = automask_reference(settings);
    if (automask_seed_ != kNoClass) in.automask_seed = &seed_item;
    in.reader.normal_at = &MeshSculptor::normal_of_item;
    in.reader.context = this;

    in.stages = stages_;
    compose_workset(in, arena_, &region_);

    // The workset's peak, published once the region is FINAL -- after the
    // falloff and the automask have dropped what they drop. Reporting the
    // pre-compaction count would make the number a property of the brush radius
    // rather than of the memory a stamp actually stands on, which is what a
    // host tunes a profile against.
    //
    // Everything above this line used to be spelled out here: the automask's
    // second pass, the compaction it drives, and the plane. `compose_workset`
    // owns all of it now (add-shared-brush-runtime), which is why this branch
    // keeps only the measurement -- the thing being measured moved, the
    // measurement did not.
    if (telemetry_ != nullptr) telemetry_->observe_workset(region_.size());

}

// THE PLAN IS COMPILED ONCE PER STROKE, not per stamp.
//
// Nothing in it depends on where the stamp landed or how hard: the verb decides
// the kernel, the kernel decides what the gather owes it, and only
// `smooth_iterations` and the geodesic override come from the settings. So the
// key is those three, and a stroke of two hundred stamps with an unchanged
// brush compiles exactly once.
const BrushRuntimePlan& MeshSculptor::plan_for(MeshBrush verb,
                                               const MeshBrushSettings& settings) {
    if (!plan_valid_ || plan_verb_ != verb || plan_iterations_ != settings.smooth_iterations ||
        plan_geodesic_ != settings.geodesic) {
        plan_ = compile_plan(model_of(verb), settings);
        plan_verb_ = verb;
        plan_iterations_ = settings.smooth_iterations;
        plan_geodesic_ = settings.geodesic;
        plan_valid_ = true;
        ++plan_compilations_;
    }
    return plan_;
}

std::size_t MeshSculptor::stamp(MeshBrush verb, const MeshBrushSettings& settings,
                                const field::MaskGate& gate, VertexDeltas* record) {
    if (!valid() || settings.radius <= 0.0f || mesh_.positions.empty()) return 0;
    const BrushRuntimePlan& plan = plan_for(verb, settings);
    gather(settings, gate);
    if (region_.empty()) return 0;

    if (plan.needs_neighbors)
        build_neighbors(plan.needs_neighbor_normals, plan.needs_neighbor_colors);

    StageTimer snapshot_timer(stages_, SculptStage::Snapshot);
    const SculptSnapshot snapshot = snapshot_of();
    snapshot_timer.stop();
    StageTimer neighbor_timer(stages_, SculptStage::NeighborBuild);
    const SculptNeighbors neighbors = plan.needs_neighbors ? neighbors_of() : SculptNeighbors{};
    neighbor_timer.stop();

    // The colour verbs take a different path end to end: they fill a colour
    // target rather than a displacement, and they write through write_colors.
    // Sharing `displacement_` would have made "moved nothing" and "painted
    // nothing" the same number.
    if (plan.model.target == BrushWriteTarget::Color)
        return stamp_color(verb, settings, snapshot, neighbors, record);

    displacement_.assign(region_.size(), kernel::cf3(0, 0, 0));
    kernel::cfloat3* out = displacement_.data();

    StageTimer kernel_timer(stages_, SculptStage::Kernel);
    switch (verb) {
        case MeshBrush::Grab:
        case MeshBrush::Snakehook:
            // One stamp of snakehook IS a grab. What makes it a snakehook is
            // the RE-ANCHORING between stamps, which brush::apply_to_mesh does
            // by walking the brush centre along the drag.
            kernel_grab(snapshot, settings, out);
            break;
        case MeshBrush::Draw:
            kernel_draw(snapshot, settings, out);
            break;
        case MeshBrush::Inflate:
            kernel_inflate(snapshot, settings, out);
            break;
        case MeshBrush::Pinch:
            kernel_pinch(snapshot, settings, out);
            break;
        case MeshBrush::Flatten:
            kernel_flatten(snapshot, settings, out);
            break;
        case MeshBrush::Clay:
            kernel_clay(snapshot, settings, out);
            break;
        case MeshBrush::Crease:
            kernel_crease(snapshot, settings, out);
            break;
        case MeshBrush::Smooth:
        case MeshBrush::Polish:
        case MeshBrush::Scrape:
            kernel_smooth_family(verb, snapshot, neighbors, settings, scratch_, out);
            break;
        case MeshBrush::Nudge:
            kernel_nudge(snapshot, settings, out);
            break;
        case MeshBrush::Relax:
            kernel_relax(snapshot, neighbors, settings, scratch_, out);
            break;
        case MeshBrush::Layer:
            // Without a record there is no stroke to measure from, and every
            // stamp would clamp against the CURRENT surface — which is draw
            // wearing layer's name. A verb that silently becomes a different
            // verb is worse than one that refuses. The plan is what says this
            // kernel needs the gesture's origin.
            if (plan.needs_stroke_origin && !record) return 0;
            gather_stroke_origin(*record);
            kernel_layer(snapshot, settings, origin_.data(), out);
            break;
        case MeshBrush::Paint:
        case MeshBrush::Smear:
            // Returned above, through write_colors. Named here rather than
            // left to a default so that adding a verb still fails the switch.
            break;
    }
    kernel_timer.stop();
    return write(record);
}

// The colour half of `stamp`, lifted out so the displacement switch reads as
// one thing. It shares the gather, the snapshot and the neighbours with the
// verbs above and diverges only in where it writes.
std::size_t MeshSculptor::stamp_color(MeshBrush verb, const MeshBrushSettings& settings,
                                      const SculptSnapshot& snapshot,
                                      const SculptNeighbors& neighbors, VertexDeltas* record) {
    if (!has_colors()) return 0;  // an explicit ensure_colors is the fix
    color_target_.resize(region_.size());
    for (std::size_t i = 0; i < region_.size(); ++i) {
        std::size_t mc = 0;
        const std::uint32_t v =
            adjacency_.members(region_.items[i].as_weld_class(), &mc)[0];
        color_target_[i] = mesh_.colors[v];
    }
    // A copy, because both kernels read every entry's PRE-STAMP colour while
    // writing the same array: a smear that read what it had already written
    // would be a sweep whose result depends on the order the entries sit in.
    //
    // A MEMBER rather than a local, which is the allocation gate rather than a
    // style choice: a local vector here allocated and freed on every dab of
    // every colour stroke, which is exactly the "an ordinary local stamp
    // performs no heap allocation" rule this change is asserting.
    color_current_.assign(color_target_.begin(), color_target_.end());
    if (verb == MeshBrush::Paint)
        kernel_paint(snapshot, settings, color_current_.data(), color_target_.data());
    else
        kernel_smear(snapshot, neighbors, settings, color_current_.data(), color_target_.data());
    return write_colors(record);
}

// The snapshot the kernels read: the region, in the neutral shape. No copy —
// the region already holds these arrays contiguously, which is why it was
// worth keeping them parallel rather than as a vector of structs.
SculptSnapshot MeshSculptor::snapshot_of() const {
    SculptSnapshot s;
    s.positions = region_.positions.data();
    s.normals = region_.normals.data();
    s.weights = region_.weights.data();
    s.count = region_.size();
    s.average_normal = region_.average_normal;
    s.centroid = region_.centroid;
    s.plane_point = region_.plane_point;
    s.plane_normal = region_.plane_normal;
    return s;
}

SculptNeighbors MeshSculptor::neighbors_of() const {
    SculptNeighbors n;
    n.offsets = nb_offsets_.data();
    n.slots = nb_slots_.data();
    n.positions = nb_positions_.data();
    n.normals = nb_normals_.empty() ? nullptr : nb_normals_.data();
    n.colors = nb_colors_.empty() ? nullptr : nb_colors_.data();
    return n;
}

// Flatten the region's one-rings into the CSR the kernels read.
//
// The walk is the one the verbs used to do inline, done once instead of once
// per verb and once per smoothing pass. `r.slot` already answers "is this
// neighbour under the brush", and `kNoClass` and `kOutsideRegion` are the same
// value on purpose — the static_assert below is what keeps them that way, since
// a region slot is copied straight into the neighbour list.
void MeshSculptor::build_neighbors(bool want_normals, bool want_colors) {
    static_assert(kNoClass == kOutsideRegion,
                  "a region slot is copied into the neighbour list unchanged");
    const BrushRegion& r = region_;
    // Cleared keeping capacity: a stroke of similar stamps allocates on its
    // first one and never again.
    nb_offsets_.clear();
    nb_slots_.clear();
    nb_positions_.clear();
    nb_normals_.clear();
    nb_colors_.clear();
    nb_offsets_.push_back(0);
    const bool colors = want_colors && has_colors();
    for (std::size_t i = 0; i < r.size(); ++i) {
        std::size_t n = 0;
        const std::uint32_t* ring = adjacency_.ring(r.items[i].as_weld_class(), &n);
        for (std::size_t k = 0; k < n; ++k) {
            const std::uint32_t nc = ring[k];
            nb_slots_.push_back(nc < r.slot.size() ? r.slot[nc] : kOutsideRegion);
            std::size_t mc = 0;
            const std::uint32_t nv = adjacency_.members(nc, &mc)[0];
            nb_positions_.push_back(mesh_.positions[nv]);
            if (colors) nb_colors_.push_back(mesh_.colors[nv]);
            if (want_normals) nb_normals_.push_back(class_normal(mesh_, adjacency_, nc));
        }
        nb_offsets_.push_back(static_cast<std::uint32_t>(nb_slots_.size()));
    }
}
void MeshSculptor::gather_stroke_origin(const VertexDeltas& record) {
    origin_.resize(region_.size());
    for (std::size_t i = 0; i < region_.size(); ++i) {
        std::size_t members = 0;
        const std::uint32_t v =
            adjacency_.members(region_.items[i].as_weld_class(), &members)[0];
        const std::optional<kernel::cfloat3> seen = record.origin_of(v);
        // Not yet touched by this stroke: it starts here.
        origin_[i] = seen ? *seen : region_.positions[i];
    }
}

bool MeshSculptor::has_colors() const {
    return mesh_.colors.size() == mesh_.positions.size() && !mesh_.positions.empty();
}

bool MeshSculptor::ensure_colors(kernel::cfloat3 fill) {
    if (has_colors()) return false;
    mesh_.colors.assign(mesh_.positions.size(), fill);
    return true;
}

// The colour counterpart of `write`. No normals to recompute and no ring to
// mark: a colour is a per-vertex value that changes nothing about the surface,
// which is why this is twenty lines and `write` is fifty.
std::size_t MeshSculptor::write_colors(VertexDeltas* record) {
    region_.write_region.clear();
    region_.write_bounds = math::Aabb{};
    std::size_t painted = 0;
    for (std::size_t i = 0; i < region_.size(); ++i) {
        const std::uint32_t c = region_.items[i].as_weld_class();
        std::size_t mc = 0;
        const std::uint32_t* members = adjacency_.members(c, &mc);
        // Unchanged means unwritten, so a zero-weight rim costs nothing and a
        // record does not grow entries whose before and after are equal.
        if (is_zero(color_target_[i] - mesh_.colors[members[0]])) continue;
        ++painted;
        region_.write_region.push_back(region_.items[i]);
        region_.write_bounds.expand(region_.positions[i]);
        for (std::size_t k = 0; k < mc; ++k) {
            if (record) record->note(members[k], mesh_);
            mesh_.colors[members[k]] = color_target_[i];
            if (record) record->sync_after(members[k], mesh_);
        }
    }
    // Attributes only: a colour verb comes back with `positions` and `normals`
    // byte-identical, which is the contract `paint` and `smear` are the two
    // verbs holding, so telling a host to re-upload geometry here would be
    // telling it to re-upload what it already has.
    if (painted != 0) publish_chunks(/*normals_changed=*/false, /*attributes_changed=*/true);
    return painted;
}

std::size_t MeshSculptor::write(VertexDeltas* record) {
    StageTimer write_timer(stages_, SculptStage::Writeback);
    // Retire the marks the LAST write set, which `pending_normals_` names
    // exactly, rather than clearing an array the size of the mesh. Same rule as
    // `gather`'s slot reset above.
    if (normal_mark_.size() != adjacency_.class_count()) {
        normal_mark_.assign(adjacency_.class_count(), 0);
    } else {
        for (std::uint32_t c : pending_normals_)
            if (c < normal_mark_.size()) normal_mark_[c] = 0;
    }
    pending_normals_.clear();
    // THE WRITE REGION, not the workset. The rim of a falloff, a fully masked
    // vertex and a verb that declined all leave entries in `classes` that never
    // move, and a host told about those would upload geometry that did not
    // change. Smooth, Relax and Polish additionally READ a ring they do not
    // write, and that ring is not here either.
    region_.write_region.clear();
    region_.write_bounds = math::Aabb{};
    std::size_t moved = 0;
    for (std::size_t i = 0; i < region_.size(); ++i) {
        if (is_zero(displacement_[i])) continue;
        ++moved;
        const std::uint32_t c = region_.items[i].as_weld_class();
        mark_bvh_dirty(c);  // for the next refit_bvh, which drains the whole set
        const kernel::cfloat3 target = region_.positions[i] + displacement_[i];
        region_.write_region.push_back(region_.items[i]);
        region_.write_bounds.expand(region_.positions[i]);
        region_.write_bounds.expand(target);
        std::size_t mc = 0;
        const std::uint32_t* members = adjacency_.members(c, &mc);
        for (std::size_t k = 0; k < mc; ++k) {
            if (record) record->note(members[k], mesh_);
            mesh_.positions[members[k]] = target;
            if (record) record->sync_after(members[k], mesh_);
        }
        // A triangle's normal changes when ANY of its corners moves, so the
        // moved class's whole ring needs recomputing, not just the class.
        if (!normal_mark_[c]) {
            normal_mark_[c] = 1;
            pending_normals_.push_back(c);
        }
        std::size_t rc = 0;
        const std::uint32_t* ring = adjacency_.ring(c, &rc);
        for (std::size_t k = 0; k < rc; ++k)
            if (!normal_mark_[ring[k]]) {
                normal_mark_[ring[k]] = 1;
                pending_normals_.push_back(ring[k]);
            }
    }
    if (moved == 0) return 0;
    write_timer.stop();
    {
        StageTimer normal_timer(stages_, SculptStage::NormalRefresh);
        if (defer_normals_) {
            deferred_normals_.insert(deferred_normals_.end(), pending_normals_.begin(),
                                     pending_normals_.end());
        } else {
            recompute_normals(pending_normals_, record);
        }
    }
    StageTimer chunk_timer(stages_, SculptStage::ChunkMark);
    // The dirty stream, computed here rather than by every host. Normals are
    // marked only where they were actually recomputed: a DEFERRED flush marks
    // them when it runs, which is what makes "positions now, normals at the end
    // of the stroke" two uploads rather than one wrong one.
    publish_chunks(/*normals_changed=*/!defer_normals_, /*attributes_changed=*/false);
    return moved;
}

void MeshSculptor::flush_normals(VertexDeltas* record) {
    if (deferred_normals_.empty()) return;
    std::sort(deferred_normals_.begin(), deferred_normals_.end());
    deferred_normals_.erase(std::unique(deferred_normals_.begin(), deferred_normals_.end()),
                            deferred_normals_.end());
    recompute_normals(deferred_normals_, record);
    // The chunks the flush rewrote normals in, which is the union over every
    // deferred stamp rather than the last one's write region. Marked through
    // the same path a stamp uses, so a host draining the stream cannot tell a
    // deferred flush from an immediate one except by when it arrived.
    if (chunks_ != nullptr) {
        if (chunk_of_.size() != mesh_.positions.size()) rebuild_chunk_of();
        if (dirty_mark_.size() != chunks_->slot_count())
            dirty_mark_.assign(chunks_->slot_count(), 0);
        std::vector<std::uint32_t>& hit = dirty_chunks_;
        hit.clear();
        for (std::uint32_t c : deferred_normals_) {
            if (c >= adjacency_.class_count()) continue;
            std::size_t n = 0;
            const std::uint32_t* members = adjacency_.members(c, &n);
            for (std::size_t i = 0; i < n; ++i) {
                const std::uint32_t v = members[i];
                if (v >= chunk_of_.size()) continue;
                const std::uint32_t id = chunk_of_[v];
                if (id == ChunkTable::kNoChunk || id >= dirty_mark_.size() || dirty_mark_[id])
                    continue;
                dirty_mark_[id] = 1;
                hit.push_back(id);
            }
        }
        for (std::uint32_t id : hit) {
            dirty_mark_[id] = 0;
            chunks_->mark(id, ChunkDirty::Normals);
        }
    }
    deferred_normals_.clear();
}

std::size_t MeshSculptor::apply_lattice(const Lattice& cage, VertexDeltas* record) {
    // An untouched cage displaces nothing anywhere, so the walk is skipped
    // rather than run to write every vertex back to itself — which would also
    // fill an undo record with entries that changed nothing.
    if (cage.is_identity()) return 0;
    // A whole-mesh operation has no small dirty set to name, so the next refit
    // refits everything. Cheaper than a rebuild and correct, where a refit of
    // the last stamp's region would be neither.
    bvh_all_dirty_ = true;

    std::size_t moved = 0;
    // By WELD CLASS rather than by raw vertex, so that position-coincident
    // vertices — the split ones holding a hard edge — stay coincident. Walking
    // `positions` directly would give each copy its own evaluation; they agree
    // mathematically, but only up to float rounding, and a seam that opens by
    // an ulp is a visible crack.
    const std::uint32_t classes = static_cast<std::uint32_t>(adjacency_.class_count());
    std::vector<std::uint32_t> touched;
    touched.reserve(classes);
    for (std::uint32_t c = 0; c < classes; ++c) {
        std::size_t count = 0;
        const std::uint32_t* members = adjacency_.members(c, &count);
        if (count == 0) continue;
        const kernel::cfloat3 d = cage.displacement(mesh_.positions[members[0]]);
        if (d.x == 0.0f && d.y == 0.0f && d.z == 0.0f) continue;
        for (std::size_t k = 0; k < count; ++k) {
            if (record) record->note(members[k], mesh_);
            mesh_.positions[members[k]] = mesh_.positions[members[k]] + d;
            ++moved;
        }
        touched.push_back(c);
    }
    if (touched.empty()) return 0;

    // The cage moved vertices, so their normals are stale — and their
    // neighbours' are too, since a face normal is shared. Recomputing over the
    // touched classes is what every stamp does; deferring follows the same
    // switch, so a host draining a gesture pays once.
    if (defer_normals_) {
        deferred_normals_.insert(deferred_normals_.end(), touched.begin(), touched.end());
    } else {
        recompute_normals(touched, record);
    }
    if (record)
        for (std::uint32_t c : touched) {
            std::size_t count = 0;
            const std::uint32_t* members = adjacency_.members(c, &count);
            for (std::size_t k = 0; k < count; ++k) record->sync_after(members[k], mesh_);
        }
    return moved;
}

std::size_t MeshSculptor::apply_deformer(const MeshDeformSettings& settings,
                                        const field::MaskGate& gate, VertexDeltas* record) {
    // An identity deformer displaces nothing anywhere, so the walk is skipped
    // rather than run to write every vertex back to itself — which would also
    // fill an undo record with entries that changed nothing. Same rule as an
    // untouched lattice cage.
    if (settings.is_identity()) return 0;

    std::size_t moved = 0;
    // BY WELD CLASS, for apply_lattice's reason and with the same consequence:
    // position-coincident vertices holding a hard edge or a UV seam must stay
    // coincident, and evaluating each copy separately agrees only up to float
    // rounding — a seam that opens by an ulp is a visible crack.
    bvh_all_dirty_ = true;  // whole-mesh, as apply_lattice
    const std::uint32_t classes = static_cast<std::uint32_t>(adjacency_.class_count());
    std::vector<std::uint32_t> touched;
    touched.reserve(classes);
    for (std::uint32_t c = 0; c < classes; ++c) {
        std::size_t count = 0;
        const std::uint32_t* members = adjacency_.members(c, &count);
        if (count == 0) continue;
        const kernel::cfloat3 rest = mesh_.positions[members[0]];
        kernel::cfloat3 target = deform_point(settings, rest);

        // The gate holds part of the form still. It scales the DISPLACEMENT
        // rather than the parameters, so a half-gated vertex travels half way
        // — the rule every other verb follows — and a fully gated one is
        // bit-identical to where it started rather than a lerp that lands one
        // ulp away.
        if (gate) {
            const float g = std::clamp(gate(rest), 0.0f, 1.0f);
            if (g >= 1.0f) continue;
            if (g > 0.0f) target = rest + (target - rest) * (1.0f - g);
        }
        if (is_zero(target - rest)) continue;

        for (std::size_t k = 0; k < count; ++k) {
            if (record) record->note(members[k], mesh_);
            mesh_.positions[members[k]] = target;
            ++moved;
        }
        touched.push_back(c);
    }
    if (touched.empty()) return 0;

    if (defer_normals_) {
        deferred_normals_.insert(deferred_normals_.end(), touched.begin(), touched.end());
    } else {
        recompute_normals(touched, record);
    }
    if (record)
        for (std::uint32_t c : touched) {
            std::size_t count = 0;
            const std::uint32_t* members = adjacency_.members(c, &count);
            for (std::size_t k = 0; k < count; ++k) record->sync_after(members[k], mesh_);
        }
    return moved;
}

void MeshSculptor::recompute_normals(const std::vector<std::uint32_t>& classes,
                                     VertexDeltas* record) {
    if (mesh_.normals.size() != mesh_.positions.size() || mesh_.normals.empty()) return;
    for (std::uint32_t c : classes) {
        std::size_t mc = 0;
        const std::uint32_t* members = adjacency_.members(c, &mc);
        for (std::size_t k = 0; k < mc; ++k) {
            if (record) record->note(members[k], mesh_);
            mesh_.normals[members[k]] = kernel::cf3(0, 0, 0);
        }
    }
    for (std::uint32_t c : classes) {
        std::size_t tc = 0;
        const std::uint32_t* tris = adjacency_.triangles_of(c, &tc);
        for (std::size_t k = 0; k < tc; ++k) {
            // Angle-weighted, for the reason class_normal gives: an
            // area-weighted normal on a lattice-derived mesh tracks the
            // lattice rather than the surface.
            const kernel::cfloat3 fn =
                safe_normalize(face_normal(mesh_, tris[k]), kernel::cf3(0, 0, 0));
            for (int corner = 0; corner < 3; ++corner) {
                const std::uint32_t v = mesh_.indices[tris[k] * 3 + corner];
                // Only this class's own corners: a split vertex keeps its own
                // faces, which is what preserves a hard edge's shading.
                if (adjacency_.class_of(v) == c)
                    mesh_.normals[v] = mesh_.normals[v] + fn * corner_angle(mesh_, tris[k], corner);
            }
        }
    }
    for (std::uint32_t c : classes) {
        std::size_t mc = 0;
        const std::uint32_t* members = adjacency_.members(c, &mc);
        for (std::size_t k = 0; k < mc; ++k) {
            mesh_.normals[members[k]] =
                safe_normalize(mesh_.normals[members[k]], kernel::cf3(0, 1, 0));
            if (record) record->sync_after(members[k], mesh_);
        }
    }
}

// -- VertexDeltas encoding (survive-a-crash) ---------------------------------
//
// Layout, little-endian throughout:
//
//   u32 magic 'CVDL'   u16 version   u8 has_normals   u8 has_colors
//   u32 count
//   u32 vertices[count]
//   f32 before_position[count][3]   f32 after_position[count][3]
//   f32 before_normal[count][3]     f32 after_normal[count][3]     (if has_normals)
//   f32 before_color[count][3]      f32 after_color[count][3]      (if has_colors)
//
// Fixed-width and positional rather than tagged: this is a crash artifact
// paired with one snapshot, not a document, so it needs to be cheap to write on
// every step rather than forgiving to read years later. The version is there so
// a build that does not understand it REFUSES, which is the whole point — a
// recovery that silently drops what it could not read is the failure the
// feature exists to prevent.
namespace {

constexpr std::uint32_t kDeltaMagic = 0x4C445643u;  // 'CVDL'
constexpr std::uint16_t kDeltaVersion = 1;

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 24));
}

void put_f32(std::vector<std::uint8_t>& out, float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    put_u32(out, bits);
}

void put_vec3(std::vector<std::uint8_t>& out, const std::vector<kernel::cfloat3>& v) {
    for (const kernel::cfloat3& p : v) {
        put_f32(out, p.x);
        put_f32(out, p.y);
        put_f32(out, p.z);
    }
}

struct Reader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t at = 0;
    bool ok = true;

    std::uint32_t u32() {
        if (at + 4 > size) {
            ok = false;
            return 0;
        }
        const std::uint32_t v = static_cast<std::uint32_t>(data[at]) |
                                (static_cast<std::uint32_t>(data[at + 1]) << 8) |
                                (static_cast<std::uint32_t>(data[at + 2]) << 16) |
                                (static_cast<std::uint32_t>(data[at + 3]) << 24);
        at += 4;
        return v;
    }
    std::uint8_t u8() {
        if (at + 1 > size) {
            ok = false;
            return 0;
        }
        return data[at++];
    }
    float f32() {
        const std::uint32_t bits = u32();
        float f = 0.0f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    void vec3(std::vector<kernel::cfloat3>& out, std::size_t count) {
        out.resize(count);
        for (std::size_t i = 0; i < count && ok; ++i) {
            const float x = f32();
            const float y = f32();
            const float z = f32();
            out[i] = kernel::cf3(x, y, z);
        }
    }
};

}  // namespace

std::size_t VertexDeltas::bytes() const {
    std::size_t n = sizeof(VertexDeltas);
    n += vertices_.capacity() * sizeof(std::uint32_t);
    const std::size_t vec3 = sizeof(kernel::cfloat3);
    n += (before_position_.capacity() + after_position_.capacity()) * vec3;
    n += (before_normal_.capacity() + after_normal_.capacity()) * vec3;
    n += (before_color_.capacity() + after_color_.capacity()) * vec3;
    // The slot index is rebuildable but it is really allocated, so a budget
    // that ignored it would under-report a record following many vertices.
    n += slot_.size() * (sizeof(std::uint32_t) * 2 + sizeof(void*));
    return n;
}

std::vector<std::uint8_t> VertexDeltas::encode() const {
    std::vector<std::uint8_t> out;
    const std::size_t n = vertices_.size();
    out.reserve(16 + n * (4 + 24 + (normals_ ? 24u : 0u) + (colors_ ? 24u : 0u)));
    put_u32(out, kDeltaMagic);
    out.push_back(static_cast<std::uint8_t>(kDeltaVersion));
    out.push_back(static_cast<std::uint8_t>(kDeltaVersion >> 8));
    out.push_back(normals_ ? 1 : 0);
    out.push_back(colors_ ? 1 : 0);
    put_u32(out, static_cast<std::uint32_t>(n));
    for (std::uint32_t v : vertices_) put_u32(out, v);
    put_vec3(out, before_position_);
    put_vec3(out, after_position_);
    if (normals_) {
        put_vec3(out, before_normal_);
        put_vec3(out, after_normal_);
    }
    if (colors_) {
        put_vec3(out, before_color_);
        put_vec3(out, after_color_);
    }
    return out;
}

bool VertexDeltas::decode(const std::uint8_t* data, std::size_t size, VertexDeltas* out) {
    if (!data || !out) return false;
    Reader r{data, size};
    if (r.u32() != kDeltaMagic) return false;
    const std::uint16_t version =
        static_cast<std::uint16_t>(r.u8() | (static_cast<std::uint16_t>(r.u8()) << 8));
    // Refused rather than reinterpreted. A newer layout read as this one would
    // revert a mesh to values that were never in it.
    if (!r.ok || version != kDeltaVersion) return false;
    const bool has_normals = r.u8() != 0;
    const bool has_colors = r.u8() != 0;
    const std::uint32_t count = r.u32();
    if (!r.ok) return false;
    // A count larger than the buffer could hold is a malformed or hostile
    // record, and sizing from it is how a reader gets asked to allocate a
    // gigabyte. The smallest a vertex can be is its id plus two positions.
    const std::size_t least_per_vertex = 4 + 24;
    if (static_cast<std::size_t>(count) > (size - r.at) / least_per_vertex + 1) return false;

    VertexDeltas built;
    built.normals_ = has_normals;
    built.colors_ = has_colors;
    built.vertices_.resize(count);
    for (std::uint32_t i = 0; i < count && r.ok; ++i) built.vertices_[i] = r.u32();
    r.vec3(built.before_position_, count);
    r.vec3(built.after_position_, count);
    if (has_normals) {
        r.vec3(built.before_normal_, count);
        r.vec3(built.after_normal_, count);
    }
    if (has_colors) {
        r.vec3(built.before_color_, count);
        r.vec3(built.after_color_, count);
    }
    if (!r.ok) return false;
    // The slot index is rebuilt rather than stored: it is derivable, and a
    // stored hash map would be a second source of truth for the same thing.
    built.slot_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) built.slot_.emplace(built.vertices_[i], i);
    *out = std::move(built);
    return true;
}

}  // namespace mesh
}  // namespace clay
