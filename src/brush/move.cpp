// The Move brush (brush-engine spec, add-move-brush). See
// include/clay/brush/move.h for the three ways a caller gets this wrong and why
// the resolver owns them — and for why, under symmetry, it is the BRUSH that
// is reflected and not the item's bound.

#include "clay/brush/move.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <utility>

#include "clay/math/transform.h"
#include "clay/scene/bounds.h"

namespace clay {
namespace brush {

using kernel::cf3;
using kernel::cfloat3;

namespace {

// -- the images of a drag -----------------------------------------------------

// The linear maps, in the LAYER's frame, of the copies the compiler emits of a
// participating item: one reflection per set mirror axis, then one rotation
// per radial copy. Empty without symmetry. Copy k of a radial item is the item
// rotated by +angle, so the item that sits under the ball THROUGH copy k has
// its body at the ball rotated by -angle — the inverse emit_item composes for
// that copy.
std::vector<math::cfloat4x4> copy_linears(const scene::Layer& layer) {
    std::vector<math::cfloat4x4> out;
    for (int axis = 0; axis < 3; ++axis) {
        if (layer.mirror_axes & (1u << axis)) out.push_back(math::reflection_matrix(axis));
    }
    if (layer.radial_count > 1) {
        const int axis = layer.radial_axis < 3 ? layer.radial_axis : 1;
        const int count = static_cast<int>(layer.radial_count);
        for (int k = 1; k < count; ++k) {
            const float angle =
                6.2831853071795864769f * static_cast<float>(k) / static_cast<float>(count);
            out.push_back(math::rotation_matrix(axis, -angle));
        }
    }
    return out;
}

// The symmetry is the LAYER's, about the layer's own planes and axis, so the
// drag is reflected in layer-local coordinates and mapped back. On an identity
// layer transform a reflection is then an exact sign flip, which is what makes
// a drag and its mirror image bit-identical. ONE arithmetic for both halves of
// the drag — the images a host is told about and the grabs the prepared drag
// resolves — so the two cannot disagree in the last bit.
cfloat3 image_centre(const math::Transform& xf, const math::cfloat4x4& linear, cfloat3 centre) {
    return xf.apply(kernel::cmul_dir(linear, xf.apply_inverse(centre)));
}

// A displacement is a vector: rotation and scale, no translation.
cfloat3 image_displacement(const math::Transform& xf, const math::cfloat4x4& linear,
                           cfloat3 displacement) {
    const float scale = xf.scale != 0.0f ? xf.scale : 1.0f;
    const cfloat3 local = xf.rotation.conjugate().rotate(displacement) / scale;
    return xf.rotation.rotate(kernel::cmul_dir(linear, local) * scale);
}

// Where each image's ball is, and its box. Both are per DRAG, computed once
// rather than once per item they are tested against: the resolver visits
// every visible item in the layer, and per-item work that the single-image
// drag did not pay before shows up whole on BM_MoveDrag1000 (measured 9 ns an
// item, 1.11x, for byte-identical warps).
struct ImageBall {
    cfloat3 centre;
    math::Aabb box;
    const math::cfloat4x4* linear;  // null for the drag itself
    std::size_t leader;             // its group's leader; itself for a leader
    std::size_t next;               // see PreparedImage::next
    std::size_t tail;               // a leader's last member, where the next links
};

constexpr std::size_t kNoImage = PreparedImage::kOwnBall;

float max_abs(cfloat3 v) { return std::max({std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)}); }

// Are two images the SAME ball (#663)? A centre on the plane reflects onto
// itself exactly on an identity layer (0 and -0 compare equal), but through a
// placed layer it goes out and back through the layer transform and misses by
// a few ulps of the coordinates involved. So the tolerance has two terms: a
// ten-thousandth of the radius, which no artist can place a drag inside of on
// purpose, and a millionth of the coordinates' magnitude, which covers that
// rounding however far from the origin the layer sits. Anything further apart
// is two balls, and keeps two grabs, exactly as before.
bool coincident(cfloat3 a, cfloat3 b, float radius, const math::Transform& xf) {
    const float magnitude = std::max({max_abs(a), max_abs(b), max_abs(xf.position)});
    const float tolerance = 1e-4f * radius + 1e-6f * magnitude;
    return max_abs(a - b) <= tolerance;
}

// The earlier images that lead a group, keyed by where their centres fall
// along one fixed direction, so the leader a new image coincides with is a
// range query rather than a test against every earlier image. The scan was
// images^2 before the first frame: 19.5 ms at a radial count of 4096, against
// 0.2 ms with no merge at all. Centres within `tolerance` of each other on
// every axis project within 1.61 `tolerance` plus the projection's own
// rounding, which the doubled window covers; the direction is generic, so
// distinct images rarely share a window. A non-finite centre is never indexed
// (NaN has no place in an ordered map) and so leads its own group.
class LeaderIndex {
public:
    explicit LeaderIndex(float tolerance) : window_(2.0f * tolerance) {}

    // The earliest leader whose ball `c` coincides with, or kNoImage.
    std::size_t find(const std::vector<ImageBall>& balls, cfloat3 c, float radius,
                     const math::Transform& xf) const {
        const float key = project(c);
        std::size_t found = kNoImage;
        if (!std::isfinite(key)) return found;
        for (auto it = leaders_.lower_bound(key - window_);
             it != leaders_.end() && it->first <= key + window_; ++it) {
            if (it->second < found && coincident(balls[it->second].centre, c, radius, xf))
                found = it->second;
        }
        return found;
    }

    void add(cfloat3 c, std::size_t k) {
        const float key = project(c);
        if (std::isfinite(key)) leaders_.emplace(key, k);
    }

private:
    static float project(cfloat3 c) {
        return 0.26726124f * c.x + 0.53452248f * c.y + 0.80178373f * c.z;
    }

    float window_;
    std::multimap<float, std::size_t> leaders_;
};

// Links each image to the earliest earlier LEADER whose ball it coincides
// with, and appends it to that leader's list; an image that coincides with no
// leader leads its own group.
void group_coincident(const math::Transform& xf, float radius, std::vector<ImageBall>* balls) {
    float magnitude = max_abs(xf.position);
    for (const ImageBall& ball : *balls) magnitude = std::max(magnitude, max_abs(ball.centre));
    // Every pair `coincident` accepts is within this, on every axis.
    LeaderIndex leaders(1e-4f * radius + 2e-6f * magnitude);
    for (std::size_t k = 0; k < balls->size(); ++k) {
        const std::size_t lead = leaders.find(*balls, (*balls)[k].centre, radius, xf);
        if (lead == kNoImage) {
            leaders.add((*balls)[k].centre, k);
            continue;
        }
        ImageBall& head = (*balls)[lead];
        (*balls)[k].leader = lead;
        (*balls)[head.tail].next = k;
        head.tail = k;
    }
}

std::vector<ImageBall> image_balls(const math::Transform& xf, cfloat3 world_centre,
                                   const std::vector<math::cfloat4x4>& linears, float radius) {
    const cfloat3 r = cf3(radius, radius, radius);
    std::vector<ImageBall> balls;
    balls.reserve(linears.size() + 1);
    balls.push_back({world_centre, math::Aabb{world_centre - r, world_centre + r}, nullptr, 0u,
                     kNoImage, 0u});
    for (const math::cfloat4x4& linear : linears) {
        const cfloat3 c = image_centre(xf, linear, world_centre);
        const std::size_t k = balls.size();
        balls.push_back({c, math::Aabb{c - r, c + r}, &linear, k, kNoImage, k});
    }
    if (balls.size() > 1) group_coincident(xf, radius, &balls);
    return balls;
}

// -- reach ----------------------------------------------------------------------

// Does the drag reach this item at all? Compared as boxes: the item's influence
// bound is one already, and a sphere's is exact enough for a cull whose only
// job is to not emit a deformer that provably does nothing.
bool reaches(const math::Aabb& influence, const math::Aabb& drag) {
    if (influence.empty()) return false;
    if (influence.is_infinite()) return true;  // a non-local op: never cull it
    return !(influence.max.x < drag.min.x || influence.min.x > drag.max.x ||
             influence.max.y < drag.min.y || influence.min.y > drag.max.y ||
             influence.max.z < drag.min.z || influence.min.z > drag.max.z);
}

// How many of the drag's images this item sees. EXACTLY the compiler's
// participation gate (tape_build.cpp, emit_item): an item that opted out of
// the mirror, or a feathered volume replace, is emitted once and has no copy
// for a reflected image to reach. The two tests must stay identical, or the
// brush warps an item where the compiler put no geometry.
std::size_t images_seen(const scene::Node& n, std::size_t images) {
    if (images == 1) return 1u;  // no symmetry: nothing to gate, and no call per item
    const bool participates = n.mirror && !scene::item_is_feathered_replace(n);
    return participates ? images : 1u;
}

bool any_reaches(const math::Aabb& own, const std::vector<ImageBall>& balls, std::size_t seen) {
    for (std::size_t k = 0; k < seen; ++k) {
        if (reaches(own, balls[k].box)) return true;
    }
    return false;
}

// -- preparing one item -----------------------------------------------------------

// The item's share of the drag, short of the displacement: its frame, and each
// image it sees mapped into that frame with whether it reaches the item's OWN
// bound. Called only for an item some image reaches.
PreparedMove prepare_item(const scene::Layer& layer, const scene::Node& n, scene::NodeId id,
                          const math::Aabb& own, const std::vector<ImageBall>& balls,
                          std::size_t seen, const MoveSettings& settings) {
    // The item's world frame. `layer.xform * node.xform` PLUS the item's
    // own per-axis scale, which is innermost and which this composition
    // used to drop (#320): the tape applies the whole inverse before the
    // deformer chain, so a warp authored in the placed frame lands
    // somewhere the squashed item is not. Dragging the surface of an item
    // scaled 3x on one axis did nothing at all.
    // The LAYER's per-axis scale sits BETWEEN the two placements
    // (`layer.xform * diag(L) * node.xform * diag(N)`, #373), so it cannot ride
    // the composed `math::Transform` the unsquashed path uses. Only that path
    // is taken when the layer carries no per-axis scale, which is every
    // document written before this one — and the branch is the reason a
    // uniform layer's warps stay bit-identical rather than merely equal: the
    // squashed path applies two placements where this one applies their
    // composition, and those round differently.
    const bool layer_squashed = scene::layer_is_squashed(layer);
    const math::Transform world = layer.xform * n.xform;
    const float scale = world.scale != 0.0f ? world.scale : 1.0f;
    const cfloat3 axes = n.scale_axes;
    const float layer_scale = layer.xform.scale != 0.0f ? layer.xform.scale : 1.0f;
    const float node_scale = n.xform.scale != 0.0f ? n.xform.scale : 1.0f;

    // World -> the space the item's DEFORMER chain runs in, with the layer's
    // per-axis scale taken off between the two placements.
    auto to_item_local = [&](cfloat3 p) {
        return scene::into_scaled_local(
            n.xform.apply_inverse(
                scene::into_scaled_local(layer.xform.apply_inverse(p), layer.scale_axes)),
            axes);
    };

    PreparedMove prepared;
    prepared.node = id;
    prepared.images.reserve(seen);
    for (std::size_t k = 0; k < seen; ++k) {
        PreparedImage image;
        // A point maps through the full inverse; the DISPLACEMENT is a vector
        // and takes the rotation and the scale but not the translation — that
        // half is `resolve_prepared_move`, held to the same order and the same
        // operations. The per-axis scale comes off both last, because it is
        // innermost.
        image.local_centre = layer_squashed
                                 ? to_item_local(balls[k].centre)
                                 : scene::into_scaled_local(
                                       world.apply_inverse(balls[k].centre), axes);
        if (balls[k].linear) {
            image.linear = *balls[k].linear;
            image.copy = true;
        }
        image.reaches = reaches(own, balls[k].box);
        // A leader always precedes its followers, so it is inside `seen`
        // whenever they are.
        image.leader = balls[k].leader == k ? PreparedImage::kOwnBall : balls[k].leader;
        // ...and a follower is always after its leader, so an item that sees
        // only the drag itself ends its list there.
        image.next = balls[k].next < seen ? balls[k].next : PreparedImage::kOwnBall;
        prepared.images.push_back(image);
    }
    prepared.inverse_rotation = world.rotation.conjugate();
    prepared.world_scale = scale;
    prepared.scale_axes = axes;
    prepared.layer_xform = layer.xform;
    // The squashed path's own half of the displacement map, so
    // resolve_prepared_move needs no Layer and the two halves stay one
    // arithmetic. Left at the identity otherwise, which is the branch above.
    prepared.layer_squashed = layer_squashed;
    prepared.layer_scale_axes = layer.scale_axes;
    prepared.layer_rotation = layer.xform.rotation;
    prepared.node_rotation = n.xform.rotation;
    prepared.layer_uniform_scale = layer_scale;
    prepared.node_uniform_scale = node_scale;
    // A grab carries ONE radius, and a squashed frame turns the artist's
    // world-space sphere into a local ELLIPSOID, so no scalar is exact.
    // Dividing by the LARGEST factor is the conservative reading: every
    // world reach `R * s_i * scale` is then at most the radius circled, so
    // a drag never takes geometry the artist did not enclose. Under-reach
    // is recoverable by dragging again; over-reach is not. This is the same
    // instinct cscale_nu_dist follows by taking the smallest factor.
    // The LAYER's largest factor joins it for the same reason and by the same
    // rule: the artist's world sphere is stretched by both, and dividing by the
    // largest of each keeps every world reach at most the radius circled.
    prepared.local_radius =
        settings.radius /
        (scale * scene::scale_axes_reach(axes) * scene::scale_axes_reach(layer.scale_axes));
    prepared.ease = settings.ease;
    prepared.gesture_id = settings.gesture_id;
    prepared.front_only = settings.front_only;
    return prepared;
}

// The items a drag reaches, and their frames. Everything here depends on the
// anchor and the radius and NOT on the displacement, which is what lets a live
// drag pay for it once — see brush/move.h.
void collect(const scene::SdfContent& content, const scene::Layer& layer,
             const std::vector<scene::NodeId>& ids, const std::vector<ImageBall>& balls,
             const MoveSettings& settings, std::vector<PreparedMove>* out,
             MovePrepareStats* stats) {
    for (scene::NodeId id : ids) {
        const scene::Node* n = content.find(id);
        if (!n) continue;
        if (stats) ++stats->visited;
        if (!n->visible) continue;
        if (n->is_group) {
            // A group takes no warp of its own: its transform does not reach
            // its children here, so the children are what carry the drag.
            collect(content, layer, n->children, balls, settings, out, stats);
            continue;
        }
        // Reach is decided here, before anything is prepared: the resolver
        // visits every visible item in the layer and a drag reaches a handful,
        // so the frame maths and the allocations are paid for the handful, not
        // the layer. Built per item first, it measured 45 ns per visible item
        // -- 1.4-1.6x on BM_MoveDrag1000/10000 for output that was
        // byte-identical.
        const math::Aabb own = scene::item_own_influence_bound(*n, layer);
        const std::size_t seen = images_seen(*n, balls.size());
        if (!any_reaches(own, balls, seen)) continue;
        if (stats) ++stats->reached;
        out->push_back(prepare_item(layer, *n, id, own, balls, seen, settings));
    }
}

// -- ordering -------------------------------------------------------------------

// Is this chain entry a deformer of the gesture `warp` continues — the same
// KIND, centre and radius as one of the gesture's images on this item, one
// frame earlier? Matched against every image the item can see, not only the
// ones reaching it this frame (see MoveWarp::gesture); `deformers` is a subset
// of `gesture` when the resolver built the warp, and the whole identity when a
// caller did.
//
// The KIND is part of the match rather than fixed at grab, because a magnify
// gesture (brush/magnify.h) continues itself by the same rule and a pinch must
// not replace a drag's leading grab that happens to share its ball.
// IS THIS GRAB ONE THIS GESTURE ALREADY EMITTED?
//
// When the host NAMED the gesture, that name answers it and nothing else is
// consulted: the anchor may move, the radius may follow pressure, and either
// may be recomputed through any float path -- it is still one drag, and one
// warp. A named gesture never matches an unnamed one, or a differently named
// one, so two live drags cannot fold into each other.
//
// When it did not, the old rule stands exactly: type, centre and radius by raw
// float ==. That is correct for a drag holding both fixed, which is what the
// original design assumed, and it is what a loaded document falls back to
// because the id is not serialised.
//
// NOT AN EPSILON, deliberately. A tolerance would paper over a recomputed
// anchor while silently folding two genuinely distinct gestures -- a re-grab a
// hair from the last -- into one, which changes the document rather than
// speeding it up. Quantising is worse: it makes the fold depend on where the
// gesture happens to sit relative to the grid.
bool continues_gesture(const scene::Deformer& lead, const MoveWarp& warp) {
    const auto same_identity = [&](const scene::Deformer& f) {
        if (lead.type != f.type) return false;
        if (lead.gesture_id != 0 || f.gesture_id != 0) return lead.gesture_id == f.gesture_id;
        return lead.k == f.k && lead.a == f.a && lead.b == f.b && lead.c == f.c;
    };
    return std::any_of(warp.gesture.begin(), warp.gesture.end(), same_identity) ||
           std::any_of(warp.deformers.begin(), warp.deformers.end(), same_identity);
}

}  // namespace

// A STRICT TOTAL ORDER over a gesture's deformers on one item — centre, then
// payload — descending. Never by which image produced them: the +x drag's
// "self" image is the -x drag's "reflection", and once the item is not itself
// plane-symmetric the two orders are two different fields (325 samples apart on
// an off-centre straddler). The payload is in the key because a drag centred ON
// the plane gives both images one centre and opposite pulls, and a centre-only
// key left that order gesture-dependent (1,365 samples apart). Bitwise-equal
// values compare equal and keep their insertion order, +0/-0 included: the
// kernel cannot see the sign of a zero, so two such deformers are one
// deformation whichever comes first.
//
// Shared with the magnify resolver, which needs the same determinism for the
// same reason and whose payload sits in the same `ext` slots.
void order_by_value(std::vector<scene::Deformer>* deformers) {
    // One deformer needs no order, and stable_sort on a non-trivially-copyable
    // element takes a temporary buffer whatever the count: on the live frame
    // that was one allocation per item for nothing (measured 28 ns an item).
    if (deformers->size() < 2) return;
    const auto key = [](const scene::Deformer& d) {
        return std::make_tuple(d.k, d.a, d.b, d.ext[0], d.ext[1], d.ext[2]);
    };
    std::stable_sort(deformers->begin(), deformers->end(),
                     [&key](const scene::Deformer& x, const scene::Deformer& y) {
                         return key(x) > key(y);
                     });
}

std::vector<DragImage> drag_images(const scene::Layer& layer, cfloat3 world_centre,
                                   cfloat3 world_displacement) {
    std::vector<DragImage> out;
    out.push_back({world_centre, world_displacement});
    for (const math::cfloat4x4& linear : copy_linears(layer)) {
        out.push_back({image_centre(layer.xform, linear, world_centre),
                       image_displacement(layer.xform, linear, world_displacement)});
    }
    return out;
}

std::vector<PreparedMove> prepare_move(const scene::Layer& layer, cfloat3 world_centre,
                                       const MoveSettings& settings,
                                       MovePrepareStats* out_stats) {
    std::vector<PreparedMove> out;
    if (out_stats) *out_stats = MovePrepareStats{};
    if (!(settings.radius > 0.0f)) return out;
    if (!layer.sdf) return out;
    const std::vector<math::cfloat4x4> linears = copy_linears(layer);
    const std::vector<ImageBall> balls =
        image_balls(layer.xform, world_centre, linears, settings.radius);
    collect(*layer.sdf, layer, layer.sdf->roots, balls, settings, &out, out_stats);
    return out;
}

MoveWarp resolve_prepared_move(const PreparedMove& prepared, cfloat3 total_world_displacement) {
    MoveWarp warp;
    resolve_prepared_move(prepared, total_world_displacement, &warp);
    return warp;
}

namespace {

// The world displacement one image pulls with: the drag's own, or its
// reflection or rotation for a copy.
cfloat3 image_world_displacement(const PreparedMove& prepared, const PreparedImage& image,
                                 cfloat3 total_world_displacement) {
    return image.copy
               ? image_displacement(prepared.layer_xform, image.linear, total_world_displacement)
               : total_world_displacement;
}

// A displacement is a vector: the same map as a point, without the
// translation. Held to the same order and the same operations as
// `prepare_item`'s centre, on both branches, or a grab's centre and its
// displacement would describe different spaces.
cfloat3 local_displacement(const PreparedMove& prepared, cfloat3 world_displacement) {
    if (prepared.layer_squashed) {
        return scene::into_scaled_local(
            prepared.node_rotation.conjugate().rotate(scene::into_scaled_local(
                prepared.layer_rotation.conjugate().rotate(world_displacement) /
                    prepared.layer_uniform_scale,
                prepared.layer_scale_axes)) /
                prepared.node_uniform_scale,
            prepared.scale_axes);
    }
    return scene::into_scaled_local(
        prepared.inverse_rotation.rotate(world_displacement) / prepared.world_scale,
        prepared.scale_axes);
}

void emit_grab(const PreparedMove& prepared, const PreparedImage& image, bool reaching,
               cfloat3 world_displacement, MoveWarp* warp) {
    scene::Deformer grab = scene::Deformer::grab(
        image.local_centre, prepared.local_radius, local_displacement(prepared, world_displacement),
        prepared.ease, prepared.front_only);
    // Stamped on every image, so a mirrored drag's copies belong to the
    // same gesture as the one that produced them (#533).
    grab.gesture_id = prepared.gesture_id;
    // Each grab lands in exactly one of the two: the reaching ones are the
    // warp, the others only its identity. With one image nothing lands in
    // `gesture`, and a fresh warp costs one allocation; a reused one, none.
    if (reaching) {
        warp->deformers.push_back(std::move(grab));
    } else {
        warp->gesture.push_back(std::move(grab));
    }
}

// The images that share leader `lead`'s ball: how many, whether any reaches
// the item, and the mean of their displacements.
struct CoincidentGroup {
    std::size_t count = 0;
    bool reaches = false;
    cfloat3 mean = cf3(0, 0, 0);
};

CoincidentGroup coincident_group(const PreparedMove& prepared, std::size_t lead,
                                 cfloat3 total_world_displacement) {
    CoincidentGroup group;
    cfloat3 sum = cf3(0, 0, 0);
    for (std::size_t j = lead; j != PreparedImage::kOwnBall; j = prepared.images[j].next) {
        const PreparedImage& image = prepared.images[j];
        ++group.count;
        group.reaches = group.reaches || image.reaches;
        sum = sum + image_world_displacement(prepared, image, total_world_displacement);
    }
    group.mean = sum / static_cast<float>(group.count);
    return group;
}

// Images sharing one ball are ONE brush for what they agree on and separate
// brushes only for what they do not (#663, and brush/move.h). `tolerance`
// absorbs the rounding a reflection picks up through a placed layer, so a
// pull along the plane on any layer resolves to the drag's own grab.
void emit_coincident(const PreparedMove& prepared, std::size_t lead,
                     cfloat3 total_world_displacement, MoveWarp* warp) {
    const CoincidentGroup group = coincident_group(prepared, lead, total_world_displacement);
    const PreparedImage& leader = prepared.images[lead];
    const float tolerance = 1e-5f * kernel::clength(total_world_displacement);
    const auto residual = [&](const PreparedImage& image) {
        return image_world_displacement(prepared, image, total_world_displacement) - group.mean;
    };
    bool agree = true;
    for (std::size_t j = lead; j != PreparedImage::kOwnBall && agree; j = prepared.images[j].next)
        agree = kernel::clength(residual(prepared.images[j])) <= tolerance;
    // All the same pull: the leader's own grab, exactly as without symmetry.
    if (agree) {
        emit_grab(prepared, leader, group.reaches,
                  image_world_displacement(prepared, leader, total_world_displacement), warp);
        return;
    }
    // The shared part once — zero, and so absent, for a pull straight across a
    // plane, which keeps that drag's two opposite grabs bit for bit — then
    // what each image adds beyond it.
    if (kernel::clength(group.mean) > tolerance)
        emit_grab(prepared, leader, group.reaches, group.mean, warp);
    for (std::size_t j = lead; j != PreparedImage::kOwnBall; j = prepared.images[j].next) {
        const cfloat3 own = residual(prepared.images[j]);
        if (kernel::clength(own) > tolerance) emit_grab(prepared, leader, group.reaches, own, warp);
    }
}

}  // namespace

void resolve_prepared_move(const PreparedMove& prepared, cfloat3 total_world_displacement,
                           MoveWarp* out) {
    MoveWarp& warp = *out;
    warp.node = prepared.node;
    warp.deformers.clear();
    warp.gesture.clear();
    for (std::size_t k = 0; k < prepared.images.size(); ++k) {
        const PreparedImage& image = prepared.images[k];
        if (image.leader != PreparedImage::kOwnBall) continue;  // resolved with its leader
        if (image.next != PreparedImage::kOwnBall) {  // it leads a group
            emit_coincident(prepared, k, total_world_displacement, &warp);
            continue;
        }
        emit_grab(prepared, image, image.reaches,
                  image_world_displacement(prepared, image, total_world_displacement), &warp);
    }
    // A non-local item (infinite own bound) takes one grab per distinct image
    // it sees: its field reaches everywhere, so every image's ball is on it.
    order_by_value(&warp.deformers);
}

std::vector<MoveWarp> move_brush(const scene::Layer& layer, cfloat3 world_centre,
                                 cfloat3 world_displacement, const MoveSettings& settings) {
    std::vector<MoveWarp> out;
    if (kernel::clength(world_displacement) <= 0.0f) return out;
    // Prepare-then-resolve, so there is ONE resolver and a live drag cannot
    // drift away from what a commit through this entry point would produce.
    // Preparing per call is what this always did — the traversal is the same
    // one — and the guards it keeps are the ones only it can answer.
    const std::vector<PreparedMove> prepared = prepare_move(layer, world_centre, settings);
    out.reserve(prepared.size());
    for (const PreparedMove& p : prepared)
        out.push_back(resolve_prepared_move(p, world_displacement));
    return out;
}

std::vector<scene::Deformer> moved_chain(const scene::Node& node, const MoveWarp& warp) {
    return moved_chain(node.deformers, warp);
}

std::vector<scene::Deformer> moved_chain(const std::vector<scene::Deformer>& existing,
                                         const MoveWarp& warp) {
    // FRONT, not back. deformers[0] warps the point first and is therefore the
    // outermost warp on the geometry, which is what "drag the assembled shape"
    // means; appended, the grab's region weight would be read at a point the
    // existing deformers had already moved.
    std::vector<scene::Deformer> chain;
    chain.reserve(warp.deformers.size() + existing.size());
    chain.insert(chain.end(), warp.deformers.begin(), warp.deformers.end());
    // ...and REPLACING the leading warps when they belong to the drag still in
    // progress, rather than stacking more on top of them. A drag holds its
    // centre and radius fixed and only grows the displacement, so those two
    // identify it without a drag id having to be threaded through. Without
    // this a drag appends one deformer per frame: the chain grows without
    // bound, and the declared Lipschitz compounds with every frame of it.
    //
    // Every leading grab that matches ANY image of this gesture is this
    // gesture's, one frame earlier — an item both images reach carries two,
    // and an image that starts reaching the item mid-drag (the first frames'
    // pull widened its bound) leaves it with one to replace and one to add.
    // Stopping at the first entry that does not match is what keeps a
    // different gesture's grab, and any other deformer, in place.
    std::size_t skip = 0;
    while (skip < existing.size() && continues_gesture(existing[skip], warp)) ++skip;
    chain.insert(chain.end(), existing.begin() + static_cast<std::ptrdiff_t>(skip),
                 existing.end());
    return chain;
}

}  // namespace brush
}  // namespace clay
