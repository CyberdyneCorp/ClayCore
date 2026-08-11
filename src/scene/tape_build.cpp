#include "clay/scene/bounds.h"
#include "clay/scene/curve.h"
#include "clay/scene/tape.h"

namespace clay {
namespace scene {

using kernel::cfloat4x4;
using kernel::CTapeInstr;

namespace {

// Repetition preserves exactness only when the item plus its rounding and blend
// influence fits inside its half-cell (docs/01 2.4). Checked rather than
// assumed: an overflowing array is a bound field, not a distance.
bool repeat_preserves_exactness(const Node& item) {
    math::Aabb local = prim_local_bounds(item);
    float influence =
        kernel::cmax(item.rounding, 0.0f) + kernel::cmax(item.blend.support(), item.blend.k);
    if (item.repeat.type == kernel::crepeat_radial) {
        int count = static_cast<int>(item.repeat.spacing.x);
        float ring = kernel::cabs(item.repeat.spacing.y);
        float reach = 0.0f;
        if (!local.empty()) {
            for (int i = 0; i < 4; ++i) {
                float x = (i & 1) ? local.max.x : local.min.x;
                float z = (i & 2) ? local.max.z : local.min.z;
                reach = kernel::cmax(reach, kernel::clength(kernel::cf2(x, z)));
            }
        }
        // the copy must fit inside its angular sector at its radius
        float sector_reach =
            ring * kernel::csin(3.14159265f / kernel::cmax((float)count, 2.0f));
        return count >= 2 && (reach + influence) <= sector_reach;
    }
    if (local.empty()) return true;
    const float sx = item.repeat.spacing.x, sy = item.repeat.spacing.y,
                sz = item.repeat.spacing.z;
    float hx = kernel::cmax(kernel::cabs(local.min.x), kernel::cabs(local.max.x));
    float hy = kernel::cmax(kernel::cabs(local.min.y), kernel::cabs(local.max.y));
    float hz = kernel::cmax(kernel::cabs(local.min.z), kernel::cabs(local.max.z));
    return (hx + influence) <= sx * 0.5f && (hy + influence) <= sy * 0.5f &&
           (hz + influence) <= sz * 0.5f;
}

// The guide's tightest turn against the widest profile: a point at perpendicular
// offset r inside a bend of radius R is compressed by R / (R - r), which diverges
// as the profile outgrows the bend. Not refused — a guide is editable after the
// fact — so it degrades to a very small step instead.
kernel::CFieldInfo swept_field_info(const Node& item) {
    std::vector<StrokePoint> guide = curve_is_polyline(item.stroke, false)
                                         ? item.stroke
                                         : tessellate_curve(item.stroke, false,
                                                            item.curve_tolerance);
    // The CIRCUMRADIUS of each consecutive triple, not the turn angle over the
    // arc: the angle estimate is fooled by tessellation density, reading a
    // finely-sampled gentle curve as a tight one because short segments
    // accumulate angle. A circumradius is the radius of the circle through the
    // three points and is stable however finely the guide is sampled.
    float tightest = 1e6f;
    float total_len = 0.0f;
    for (std::size_t i = 1; i < guide.size(); ++i)
        total_len += kernel::clength(guide[i].pos - guide[i - 1].pos);
    for (std::size_t i = 1; i + 1 < guide.size(); ++i) {
        kernel::cfloat3 u = guide[i].pos - guide[i - 1].pos;
        kernel::cfloat3 v = guide[i + 1].pos - guide[i].pos;
        kernel::cfloat3 w = guide[i + 1].pos - guide[i - 1].pos;
        float lu = kernel::clength(u), lv = kernel::clength(v), lw = kernel::clength(w);
        float area2 = kernel::clength(kernel::ccross(u, v));  // 2 * triangle area
        if (area2 < 1e-9f) continue;                          // collinear: straight
        tightest = kernel::cmin(tightest, lu * lv * lw / (2.0f * area2));
    }
    float widest = 0.0f;
    for (std::size_t i = 0; i < item.profiles.size(); ++i) {
        const std::vector<kernel::cfloat2>& pts =
            i < item.profile_polygons.size() ? item.profile_polygons[i]
                                             : std::vector<kernel::cfloat2>{};
        kernel::cfloat2 e = profile_extent_of(item.profiles[i], pts);
        widest = kernel::cmax(widest, kernel::cmax(e.x, e.y));
    }
    // The profiles are lerped along the guide the way a loft's are along Z, so
    // that term applies over the arc length between them.
    float spread = 2.0f * widest;
    float span = kernel::cmax(total_len, 1e-6f) /
                 kernel::cmax((float)(item.profiles.size() - 1), 1.0f);
    return kernel::cfi_swept(widest, tightest, spread, span,
                             ease_max_slope(static_cast<std::uint8_t>(item.prim.params[0])));
}

// The extra width the cull test needs when this content holds a feathered
// volume replace. The feathered combine moves the accumulated value by up to
// the volume's band (the kernel clamps the correction there), so an item
// whose field the caller's dilation put just beyond the clamp can still steer
// a value back inside it. Testing against the region dilated by that band
// restores the contract the CullRegion states: band-clamped results identical
// to the full tape. Zero — the common case — leaves the test exactly as it was.
float feather_cull_pad(const SdfContent& content, const Layer& layer) {
    float pad = 0.0f;
    for (const auto& [id, n] : content.nodes()) {
        (void)id;
        if (n.is_group || !n.visible) continue;
        if (n.op != Op::Replace || !prim_is_volume(n.prim.type)) continue;
        if (n.volume && n.volume->feather() > 0.0f)
            pad = kernel::cmax(pad, n.volume->band() * layer.xform.scale * n.xform.scale);
    }
    return pad;
}

struct Compiler {
    Tape tape;
    const CullRegion* cull;
    // What cull tests actually intersect against: the caller's region, wider
    // by feather_cull_pad when a feathered replace is present.
    math::Aabb cull_test;
    // Reused across items so a document full of curves does not allocate a
    // fresh vector per item; only ever read between assignment and use.
    std::vector<StrokePoint> scratch_curve;

    void begin_cull(const CullRegion* cull_region, float pad) {
        cull = cull_region;
        if (cull) cull_test = pad > 0.0f ? cull->region.dilated(pad) : cull->region;
    }

    bool culled(const math::Aabb& bound) const {
        if (!cull) return false;
        if (bound.is_infinite()) return false;
        return !bound.intersects(cull_test);
    }

    // -- emission ------------------------------------------------------------

    void emit_prim(unsigned int op, const cfloat4x4& inv, float scale, float round,
                   kernel::cfloat3 color, const float* prim_params, int prim_param_count,
                   const std::vector<Deformer>& deformers, const Repeat& repeat = Repeat{}) {
        CTapeInstr instr;
        instr.op = op;
        instr.param_offset = static_cast<unsigned int>(tape.params.size());
        tape.instrs.push_back(instr);
        const float xf[12] = {inv.c0.x, inv.c0.y, inv.c0.z, inv.c1.x, inv.c1.y, inv.c1.z,
                              inv.c2.x, inv.c2.y, inv.c2.z, inv.c3.x, inv.c3.y, inv.c3.z};
        tape.params.insert(tape.params.end(), xf, xf + 12);
        tape.params.push_back(scale);
        tape.params.push_back(round);
        tape.params.push_back(color.x);
        tape.params.push_back(color.y);
        tape.params.push_back(color.z);
        // prim params are fixed-width so the deformer block lands at a
        // known offset for the interpreter
        tape.params.insert(tape.params.end(), prim_params, prim_params + prim_param_count);
        for (int i = prim_param_count; i < CLAY_TAPE_PRIM_PARAMS; ++i) tape.params.push_back(0.0f);
        // repeat record, then the deformer block
        tape.params.push_back(static_cast<float>(repeat.type));
        tape.params.push_back(repeat.spacing.x);
        tape.params.push_back(repeat.spacing.y);
        tape.params.push_back(repeat.spacing.z);
        tape.params.push_back(repeat.counts.x);
        tape.params.push_back(repeat.counts.y);
        tape.params.push_back(repeat.counts.z);
        tape.params.push_back(static_cast<float>(deformers.size()));
        for (const Deformer& d : deformers) {
            tape.params.push_back(static_cast<float>(d.type));
            tape.params.push_back(d.k);
            tape.params.push_back(d.a);
            tape.params.push_back(d.b);
            tape.params.push_back(d.c);
            tape.params.push_back(static_cast<float>(d.ease));
            // The record is fixed width, so always emit the extension slots;
            // the types that do not use them read zeros.
            for (int i = 0; i < 6; ++i) tape.params.push_back(d.ext[i]);
        }
    }

    // Far-field accumulator seed for material-creating combines whose chain
    // is empty (kernel/tape.h ctape_empty). Emitted explicitly because the
    // postfix stack may hold outer-chain values, so interpreter underflow
    // seeding cannot be relied on inside group subtrees.
    void emit_empty(kernel::cfloat3 color) {
        kernel::cfloat4x4 ident;
        ident.c0 = kernel::cf4(1, 0, 0, 0);
        ident.c1 = kernel::cf4(0, 1, 0, 0);
        ident.c2 = kernel::cf4(0, 0, 1, 0);
        ident.c3 = kernel::cf4(0, 0, 0, 1);
        float none = 0.0f;
        emit_prim(kernel::ctape_empty, ident, 1.0f, 0.0f, color, &none, 0, {});
    }

    // Blob offset of the most recently emitted volume header; consumed by
    // emit_replace_feather immediately after the emit_item that set it.
    std::size_t last_volume_blob = 0;

    // Whether this item is a volume placed with Replace that asked for a
    // feathered placement. The one predicate the mirror skip, the combine
    // choice and the field-info fold all share, so they cannot disagree.
    static bool is_feathered_replace(const Node& item) {
        return item.op == Op::Replace && prim_is_volume(item.prim.type) && item.volume &&
               item.volume->feather() > 0.0f;
    }

    // The feathered replace: mode ccombine_replace_feather with the volume's
    // header offset and the instance's world-to-local transform appended, so
    // the kernel can weigh the crossfade by the inset into the sampled box.
    void emit_replace_feather(const Node& item, const Layer& layer) {
        math::Transform world = layer.xform * item.xform;
        cfloat4x4 inv = world.inverse_matrix();
        CTapeInstr instr;
        instr.op = kernel::ctape_combine;
        instr.param_offset = static_cast<unsigned int>(tape.params.size());
        tape.instrs.push_back(instr);
        tape.params.push_back(static_cast<float>(static_cast<int>(kernel::ccombine_replace_feather)));
        tape.params.push_back(0.0f);  // profile: unread by this mode
        tape.params.push_back(0.0f);  // k: unread
        tape.params.push_back(0.0f);  // rb: unread
        tape.params.push_back(static_cast<float>(last_volume_blob));
        const float xf[12] = {inv.c0.x, inv.c0.y, inv.c0.z, inv.c1.x, inv.c1.y, inv.c1.z,
                              inv.c2.x, inv.c2.y, inv.c2.z, inv.c3.x, inv.c3.y, inv.c3.z};
        tape.params.insert(tape.params.end(), xf, xf + 12);
        tape.params.push_back(world.scale);
    }

    // rb: second radius of the two-parameter extended modes (groove/tongue
    // half-width), already in world units; 0 for everything else.
    void emit_combine(Op op, Blend blend, float rb, const Transition* transition = nullptr) {
        CTapeInstr instr;
        instr.op = kernel::ctape_combine;
        instr.param_offset = static_cast<unsigned int>(tape.params.size());
        tape.instrs.push_back(instr);
        tape.params.push_back(static_cast<float>(static_cast<int>(op)));
        tape.params.push_back(static_cast<float>(static_cast<int>(blend.profile)));
        tape.params.push_back(blend.k);
        tape.params.push_back(rb);
        // transition modes append their own parameters after the shared four
        if (op == Op::TransitionLinear) {
            const Transition& t = transition ? *transition : default_transition_;
            for (float v : {t.a.x, t.a.y, t.a.z, t.b.x, t.b.y, t.b.z}) tape.params.push_back(v);
            tape.params.push_back(static_cast<float>(t.ease));
        } else if (op == Op::TransitionRadial) {
            const Transition& t = transition ? *transition : default_transition_;
            tape.params.push_back(t.r0);
            tape.params.push_back(t.r1);
            tape.params.push_back(static_cast<float>(t.ease));
        }
    }

    Transition default_transition_{};

    // -- field-info bookkeeping ----------------------------------------------

    // `round_world` is the item's rounding in WORLD units, which relief reads
    // as its falloff width. Passed in rather than recomputed from item.rounding:
    // that one is local, and dividing an amplitude by a width that is too large
    // understates the slope — the direction that makes a marcher overstep.
    void fold_info(const Node& item, Op op, bool smooth, float round_world) {
        kernel::CFieldInfo prim_info =
            prim_is_bound_field(item.prim.type) ? kernel::cfi_bound() : kernel::cfi_exact();
        if (item.repeat.active() && !repeat_preserves_exactness(item))
            prim_info = kernel::cfi_bound();

        if (prim_is_volume(item.prim.type)) {
            // Interpolated samples are not an exact distance, and where there
            // are none the value is a lower bound rather than a distance.
            // The interpolant can also be steeper than the field it samples;
            // see cfi_volume for why that is sqrt(3) and not 1.
            prim_info = kernel::cfi_volume(item.volume ? item.volume->sample_lipschitz() : 1.0f);
        } else if (prim_is_swept(item.prim.type) && item.profiles.size() >= 2) {
            prim_info = swept_field_info(item);
        } else if (prim_is_loft(item.prim.type) && item.profiles.size() >= 2) {
            // Interpolating two profile fields along Z adds |da - db| over the
            // depth they are mixed across. The profiles' combined extent
            // bounds how far apart the two fields can be where it matters, and
            // the easing curve steepens the ramp by its own maximum slope.
            math::Aabb local = prim_local_bounds(item);
            float spread = local.empty() ? 0.0f
                                         : kernel::clength(kernel::cf2(local.extent().x,
                                                                       local.extent().y));
            float depth = kernel::cmax(2.0f * item.prim.params[0], 1e-6f) /
                          kernel::cmax((float)(item.profiles.size() - 1), 1.0f);
            prim_info = kernel::cfi_loft(
                spread, depth, ease_max_slope(static_cast<std::uint8_t>(item.prim.params[1])));
        }

        // domain warps break the metric: fold the chain's Lipschitz factor
        // (shared with the influence bound) so the safe step scale drops
        float deform_l = deformer_lipschitz(item);
        if (deform_l > 1.0f) prim_info = kernel::CFieldInfo{false, prim_info.lipschitz * deform_l};
        if (deformers_break_exactness(item)) prim_info.is_exact = false;
        if (op_is_transition(op)) {
            // A lerp of two fields is not a distance. |d1 - d2| is bounded by
            // how far apart the two surfaces can be, and both live inside the
            // union of their influence bounds, so its diagonal is a safe
            // bound. The weight's slope is the easing curve's steepest
            // measured rise over the transition's span.
            const math::Aabb& region = tape.bounds;  // already includes this item
            float diff_bound = region.empty() || region.is_infinite()
                                   ? 1e3f
                                   : kernel::clength(region.extent());
            float span = item.op == Op::TransitionLinear
                             ? kernel::cmax(kernel::clength(item.transition.b - item.transition.a),
                                            1e-6f)
                             : kernel::cmax(kernel::cabs(item.transition.r1 - item.transition.r0),
                                            1e-6f);
            span /= kernel::cmax(ease_max_slope(item.transition.ease), 1e-6f);
            tape.info = kernel::cfi_transition(tape.info, prim_info, diff_bound, span);
        } else if (op == Op::Relief || op == Op::Incise) {
            // Relief does not blend two fields — it offsets the accumulated one
            // by a weighted amplitude, so its cost is that term's gradient, not
            // a blend's. The item's own field never reaches the result, which
            // is why prim_info plays no part here.
            tape.info = kernel::cfi_relief(tape.info, item.blend.k, round_world);
        } else if (is_feathered_replace(item)) {
            // The crossfade adds its clamped correction over the feather; see
            // cfi_replace_feather. Declared whenever the item ASKS for a
            // feather: the seeded-empty chain emits the hard mode instead, and
            // for it this bound is merely conservative, never an understatement.
            tape.info = kernel::cfi_replace_feather(tape.info, prim_info, item.volume->band(),
                                                    item.volume->feather());
        } else if (op_is_extended(op))
            tape.info = kernel::cfi_extended_blend(tape.info, prim_info, op_is_diagonal(op));
        else
            tape.info = smooth ? kernel::cfi_smooth_blend(tape.info, prim_info)
                               : kernel::cfi_boolean(tape.info, prim_info);
    }

    // The profile records for a loft or a sweep: vertices first so their
    // offsets are known by the time the record that points at them is
    // written, then one CLAY_TAPE_PROFILE_FLOATS block per profile.
    std::size_t emit_profile_records(const Node& item) {
        const std::size_t count = item.profiles.size();
        std::vector<std::size_t> vertex_offsets(count, 0);
        for (std::size_t i = 0; i < count; ++i) {
            if (!item.profiles[i].is_polygon()) continue;
            vertex_offsets[i] = tape.blob.size();
            const std::vector<kernel::cfloat2>& pts =
                i < item.profile_polygons.size() ? item.profile_polygons[i]
                                                 : std::vector<kernel::cfloat2>{};
            for (const kernel::cfloat2& v : pts) {
                tape.blob.push_back(v.x);
                tape.blob.push_back(v.y);
            }
        }
        std::size_t records = tape.blob.size();
        for (std::size_t i = 0; i < count; ++i) {
            const Profile& profile = item.profiles[i];
            tape.blob.push_back(static_cast<float>(profile.type));
            if (profile.is_polygon()) {
                const std::vector<kernel::cfloat2>& pts =
                    i < item.profile_polygons.size() ? item.profile_polygons[i]
                                                     : std::vector<kernel::cfloat2>{};
                tape.blob.push_back(static_cast<float>(vertex_offsets[i]));
                tape.blob.push_back(static_cast<float>(pts.size()));
                tape.blob.push_back(0.0f);
                tape.blob.push_back(0.0f);
            } else {
                for (int k = 0; k < 4; ++k) tape.blob.push_back(profile.params[k]);
            }
        }
        return records;
    }

    // A sweep's guide, with a PARALLEL-TRANSPORTED frame per vertex.
    //
    // Transport is sequential — each frame is the previous one rotated by the
    // minimum turn that carries the old tangent onto the new — so it cannot be
    // done per sample, which is exactly why the frames go in the blob. A
    // Frenet frame would flip at an inflection and be undefined where the
    // guide is straight; this one does neither.
    void emit_swept(const Node& item, const kernel::cfloat4x4& inv_world, float scale,
                    float round_world) {
        std::vector<StrokePoint> guide =
            curve_is_polyline(item.stroke, false)
                ? item.stroke
                : tessellate_curve(item.stroke, false, item.curve_tolerance);
        if (guide.size() < 2 || item.profiles.size() < 2) return;

        std::size_t guide_offset = tape.blob.size();
        kernel::cfloat3 prev_tangent = kernel::cnormalize(guide[1].pos - guide[0].pos);
        // A first normal perpendicular to the tangent; which one is arbitrary,
        // and transport carries that choice consistently down the whole guide.
        kernel::cfloat3 seed = kernel::cabs(prev_tangent.y) < 0.9f ? kernel::cf3(0, 1, 0)
                                                                   : kernel::cf3(1, 0, 0);
        kernel::cfloat3 normal =
            kernel::cnormalize(seed - prev_tangent * kernel::cdot(seed, prev_tangent));
        float arclen = 0.0f;

        for (std::size_t i = 0; i < guide.size(); ++i) {
            if (i > 0) {
                arclen += kernel::clength(guide[i].pos - guide[i - 1].pos);
                kernel::cfloat3 tangent =
                    i + 1 < guide.size()
                        ? kernel::cnormalize(guide[i + 1].pos - guide[i].pos)
                        : prev_tangent;
                // Rotate the normal by the same minimal turn the tangent took,
                // then re-orthogonalize against the new tangent. Drift is what
                // the re-orthogonalization is for.
                kernel::cfloat3 axis = kernel::ccross(prev_tangent, tangent);
                float sin_a = kernel::clength(axis);
                if (sin_a > 1e-6f) {
                    axis = axis * (1.0f / sin_a);
                    float cos_a = kernel::cclamp(kernel::cdot(prev_tangent, tangent), -1.0f, 1.0f);
                    float angle = kernel::catan2(sin_a, cos_a);
                    float c = kernel::ccos(angle), sn = kernel::csin(angle);
                    normal = normal * c + kernel::ccross(axis, normal) * sn +
                             axis * (kernel::cdot(axis, normal) * (1.0f - c));
                }
                normal = kernel::cnormalize(normal - tangent * kernel::cdot(normal, tangent));
                prev_tangent = tangent;
            }
            tape.blob.push_back(guide[i].pos.x);
            tape.blob.push_back(guide[i].pos.y);
            tape.blob.push_back(guide[i].pos.z);
            tape.blob.push_back(normal.x);
            tape.blob.push_back(normal.y);
            tape.blob.push_back(normal.z);
            tape.blob.push_back(arclen);
        }

        std::size_t records = emit_profile_records(item);
        float prim_params[5] = {static_cast<float>(guide_offset),
                                static_cast<float>(guide.size()),
                                static_cast<float>(records),
                                static_cast<float>(item.profiles.size()),
                                item.prim.params[0]};
        emit_prim(kernel::ctape_swept, inv_world, scale, round_world, item.color, prim_params, 5,
                  item.deformers, item.repeat);
    }

    // -- items ---------------------------------------------------------------

    // One primitive instance evaluated through the given world matrix.
    void emit_item_instance(const Node& item, const math::cfloat4x4& inv_world, float scale) {
        float round_world = item.rounding * scale;
        if (item.prim.type == PrimType::Stroke) {
            // Typed points are lowered here into the chain the opcode already
            // reads. An all-hard open list tessellates to itself, so this is
            // the identity for every stroke authored before curves existed.
            const std::vector<StrokePoint>& pts =
                curve_is_polyline(item.stroke, item.stroke_closed)
                    ? item.stroke
                    : (scratch_curve = tessellate_curve(item.stroke, item.stroke_closed,
                                                        item.curve_tolerance));
            float prim_params[3];
            prim_params[0] = item.stroke_blend_k;
            prim_params[1] = static_cast<float>(tape.blob.size());
            prim_params[2] = static_cast<float>(pts.size());
            for (const StrokePoint& sp : pts) {
                tape.blob.push_back(sp.pos.x);
                tape.blob.push_back(sp.pos.y);
                tape.blob.push_back(sp.pos.z);
                tape.blob.push_back(sp.radius);
            }
            emit_prim(kernel::ctape_stroke, inv_world, scale, round_world, item.color,
                      prim_params, 3, item.deformers, item.repeat);
        } else if (prim_is_armature(item.prim.type)) {
            // Nodes verbatim: an armature's links are straight, so unlike the
            // stroke there is no curve to tessellate. The parents ride in the
            // blob beside them as floats, which is how every other index in a
            // prim block travels — the blob has one element type.
            const std::vector<StrokePoint>& nodes = item.stroke;
            if (nodes.empty()) return;
            float prim_params[4];
            prim_params[0] = item.stroke_blend_k;
            prim_params[1] = static_cast<float>(tape.blob.size());
            for (const StrokePoint& n : nodes) {
                tape.blob.push_back(n.pos.x);
                tape.blob.push_back(n.pos.y);
                tape.blob.push_back(n.pos.z);
                tape.blob.push_back(n.radius);
            }
            prim_params[2] = static_cast<float>(tape.blob.size());
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                // A parent out of range, or absent because the tree was built
                // shorter than its points, reads as a root. The kernel makes the
                // same reading, so a malformed armature degrades to loose
                // spheres rather than to undefined behaviour.
                std::uint32_t parent = i < item.armature_parents.size()
                                           ? item.armature_parents[i]
                                           : static_cast<std::uint32_t>(i);
                if (parent >= nodes.size()) parent = static_cast<std::uint32_t>(i);
                tape.blob.push_back(static_cast<float>(parent));
            }
            prim_params[3] = static_cast<float>(nodes.size());
            emit_prim(kernel::ctape_armature, inv_world, scale, round_world, item.color,
                      prim_params, 4, item.deformers, item.repeat);
        } else if (prim_is_volume(item.prim.type)) {
            if (!item.volume || item.volume->empty()) return;  // nothing to read
            std::vector<float> flat = item.volume->to_blob();
            // Remembered for the feathered replace combine, which reads the
            // box, band and feather from the same header the prim reads.
            last_volume_blob = tape.blob.size();
            float prim_params[1] = {static_cast<float>(tape.blob.size())};
            tape.blob.insert(tape.blob.end(), flat.begin(), flat.end());
            emit_prim(kernel::ctape_volume, inv_world, scale, round_world, item.color,
                      prim_params, 1, item.deformers, item.repeat);
        } else if (prim_is_swept(item.prim.type)) {
            emit_swept(item, inv_world, scale, round_world);
        } else if (prim_is_loft(item.prim.type)) {
            std::size_t records = emit_profile_records(item);
            float prim_params[4] = {item.prim.params[0], item.prim.params[1],
                                    static_cast<float>(records),
                                    static_cast<float>(item.profiles.size())};
            emit_prim(kernel::ctape_loft, inv_world, scale, round_world, item.color, prim_params,
                      4, item.deformers, item.repeat);
        } else if (prim_is_lift(item.prim.type)) {
            // [profile type][p0..p3][lift param]; polygon vertices go to the
            // out-of-line pool as consecutive (x, y) pairs
            float prim_params[CLAY_TAPE_PROFILE_FLOATS + 1];
            prim_params[0] = static_cast<float>(item.profile.type);
            for (int i = 0; i < 4; ++i) prim_params[i + 1] = item.profile.params[i];
            if (item.profile.is_polygon()) {
                prim_params[1] = static_cast<float>(tape.blob.size());
                prim_params[2] = static_cast<float>(item.profile_points.size());
                for (const kernel::cfloat2& v : item.profile_points) {
                    tape.blob.push_back(v.x);
                    tape.blob.push_back(v.y);
                }
            }
            prim_params[CLAY_TAPE_PROFILE_FLOATS] = item.prim.params[0];
            emit_prim(static_cast<unsigned int>(item.prim.type), inv_world, scale, round_world,
                      item.color, prim_params, CLAY_TAPE_PROFILE_FLOATS + 1, item.deformers,
                      item.repeat);
        } else {
            emit_prim(static_cast<unsigned int>(item.prim.type), inv_world, scale, round_world,
                      item.color, item.prim.params, kMaxPrimParams, item.deformers,
                      item.repeat);
        }
    }

    // Item plus its mirror copies, pre-combined with the layer's Mirror
    // Blend, left on the stack as one value.
    void emit_item(const Node& item, const Layer& layer) {
        math::Transform world = layer.xform * item.xform;
        emit_item_instance(item, world.inverse_matrix(), world.scale);
        // A feathered replace does not participate in the layer mirror: its
        // combine crossfades by the inset into ONE sampled box, and a mirror
        // copy pre-combined into the same operand would sit outside that box
        // and be blended away entirely — worse than either behaviour a caller
        // could mean. A feathered bake is a world-space patch; mirror the
        // strokes it was baked from, or bake each side. Documented on
        // clay_volume_params.feather.
        if (item.mirror && layer.mirror_axes != 0 && !is_feathered_replace(item)) {
            Blend mirror_blend{layer.mirror_k > 0.0f ? BlendProfile::Quadratic
                                                     : BlendProfile::Hard,
                               layer.mirror_k};
            for (int axis = 0; axis < 3; ++axis) {
                if (!(layer.mirror_axes & (1u << axis))) continue;
                // inv of (layer * R * item) = item^-1 * R * layer^-1
                cfloat4x4 inv = math::mul(
                    item.xform.inverse_matrix(),
                    math::mul(math::reflection_matrix(axis), layer.xform.inverse_matrix()));
                emit_item_instance(item, inv, world.scale);
                emit_combine(Op::Add, mirror_blend, 0.0f);
            }
            if (layer.mirror_k > 0.0f)
                tape.info = kernel::cfi_smooth_blend(tape.info, kernel::cfi_exact());
        }
    }

    // -- chains --------------------------------------------------------------

    // Compile an ordered node list. have_acc says whether a running value is
    // already on the stack below; returns whether one is there afterwards.
    bool compile_list(const std::vector<NodeId>& ids, const SdfContent& content,
                      const Layer& layer, bool have_acc) {
        // Whether the cull dropped anything from THIS chain. A feathered
        // replace over a chain the cull emptied must still blend against the
        // far-field seed — the dropped items are real, just out of reach —
        // while one over a chain that was truly empty has nothing to blend
        // into and degrades to the hard replace, so the full tape and every
        // per-brick tape agree on what a lone volume shows.
        bool cull_dropped = false;
        for (NodeId id : ids) {
            const Node* n = content.find(id);
            if (!n || !n->visible) continue;
            if (n->is_group) {
                if (culled(node_influence_bound(content, n->id, layer))) {
                    cull_dropped = true;
                    continue;
                }
                have_acc = compile_group(*n, content, layer, have_acc);
            } else {
                // carving/painting with nothing beneath produces nothing;
                // material-creating modes seed the chain against empty space
                if (!have_acc && n->op != Op::Add && !op_creates_material(n->op)) continue;
                // ONE geometry bound per item. It used to be computed twice:
                // item_influence_bound returns item_geometry_bound for a local
                // item, and the next line asked for item_geometry_bound again.
                // Both calls do the real work — for a stroke or a sweep they
                // re-tessellate the curve — so the second was pure waste on
                // every compile, culled or not.
                const math::Aabb geometry = item_geometry_bound(*n, layer);
                // A non-local item has an infinite influence bound and so can
                // never be culled; item_influence_is_local is the single
                // definition of that test, shared with item_influence_bound.
                if (cull && item_influence_is_local(*n) && culled(geometry)) {
                    cull_dropped = true;
                    continue;
                }
                // tape.bounds is the geometric extent meshing and raycast
                // clipping use — never infinite, even for non-local ops
                tape.bounds.expand(geometry);
                bool seeded = !have_acc && n->op != Op::Add;
                if (seeded) emit_empty(n->color);
                emit_item(*n, layer);
                bool smooth = n->blend.profile != BlendProfile::Hard && n->blend.k > 0.0f;
                if (have_acc || seeded) {
                    // The feather blends the volume into what is beneath it.
                    // With something there — or with something the cull put
                    // out of reach — the feathered mode; over a chain that
                    // was truly empty, the hard replace, because a crossfade
                    // toward the far-field seed would blend the volume away.
                    if (is_feathered_replace(*n) && (have_acc || cull_dropped))
                        emit_replace_feather(*n, layer);
                    else
                        emit_combine(n->op, n->blend,
                                     n->rounding * layer.xform.scale * n->xform.scale,
                                     &n->transition);
                }
                fold_info(*n, (have_acc || seeded) ? n->op : Op::Add, smooth && have_acc,
                          n->rounding * layer.xform.scale * n->xform.scale);
                have_acc = true;
            }
        }
        return have_acc;
    }

    // The cull test for a whole group lives in compile_list, which needs to
    // know a group was dropped (see cull_dropped there); this trusts it.
    bool compile_group(const Node& group, const SdfContent& content, const Layer& layer,
                       bool have_acc) {
        if (group.op == Op::None) {
            // inline: children continue the outer chain
            return compile_list(group.children, content, layer, have_acc);
        }
        // a carving/painting group with nothing beneath produces nothing;
        // material-creating groups seed the chain against empty space
        if (!have_acc && group.op != Op::Add && !op_creates_material(group.op)) return have_acc;
        std::size_t saved_instrs = tape.instrs.size();
        std::size_t saved_params = tape.params.size();
        std::size_t saved_strokes = tape.blob.size();
        bool seeded = !have_acc && group.op != Op::Add;
        if (seeded) emit_empty(group.color);
        bool sub = compile_list(group.children, content, layer, false);
        if (!sub) {  // empty subtree: roll back any partial emission
            tape.instrs.resize(saved_instrs);
            tape.params.resize(saved_params);
            tape.blob.resize(saved_strokes);
            return have_acc;
        }
        if (have_acc || seeded) {
            emit_combine(group.op, group.blend, group.rounding * layer.xform.scale);
            bool smooth = group.blend.profile != BlendProfile::Hard && group.blend.k > 0.0f;
            if (op_is_extended(group.op))
                tape.info = kernel::cfi_extended_blend(tape.info, tape.info,
                                                       op_is_diagonal(group.op));
            else if (smooth)
                tape.info = kernel::cfi_smooth_blend(tape.info, tape.info);
        }
        return true;
    }

    void run(const Document& doc, const CullRegion* cull_region) {
        float pad = 0.0f;
        if (cull_region)
            for (const Layer& layer : doc.layers)
                if (layer.visible && layer.kind == LayerKind::Sdf && layer.sdf)
                    pad = kernel::cmax(pad, feather_cull_pad(*layer.sdf, layer));
        begin_cull(cull_region, pad);
        bool have_acc = false;
        for (const Layer& layer : doc.layers) {
            if (!layer.visible || layer.kind != LayerKind::Sdf || !layer.sdf) continue;
            bool layer_val = compile_list(layer.sdf->roots, *layer.sdf, layer, false);
            if (!layer_val) continue;
            if (have_acc) emit_combine(Op::Add, Blend{}, 0.0f);  // layers union hard
            have_acc = true;
        }
    }
};

}  // namespace

Tape compile_document(const Document& doc, const CullRegion* cull) {
    Compiler c;
    c.run(doc, cull);
    return std::move(c.tape);
}

Tape compile_layer(const Layer& layer, const CullRegion* cull) {
    Compiler c;
    bool usable = layer.visible && layer.kind == LayerKind::Sdf && layer.sdf;
    c.begin_cull(cull, cull && usable ? feather_cull_pad(*layer.sdf, layer) : 0.0f);
    if (usable) c.compile_list(layer.sdf->roots, *layer.sdf, layer, false);
    return std::move(c.tape);
}

}  // namespace scene
}  // namespace clay
