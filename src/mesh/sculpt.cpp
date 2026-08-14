#include "clay/mesh/sculpt.h"

#include <algorithm>
#include <cmath>

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

// A weld class's geometric normal, area-weighted over its incident triangles.
// GEOMETRIC rather than the mesh's stored normals on purpose: displacement is
// about where the surface is, not about how it shades, and this way a mesh
// imported without normals sculpts exactly like one that has them.
kernel::cfloat3 class_normal(const Mesh& m, const Adjacency& adj, std::uint32_t cls) {
    std::size_t n = 0;
    const std::uint32_t* tris = adj.triangles_of(cls, &n);
    kernel::cfloat3 sum = kernel::cf3(0, 0, 0);
    for (std::size_t i = 0; i < n; ++i) sum = sum + face_normal(m, tris[i]);
    return safe_normalize(sum, kernel::cf3(0, 1, 0));
}

// How far the faces around a class disagree, in radians: the widest angle
// between the class normal and any incident face's normal. Polish's gate.
float ring_disagreement(const Mesh& m, const Adjacency& adj, std::uint32_t cls,
                        kernel::cfloat3 n) {
    std::size_t count = 0;
    const std::uint32_t* tris = adj.triangles_of(cls, &count);
    float worst = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        const kernel::cfloat3 fn = safe_normalize(face_normal(m, tris[i]), n);
        worst = std::max(worst, std::acos(std::clamp(kernel::cdot(fn, n), -1.0f, 1.0f)));
    }
    return worst;
}

// Everything one stamp reads. Bundled so the verbs below are free functions
// with one argument rather than members with six.
struct StampContext {
    const Mesh& mesh;
    const Adjacency& adj;
    const BrushRegion& region;
    const MeshBrushSettings& settings;

    kernel::cfloat3 deposit_direction() const {
        return is_zero(settings.deposit_normal) ? region.average_normal
                                                : safe_normalize(settings.deposit_normal,
                                                                 region.average_normal);
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
    slot_.clear();
    normals_ = false;
}

void VertexDeltas::note(std::uint32_t v, const Mesh& m) {
    if (slot_.find(v) != slot_.end()) return;
    const bool has_normals = m.normals.size() == m.positions.size();
    if (vertices_.empty()) normals_ = has_normals;
    slot_.emplace(v, static_cast<std::uint32_t>(vertices_.size()));
    vertices_.push_back(v);
    before_position_.push_back(m.positions[v]);
    after_position_.push_back(m.positions[v]);
    if (normals_ && has_normals) {
        before_normal_.push_back(m.normals[v]);
        after_normal_.push_back(m.normals[v]);
    }
}

void VertexDeltas::sync_after(std::uint32_t v, const Mesh& m) {
    auto it = slot_.find(v);
    if (it == slot_.end()) return;
    after_position_[it->second] = m.positions[v];
    if (!after_normal_.empty() && m.normals.size() == m.positions.size())
        after_normal_[it->second] = m.normals[v];
}

bool VertexDeltas::revert(Mesh& m) const {
    if (vertices_.empty()) return true;
    const bool normals = !before_normal_.empty() && m.normals.size() == m.positions.size();
    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        const std::uint32_t v = vertices_[i];
        if (v >= m.positions.size()) return false;
    }
    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        m.positions[vertices_[i]] = before_position_[i];
        if (normals) m.normals[vertices_[i]] = before_normal_[i];
    }
    return true;
}

bool VertexDeltas::apply(Mesh& m) const {
    if (vertices_.empty()) return true;
    const bool normals = !after_normal_.empty() && m.normals.size() == m.positions.size();
    for (std::size_t i = 0; i < vertices_.size(); ++i)
        if (vertices_[i] >= m.positions.size()) return false;
    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        m.positions[vertices_[i]] = after_position_[i];
        if (normals) m.normals[vertices_[i]] = after_normal_[i];
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

void MeshSculptor::refresh_bvh() { bvh_ = std::make_unique<Bvh>(Bvh::build(mesh_)); }

void MeshSculptor::gather(const MeshBrushSettings& settings, const field::MaskGate& gate) {
    BrushRegion& r = region_;
    if (settings.geodesic)
        geodesic_region(mesh_, adjacency_, settings.center, settings.radius, walk_, &r.classes,
                        &distance_, settings.seed_class);
    else
        euclidean_region(mesh_, adjacency_, settings.center, settings.radius, &r.classes,
                         &distance_);

    r.weights.resize(r.classes.size());
    r.positions.resize(r.classes.size());
    r.normals.resize(r.classes.size());

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
        if (gate) w *= 1.0f - std::clamp(gate(p), 0.0f, 1.0f);
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

    r.slot.assign(adjacency_.class_count(), kNoClass);
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
        (*d)[i] = plane_offset(ctx.region.positions[i], plane_pt, dir, side) *
                  ctx.region.weights[i];
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

// Smooth, polish and scrape all need the Laplacian target, and scrape needs the
// plane as well — from the SAME snapshot, which is the rule sculpt_scrape
// already states for voxels: calling flatten and then smooth in sequence is not
// this.
void verb_smooth_family(MeshBrush verb, const StampContext& ctx,
                        std::vector<kernel::cfloat3>* smoothed,
                        std::vector<kernel::cfloat3>* scratch,
                        std::vector<kernel::cfloat3>* d) {
    smooth_targets(ctx, ctx.settings.smooth_iterations, smoothed, scratch);
    const BrushRegion& r = ctx.region;
    const float s = std::clamp(ctx.settings.strength, 0.0f, 1.0f);
    for (std::size_t i = 0; i < r.size(); ++i) {
        const kernel::cfloat3 p = r.positions[i];
        const kernel::cfloat3 relax = (*smoothed)[i] - p;
        const float w = r.weights[i];
        if (verb == MeshBrush::Smooth) {
            (*d)[i] = relax * (w * s);
        } else if (verb == MeshBrush::Polish) {
            // Full strength where the one-ring's normals agree, fading to zero
            // at twice the threshold. A hard edge disagrees, so it survives a
            // pass that removes the noise beside it.
            const float angle = ring_disagreement(ctx.mesh, ctx.adj, r.classes[i], r.normals[i]);
            const float a = std::max(ctx.settings.polish_angle, 1e-4f);
            const float keep = 1.0f - std::clamp((angle - a) / a, 0.0f, 1.0f);
            (*d)[i] = relax * (w * s * keep);
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

    displacement_.assign(region_.size(), kernel::cf3(0, 0, 0));
    const StampContext ctx{mesh_, adjacency_, region_, settings};

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
            verb_smooth_family(verb, ctx, &smoothed_, &smooth_tmp_, &displacement_);
            break;
    }
    return write(record);
}

std::size_t MeshSculptor::write(VertexDeltas* record) {
    normal_mark_.assign(adjacency_.class_count(), 0);
    pending_normals_.clear();
    std::size_t moved = 0;
    for (std::size_t i = 0; i < region_.size(); ++i) {
        if (is_zero(displacement_[i])) continue;
        ++moved;
        const std::uint32_t c = region_.classes[i];
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
            const kernel::cfloat3 fn = face_normal(mesh_, tris[k]);
            for (int corner = 0; corner < 3; ++corner) {
                const std::uint32_t v = mesh_.indices[tris[k] * 3 + corner];
                // Only this class's own corners: a split vertex keeps its own
                // faces, which is what preserves a hard edge's shading.
                if (adjacency_.class_of(v) == c) mesh_.normals[v] = mesh_.normals[v] + fn;
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
