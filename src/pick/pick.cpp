#include "clay/pick/pick.h"

#include <cmath>

#include "clay/kernel/field.h"
#include "clay/scene/bounds.h"
#include "clay/scene/cull_index.h"

namespace clay {
namespace pick {

using kernel::cf3;
using kernel::cfloat3;

// ---------------------------------------------------------------------------
// scene raycast + attribution
// ---------------------------------------------------------------------------

scene::Tape pickable_tape(const scene::Document& doc, const scene::CullRegion* cull,
                          const scene::CullIndex* index, const scene::CullPlan* plan) {
    bool any_ghost = false;
    for (const scene::Layer& l : doc.layers) any_ghost = any_ghost || l.ghost;
    if (!any_ghost) return scene::compile_document(doc, cull, index, plan);
    // A shallow copy: Layer holds its content by shared_ptr, so this shares
    // the edit lists and only the flags differ. Cheap next to compiling.
    //
    // The index and its plan stay behind: the index caches bounds by layer
    // address, and the copy's layers live somewhere else. compile_document
    // would drop it for that reason anyway (index->document() != &doc); not
    // passing it says so here rather than relying on the check.
    scene::Document without_ghosts = doc;
    for (scene::Layer& l : without_ghosts.layers)
        if (l.ghost) l.visible = false;
    return scene::compile_document(without_ghosts, cull);
}

namespace {

// The first surface crossing along a ray, within `tmax`, or a negative t.
//
// STEPS BY |f| AND DETECTS BY SIGN CHANGE, which is what makes it work from
// either side of the surface. Two things a plain sphere-march gets wrong here:
//
//  - Started INSIDE, the signed distance is negative and the march cannot take
//    a step at all. A cage point sits inside the high-polygon surface wherever
//    the low-polygon pinches inward, so this is not an edge case, it is half
//    the cage.
//  - Marching |f| instead fixes the stepping and breaks the stopping: with no
//    sign to watch, a hit can only be "close enough", and the over-relaxation
//    that makes a march fast can step straight past the surface and never come
//    back within tolerance.
//
// Stepping by the magnitude keeps the speed of a march — it is still the safe
// distance to the nearest surface — while a sign change between consecutive
// samples is an unambiguous crossing, whichever side it came from. Bisection
// then places it exactly.
float first_crossing(const std::function<float(cfloat3)>& field, const math::Ray& ray,
                     float tmax) {
    float t = 0.0f;
    float f_prev = field(ray.origin);
    if (f_prev == 0.0f) return 0.0f;
    // A floor on the step, or a ray running parallel to a surface takes
    // vanishing steps and never reaches tmax.
    const float min_step = tmax * 1e-3f;
    for (int i = 0; i < 512; ++i) {
        const float step = kernel::cmax(std::fabs(f_prev), min_step);
        const float t_next = t + step;
        if (t_next > tmax) return -1.0f;
        const float f = field(ray.at(t_next));
        if ((f_prev < 0.0f) != (f < 0.0f)) {
            float lo = t, hi = t_next, f_lo = f_prev;
            for (int b = 0; b < 24; ++b) {
                const float mid = (lo + hi) * 0.5f;
                const float f_mid = field(ray.at(mid));
                if ((f_lo < 0.0f) != (f_mid < 0.0f)) {
                    hi = mid;
                } else {
                    lo = mid;
                    f_lo = f_mid;
                }
            }
            return (lo + hi) * 0.5f;
        }
        t = t_next;
        f_prev = f;
    }
    return -1.0f;
}

}  // namespace

Projection project_to_surface(const scene::Tape& tape, cfloat3 point, cfloat3 direction,
                             float max_distance) {
    Projection out;
    if (!(max_distance > 0.0f)) return out;
    const float len = kernel::clength(direction);
    if (!(len > 0.0f)) return out;
    const cfloat3 dir = kernel::cf3(direction.x / len, direction.y / len, direction.z / len);
    auto field = [&](cfloat3 p) { return tape.eval(p).d; };

    // Both ways, nearest wins.
    const math::Ray forward{point, dir};
    const math::Ray backward{point, kernel::cf3(-dir.x, -dir.y, -dir.z)};
    const float tf = first_crossing(field, forward, max_distance);
    const float tb = first_crossing(field, backward, max_distance);
    if (tf < 0.0f && tb < 0.0f) return out;

    // A point sitting exactly ON the surface crosses at t ~ 0 both ways; either
    // answer is correct and the forward one is chosen so the sign is stable
    // rather than decided by a float comparison of two zeros.
    const bool take_forward = tf >= 0.0f && (tb < 0.0f || tf <= tb);
    out.hit = true;
    out.distance = take_forward ? tf : -tb;
    out.position = take_forward ? forward.at(tf) : backward.at(tb);
    // The NORMAL comes from the signed field, which is what has a meaningful
    // gradient at the surface.
    out.normal = kernel::cnormal(field, out.position, 1e-4f);
    return out;
}

float next_visible_crossing(const std::function<float(cfloat3)>& field, const math::Ray& ray,
                            float t_start, float tmax, float step,
                            const voxel::GroupField& groups) {
    if (!(step > 0.0f)) return -1.0f;
    constexpr int kMaxSteps = 8192;
    float t_prev = t_start;
    float f_prev = field(ray.at(t_prev));
    for (int i = 0; i < kMaxSteps; ++i) {
        const float t = t_prev + step;
        if (t > tmax) return -1.0f;
        const float f = field(ray.at(t));
        // A sign change in EITHER direction is a surface: entering a solid and
        // leaving it are both crossings, and the far wall of a hidden shell is
        // reached by the second kind.
        if ((f_prev < 0.0f) != (f < 0.0f)) {
            // Bisect. Ten halvings take a cell-sized bracket to about a
            // thousandth of a cell, which is far below what the group lattice
            // can distinguish anyway.
            float lo = t_prev, hi = t, f_lo = f_prev;
            for (int b = 0; b < 10; ++b) {
                const float mid = (lo + hi) * 0.5f;
                const float f_mid = field(ray.at(mid));
                if ((f_lo < 0.0f) != (f_mid < 0.0f)) {
                    hi = mid;
                } else {
                    lo = mid;
                    f_lo = f_mid;
                }
            }
            const float t_hit = (lo + hi) * 0.5f;
            if (!groups.point_hidden(ray.at(t_hit))) return t_hit;
            // Hidden: keep scanning from just past it.
        }
        t_prev = t;
        f_prev = f;
    }
    return -1.0f;
}

namespace {

// How far the ray's cull box reaches beyond the segment itself. Everything
// the march evaluates lies ON the segment — craycast and the hidden-surface
// scan sample ray.at(t) for t in [tmin, tmax] — except the normal, whose
// tetrahedron taps sit 1e-4 off the hit point. Ten times that covers the taps
// and the rounding of ray.at() against the box built from its endpoints, and
// nothing else needs covering: the compile pads the box by the document's own
// blend and feather reach (CullIndex::cull_pad), which is what makes the
// culled field exact inside it.
constexpr float kRayCullDilation = 1e-3f;

// Clips [tmin, tmax] to the tape's bounds. False when the ray misses them
// outright; true — with the range untouched — when there is nothing finite
// to clip against.
bool clip_to_bounds(const scene::Tape& tape, const math::Ray& ray, float* tmin, float* tmax) {
    if (tape.bounds.empty() || tape.bounds.is_infinite()) return true;
    float t0, t1;
    if (!math::ray_aabb(ray, tape.bounds.dilated(0.01f), &t0, &t1)) return false;
    *tmin = kernel::cmax(*tmin, t0);
    *tmax = kernel::cmin(*tmax, t1);
    return true;
}

// The tape the march runs on: `whole`, or a tape culled to the ray's segment
// when that culls away whatever is holding the step scale down (the reasoning
// is on raycast_scene's declaration). The culled tape is built into `local`,
// which the caller owns so the reference returned stays valid.
//
// Two ways out without a compile. A scale already at 1 has nothing to win. An
// unbounded tape — a plane, an infinite repeat — cannot be clipped, so the
// segment runs to options.tmax and its box culls nothing; the compile would
// be the whole document again, at the cost of the whole document.
//
// And one way out after it: the culled scale must be STRICTLY larger. Equal
// means the steep item survived the cull (the ray runs through it, or the
// pad reaches it) and the whole tape marches just as fast without having
// been compiled for this ray.
const scene::Tape& march_tape(const scene::Document& doc, const scene::Tape& whole,
                              const scene::CullIndex* index, const math::Ray& ray, float tmin,
                              float tmax, const RaycastOptions& options, scene::Tape* local) {
    if (!options.local_tape || whole.safe_step_scale() >= 1.0f) return whole;
    if (whole.bounds.empty() || whole.bounds.is_infinite()) return whole;
    math::Aabb segment;
    segment.expand(ray.at(tmin));
    segment.expand(ray.at(tmax));
    const scene::CullRegion cull{segment.dilated(kRayCullDilation)};
    if (index) {
        const scene::CullPlan plan = index->plan(cull.region);
        *local = pickable_tape(doc, &cull, index, &plan);
    } else {
        *local = pickable_tape(doc, &cull, index);
    }
    if (local->empty() || local->safe_step_scale() <= whole.safe_step_scale()) return whole;
    return *local;
}

}  // namespace

SceneHit raycast_scene(const scene::Document& doc, const math::Ray& ray,
                       const RaycastOptions& options) {
    // No index: this entry point has nowhere to keep one between calls, and
    // building it per ray would walk the document to save walking the
    // document. The culled compile computes its own bounds and pad instead,
    // to the same tape (the index is a pure acceleration).
    return raycast_scene(doc, pickable_tape(doc), nullptr, ray, options);
}

SceneHit raycast_scene(const scene::Document& doc, const scene::Tape& whole,
                       const scene::CullIndex* index, const math::Ray& ray,
                       const RaycastOptions& options) {
    SceneHit hit;
    if (whole.empty()) return hit;

    float tmin = options.tmin, tmax = options.tmax;
    if (!clip_to_bounds(whole, ray, &tmin, &tmax)) return hit;
    scene::Tape local;
    const scene::Tape& tape = march_tape(doc, whole, index, ray, tmin, tmax, options, &local);
    auto field = [&](cfloat3 p) { return tape.eval(p).d; };

    // Marched in segments so a hit on hidden surface can be stepped over rather
    // than becoming a miss: hiding the front of a head is how an artist reaches
    // the inside of it, and a ray that stopped there would defeat the feature.
    //
    // The bound is on SEGMENTS, not on total steps — each restart carries the
    // caller's own max_steps — so a ray crossing several hidden shells is
    // bounded without a hidden region silently shortening the visible march.
    constexpr int kMaxHiddenSkips = 16;
    for (int skip = 0; skip <= kMaxHiddenSkips; ++skip) {
        kernel::CRayHit r = kernel::craycast(field, ray.origin, ray.dir, tmin, tmax, options.eps,
                                             tape.safe_step_scale(), 1.4f, options.max_steps);
        hit.steps += r.steps;
        if (!r.hit) return hit;
        const cfloat3 p = ray.at(r.t);
        if (options.groups && options.groups->point_hidden(p)) {
            // Hand off to the scan: a sphere-march cannot resume from inside
            // the solid it just hit, and walking out of that solid overshoots
            // the far wall, which is the surface being looked for. See
            // next_visible_crossing.
            const float step = kernel::cmax(options.groups->cell_size(), options.eps * 4.0f);
            const float t_vis = next_visible_crossing(field, ray, r.t + step * 0.5f, tmax, step,
                                                      *options.groups);
            if (t_vis < 0.0f) return hit;  // nothing visible behind it
            hit.hit = true;
            hit.t = t_vis;
            hit.position = ray.at(t_vis);
            hit.normal = kernel::cnormal(field, hit.position, 1e-4f);
            attribute(doc, hit.position, &hit.layer, &hit.item);
            return hit;
        }
        hit.hit = true;
        hit.t = r.t;
        hit.position = p;
        hit.normal = kernel::cnormal(field, hit.position, 1e-4f);
        attribute(doc, hit.position, &hit.layer, &hit.item);
        return hit;
    }
    return hit;
}

namespace {

// |field| of one item evaluated in isolation at p (its own tape).
float item_field_distance(const scene::Layer& layer, const scene::Node& item, cfloat3 p) {
    scene::Document single;
    scene::Layer& l = single.add_sdf_layer("probe");
    l.xform = layer.xform;
    l.scale_axes = layer.scale_axes;  // a probe of a squashed layer is squashed
    l.mirror_axes = layer.mirror_axes;
    l.mirror_k = layer.mirror_k;
    scene::Node copy = item;
    copy.op = scene::Op::Add;  // isolate the shape regardless of its op
    copy.id = scene::kNoNode;
    copy.children.clear();
    l.sdf->insert(copy);
    scene::Tape t = scene::compile_document(single);
    return kernel::cabs(t.eval(p).d);
}

void attribute_content(const scene::Layer& layer, const scene::SdfContent& content,
                       const std::vector<scene::NodeId>& ids, cfloat3 p, float* best,
                       scene::NodeId* best_item) {
    for (scene::NodeId id : ids) {
        const scene::Node* n = content.find(id);
        if (!n || !n->visible) continue;
        if (n->is_group) {
            attribute_content(layer, content, n->children, p, best, best_item);
            continue;
        }
        // cheap reject: influence bound
        if (!scene::item_influence_bound(*n, layer).dilated(0.05f).contains(p)) continue;
        float d = item_field_distance(layer, *n, p);
        if (d < *best) {
            *best = d;
            *best_item = id;
        }
    }
}

}  // namespace

void attribute(const scene::Document& doc, cfloat3 position, scene::LayerId* layer,
               scene::NodeId* item) {
    *layer = 0;
    *item = scene::kNoNode;
    float best_layer_d = 3.4e38f;
    for (const scene::Layer& l : doc.layers) {
        if (!l.visible || l.ghost || l.kind != scene::LayerKind::Sdf || !l.sdf) continue;
        scene::Tape t = scene::compile_layer(l);
        if (t.empty()) continue;
        float d = kernel::cabs(t.eval(position).d);
        if (d < best_layer_d) {
            best_layer_d = d;
            *layer = l.id;
        }
    }
    const scene::Layer* winner = doc.find_layer(*layer);
    if (!winner || !winner->sdf) return;
    float best_item_d = 3.4e38f;
    attribute_content(*winner, *winner->sdf, winner->sdf->roots, position, &best_item_d, item);
}

// ---------------------------------------------------------------------------
// brick raycast
// ---------------------------------------------------------------------------

namespace {

// Trilinear sample of the band-clamped brick field at a world position.
float brick_field(const brick::BrickCache& cache, cfloat3 p) {
    const int dim = cache.config().dim;
    const float vs = cache.config().voxel_size;
    float gx = p.x / vs, gy = p.y / vs, gz = p.z / vs;
    int i0 = static_cast<int>(std::floor(gx));
    int j0 = static_cast<int>(std::floor(gy));
    int k0 = static_cast<int>(std::floor(gz));
    float fx = gx - static_cast<float>(i0);
    float fy = gy - static_cast<float>(j0);
    float fz = gz - static_cast<float>(k0);
    auto fdiv = [](int a, int b) { return a >= 0 ? a / b : -(((-a) + b - 1) / b); };
    auto sample = [&](int i, int j, int k) {
        brick::BrickKey key{fdiv(i, dim), fdiv(j, dim), fdiv(k, dim)};
        return cache.sample(key, i - key.x * dim, j - key.y * dim, k - key.z * dim);
    };
    float c00 = kernel::cmix(sample(i0, j0, k0), sample(i0 + 1, j0, k0), fx);
    float c10 = kernel::cmix(sample(i0, j0 + 1, k0), sample(i0 + 1, j0 + 1, k0), fx);
    float c01 = kernel::cmix(sample(i0, j0, k0 + 1), sample(i0 + 1, j0, k0 + 1), fx);
    float c11 = kernel::cmix(sample(i0, j0 + 1, k0 + 1), sample(i0 + 1, j0 + 1, k0 + 1), fx);
    return kernel::cmix(kernel::cmix(c00, c10, fy), kernel::cmix(c01, c11, fy), fz);
}

}  // namespace

SceneHit raycast_bricks(const brick::BrickCache& cache, const math::Ray& ray,
                        const RaycastOptions& options) {
    SceneHit hit;
    const float band = cache.config().band();
    const float brick_span =
        static_cast<float>(cache.config().dim) * cache.config().voxel_size;

    // Overall bounds of the surface bricks — the cache keeps this current, so
    // a ray no longer enumerates every surface key before its first step.
    // Picking runs once per Pencil event, and on a whole-model cache that
    // walk cost more than the march it preceded.
    const math::Aabb domain = cache.surface_bounds();
    if (domain.empty()) return hit;
    float t0, t1;
    if (!math::ray_aabb(ray, domain.dilated(band), &t0, &t1)) return hit;
    float t = kernel::cmax(options.tmin, t0);
    float tmax = kernel::cmin(options.tmax, t1);

    float prev_d = 3.4e38f;
    float prev_t = t;
    for (int i = 0; i < options.max_steps && t <= tmax; ++i) {
        float d = brick_field(cache, ray.at(t));
        if (d < options.eps * kernel::cmax(t, 1.0f)) {
            // refine between the last two samples
            float denom = d - prev_d;
            float th = (prev_d < 3.4e38f && kernel::cabs(denom) > 1e-12f)
                           ? kernel::cmix(prev_t, t, kernel::cclamp(-prev_d / denom, 0.0f, 1.0f))
                           : t;
            hit.hit = true;
            hit.t = th;
            hit.position = ray.at(th);
            float h = cache.config().voxel_size * 0.5f;
            hit.normal = kernel::cnormalize(
                cf3(brick_field(cache, hit.position + cf3(h, 0, 0)) -
                        brick_field(cache, hit.position - cf3(h, 0, 0)),
                    brick_field(cache, hit.position + cf3(0, h, 0)) -
                        brick_field(cache, hit.position - cf3(0, h, 0)),
                    brick_field(cache, hit.position + cf3(0, 0, h)) -
                        brick_field(cache, hit.position - cf3(0, 0, h))));
            return hit;
        }
        prev_d = d;
        prev_t = t;
        // clamped field caps steps at the band; jump a brick when saturated
        t += (d >= band * 0.999f) ? kernel::cmax(band, brick_span * 0.5f) : d;
    }
    return hit;
}

// ---------------------------------------------------------------------------
// mesh picking
// ---------------------------------------------------------------------------

MeshHit raycast_mesh(const mesh::Mesh& m, const mesh::Bvh& bvh, const math::Ray& ray,
                     const math::Transform& xform, float tmax) {
    MeshHit out;
    if (bvh.empty() || m.indices.empty()) return out;

    // Into layer space. The transform's scale is uniform, so a direction stays
    // unit after the inverse rotation and only the scale divides — which is
    // also why `t` comes back multiplied by it rather than recomputed.
    const math::Transform inv = xform.inverse();
    math::Ray local{inv.apply(ray.origin), xform.rotation.conjugate().rotate(ray.dir)};
    local.dir = kernel::cnormalize(local.dir);
    const float scale = xform.scale != 0.0f ? xform.scale : 1.0f;

    const mesh::Bvh::RayHit hit = bvh.raycast(local, 0.0f, tmax / scale);
    if (!hit.hit) return out;

    const std::uint32_t i0 = m.indices[hit.triangle * 3];
    const std::uint32_t i1 = m.indices[hit.triangle * 3 + 1];
    const std::uint32_t i2 = m.indices[hit.triangle * 3 + 2];
    const float w = 1.0f - hit.u - hit.v;
    const cfloat3 local_p = m.positions[i0] * w + m.positions[i1] * hit.u + m.positions[i2] * hit.v;

    cfloat3 local_n;
    if (m.normals.size() == m.positions.size()) {
        local_n = m.normals[i0] * w + m.normals[i1] * hit.u + m.normals[i2] * hit.v;
    } else {
        local_n =
            kernel::ccross(m.positions[i1] - m.positions[i0], m.positions[i2] - m.positions[i0]);
    }
    const float len = kernel::clength(local_n);
    // A zero-area triangle or three cancelling vertex normals: report the face
    // as facing the ray rather than handing back a zero vector.
    local_n = len > 1e-20f ? local_n / len : -local.dir;

    out.hit = true;
    out.t = hit.t * scale;
    out.position = xform.apply(local_p);
    out.normal = kernel::cnormalize(xform.rotation.rotate(local_n));
    out.triangle = hit.triangle;
    out.u = hit.u;
    out.v = hit.v;
    return out;
}

// ---------------------------------------------------------------------------
// snapping
// ---------------------------------------------------------------------------

SnapResult snap_to_surface(const scene::Tape& tape, cfloat3 p, int max_iters, float tolerance) {
    SnapResult out;
    auto field = [&](cfloat3 q) { return tape.eval(q).d; };
    float scale = tape.safe_step_scale();
    cfloat3 q = p;
    for (int i = 0; i < max_iters; ++i) {
        float d = field(q);
        if (kernel::cabs(d) < tolerance) {
            out.ok = true;
            out.position = q;
            out.normal = kernel::cnormal(field, q, 1e-4f);
            return out;
        }
        cfloat3 n = kernel::cnormal(field, q, 1e-4f);
        q = q - n * (d * scale);
    }
    // report the best point found even without full convergence
    out.ok = kernel::cabs(field(q)) < tolerance * 10.0f;
    out.position = q;
    out.normal = kernel::cnormal(field, q, 1e-4f);
    return out;
}

// ---------------------------------------------------------------------------
// voxel picking
// ---------------------------------------------------------------------------

VoxelHit raycast_voxels(const voxel::VoxelGrid& grid, const math::Ray& ray, float tmax) {
    VoxelHit hit;
    auto bmin = grid.bounds_min();
    auto bmax = grid.bounds_max();
    if (!bmin || !bmax) return hit;
    const float vs = grid.voxel_size();
    math::Aabb box{cf3(static_cast<float>(bmin->x), static_cast<float>(bmin->y),
                       static_cast<float>(bmin->z)) *
                       vs,
                   cf3(static_cast<float>(bmax->x + 1), static_cast<float>(bmax->y + 1),
                       static_cast<float>(bmax->z + 1)) *
                       vs};
    float t0, t1;
    if (!math::ray_aabb(ray, box, &t0, &t1)) return hit;
    float t = kernel::cmax(t0, 0.0f) + 1e-6f;
    if (t > tmax) return hit;

    // Amanatides & Woo DDA
    cfloat3 p = ray.at(t);
    voxel::VoxelCoord cell{static_cast<std::int32_t>(std::floor(p.x / vs)),
                           static_cast<std::int32_t>(std::floor(p.y / vs)),
                           static_cast<std::int32_t>(std::floor(p.z / vs))};
    const float dirs[3] = {ray.dir.x, ray.dir.y, ray.dir.z};
    int step[3];
    float t_next[3], t_delta[3];
    const float origin[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
    int cur[3] = {cell.x, cell.y, cell.z};
    for (int a = 0; a < 3; ++a) {
        if (kernel::cabs(dirs[a]) < 1e-12f) {
            step[a] = 0;
            t_next[a] = 3.4e38f;
            t_delta[a] = 3.4e38f;
        } else {
            step[a] = dirs[a] > 0 ? 1 : -1;
            float boundary = (static_cast<float>(cur[a]) + (dirs[a] > 0 ? 1.0f : 0.0f)) * vs;
            t_next[a] = (boundary - origin[a]) / dirs[a];
            t_delta[a] = vs / kernel::cabs(dirs[a]);
        }
    }
    int entry_axis = -1;
    // entry face of the first cell comes from the slab entry axis
    {
        float best = -3.4e38f;
        for (int a = 0; a < 3; ++a) {
            float boundary = (static_cast<float>(cur[a]) + (step[a] > 0 ? 0.0f : 1.0f)) * vs;
            if (step[a] != 0) {
                float ta = (boundary - origin[a]) / dirs[a];
                if (ta > best && ta <= t) {
                    best = ta;
                    entry_axis = a;
                }
            }
        }
    }

    for (int i = 0; i < 100000; ++i) {
        if (grid.get({cur[0], cur[1], cur[2]}) != 0) {
            hit.hit = true;
            hit.cell = {cur[0], cur[1], cur[2]};
            hit.t = t;
            int axis = entry_axis < 0 ? 0 : entry_axis;
            hit.face = axis * 2 + (step[axis] > 0 ? 1 : 0);  // entered through -side if stepping +
            return hit;
        }
        int a = 0;
        if (t_next[1] < t_next[a]) a = 1;
        if (t_next[2] < t_next[a]) a = 2;
        t = t_next[a];
        if (t > tmax || t > t1 + 1e-6f) return hit;
        cur[a] += step[a];
        t_next[a] += t_delta[a];
        entry_axis = a;
    }
    return hit;
}

voxel::VoxelCoord adjacent_cell(const VoxelHit& hit) {
    voxel::VoxelCoord c = hit.cell;
    int axis = hit.face / 2;
    int positive = (hit.face % 2) == 0;  // face 0 = +X side
    if (axis == 0) c.x += positive ? 1 : -1;
    if (axis == 1) c.y += positive ? 1 : -1;
    if (axis == 2) c.z += positive ? 1 : -1;
    return c;
}

std::optional<voxel::VoxelCoord> pick_build_plane(const voxel::VoxelGrid& grid,
                                                  const math::Ray& ray,
                                                  std::int32_t plane_cell) {
    return grid.build_plane_pick(ray, plane_cell);
}

// ---------------------------------------------------------------------------
// bounds utilities
// ---------------------------------------------------------------------------

namespace {

math::Aabb node_shape_bounds(const scene::SdfContent& content, const scene::Node& n,
                             const scene::Layer& layer) {
    if (n.is_group) {
        math::Aabb b;
        for (scene::NodeId c : n.children) {
            const scene::Node* child = content.find(c);
            if (child && child->visible) b.expand(node_shape_bounds(content, *child, layer));
        }
        return b;
    }
    math::Aabb local = scene::item_local_bounds(n);
    if (local.empty()) return local;
    // Both per-axis scales, each innermost at its own level, as
    // scene::geometry_bound composes them — a selection box around the shape
    // the item IS, not around the one its primitive was authored as.
    const math::cfloat4x4 axes = math::scale_matrix(n.scale_axes);
    math::Aabb bound = local.transformed(scene::placed_matrix(layer, n));
    if (n.mirror && layer.mirror_axes != 0) {
        for (int axis = 0; axis < 3; ++axis) {
            if (!(layer.mirror_axes & (1u << axis))) continue;
            bound.expand(local.transformed(math::mul(
                scene::layer_matrix(layer),
                math::mul(math::reflection_matrix(axis), math::mul(n.xform.matrix(), axes)))));
        }
    }
    return bound.dilated(
        kernel::cmax(n.rounding * scene::placed_distance_scale(layer, n), 0.0f));
}

}  // namespace

math::Aabb selection_bounds(const scene::Document& doc, scene::LayerId layer_id,
                            const std::vector<scene::NodeId>& nodes) {
    math::Aabb out;
    const scene::Layer* layer = doc.find_layer(layer_id);
    if (!layer || !layer->sdf) return out;
    for (scene::NodeId id : nodes) {
        const scene::Node* n = layer->sdf->find(id);
        if (n) out.expand(node_shape_bounds(*layer->sdf, *n, *layer));
    }
    return out;
}

math::Aabb layer_bounds(const scene::Layer& layer) {
    math::Aabb out;
    if (!layer.sdf) return out;
    for (scene::NodeId id : layer.sdf->roots) {
        const scene::Node* n = layer.sdf->find(id);
        if (n && n->visible) out.expand(node_shape_bounds(*layer.sdf, *n, layer));
    }
    return out;
}

}  // namespace pick
}  // namespace clay
