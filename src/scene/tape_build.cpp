#include <atomic>

#include "clay/scene/bounds.h"
#include "clay/scene/cull_index.h"
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
    const float tightest = guide_bend_radius(guide);
    const float total_len = guide_arc_length(guide);
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

struct Compiler {
    Tape tape;
    const CullRegion* cull;
    // Per-revision cached bounds and per-batch coarse survivors (see
    // cull_index.h). Both optional, both pure accelerations: with either
    // present the cull decisions — and so the emitted tape — are identical.
    const CullIndex* index = nullptr;
    const CullPlan* plan = nullptr;
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
                   const std::vector<Deformer>& deformers, const Repeat& repeat = Repeat{},
                   float curve_tolerance = 0.01f) {
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
            // A guide is not a fixed size, so it goes in the blob and slots 1
            // and 2 carry a handle to it — the same arrangement a swept
            // primitive uses. Emitted here rather than by the caller because
            // this is where the record that has to point at it is written.
            float handle_off = d.k, handle_count = d.a;
            if (d.type == kernel::cdeform_bend_curve) {
                std::vector<StrokePoint> guide =
                    curve_is_polyline(d.guide, false)
                        ? d.guide
                        : tessellate_curve(d.guide, false, curve_tolerance);
                // Fewer than two points is no curve at all. The record is
                // still emitted, with a count the kernel reads as "leave the
                // point alone" — refusing here would drop a deformer the
                // caller can still fix by editing its guide.
                handle_off = guide.size() >= 2
                                 ? static_cast<float>(emit_guide(guide)) : 0.0f;
                handle_count = static_cast<float>(guide.size());
            }
            if (d.type == kernel::cdeform_lattice_xform) {
                // Transform, then its inverse, then the offsets. The inverse is
                // stored rather than derived per sample: it is a quaternion
                // conjugate and a divide to recover something known here.
                handle_off = static_cast<float>(tape.blob.size());
                for (const kernel::cfloat4x4& m :
                     {d.cage_xform.matrix(), d.cage_xform.inverse_matrix()}) {
                    const float cage_xf[12] = {m.c0.x, m.c0.y, m.c0.z, m.c1.x, m.c1.y, m.c1.z,
                                          m.c2.x, m.c2.y, m.c2.z, m.c3.x, m.c3.y, m.c3.z};
                    tape.blob.insert(tape.blob.end(), cage_xf, cage_xf + 12);
                }
                for (const kernel::cfloat3& o : d.cage) {
                    tape.blob.push_back(o.x);
                    tape.blob.push_back(o.y);
                    tape.blob.push_back(o.z);
                }
                handle_count = d.a;
            } else if (d.type == kernel::cdeform_lattice) {
                // The cage goes in the blob for the same reason a guide does:
                // nx*ny*nz offsets are not a fixed number of floats. Only slot
                // 1 is a handle here — the divisions and box ride the record.
                handle_off = static_cast<float>(tape.blob.size());
                for (const kernel::cfloat3& o : d.cage) {
                    tape.blob.push_back(o.x);
                    tape.blob.push_back(o.y);
                    tape.blob.push_back(o.z);
                }
                handle_count = d.a;  // nx, which slot 2 carries for a lattice
            }
            if (d.type == kernel::cdeform_alpha) {
                // Header then samples. The five scalars sit here rather than in
                // the record because the record is exactly full with the handle,
                // the centre and the stamp's frame — and dropping the frame to
                // make room would cost the TANGENT, which is what lets an
                // artist align a stamp to a seam.
                handle_off = static_cast<float>(tape.blob.size());
                tape.blob.push_back(static_cast<float>(d.stamp.width));
                tape.blob.push_back(static_cast<float>(d.stamp.height));
                tape.blob.push_back(d.stamp.extent);
                tape.blob.push_back(d.stamp.radius);
                tape.blob.push_back(d.stamp.amplitude);
                tape.blob.insert(tape.blob.end(), d.stamp.samples.begin(), d.stamp.samples.end());
                handle_count = d.a;  // the centre's x, which slot 2 carries here
            }
            tape.params.push_back(static_cast<float>(d.type));
            tape.params.push_back(handle_off);
            tape.params.push_back(handle_count);
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
    // feathered placement — bounds.h owns the single definition, shared with
    // the cull index's refusal to prune chains that hold one.
    static bool is_feathered_replace(const Node& item) {
        return item_is_feathered_replace(item);
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
        tape.params.push_back(emit_gate(&item, &layer));
        tape.params.push_back(static_cast<float>(last_volume_blob));
        const float xf[12] = {inv.c0.x, inv.c0.y, inv.c0.z, inv.c1.x, inv.c1.y, inv.c1.z,
                              inv.c2.x, inv.c2.y, inv.c2.z, inv.c3.x, inv.c3.y, inv.c3.z};
        tape.params.insert(tape.params.end(), xf, xf + 12);
        tape.params.push_back(world.scale);
    }

    // rb: second radius of the two-parameter extended modes (groove/tongue
    // half-width), already in world units; 0 for everything else.
    // The gate payload for an item that carries one, written into the blob and
    // returned as its offset; -1 for an ungated item, which is almost all of
    // them and costs one comparison in the kernel.
    //
    // Fifteen floats — a volume handle, the world-to-gate transform, the scale
    // and the falloff width — which is why it is blob-carried rather than
    // squeezed into the combine record.
    float emit_gate(const Node* item, const Layer* layer) {
        if (!item || !layer || !item->gated() || item->gate->empty()) return -1.0f;
        const std::size_t rec = tape.blob.size();
        // The volume's own samples first, then the record that points at them,
        // so the offset the record stores is already known when it is written.
        const std::size_t volume_off = tape.blob.size() + CLAY_TAPE_GATE_FLOATS;
        tape.blob.push_back(static_cast<float>(volume_off));
        // A gate is placed by the ITEM's transform, so it travels with the item
        // it protects rather than staying where the mask was painted.
        const math::Transform world = layer->xform * item->xform;
        const kernel::cfloat4x4 inv = world.inverse_matrix();
        for (float v : {inv.c0.x, inv.c0.y, inv.c0.z, inv.c1.x, inv.c1.y, inv.c1.z, inv.c2.x,
                        inv.c2.y, inv.c2.z, inv.c3.x, inv.c3.y, inv.c3.z})
            tape.blob.push_back(v);
        tape.blob.push_back(world.scale);
        tape.blob.push_back(item->gate_width);
        const std::vector<float> flat = item->gate->to_blob();
        tape.blob.insert(tape.blob.end(), flat.begin(), flat.end());
        return static_cast<float>(rec);
    }

    void emit_combine(Op op, Blend blend, float rb, const Transition* transition = nullptr,
                      const Node* gated = nullptr, const Layer* layer = nullptr) {
        CTapeInstr instr;
        instr.op = kernel::ctape_combine;
        instr.param_offset = static_cast<unsigned int>(tape.params.size());
        tape.instrs.push_back(instr);
        tape.params.push_back(static_cast<float>(static_cast<int>(op)));
        tape.params.push_back(static_cast<float>(static_cast<int>(blend.profile)));
        tape.params.push_back(blend.k);
        tape.params.push_back(rb);
        tape.params.push_back(emit_gate(gated, layer));
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

    // A gate's width is authored in the item's own units and applies in world
    // space, so the layer's scale carries it across. Kept beside fold_info
    // rather than threaded through it, because fold_info already takes its
    // world rounding the same way and adding a second scale argument for one
    // caller reads worse than a member the compile loop sets.
    float layer_scale_for_gate_ = 1.0f;
    // The gated item's own reach, set immediately before fold_info by the
    // caller that already computed it, so the bound is not recomputed and
    // cannot drift from the one culling used.
    math::Aabb gate_reach_{};

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

        fold_gate(item);
    }

    // A GATE mixes the combined result back toward the accumulator by a
    // spatially varying weight, so it costs what every other such mix here
    // costs. Charged AFTER the op's own fold and in its own function: the gate
    // acts on whatever that op produced, whichever op it was, so it is not one
    // more branch in fold_info's dispatch — it is a second, unconditional step.
    void fold_gate(const Node& item) {
        if (!item.gated() || !item.gate || item.gate->empty()) return;
        // How far the gated and ungated fields can differ. Bounded by the
        // ITEM's own reach rather than the whole region's, because outside
        // where the item acts the two are the same field and their difference
        // is zero — a region-wide bound would charge a small gated dab as
        // though it moved the entire document.
        //
        // Still loose: the difference only matters where the gate's weight
        // VARIES, which is a band `width` wide around the mask's boundary, and
        // nothing here knows where that sits relative to the item. Loose in the
        // safe direction, which is the only kind worth being.
        const math::Aabb& reach = gate_reach_;
        const float diff_bound =
            reach.empty() || reach.is_infinite() ? 1e3f : kernel::clength(reach.extent());
        tape.info =
            kernel::cfi_gate(tape.info, diff_bound, item.gate_width * layer_scale_for_gate_);
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

    // A guide polyline into the blob, one 7-float vertex each: position,
    // PARALLEL-TRANSPORTED normal, arc length. Returns the blob offset.
    //
    // Transport is sequential — each frame is the previous one rotated by the
    // minimum turn that carries the old tangent onto the new — so it cannot be
    // done per sample, which is exactly why the frames go in the blob. A Frenet
    // frame would flip at an inflection and be undefined where the guide is
    // straight; this one does neither.
    //
    // Shared by the swept primitive and by the bend-along-a-curve deformer, so
    // the two cannot disagree about what a guide's frames are.
    std::size_t emit_guide(const std::vector<StrokePoint>& guide) {
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
        return guide_offset;
    }

    // A sweep: the guide's frames, then the profiles carried along them.
    void emit_swept(const Node& item, const kernel::cfloat4x4& inv_world, float scale,
                    float round_world) {
        std::vector<StrokePoint> guide =
            curve_is_polyline(item.stroke, false)
                ? item.stroke
                : tessellate_curve(item.stroke, false, item.curve_tolerance);
        if (guide.size() < 2 || item.profiles.size() < 2) return;

        std::size_t guide_offset = emit_guide(guide);

        std::size_t records = emit_profile_records(item);
        float prim_params[5] = {static_cast<float>(guide_offset),
                                static_cast<float>(guide.size()),
                                static_cast<float>(records),
                                static_cast<float>(item.profiles.size()),
                                item.prim.params[0]};
        emit_prim(kernel::ctape_swept, inv_world, scale, round_world, item.color, prim_params, 5,
                  item.deformers, item.repeat, item.curve_tolerance);
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
                      prim_params, 3, item.deformers, item.repeat,
                      item.curve_tolerance);
        } else if (prim_is_armature(item.prim.type)) {
            // Nodes verbatim: an armature's links are straight, so unlike the
            // stroke there is no curve to tessellate. The parents ride in the
            // blob beside them as floats, which is how every other index in a
            // prim block travels — the blob has one element type.
            const std::vector<StrokePoint>& nodes = item.stroke;
            if (nodes.empty()) return;
            float prim_params[5];
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
            prim_params[4] = static_cast<float>(tape.blob.size());
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                // A sign absent because the array was stored shorter than the
                // points reads as positive, the same padding short parents get.
                std::int8_t sign =
                    i < item.armature_signs.size() ? item.armature_signs[i] : std::int8_t{1};
                tape.blob.push_back(sign < 0 ? -1.0f : 1.0f);
            }
            emit_prim(kernel::ctape_armature, inv_world, scale, round_world, item.color,
                      prim_params, 5, item.deformers, item.repeat,
                      item.curve_tolerance);
        } else if (prim_is_volume(item.prim.type)) {
            if (!item.volume || item.volume->empty()) return;  // nothing to read
            std::vector<float> flat = item.volume->to_blob();
            // Remembered for the feathered replace combine, which reads the
            // box, band and feather from the same header the prim reads.
            last_volume_blob = tape.blob.size();
            float prim_params[1] = {static_cast<float>(tape.blob.size())};
            tape.blob.insert(tape.blob.end(), flat.begin(), flat.end());
            emit_prim(kernel::ctape_volume, inv_world, scale, round_world, item.color,
                      prim_params, 1, item.deformers, item.repeat,
                      item.curve_tolerance);
        } else if (prim_is_swept(item.prim.type)) {
            emit_swept(item, inv_world, scale, round_world);
        } else if (prim_is_loft(item.prim.type)) {
            std::size_t records = emit_profile_records(item);
            float prim_params[4] = {item.prim.params[0], item.prim.params[1],
                                    static_cast<float>(records),
                                    static_cast<float>(item.profiles.size())};
            emit_prim(kernel::ctape_loft, inv_world, scale, round_world, item.color, prim_params,
                      4, item.deformers, item.repeat, item.curve_tolerance);
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
                      item.repeat, item.curve_tolerance);
        } else {
            emit_prim(static_cast<unsigned int>(item.prim.type), inv_world, scale, round_world,
                      item.color, item.prim.params, kMaxPrimParams, item.deformers,
                      item.repeat, item.curve_tolerance);
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
        layer_scale_for_gate_ = layer.xform.scale;
        // Whether the cull dropped anything from THIS chain. A feathered
        // replace over a chain the cull emptied must still blend against the
        // far-field seed — the dropped items are real, just out of reach —
        // while one over a chain that was truly empty has nothing to blend
        // into and degrades to the hard replace, so the full tape and every
        // per-brick tape agree on what a lone volume shows.
        bool cull_dropped = false;
        // The plan's coarse survivors, when a batch compile supplies one: a
        // chain member whose bound misses the batch's union region cannot
        // survive any member brick's test either, so iterating the survivor
        // entries — which carry the cached bounds — makes the same decisions
        // with none of the per-item work. Chains a plan must not prune (a
        // feathered replace reads cull_dropped) come back null and take the
        // walk below unchanged.
        const std::vector<CullIndex::Entry>* pruned = plan ? plan->chain(layer, ids) : nullptr;
        const std::size_t member_count = pruned ? pruned->size() : ids.size();
        for (std::size_t at = 0; at < member_count; ++at) {
            const CullIndex::Entry* e = pruned ? &(*pruned)[at] : nullptr;
            const Node* n = e ? e->node : content.find(ids[at]);
            if (!n || !n->visible) continue;
            if (n->is_group) {
                if (culled(e ? e->bound : node_influence_bound(content, n->id, layer))) {
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
                const math::Aabb geometry = e ? e->bound : item_geometry_bound(*n, layer);
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
                                     &n->transition, n, &layer);
                }
                gate_reach_ = geometry;
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
        if (cull_region && index)
            pad = index->feather_pad();
        else if (cull_region)
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

// Process-unique nonzero ids for compiled tapes (Tape::compile_id): equal ids
// mean the same compile produced the bytes, which is what lets a backend keep
// an uploaded tape resident without comparing or hashing its contents.
std::uint64_t next_compile_id() {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

}  // namespace

Tape compile_document(const Document& doc, const CullRegion* cull, const CullIndex* index,
                      const CullPlan* plan) {
    Compiler c;
    // An index for another document caches bounds under other layers'
    // addresses: every lookup would miss and the compile would silently run
    // at the uncached speed, so it is dropped rather than trusted. A plan
    // without a cull could only mean a pruned whole-document tape, which no
    // caller can want.
    if (index && index->document() != &doc) index = nullptr;
    c.index = index;
    c.plan = cull && index ? plan : nullptr;
    c.run(doc, cull);
    c.tape.compile_id = next_compile_id();
    return std::move(c.tape);
}

Tape compile_layer(const Layer& layer, const CullRegion* cull) {
    Compiler c;
    bool usable = layer.visible && layer.kind == LayerKind::Sdf && layer.sdf;
    c.begin_cull(cull, cull && usable ? feather_cull_pad(*layer.sdf, layer) : 0.0f);
    if (usable) c.compile_list(layer.sdf->roots, *layer.sdf, layer, false);
    c.tape.compile_id = next_compile_id();
    return std::move(c.tape);
}

}  // namespace scene
}  // namespace clay
