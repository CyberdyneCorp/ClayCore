#include "clay/pick/pick.h"

#include <cmath>

#include "clay/kernel/field.h"
#include "clay/scene/bounds.h"

namespace clay {
namespace pick {

using kernel::cf3;
using kernel::cfloat3;

// ---------------------------------------------------------------------------
// scene raycast + attribution
// ---------------------------------------------------------------------------

scene::Tape pickable_tape(const scene::Document& doc, const scene::CullRegion* cull) {
    bool any_ghost = false;
    for (const scene::Layer& l : doc.layers) any_ghost = any_ghost || l.ghost;
    if (!any_ghost) return scene::compile_document(doc, cull);
    // A shallow copy: Layer holds its content by shared_ptr, so this shares
    // the edit lists and only the flags differ. Cheap next to compiling.
    scene::Document without_ghosts = doc;
    for (scene::Layer& l : without_ghosts.layers)
        if (l.ghost) l.visible = false;
    return scene::compile_document(without_ghosts, cull);
}

SceneHit raycast_scene(const scene::Document& doc, const math::Ray& ray,
                       const RaycastOptions& options) {
    SceneHit hit;
    scene::Tape tape = pickable_tape(doc);
    if (tape.empty()) return hit;

    float tmin = options.tmin, tmax = options.tmax;
    if (!tape.bounds.empty() && !tape.bounds.is_infinite()) {
        float t0, t1;
        if (!math::ray_aabb(ray, tape.bounds.dilated(0.01f), &t0, &t1)) return hit;
        tmin = kernel::cmax(tmin, t0);
        tmax = kernel::cmin(tmax, t1);
    }
    auto field = [&](cfloat3 p) { return tape.eval(p).d; };
    kernel::CRayHit r = kernel::craycast(field, ray.origin, ray.dir, tmin, tmax, options.eps,
                                         tape.safe_step_scale(), 1.4f, options.max_steps);
    if (!r.hit) return hit;
    hit.hit = true;
    hit.t = r.t;
    hit.position = ray.at(r.t);
    hit.normal = kernel::cnormal(field, hit.position, 1e-4f);
    attribute(doc, hit.position, &hit.layer, &hit.item);
    return hit;
}

namespace {

// |field| of one item evaluated in isolation at p (its own tape).
float item_field_distance(const scene::Layer& layer, const scene::Node& item, cfloat3 p) {
    scene::Document single;
    scene::Layer& l = single.add_sdf_layer("probe");
    l.xform = layer.xform;
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

    // overall bounds of surface bricks
    math::Aabb domain;
    for (const brick::BrickKey& key : cache.surface_bricks())
        domain.expand(cache.brick_bounds(key));
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
    math::Transform world = layer.xform * n.xform;
    math::Aabb bound = local.transformed(world.matrix());
    if (n.mirror && layer.mirror_axes != 0) {
        for (int axis = 0; axis < 3; ++axis) {
            if (!(layer.mirror_axes & (1u << axis))) continue;
            bound.expand(local.transformed(
                math::mul(layer.xform.matrix(),
                          math::mul(math::reflection_matrix(axis), n.xform.matrix()))));
        }
    }
    return bound.dilated(kernel::cmax(n.rounding * world.scale, 0.0f));
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
