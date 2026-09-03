#include "clay/pick/pick.h"

#include <algorithm>
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
            attribute(doc, whole, hit.position, &hit.layer, &hit.item);
            return hit;
        }
        hit.hit = true;
        hit.t = r.t;
        hit.position = p;
        hit.normal = kernel::cnormal(field, hit.position, 1e-4f);
        attribute(doc, whole, hit.position, &hit.layer, &hit.item);
        return hit;
    }
    return hit;
}

namespace {

// The layer a probe of one item is compiled under: the source layer's
// placement, per-axis scale and mirror, and nothing else. A probe of a
// squashed layer is squashed, a probe of a mirrored one carries its copies —
// a hit on the reflected side attributes to the item it reflects.
//
// Not the source layer itself, and not its radial symmetry: this is exactly
// the layer the probe Document used to build per item (`add_sdf_layer` and
// these four fields), kept as it was so a hit attributes the same before and
// after the probe stopped building a Document. Carrying the radial copies is
// a change to what a hit on one names, and belongs to its own change.
scene::Layer probe_layer(const scene::Layer& layer) {
    scene::Layer probe;
    probe.xform = layer.xform;
    probe.scale_axes = layer.scale_axes;  // a probe of a squashed layer is squashed
    probe.mirror_axes = layer.mirror_axes;
    probe.mirror_k = layer.mirror_k;
    return probe;
}

// |field| of one item evaluated in isolation at p (its own tape, under the
// probe layer). One emit and one eval per candidate, where it used to build a
// Document, a Layer and an SdfContent, insert a copy of the node and walk the
// whole document, per candidate per pick — the larger half of a 0.80 ms pick
// at 1,500 overlapping dabs, of which the march itself was 0.38. The tape's
// own allocations are not what is left: carrying one tape's storage across
// the candidates measured the same to within 1%, so it is not done.
float item_field_distance(const scene::Layer& probe, const scene::Node& item, cfloat3 p) {
    return kernel::cabs(scene::compile_item(probe, item).eval(p).d);
}

// `layer` is the real layer, whose influence bounds decide which items are
// asked; `probe` is probe_layer(layer), what each is asked under.
void attribute_content(const scene::Layer& layer, const scene::Layer& probe,
                       const scene::SdfContent& content, const std::vector<scene::NodeId>& ids,
                       cfloat3 p, float* best, scene::NodeId* best_item) {
    for (scene::NodeId id : ids) {
        const scene::Node* n = content.find(id);
        if (!n || !n->visible) continue;
        if (n->is_group) {
            attribute_content(layer, probe, content, n->children, p, best, best_item);
            continue;
        }
        // cheap reject: influence bound
        if (!scene::item_influence_bound(*n, layer).dilated(0.05f).contains(p)) continue;
        float d = item_field_distance(probe, *n, p);
        if (d < *best) {
            *best = d;
            *best_item = id;
        }
    }
}

bool layer_is_candidate(const scene::Layer& l) {
    return l.visible && !l.ghost && l.kind == scene::LayerKind::Sdf && l.sdf;
}

// The item within the winning layer. `*item` stays kNoNode when no visible
// item's influence bound reaches the position.
void attribute_item(const scene::Document& doc, scene::LayerId layer, cfloat3 position,
                    scene::NodeId* item) {
    const scene::Layer* winner = doc.find_layer(layer);
    if (!winner || !winner->sdf) return;
    const scene::Layer probe = probe_layer(*winner);
    float best_item_d = 3.4e38f;
    attribute_content(*winner, probe, *winner->sdf, winner->sdf->roots, position, &best_item_d,
                      item);
}

// The layer whose own field is nearest the position, by compiling each
// candidate's tape; 0 when no candidate has a non-empty tape.
scene::LayerId nearest_layer_compiled(const scene::Document& doc, cfloat3 position) {
    scene::LayerId layer = 0;
    float best_layer_d = 3.4e38f;
    for (const scene::Layer& l : doc.layers) {
        if (!layer_is_candidate(l)) continue;
        scene::Tape t = scene::compile_layer(l);
        if (t.empty()) continue;
        float d = kernel::cabs(t.eval(position).d);
        if (d < best_layer_d) {
            best_layer_d = d;
            layer = l.id;
        }
    }
    return layer;
}

}  // namespace

void attribute(const scene::Document& doc, cfloat3 position, scene::LayerId* layer,
               scene::NodeId* item) {
    *item = scene::kNoNode;
    *layer = nearest_layer_compiled(doc, position);
    attribute_item(doc, *layer, position, item);
}

void attribute(const scene::Document& doc, const scene::Tape& tape, cfloat3 position,
               scene::LayerId* layer, scene::NodeId* item) {
    *layer = 0;
    *item = scene::kNoNode;
    // With one candidate layer the pickable tape IS that layer's tape: the
    // document compile chains the visible SDF layers and pickable_tape hides
    // the ghosted ones first, so the candidates here and the layers in the
    // tape are the same set, and one layer needs no union. Its tape is empty
    // exactly when the layer's own would be, and where it is not the layer
    // wins outright — the only layer whose field is finite anywhere. What the
    // per-call compile would have found, without the compile.
    //
    // Several candidates keep the per-layer compile: the union cannot say
    // which of its layers the position is nearest.
    const scene::Layer* only = nullptr;
    int candidates = 0;
    for (const scene::Layer& l : doc.layers) {
        if (!layer_is_candidate(l)) continue;
        only = &l;
        ++candidates;
    }
    if (candidates == 1) {
        if (tape.empty()) return;
        *layer = only->id;
    } else {
        *layer = nearest_layer_compiled(doc, position);
    }
    attribute_item(doc, *layer, position, item);
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

// The domain a brick ray can hit at all: the surface bricks' box dilated by the
// band, clipped to the options' range. False when the ray misses it.
bool brick_ray_range(const brick::BrickCache& cache, const math::Ray& ray,
                     const RaycastOptions& options, float* t, float* tmax) {
    // Overall bounds of the surface bricks — the cache keeps this current, so
    // a ray no longer enumerates every surface key before its first step.
    // Picking runs once per Pencil event, and on a whole-model cache that
    // walk cost more than the march it preceded.
    const math::Aabb domain = cache.surface_bounds();
    if (domain.empty()) return false;
    float t0, t1;
    if (!math::ray_aabb(ray, domain.dilated(cache.config().band()), &t0, &t1)) return false;
    *t = kernel::cmax(options.tmin, t0);
    *tmax = kernel::cmin(options.tmax, t1);
    return true;
}

}  // namespace

namespace detail {

SceneHit raycast_bricks_sphere_traced(const brick::BrickCache& cache, const math::Ray& ray,
                                      const RaycastOptions& options) {
    SceneHit hit;
    const float band = cache.config().band();
    const float brick_span =
        static_cast<float>(cache.config().dim) * cache.config().voxel_size;
    float t, tmax;
    if (!brick_ray_range(cache, ray, options, &t, &tmax)) return hit;

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

}  // namespace detail

// The analytic walk. The cache's field is a trilinear reconstruction of the
// lattice, so inside one cell it is a cubic in the ray parameter (Hansson
// Söderlund, Evans and Akenine-Möller, "Ray Tracing of Signed Distance
// Function Grids", JCGT 2022, §2), and the first crossing in a cell is a root
// of that cubic rather than something to creep up on. The ray is walked
// brick by brick, and only the cells a Surface brick (or a uniform brick's
// face layer against a differing neighbour) puts under it are solved; every
// other brick is skipped whole.
//
// Everything below works in LATTICE UNITS: positions divided by voxel_size,
// the direction left unit, so the parameter `u` is a world t over the voxel
// size. A cell is then [c, c+1]³ for an integer triple c, a brick is dim of
// them, and the cubic's coefficients see a cell as the paper's canonical
// [0, 1]³ — which also keeps the cubic well conditioned, its terms at the
// band's scale rather than the band over the voxel size cubed.
namespace {

constexpr float kFarU = 3.4e38f;

// The eight bricks a cell's corner samples can be stored in. A cell owned by
// brick K (its low corner's lattice index lies in K) has its high corners on
// K's +x/+y/+z faces, and those lattice points are STORED by the neighbour
// bricks — sample 0 of K+x is the point on K's +x face — so a gather touches
// up to eight bricks. Each slot is looked up at most once per brick walked,
// and only when a corner actually needs it: the sphere trace this replaces
// paid eight hash lookups per SAMPLE, and the lookups were most of its cost.
//
// A slot that holds no evaluated brick answers +band, as sample() does: never
// evaluated is empty space, which is what the reference reconstruction says.
class CellCorners {
  public:
    CellCorners(const brick::BrickCache& cache, brick::BrickKey key)
        : cache_(&cache), key_(key), dim_(cache.config().dim), band_(cache.config().band()) {}

    // The table for the brick one step along `axis` from `prev`'s, seeded
    // with what prev already resolved: stepping +x, prev's +x slots are this
    // brick's own slots (prev+x is this brick, prev+xy is this+y, ...), and
    // stepping -x the other way round. A ray leaves a brick through the face
    // layer whose neighbour is the brick it enters next, so the walk that
    // resolved that layer's slots has already paid for the lookups the next
    // brick starts with — most of a uniform brick's cost was those lookups.
    CellCorners(const CellCorners& prev, brick::BrickKey key, int axis, int step)
        : CellCorners(*prev.cache_, key) {
        const int bit = 1 << axis;
        for (int s = 0; s < 8; ++s) {
            if (s & bit) continue;
            const int from = step > 0 ? (s | bit) : s;
            const int to = step > 0 ? s : (s | bit);
            if (!(prev.known_ & (1u << from))) continue;
            slot_[to] = prev.slot_[from];
            known_ |= 1u << to;
        }
    }

    // Uniform bricks fall into two sign classes, and a Surface brick is its
    // own: the classes decide whether the cells between two bricks can hold
    // a crossing at all.
    enum Class { Inside = -1, Surface = 0, Outside = 1 };
    Class klass(int slot) {
        const brick::Brick* b = brick(slot);
        if (!b || b->state == brick::BrickState::Outside) return Outside;
        return b->state == brick::BrickState::Inside ? Inside : Surface;
    }

    // The decoded, band-clamped sample at local lattice coordinates in
    // [0, dim]³ — dim on an axis meaning "the neighbour's face sample".
    float at(int i, int j, int k) {
        const int slot = (i == dim_ ? 1 : 0) | (j == dim_ ? 2 : 0) | (k == dim_ ? 4 : 0);
        const brick::Brick* b = brick(slot);
        if (!b || b->state == brick::BrickState::Outside) return band_;
        if (b->state == brick::BrickState::Inside) return -band_;
        if (slot & 1) i = 0;
        if (slot & 2) j = 0;
        if (slot & 4) k = 0;
        return brick::half_to_float(b->values[(static_cast<std::size_t>(k) * dim_ + j) * dim_ + i]);
    }

  private:
    const brick::Brick* brick(int slot) {
        if (!(known_ & (1u << slot))) {
            slot_[slot] = cache_->find(brick::BrickKey{key_.x + (slot & 1), key_.y + ((slot >> 1) & 1),
                                                      key_.z + (slot >> 2)});
            known_ |= 1u << slot;
        }
        return slot_[slot];
    }

    // A pointer so the table can be assigned: the walk replaces it per brick.
    const brick::BrickCache* cache_;
    brick::BrickKey key_;
    int dim_;
    float band_;
    const brick::Brick* slot_[8] = {};
    unsigned known_ = 0;
};

// Amanatides & Woo over a grid of `size` lattice units, at the brick level and
// again at the cell level. Boundaries are computed from the ray origin each
// time rather than accumulated, so the brick walk and the cell walk inside it
// agree on where a brick ends: (key+1)*dim and (cell+1) are the same integer,
// and the same integer through the same expression is the same float.
struct Dda {
    int cell[3];
    int step[3];
    float next[3];
    float delta[3];

    void init(cfloat3 o, cfloat3 d, float size, const int start[3]) {
        const float os[3] = {o.x, o.y, o.z};
        const float ds[3] = {d.x, d.y, d.z};
        for (int a = 0; a < 3; ++a) {
            cell[a] = start[a];
            if (kernel::cabs(ds[a]) < 1e-12f) {
                step[a] = 0;
                next[a] = kFarU;
                delta[a] = kFarU;
                continue;
            }
            step[a] = ds[a] > 0 ? 1 : -1;
            const float boundary = static_cast<float>(cell[a] + (ds[a] > 0 ? 1 : 0)) * size;
            next[a] = (boundary - os[a]) / ds[a];
            delta[a] = size / kernel::cabs(ds[a]);
        }
    }
    float exit() const { return kernel::cmin(next[0], kernel::cmin(next[1], next[2])); }
    // Step into the next cell along the axis whose boundary comes first, and
    // say which axis that was.
    int advance() {
        const int a = next[0] <= next[1] ? (next[0] <= next[2] ? 0 : 2) : (next[1] <= next[2] ? 1 : 2);
        cell[a] += step[a];
        next[a] += delta[a];
        return a;
    }
};

// f(u) - eps along the ray inside one cell, as c3 u³ + c2 u² + c1 u + c0.
struct CellCubic {
    float c0, c1, c2, c3;
    float eval(float u) const { return ((c3 * u + c2) * u + c1) * u + c0; }
    float slope(float u) const { return (3.0f * c3 * u + 2.0f * c2) * u + c1; }
};

// The paper's equations (3), (6) and (7) with the k4..k7 signs flipped: it
// writes the surface as z·K(x,y) − A(x,y) = 0, which is the NEGATIVE of the
// trilinear value, and the sign matters here because a hit is where the
// field falls below eps, not merely where it is zero. With
//   A = k0 + k1 x + k2 y + k3 xy,   K = k4 + k5 x + k6 y + k7 xy,
//   f = A + z K,
// substituting x = ox + u dx (and y, z) gives the coefficients below; eps is
// folded into c0 so a root of the cubic IS the eps crossing. s is indexed
// s[i | j<<1 | k<<2] for the corner (i, j, k) of the unit cell.
CellCubic cell_cubic(const float s[8], cfloat3 o, cfloat3 d, float eps) {
    const float k0 = s[0];
    const float k1 = s[1] - s[0];
    const float k2 = s[2] - s[0];
    const float k3 = s[3] - s[2] - k1;
    const float a = s[5] - s[4];
    const float k4 = s[4] - s[0];
    const float k5 = a - k1;
    const float k6 = (s[6] - s[4]) - k2;
    const float k7 = (s[7] - s[6] - a) - k3;
    const float m0 = o.x * o.y;
    const float m1 = d.x * d.y;
    const float m2 = o.x * d.y + o.y * d.x;
    const float m3 = k1 + k5 * o.z;
    const float m4 = k2 + k6 * o.z;
    const float m5 = k3 + k7 * o.z;
    CellCubic c;
    c.c0 = (k0 + k4 * o.z) + o.x * m3 + o.y * m4 + m0 * m5 - eps;
    c.c1 = d.x * m3 + d.y * m4 + m2 * m5 + d.z * (k4 + k5 * o.x + k6 * o.y + k7 * m0);
    c.c2 = m1 * m5 + d.z * (k5 * d.x + k6 * d.y + k7 * m2);
    c.c3 = k7 * m1 * d.z;
    return c;
}

// The gradient of the trilinear field at a point of the unit cell — the
// paper's equations (9)–(11), a bilinear blend of the corner differences on
// each axis. Lattice units, but a uniform scale leaves the direction alone.
cfloat3 cell_gradient(const float s[8], cfloat3 p) {
    using kernel::cmix;
    const float gx = cmix(cmix(s[1] - s[0], s[3] - s[2], p.y), cmix(s[5] - s[4], s[7] - s[6], p.y), p.z);
    const float gy = cmix(cmix(s[2] - s[0], s[3] - s[1], p.x), cmix(s[6] - s[4], s[7] - s[5], p.x), p.z);
    const float gz = cmix(cmix(s[4] - s[0], s[5] - s[1], p.x), cmix(s[6] - s[2], s[7] - s[3], p.x), p.y);
    return cf3(gx, gy, gz);
}

// Newton–Raphson from the secant guess, inside a bracket [lo, hi] with
// g(lo) >= 0 >= g(hi) (the paper's Listing 1, with the bracket kept so a step
// that leaves it becomes a bisection — a cubic's tangent can point anywhere).
float refine_root(const CellCubic& g, float lo, float g_lo, float hi, float g_hi) {
    float u = (g_hi != g_lo) ? (g_hi * lo - g_lo * hi) / (g_hi - g_lo) : 0.5f * (lo + hi);
    for (int i = 0; i < 8; ++i) {
        const float gu = g.eval(u);
        if (gu >= 0.0f) lo = u; else hi = u;
        const float du = g.slope(u);
        float next = du != 0.0f ? u - gu / du : 0.5f * (lo + hi);
        if (!(next > lo && next < hi)) next = 0.5f * (lo + hi);
        if (kernel::cabs(next - u) < 1e-6f) return next;
        u = next;
    }
    return u;
}

// Where g'(u) = 3 c3 u² + 2 c2 u + c1 is zero, ascending, into `ends`; how
// many there are. The stable form of the quadratic, so a near-linear
// derivative does not cancel its own smaller root away.
int turning_points(const CellCubic& g, float ends[2]) {
    const float qa = 3.0f * g.c3, qb = 2.0f * g.c2, qc = g.c1;
    if (kernel::cabs(qa) < 1e-12f) {
        if (kernel::cabs(qb) < 1e-12f) return 0;
        ends[0] = -qc / qb;
        return 1;
    }
    const float disc = qb * qb - 4.0f * qa * qc;
    if (disc < 0.0f) return 0;
    const float q = -0.5f * (qb + (qb >= 0.0f ? 1.0f : -1.0f) * std::sqrt(disc));
    const float r1 = q / qa;
    const float r2 = q != 0.0f ? qc / q : r1;
    ends[0] = kernel::cmin(r1, r2);
    ends[1] = kernel::cmax(r1, r2);
    return 2;
}

// The first root of g in (0, ufar], given g(0) >= 0. The derivative's roots
// split the span into monotone pieces (Marmitt et al.'s split, the paper's
// Figure 4); the first piece whose far end is at or below zero brackets the
// first crossing, and no piece changing sign means no crossing at all.
bool first_root(const CellCubic& g, float ufar, float* root) {
    float ends[3];
    int count = turning_points(g, ends);
    ends[count++] = ufar;
    float lo = 0.0f, g_lo = g.c0;
    for (int i = 0; i < count; ++i) {
        const float hi = ends[i];
        if (!(hi > lo) || hi > ufar) continue;   // a turning point behind us, or past the cell
        const float g_hi = g.eval(hi);
        if (g_hi <= 0.0f) {
            *root = refine_root(g, lo, g_lo, hi, g_hi);
            return true;
        }
        lo = hi;
        g_lo = g_hi;
    }
    return false;
}

class BrickWalker {
  public:
    BrickWalker(const brick::BrickCache& cache, const math::Ray& ray, float eps)
        : cache_(cache), ray_(ray), dim_(cache.config().dim), vs_(cache.config().voxel_size),
          eps_(eps), o_(ray.origin / cache.config().voxel_size), d_(ray.dir) {}

    // Walk the bricks the ray crosses in [u0, u1]; true with `hit` filled at
    // the first crossing.
    bool march(float u0, float u1, SceneHit* hit) {
        int start[3];
        cell_at(u0, static_cast<float>(dim_), start);
        Dda bricks;
        bricks.init(o_, d_, static_cast<float>(dim_), start);
        float u = u0;
        brick::BrickKey key{start[0], start[1], start[2]};
        CellCorners corners(cache_, key);
        // The DDA is monotone and u1 is finite, so this ends; the guard is
        // against a NaN that would make every comparison false.
        for (int guard = 0; guard < (1 << 20) && u < u1; ++guard) {
            const float u_exit = bricks.exit();
            if (visit_brick(corners, key, u, kernel::cmin(u_exit, u1), hit)) return true;
            u = u_exit;
            const int axis = bricks.advance();
            key = brick::BrickKey{bricks.cell[0], bricks.cell[1], bricks.cell[2]};
            corners = CellCorners(corners, key, axis, bricks.step[axis]);
        }
        return false;
    }

  private:
    void cell_at(float u, float size, int out[3]) const {
        const cfloat3 p = (o_ + d_ * u) / size;
        out[0] = static_cast<int>(std::floor(p.x));
        out[1] = static_cast<int>(std::floor(p.y));
        out[2] = static_cast<int>(std::floor(p.z));
    }

    // A uniform brick is skipped whole unless the ray's segment through it
    // reaches a face layer (the last cell on an axis) whose neighbour is of
    // a different class — only there can a cell of a uniform brick hold a
    // crossing, its high corners being the neighbour's face samples. The
    // segment is straight, so on each axis the stretch of it inside the face
    // layer is one interval of u; a diagonal neighbour (+xy, +xyz) is reached
    // only where those intervals OVERLAP, which is what makes this cheap: a
    // ray crossing a brick corner to corner reaches all three face layers
    // but rarely their common edge, and it is the lookups this decides.
    bool touches_differing_neighbour(CellCorners& corners, brick::BrickKey key, float u_in,
                                     float u_out) {
        float lo[3], hi[3];
        face_layer_intervals(key, u_in, u_out, lo, hi);
        const CellCorners::Class own = corners.klass(0);
        for (int slot = 1; slot < 8; ++slot) {
            if (!reaches_slot(slot, u_in, u_out, lo, hi)) continue;
            if (corners.klass(slot) != own) return true;
        }
        return false;
    }

    // For each axis, the u-interval of [u_in, u_out] spent in the brick's
    // high face layer (empty when hi < lo).
    void face_layer_intervals(brick::BrickKey key, float u_in, float u_out, float lo[3],
                              float hi[3]) const {
        const float span = static_cast<float>(dim_);
        const float face = static_cast<float>(dim_ - 1);
        const float o[3] = {o_.x - static_cast<float>(key.x) * span,
                            o_.y - static_cast<float>(key.y) * span,
                            o_.z - static_cast<float>(key.z) * span};
        const float d[3] = {d_.x, d_.y, d_.z};
        for (int a = 0; a < 3; ++a) {
            lo[a] = u_in;
            hi[a] = u_out;
            if (kernel::cabs(d[a]) < 1e-12f) {
                if (o[a] < face) hi[a] = -kFarU;   // never in the layer
                continue;
            }
            const float at = (face - o[a]) / d[a];
            if (d[a] > 0) lo[a] = kernel::cmax(lo[a], at); else hi[a] = kernel::cmin(hi[a], at);
        }
    }

    // Whether the segment is ever in every face layer the slot's neighbour
    // sits behind at once.
    static bool reaches_slot(int slot, float u_in, float u_out, const float lo[3],
                             const float hi[3]) {
        float from = u_in, to = u_out;
        for (int a = 0; a < 3; ++a) {
            if (!(slot & (1 << a))) continue;
            from = kernel::cmax(from, lo[a]);
            to = kernel::cmin(to, hi[a]);
        }
        return from <= to;
    }

    bool visit_brick(CellCorners& corners, brick::BrickKey key, float u_in, float u_out,
                     SceneHit* hit) {
        if (!(u_out > u_in)) return false;
        const CellCorners::Class own = corners.klass(0);
        if (own != CellCorners::Surface && !touches_differing_neighbour(corners, key, u_in, u_out)) {
            // Uniform through and through. Inside is the paper's solid voxel:
            // a hit on the entry face, which the sphere trace would also have
            // reported there (its first sample reads -band). Reachable only by
            // a ray that starts inside the solid, since the crossing into it
            // was in a cell walked before this one.
            if (own == CellCorners::Inside) return report(u_in, -d_, hit);
            return false;
        }
        // An Outside brick beside something else: only its face-layer cells
        // can differ from +band, so the walk gathers nothing for the rest.
        return walk_cells(corners, key, u_in, u_out, own == CellCorners::Outside, hit);
    }

    // Cell-level DDA through one brick. The starting cell is clamped into the
    // brick, because the entry point sits on the brick's face and rounding can
    // put floor() one cell out; the far boundary in the direction of travel is
    // then still ahead of u_in, so no cell is exited before it is entered.
    //
    // `faces_only` walks an Outside brick: every cell off its +x/+y/+z face
    // layers has eight +band corners and is passed without a gather, and the
    // corner table is rebuilt at the next face cell rather than shifted
    // across the gap.
    bool walk_cells(CellCorners& corners, brick::BrickKey key, float u_in, float u_out,
                    bool faces_only, SceneHit* hit) {
        const int lo[3] = {key.x * dim_, key.y * dim_, key.z * dim_};
        int start[3];
        cell_at(u_in, 1.0f, start);
        for (int a = 0; a < 3; ++a) start[a] = std::clamp(start[a], lo[a], lo[a] + dim_ - 1);
        Dda cells;
        cells.init(o_, d_, 1.0f, start);
        Corners8 s;
        float u = u_in;
        while (u < u_out) {
            const float u_exit = cells.exit();
            if (cell_matters(faces_only, cells.cell, lo)) {
                load_corners(corners, cells, lo, &s);
                if (solve_cell(s.v, cells.cell, u, kernel::cmin(u_exit, u_out), hit)) return true;
            } else {
                s.valid = false;
            }
            if (u_exit >= u_out) break;
            u = u_exit;
            s.moved = cells.advance();
            if (!inside_brick(cells.cell[s.moved], lo[s.moved])) return false;
        }
        return false;
    }

    // The corners of the cell the walk is on, and what it takes to reuse
    // them for the next: whether they are current, and the axis of the step
    // taken since.
    struct Corners8 {
        float v[8];
        bool valid = false;
        int moved = 0;
    };

    bool cell_matters(bool faces_only, const int cell[3], const int lo[3]) const {
        return !faces_only || cell[0] - lo[0] == dim_ - 1 || cell[1] - lo[1] == dim_ - 1 ||
               cell[2] - lo[2] == dim_ - 1;
    }
    bool inside_brick(int cell, int lo) const { return cell >= lo && cell < lo + dim_; }

    // Fill `s` for the DDA's current cell: shifted from the cell one step
    // back when that one was gathered, from scratch otherwise.
    static void load_corners(CellCorners& corners, const Dda& cells, const int lo[3],
                             Corners8* s) {
        if (s->valid) {
            shift_corners(corners, cells.cell, lo, s->moved, cells.step[s->moved], s->v);
        } else {
            for (int c = 0; c < 8; ++c) s->v[c] = corner(corners, cells.cell, lo, c);
        }
        s->valid = true;
    }

    // Corner `c` (bits x, y<<1, z<<2) of the cell at global lattice index
    // `cell`, from the brick whose low corner is `lo`.
    static float corner(CellCorners& corners, const int cell[3], const int lo[3], int c) {
        return corners.at(cell[0] - lo[0] + (c & 1), cell[1] - lo[1] + ((c >> 1) & 1),
                          cell[2] - lo[2] + (c >> 2));
    }

    // The DDA moved one cell along `axis`: the four corners on the face the
    // two cells share are already decoded, and only the far face is new.
    // Half the gathers of a cell, and the gathers were most of a cell's cost.
    static void shift_corners(CellCorners& corners, const int cell[3], const int lo[3], int axis,
                              int step, float s[8]) {
        const int bit = 1 << axis;
        for (int c = 0; c < 8; ++c) {
            const bool high = (c & bit) != 0;
            if (step > 0) {
                if (!high) s[c] = s[c | bit];
            } else {
                if (high) s[c] = s[c & ~bit];
            }
        }
        for (int c = 0; c < 8; ++c) {
            const bool high = (c & bit) != 0;
            if (high == (step > 0)) s[c] = corner(corners, cell, lo, c);
        }
    }

    // One cell, [u_in, u_out] of the ray inside it, its corners gathered in
    // `s`. The trilinear value is bounded by its corners, so a cell whose
    // lowest corner clears eps has no crossing and costs the gather and
    // nothing else — which is most of the cells under a Surface brick, and
    // the reason the cubic is cheap on average despite its 37 operations. A
    // cell entirely below eps is solid: the hit is its entry, where the
    // sphere trace would have read a sample below eps and stopped.
    bool solve_cell(const float s[8], const int cell[3], float u_in, float u_out,
                    SceneHit* hit) {
        if (!(u_out > u_in)) return false;
        float lo = kFarU, hi = -kFarU;
        for (int c = 0; c < 8; ++c) {
            lo = kernel::cmin(lo, s[c]);
            hi = kernel::cmax(hi, s[c]);
        }
        // The sphere trace stops where the field falls below eps scaled by
        // the distance, so this solves for that crossing rather than for
        // zero: the two report the same point, and the difference from the
        // true zero is eps — below anything a pick can see.
        const float eps = eps_ * kernel::cmax(u_in * vs_, 1.0f);
        if (lo > eps) return false;
        if (hi < eps) return report(u_in, -d_, hit);
        // Into the unit cell: origin at the cell's low corner, in voxels.
        const cfloat3 cell_origin = (o_ + d_ * u_in) - cf3(static_cast<float>(cell[0]),
                                                           static_cast<float>(cell[1]),
                                                           static_cast<float>(cell[2]));
        const CellCubic g = cell_cubic(s, cell_origin, d_, eps);
        float root;
        if (g.c0 < 0.0f) {
            root = 0.0f;   // already below eps on entry: the face is the hit
        } else if (!first_root(g, u_out - u_in, &root)) {
            return false;
        }
        return report(u_in + root, cell_gradient(s, cell_origin + d_ * root), hit);
    }

    bool report(float u, cfloat3 gradient, SceneHit* hit) const {
        hit->hit = true;
        hit->t = u * vs_;
        hit->position = ray_.at(hit->t);
        const float len = kernel::clength(gradient);
        // A flat gradient (a uniform cell, or a saturated one) has no normal
        // to offer; facing the ray is the one defined answer.
        hit->normal = len > 1e-20f ? gradient / len : -ray_.dir;
        return true;
    }

    const brick::BrickCache& cache_;
    const math::Ray& ray_;
    int dim_;
    float vs_;
    float eps_;
    cfloat3 o_, d_;
};

}  // namespace

SceneHit raycast_bricks(const brick::BrickCache& cache, const math::Ray& ray,
                        const RaycastOptions& options) {
    SceneHit hit;
    float t, tmax;
    if (!brick_ray_range(cache, ray, options, &t, &tmax)) return hit;
    const float vs = cache.config().voxel_size;
    BrickWalker walker(cache, ray, options.eps);
    walker.march(t / vs, tmax / vs, &hit);
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
