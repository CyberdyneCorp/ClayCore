#include "clay/mesh/sculpt.h"

#include <algorithm>
#include <cmath>

#include "clay/kernel/deform.h"  // calpha_sample, calpha_frame

namespace clay {
namespace mesh {
namespace {

// Deliberately identical to voxel::falloff_weight, curve for curve and
// constant for constant, so a brush behaves the same on both representations.
// The duplication is the module layering; see the header.
float falloff_weight(MeshFalloff curve, float d) {
    d = std::clamp(d, 0.0f, 1.0f);
    switch (curve) {
        case MeshFalloff::Linear:
            return 1.0f - d;
        case MeshFalloff::Smooth: {
            const float t = 1.0f - d;
            return t * t * (3.0f - 2.0f * t);
        }
        case MeshFalloff::Gaussian:
            return std::exp(-4.5f * d * d);
        case MeshFalloff::Constant:
        default:
            return 1.0f;
    }
}

bool is_zero(kernel::cfloat3 v) { return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f; }

kernel::cfloat3 safe_normalize(kernel::cfloat3 v, kernel::cfloat3 fallback) {
    const float len = kernel::clength(v);
    return len > 1e-20f ? v / len : fallback;
}

// The part of `v` that lies in the surface: what makes a pinch gather ALONG
// the surface instead of sinking the region into it.
// The stamp's frame for an alpha, derived once per gather. `calpha_frame` is
// the kernel's, so a mesh stamp and an SDF one orient the same samples the same
// way rather than through two constructions that could drift.
struct AlphaFrame {
    kernel::cfloat3 centre = kernel::cf3(0, 0, 0);
    kernel::cfloat3 tangent = kernel::cf3(1, 0, 0);
    kernel::cfloat3 binormal = kernel::cf3(0, 1, 0);
    float extent = 1.0f;
};

// The alpha's value at a world point, or 1 where there is no alpha — so the
// caller multiplies unconditionally and an absent stamp is exactly today's
// weight rather than a branch per vertex.
float alpha_at(const MeshBrushSettings& settings, const AlphaFrame& f, kernel::cfloat3 p) {
    if (!settings.has_alpha()) return 1.0f;
    const kernel::cfloat3 rel = p - f.centre;
    const float u = kernel::cdot(rel, f.tangent) / f.extent + 0.5f;
    const float v = kernel::cdot(rel, f.binormal) / f.extent + 0.5f;
    return kernel::calpha_sample(settings.alpha, settings.alpha_width, settings.alpha_height, u, v);
}

kernel::cfloat3 tangential(kernel::cfloat3 v, kernel::cfloat3 n) {
    return v - n * kernel::cdot(v, n);
}

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

// How far the surface around a class BENDS, in radians: the MEAN angle between
// its normal and a NEIGHBOURING CLASS's normal. Polish's gate.
//
// Two choices here, both made against the surface polish is actually for — a
// noisy flat beside a hard edge — and both wrong in the obvious reading.
//
// NEIGHBOURING CLASSES rather than incident FACES. "Dihedral angle" says
// faces, and on a noisy surface the faces are useless: noise tilts each one by
// more than a chamfer bends the whole surface, so every flat looks like an edge
// and polish declines to do anything at all. A class normal is already an
// angle-weighted average over its one-ring of faces, so most of the noise is out
// of it and what is left is the shape — which is what the gate means to read.
//
// The MEAN rather than the widest. The widest is one sample out of six and
// carries the residual noise straight back in; the mean is the statistic that
// says "how much does the surface bend here", which is the question.
float ring_disagreement(const Mesh& m, const Adjacency& adj, std::uint32_t cls, kernel::cfloat3 n) {
    std::size_t count = 0;
    const std::uint32_t* ring = adj.ring(cls, &count);
    if (count == 0) return 0.0f;
    float total = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        const kernel::cfloat3 nb = class_normal(m, adj, ring[i]);
        total += std::acos(std::clamp(kernel::cdot(nb, n), -1.0f, 1.0f));
    }
    return total / static_cast<float>(count);
}

// Everything one stamp reads. Bundled so the verbs below are free functions
// with one argument rather than members with six.
struct StampContext {
    const Mesh& mesh;
    const Adjacency& adj;
    const BrushRegion& region;
    const MeshBrushSettings& settings;

    kernel::cfloat3 deposit_direction() const {
        return is_zero(settings.deposit_normal)
                   ? region.average_normal
                   : safe_normalize(settings.deposit_normal, region.average_normal);
    }
};

// One Laplacian pass: the one-ring mean, read from `current` where the
// neighbour is inside the region and from the mesh where it is not. Reading a
// snapshot rather than the buffer being written is what makes this a
// simultaneous average instead of a Gauss-Seidel sweep whose result depends on
// vertex order.
void laplacian_pass(const StampContext& ctx, const std::vector<kernel::cfloat3>& current,
                    std::vector<kernel::cfloat3>* out) {
    const BrushRegion& r = ctx.region;
    for (std::size_t i = 0; i < r.classes.size(); ++i) {
        std::size_t n = 0;
        const std::uint32_t* ring = ctx.adj.ring(r.classes[i], &n);
        if (n == 0) {
            (*out)[i] = current[i];
            continue;
        }
        kernel::cfloat3 sum = kernel::cf3(0, 0, 0);
        for (std::size_t k = 0; k < n; ++k) {
            const std::uint32_t slot = r.slot[ring[k]];
            if (slot != kNoClass) {
                sum = sum + current[slot];
            } else {
                std::size_t mc = 0;
                sum = sum + ctx.mesh.positions[ctx.adj.members(ring[k], &mc)[0]];
            }
        }
        (*out)[i] = sum / static_cast<float>(n);
    }
}

// The Laplacian target for every class in the region, `iterations` passes deep.
void smooth_targets(const StampContext& ctx, int iterations, std::vector<kernel::cfloat3>* result,
                    std::vector<kernel::cfloat3>* scratch) {
    *result = ctx.region.positions;
    scratch->resize(result->size());
    const int passes = std::clamp(iterations, 1, kMaxSmoothIterations);
    for (int it = 0; it < passes; ++it) {
        laplacian_pass(ctx, *result, scratch);
        result->swap(*scratch);
    }
}

// The move onto a plane, clamped by the mode. CutOnly removes material above
// the plane and leaves the hollows below it — which is what makes a crisp
// facet against untouched surface, and is the whole of Trim Dynamic and
// hPolish. FillOnly is the dual.
kernel::cfloat3 plane_offset(kernel::cfloat3 p, kernel::cfloat3 point, kernel::cfloat3 normal,
                             field::FlattenMode mode) {
    const float d = kernel::cdot(p - point, normal);
    if (mode == field::FlattenMode::CutOnly && d <= 0.0f) return kernel::cf3(0, 0, 0);
    if (mode == field::FlattenMode::FillOnly && d >= 0.0f) return kernel::cf3(0, 0, 0);
    return normal * -d;
}

}  // namespace

bool writes_color(MeshBrush verb) {
    return verb == MeshBrush::Paint || verb == MeshBrush::Smear;
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

MeshSculptor::MeshSculptor(Mesh& m, float weld_epsilon)
    : mesh_(m), adjacency_(Adjacency::build(m, weld_epsilon)) {}

MeshSculptor::MeshSculptor(Mesh& m, Adjacency adjacency)
    : mesh_(m), adjacency_(std::move(adjacency)) {}

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
    if (!tree) return false;  // no index; the caller scans
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
        // No ray tree to ask, so the scan this replaced is still the answer.
        const std::uint32_t classes = static_cast<std::uint32_t>(adjacency_.class_count());
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

void MeshSculptor::gather(const MeshBrushSettings& settings, const field::MaskGate& gate) {
    BrushRegion& r = region_;
    // Retire the LAST stamp's slots before `r.classes` is overwritten, so the
    // reset costs what that stamp touched. `adjacency.h` states the rule this
    // follows and the reason: "allocating a per-class array per stamp is the
    // entire cost of the stroke". `slot` has to stay a full per-class array
    // because the verbs index it by arbitrary ring neighbours — what changes is
    // that it is sized ONCE and never cleared wholesale again.
    if (r.slot.size() != adjacency_.class_count()) {
        r.slot.assign(adjacency_.class_count(), kNoClass);
        r.classes.clear();
    } else {
        for (std::uint32_t c : r.classes)
            if (c < r.slot.size()) r.slot[c] = kNoClass;
    }
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
        std::uint32_t seed = settings.seed_class;
        if (seed >= adjacency_.class_count() && surface_index() != nullptr)
            seed = nearest_class(settings.center);
        geodesic_region(mesh_, adjacency_, settings.center, settings.radius, walk_, &r.classes,
                        &distance_, seed);
    } else {
        // The euclidean region IS a ball query, and used to be written as a
        // scan over every class. Flatten and Scrape are the two verbs that take
        // this path (`default_geodesic`), and they are the two that most want a
        // large radius.
        if (classes_in_ball(settings.center, settings.radius, &r.classes)) {
            // SORTED, so the region is the same list whether it came from the
            // tree or from the scan below — and so it does not depend on the
            // tree's shape, which a rebuild changes. The verbs accumulate a
            // weighted normal and a plane over this list, and float addition is
            // not associative, so an order that varied would make a brush's
            // result depend on whether the host happened to have picked.
            std::sort(r.classes.begin(), r.classes.end());
            distance_.resize(r.classes.size());
            for (std::size_t i = 0; i < r.classes.size(); ++i)
                distance_[i] = kernel::clength(class_position(r.classes[i]) - settings.center);
        } else {
            euclidean_region(mesh_, adjacency_, settings.center, settings.radius, &r.classes,
                             &distance_);
        }
    }

    r.weights.resize(r.classes.size());
    r.positions.resize(r.classes.size());
    r.normals.resize(r.classes.size());

    // The alpha's frame, once for the whole stamp rather than per vertex. Its
    // direction defaults to the brush's own averaged normal — which is not yet
    // computed here, so an unset direction uses the mesh normal nearest the
    // centre. Good enough, and cheaper than a second pass: the stamp is a disc
    // on a surface, and its normal barely varies across one brush radius.
    AlphaFrame alpha_frame;
    if (settings.has_alpha()) {
        alpha_frame.centre = settings.center;
        alpha_frame.extent =
            settings.alpha_extent > 0.0f ? settings.alpha_extent : 2.0f * settings.radius;
        kernel::cfloat3 dir = settings.alpha_direction;
        if (kernel::clength(dir) < 1e-9f) {
            const std::uint32_t near = nearest_class(settings.center);
            dir = near != kNoClass ? class_normal(mesh_, adjacency_, near) : kernel::cf3(0, 1, 0);
        }
        kernel::cfloat3 n, t, b;
        kernel::calpha_frame(dir, settings.alpha_tangent, &n, &t, &b);
        alpha_frame.tangent = t;
        alpha_frame.binormal = b;
    }

    // Weigh, snapshot, and drop what the falloff or the mask reduced to
    // nothing — compacting in place so the region is exactly what moves.
    std::size_t kept = 0;
    for (std::size_t i = 0; i < r.classes.size(); ++i) {
        const std::uint32_t c = r.classes[i];
        std::size_t mc = 0;
        const kernel::cfloat3 p = mesh_.positions[adjacency_.members(c, &mc)[0]];
        // THE WALK DECIDES WHAT IS REACHED; THE STRAIGHT LINE DECIDES HOW MUCH.
        //
        // Weighing by the walk's own distance was tried first and looks wrong:
        // an edge path overestimates geodesic distance by a direction-dependent
        // amount, so on an irregular triangulation the falloff picks up a
        // visible herringbone banding that a render shows and the numbers do
        // not. The straight-line distance carries no such bias, and it is
        // bounded by the walk's — a class the walk reached within the radius is
        // within the radius in space too — so weighing by it costs nothing.
        //
        // What survives is exactly the property the walk exists for: the chin
        // is not REACHED from the upper lip, so it is not in the region at all
        // and its weight never comes up. The price is a step at the region's
        // rim on a strongly folded surface, where the two distances diverge; on
        // ordinary curvature they agree to a few percent and the falloff there
        // is already near zero.
        float w = falloff_weight(settings.falloff,
                                 kernel::clength(p - settings.center) / settings.radius);
        // ...and fade out over the last stretch of the walk's path budget, so
        // the rim is smooth even where the two bounds disagree. See
        // kPathTaperStart.
        if (settings.geodesic) {
            const float over = (distance_[i] / settings.radius - kPathTaperStart) /
                               (kDefaultPathBudget - kPathTaperStart);
            if (over > 0.0f) {
                const float t = std::clamp(over, 0.0f, 1.0f);
                w *= 1.0f - t * t * (3.0f - 2.0f * t);
            }
        }
        if (gate) w *= 1.0f - std::clamp(gate(p), 0.0f, 1.0f);
        // The alpha multiplies the WEIGHT, which is why it needs no per-verb
        // code: every verb already scales by this, and every falloff already
        // shaped it.
        w *= alpha_at(settings, alpha_frame, p);
        if (w <= 0.0f) continue;
        r.classes[kept] = c;
        r.weights[kept] = w;
        r.positions[kept] = p;
        r.normals[kept] = class_normal(mesh_, adjacency_, c);
        ++kept;
    }
    r.classes.resize(kept);
    r.weights.resize(kept);
    r.positions.resize(kept);
    r.normals.resize(kept);

    // Already cleared at the top of this call, for exactly the entries the last
    // stamp set; only the new region's entries are written here.
    for (std::size_t i = 0; i < r.classes.size(); ++i)
        r.slot[r.classes[i]] = static_cast<std::uint32_t>(i);

    // The plane and the shared direction, taken from the snapshot and never
    // from what the stamp is about to deposit.
    kernel::cfloat3 nsum = kernel::cf3(0, 0, 0), psum = kernel::cf3(0, 0, 0);
    float wsum = 0.0f;
    for (std::size_t i = 0; i < kept; ++i) {
        nsum = nsum + r.normals[i] * r.weights[i];
        psum = psum + r.positions[i] * r.weights[i];
        wsum += r.weights[i];
    }
    r.average_normal = safe_normalize(nsum, kernel::cf3(0, 1, 0));
    r.centroid = wsum > 0.0f ? psum / wsum : settings.center;
    if (settings.use_given_plane) {
        r.plane_point = settings.plane_point;
        r.plane_normal = safe_normalize(settings.plane_normal, r.average_normal);
    } else {
        // The weighted centroid with the weighted average normal: the plane
        // ZBrush and Blender both flatten onto, and the one an artist means by
        // "flatten what is under the brush". A least-squares fit would be a
        // better plane for a flat noisy patch and a worse one for a curved
        // patch, which is the case that matters.
        r.plane_point = r.centroid;
        r.plane_normal = r.average_normal;
    }
}

namespace {

// Each verb writes one displacement per region class, reading only the
// snapshot. Free functions with one argument rather than members with six, so
// each verb is readable beside the row of the table it implements.
void verb_grab(const StampContext& ctx, std::vector<kernel::cfloat3>* d) {
    for (std::size_t i = 0; i < ctx.region.size(); ++i)
        (*d)[i] = ctx.settings.direction * ctx.region.weights[i];
}

void verb_draw(const StampContext& ctx, std::vector<kernel::cfloat3>* d) {
    // ONE shared direction for the whole stamp. That is what makes draw a
    // rounded organic swell rather than a balloon, and it is the entire
    // difference from inflate.
    const kernel::cfloat3 dir = ctx.deposit_direction();
    const float amount = ctx.settings.strength * ctx.settings.radius;
    for (std::size_t i = 0; i < ctx.region.size(); ++i)
        (*d)[i] = dir * (amount * ctx.region.weights[i]);
}

void verb_inflate(const StampContext& ctx, std::vector<kernel::cfloat3>* d) {
    // Each vertex along its OWN normal. The per-vertex direction is exactly
    // what distinguishes this from draw.
    const float amount = ctx.settings.strength * ctx.settings.radius;
    for (std::size_t i = 0; i < ctx.region.size(); ++i)
        (*d)[i] = ctx.region.normals[i] * (amount * ctx.region.weights[i]);
}

void verb_pinch(const StampContext& ctx, std::vector<kernel::cfloat3>* d) {
    // ONE signed deformation: positive gathers toward the centre, negative
    // spreads. Pinch and magnify are the same deformation with one sign, as
    // they are for fields and as the voxel pair documents.
    for (std::size_t i = 0; i < ctx.region.size(); ++i) {
        const kernel::cfloat3 toward = ctx.settings.center - ctx.region.positions[i];
        (*d)[i] = tangential(toward, ctx.region.normals[i]) *
                  (ctx.settings.strength * ctx.region.weights[i]);
    }
}

void verb_flatten(const StampContext& ctx, std::vector<kernel::cfloat3>* d) {
    const float s = std::clamp(ctx.settings.strength, 0.0f, 1.0f);
    for (std::size_t i = 0; i < ctx.region.size(); ++i)
        (*d)[i] = plane_offset(ctx.region.positions[i], ctx.region.plane_point,
                               ctx.region.plane_normal, ctx.settings.flatten_mode) *
                  (s * ctx.region.weights[i]);
}

void verb_clay(const StampContext& ctx, std::vector<kernel::cfloat3>* d) {
    // Draw's deposit CLAMPED to a plane floating at the stamp height: material
    // is added UP TO the plane and no further, so what the stamp leaves is a
    // flat-topped strip rather than a swell that follows whatever the surface
    // was doing underneath. That is the whole of Clay, and repeating it is
    // ClayBuildup.
    //
    // Which makes it, exactly, a FILL-ONLY flatten onto a plane offset from the
    // region — the deposit direction decides which side "fill" is on, so a
    // negative strength digs to a plane below instead.
    const kernel::cfloat3 dir = ctx.deposit_direction();
    const float height = ctx.settings.strength * ctx.settings.radius;
    const kernel::cfloat3 plane_pt = ctx.region.centroid + dir * height;
    const field::FlattenMode side =
        height >= 0.0f ? field::FlattenMode::FillOnly : field::FlattenMode::CutOnly;
    for (std::size_t i = 0; i < ctx.region.size(); ++i)
        (*d)[i] =
            plane_offset(ctx.region.positions[i], plane_pt, dir, side) * ctx.region.weights[i];
}

void verb_crease(const StampContext& ctx, std::vector<kernel::cfloat3>* d) {
    // The cut AND the squeeze, summed inside one stamp. Sequenced separately
    // they leave a rounded ditch: the pinch would gather vertices the draw had
    // already pushed down, instead of closing the fold as it forms.
    const kernel::cfloat3 dir = ctx.deposit_direction();
    const float cut = ctx.settings.strength * ctx.settings.radius;
    const float squeeze = std::fabs(ctx.settings.strength);
    for (std::size_t i = 0; i < ctx.region.size(); ++i) {
        const float w = ctx.region.weights[i] * ctx.region.weights[i];  // tighter than the curve
        const kernel::cfloat3 toward = ctx.settings.center - ctx.region.positions[i];
        (*d)[i] = dir * (-cut * w) + tangential(toward, ctx.region.normals[i]) * (squeeze * w);
    }
}

// Polish's gate, per region class: full strength where the surface around a
// class is near-planar, fading to zero where it bends, so noise goes and a hard
// edge stays.
//
// SPREAD, THEN FEATHERED, and the order matters.
//
// Raw, the gate steps from 1 to 0 across a single edge, and what a polish
// leaves along every feature it protected is a bead of untouched vertices
// beside a fully smoothed flat — visible in a render, and a worse artefact
// than the noise it removed.
//
// Feathering alone does not fix it, because it eats the protection: a crease
// one vertex wide has gate 0 at the crease and 1 on both sides, and averaging
// pulls it straight back up — the crease is then smoothed away, which is the
// one thing polish exists not to do. So the gate is SPREAD first (each class
// takes the minimum over its own ring), widening the protected band to cover
// the feature's neighbours, and only then feathered.
constexpr int kPolishGateSpread = 1;
constexpr int kPolishGateFeather = 2;

void polish_gate(const StampContext& ctx, std::vector<float>* gate, std::vector<float>* scratch) {
    const BrushRegion& r = ctx.region;
    const float a = std::max(ctx.settings.polish_angle, 1e-4f);
    gate->resize(r.size());
    scratch->resize(r.size());
    for (std::size_t i = 0; i < r.size(); ++i) {
        const float angle = ring_disagreement(ctx.mesh, ctx.adj, r.classes[i], r.normals[i]);
        (*gate)[i] = 1.0f - std::clamp((angle - a) / a, 0.0f, 1.0f);
    }
    for (int pass = 0; pass < kPolishGateSpread + kPolishGateFeather; ++pass) {
        const bool spreading = pass < kPolishGateSpread;
        for (std::size_t i = 0; i < r.size(); ++i) {
            std::size_t n = 0;
            const std::uint32_t* ring = ctx.adj.ring(r.classes[i], &n);
            float lowest = (*gate)[i], total = (*gate)[i], count = 1.0f;
            for (std::size_t k = 0; k < n; ++k) {
                const std::uint32_t slot = r.slot[ring[k]];
                // A neighbour outside the region is outside the brush too;
                // taking it as ungated would smooth past the rim.
                if (slot == kNoClass) continue;
                lowest = std::min(lowest, (*gate)[slot]);
                total += (*gate)[slot];
                count += 1.0f;
            }
            (*scratch)[i] = spreading ? lowest : total / count;
        }
        gate->swap(*scratch);
    }
}

// Smooth, polish and scrape all need the Laplacian target, and scrape needs the
// plane as well — from the SAME snapshot, which is the rule sculpt_scrape
// already states for voxels: calling flatten and then smooth in sequence is not
// this.
// NUDGE — grab's tangential sibling. Grab carries the region rigidly, so a
// drag across a surface lifts material off it; this slides material ALONG the
// surface instead, which is what an artist means by nudging a feature sideways.
//
// Per-vertex tangent planes rather than the region's average normal: on a
// curved region an averaged plane pushes the rim off the surface, which is the
// artifact the verb exists to avoid.
void verb_nudge(const StampContext& ctx, std::vector<kernel::cfloat3>* d) {
    const BrushRegion& r = ctx.region;
    for (std::size_t i = 0; i < r.size(); ++i)
        (*d)[i] = tangential(ctx.settings.direction, r.normals[i]) * r.weights[i];
}

// -- the colour pair ----------------------------------------------------------
//
// Neither verb writes a position. That is the mirror of the property the
// displacement verbs guarantee about `colors`, and it is what lets a host run a
// colour pass over a finished sculpt without a diff on the geometry.

// A blend that is EXACT at both ends. mix(a, b, 1) is a + (b - a) * 1, which is
// not b in floating point, so a fully-weighted dab would leave a one-ULP seam
// along the rim of every stroke — the same trap the gated ops hit.
kernel::cfloat3 blend_color(kernel::cfloat3 a, kernel::cfloat3 b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return a + (b - a) * t;
}

// PAINT — blend toward the target by the brush's own weight.
//
// `weights` already carries the falloff, the mask gate and the alpha stamp, so
// this composes with all three without a line of code about any of them.
void verb_paint(const StampContext& ctx, const std::vector<kernel::cfloat3>& current,
                std::vector<kernel::cfloat3>* target) {
    const BrushRegion& r = ctx.region;
    const float s = std::clamp(ctx.settings.strength, 0.0f, 1.0f);
    for (std::size_t i = 0; i < r.size(); ++i)
        (*target)[i] = blend_color(current[i], ctx.settings.color, r.weights[i] * s);
}

// SMEAR — drag colour across the surface.
//
// For each vertex, blend toward the one-ring neighbour lying most nearly
// OPPOSITE the drag: that is where the colour under the cursor just came from.
// The weight is scaled by how well that neighbour lines up, so a neighbour at
// right angles to the drag contributes nothing and the smear has a direction
// rather than being a smooth.
//
// The one-ring rather than a spatial query, because topology is fixed by
// contract: the ring IS the neighbourhood, it costs nothing to walk, and it
// cannot drift away from what the rest of the library thinks adjacency means.
// Neighbours OUTSIDE the region are read too — the colour being dragged in at
// the leading edge has to come from somewhere, and clamping to the region would
// make the stroke's rim smear against itself.
void verb_smear(const StampContext& ctx, const std::vector<kernel::cfloat3>& current,
                std::vector<kernel::cfloat3>* target) {
    const BrushRegion& r = ctx.region;
    const Mesh& m = ctx.mesh;
    const float s = std::clamp(ctx.settings.strength, 0.0f, 1.0f);
    const kernel::cfloat3 drag = ctx.settings.direction;
    if (is_zero(drag)) return;  // no direction, no smear — not a smooth
    const kernel::cfloat3 from = safe_normalize(drag, kernel::cf3(0, 0, 0)) * -1.0f;

    for (std::size_t i = 0; i < r.size(); ++i) {
        if (r.weights[i] <= 0.0f) continue;
        const std::uint32_t c = r.classes[i];
        std::size_t rc = 0;
        const std::uint32_t* ring = ctx.adj.ring(c, &rc);

        float best = 0.0f;
        kernel::cfloat3 source = current[i];
        for (std::size_t k = 0; k < rc; ++k) {
            std::size_t nc = 0;
            const std::uint32_t nv = ctx.adj.members(ring[k], &nc)[0];
            const kernel::cfloat3 step = m.positions[nv] - r.positions[i];
            const kernel::cfloat3 dir = safe_normalize(step, kernel::cf3(0, 0, 0));
            if (is_zero(dir)) continue;
            const float align = kernel::cdot(dir, from);
            if (align <= best) continue;
            best = align;
            // A neighbour inside the region is read at its PRE-STAMP colour,
            // so the smear is simultaneous rather than a sweep whose result
            // depends on the order the classes happen to sit in.
            const std::uint32_t slot = r.slot[ring[k]];
            source = slot != kNoClass ? current[slot] : m.colors[nv];
        }
        if (best <= 0.0f) continue;  // nothing upwind of this vertex
        (*target)[i] = blend_color(current[i], source, r.weights[i] * s * best);
    }
}

// RELAX — even the vertex spacing without reshaping the surface.
//
// Smooth moves toward the Laplacian average, which is INWARD on a convex region
// — that is why smoothing shrinks. Relax takes the same target and removes its
// normal component, so a vertex slides across the surface toward the centroid
// of its neighbours and the shape stays.
//
// It is not exactly shape-preserving, and this is the one verb where that is
// worth saying in the code rather than only in the docs: sliding along a
// TANGENT PLANE leaves a curved surface by a second-order amount, so a relax
// pass on a sphere shrinks it slightly. Re-projecting onto the pre-stamp
// surface would fix that and turns a cheap verb into a closest-point query per
// vertex; the drift is measured in examples/56 and is far below what smooth
// moves at the same strength.
//
// It matters more here than in a tool that can subdivide. Topology is fixed by
// contract, so a large grab stretches the triangles it has; this is what
// recovers them without a round trip through a retopo pass.
void verb_relax(const StampContext& ctx, const std::vector<kernel::cfloat3>& smoothed,
                std::vector<kernel::cfloat3>* d) {
    const BrushRegion& r = ctx.region;
    const float s = std::clamp(ctx.settings.strength, 0.0f, 1.0f);
    for (std::size_t i = 0; i < r.size(); ++i)
        (*d)[i] = tangential(smoothed[i] - r.positions[i], r.normals[i]) * (r.weights[i] * s);
}

// LAYER — deposit to a CEILING rather than accumulating.
//
// Every other deposit verb adds to wherever the surface now is, so a slow
// stroke digs deeper than a fast one over the same path. This one measures
// against where the surface was when the STROKE began and stops at
// `layer_height` above it, so the same path gives the same result at any speed.
//
// `origin` is that starting surface, per region entry: the stroke's own
// VertexDeltas already records each vertex's position the first time the stroke
// touches it, so the reference is a record the caller is already keeping rather
// than new per-stroke state.
void verb_layer(const StampContext& ctx, const std::vector<kernel::cfloat3>& origin,
                std::vector<kernel::cfloat3>* d) {
    const BrushRegion& r = ctx.region;
    const kernel::cfloat3 dir = ctx.deposit_direction();
    const float ceiling = ctx.settings.layer_height;
    for (std::size_t i = 0; i < r.size(); ++i) {
        // How far this vertex has already travelled along the deposit
        // direction since the stroke began.
        const float travelled = kernel::cdot(r.positions[i] - origin[i], dir);
        // What is left of this vertex's share of the ceiling. The weight scales
        // the CEILING, not the step, so the falloff shapes the layer's profile
        // and repeated stamps converge on it instead of past it.
        const float remaining = ceiling * r.weights[i] - travelled;
        // Signed: a negative height digs to a floor, and the clamp has to run
        // the other way for it.
        const float step = ceiling >= 0.0f ? std::max(remaining, 0.0f) : std::min(remaining, 0.0f);
        (*d)[i] = dir * (step * std::clamp(ctx.settings.strength, 0.0f, 1.0f));
    }
}

void verb_smooth_family(MeshBrush verb, const StampContext& ctx,
                        std::vector<kernel::cfloat3>* smoothed,
                        std::vector<kernel::cfloat3>* scratch, std::vector<float>* gate,
                        std::vector<float>* gate_scratch, std::vector<kernel::cfloat3>* d) {
    smooth_targets(ctx, ctx.settings.smooth_iterations, smoothed, scratch);
    if (verb == MeshBrush::Polish) polish_gate(ctx, gate, gate_scratch);
    const BrushRegion& r = ctx.region;
    const float s = std::clamp(ctx.settings.strength, 0.0f, 1.0f);
    for (std::size_t i = 0; i < r.size(); ++i) {
        const kernel::cfloat3 p = r.positions[i];
        const kernel::cfloat3 relax = (*smoothed)[i] - p;
        const float w = r.weights[i];
        if (verb == MeshBrush::Smooth) {
            (*d)[i] = relax * (w * s);
        } else if (verb == MeshBrush::Polish) {
            // Gated: see polish_gate. The gate is computed for the whole region
            // first, because it is averaged over the ring — a per-vertex gate
            // switches from 1 to 0 across a single edge and leaves a bead of
            // unsmoothed vertices along every feature it protected.
            (*d)[i] = relax * (w * s * (*gate)[i]);
        } else {
            // Scrape: the cut and the relax together, both against the snapshot.
            const kernel::cfloat3 cut =
                plane_offset(p, r.plane_point, r.plane_normal, field::FlattenMode::CutOnly);
            (*d)[i] = cut * (w * s) + relax * (w * 0.5f);
        }
    }
}

}  // namespace

std::size_t MeshSculptor::stamp(MeshBrush verb, const MeshBrushSettings& settings,
                                const field::MaskGate& gate, VertexDeltas* record) {
    if (!valid() || settings.radius <= 0.0f || mesh_.positions.empty()) return 0;
    gather(settings, gate);
    if (region_.empty()) return 0;

    const StampContext ctx{mesh_, adjacency_, region_, settings};

    // The colour verbs take a different path end to end: they fill a colour
    // target rather than a displacement, and they write through write_colors.
    // Sharing `displacement_` would have made "moved nothing" and "painted
    // nothing" the same number.
    if (writes_color(verb)) {
        if (!has_colors()) return 0;  // an explicit ensure_colors is the fix
        color_target_.resize(region_.size());
        for (std::size_t i = 0; i < region_.size(); ++i) {
            std::size_t mc = 0;
            const std::uint32_t v = adjacency_.members(region_.classes[i], &mc)[0];
            color_target_[i] = mesh_.colors[v];
        }
        const std::vector<kernel::cfloat3> current = color_target_;
        if (verb == MeshBrush::Paint)
            verb_paint(ctx, current, &color_target_);
        else
            verb_smear(ctx, current, &color_target_);
        return write_colors(record);
    }

    displacement_.assign(region_.size(), kernel::cf3(0, 0, 0));

    switch (verb) {
        case MeshBrush::Grab:
        case MeshBrush::Snakehook:
            // One stamp of snakehook IS a grab. What makes it a snakehook is
            // the RE-ANCHORING between stamps, which brush::apply_to_mesh does
            // by walking the brush centre along the drag.
            verb_grab(ctx, &displacement_);
            break;
        case MeshBrush::Draw:
            verb_draw(ctx, &displacement_);
            break;
        case MeshBrush::Inflate:
            verb_inflate(ctx, &displacement_);
            break;
        case MeshBrush::Pinch:
            verb_pinch(ctx, &displacement_);
            break;
        case MeshBrush::Flatten:
            verb_flatten(ctx, &displacement_);
            break;
        case MeshBrush::Clay:
            verb_clay(ctx, &displacement_);
            break;
        case MeshBrush::Crease:
            verb_crease(ctx, &displacement_);
            break;
        case MeshBrush::Smooth:
        case MeshBrush::Polish:
        case MeshBrush::Scrape:
            verb_smooth_family(verb, ctx, &smoothed_, &smooth_tmp_, &gate_, &gate_tmp_,
                               &displacement_);
            break;
        case MeshBrush::Nudge:
            verb_nudge(ctx, &displacement_);
            break;
        case MeshBrush::Relax:
            // The same Laplacian target smooth uses, with its normal component
            // removed by the verb — so the two cannot disagree about what the
            // neighbourhood average is.
            smooth_targets(ctx, settings.smooth_iterations, &smoothed_, &smooth_tmp_);
            verb_relax(ctx, smoothed_, &displacement_);
            break;
        case MeshBrush::Layer:
            // Without a record there is no stroke to measure from, and every
            // stamp would clamp against the CURRENT surface — which is draw
            // wearing layer's name. A verb that silently becomes a different
            // verb is worse than one that refuses.
            if (!record) return 0;
            gather_stroke_origin(*record);
            verb_layer(ctx, origin_, &displacement_);
            break;
        case MeshBrush::Paint:
        case MeshBrush::Smear:
            // Returned above, through write_colors. Named here rather than
            // left to a default so that adding a verb still fails the switch.
            break;
    }
    return write(record);
}

// The stroke's starting surface, per region entry — what LAYER measures its
// ceiling from. The record already keeps each vertex's position from the first
// time the stroke touched it, so this reads a reference the caller is keeping
// rather than inventing per-stroke state.
void MeshSculptor::gather_stroke_origin(const VertexDeltas& record) {
    origin_.resize(region_.size());
    for (std::size_t i = 0; i < region_.size(); ++i) {
        std::size_t members = 0;
        const std::uint32_t v = adjacency_.members(region_.classes[i], &members)[0];
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
    std::size_t painted = 0;
    for (std::size_t i = 0; i < region_.size(); ++i) {
        const std::uint32_t c = region_.classes[i];
        std::size_t mc = 0;
        const std::uint32_t* members = adjacency_.members(c, &mc);
        // Unchanged means unwritten, so a zero-weight rim costs nothing and a
        // record does not grow entries whose before and after are equal.
        if (is_zero(color_target_[i] - mesh_.colors[members[0]])) continue;
        ++painted;
        for (std::size_t k = 0; k < mc; ++k) {
            if (record) record->note(members[k], mesh_);
            mesh_.colors[members[k]] = color_target_[i];
            if (record) record->sync_after(members[k], mesh_);
        }
    }
    return painted;
}

std::size_t MeshSculptor::write(VertexDeltas* record) {
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
    std::size_t moved = 0;
    for (std::size_t i = 0; i < region_.size(); ++i) {
        if (is_zero(displacement_[i])) continue;
        ++moved;
        const std::uint32_t c = region_.classes[i];
        mark_bvh_dirty(c);  // for the next refit_bvh, which drains the whole set
        const kernel::cfloat3 target = region_.positions[i] + displacement_[i];
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
    if (defer_normals_) {
        deferred_normals_.insert(deferred_normals_.end(), pending_normals_.begin(),
                                 pending_normals_.end());
    } else {
        recompute_normals(pending_normals_, record);
    }
    return moved;
}

void MeshSculptor::flush_normals(VertexDeltas* record) {
    if (deferred_normals_.empty()) return;
    std::sort(deferred_normals_.begin(), deferred_normals_.end());
    deferred_normals_.erase(std::unique(deferred_normals_.begin(), deferred_normals_.end()),
                            deferred_normals_.end());
    recompute_normals(deferred_normals_, record);
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

}  // namespace mesh
}  // namespace clay
