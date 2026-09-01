#include "clay/mesh/sculpt_workset.h"

#include <algorithm>

#include "clay/mesh/automask.h"

namespace clay {
namespace mesh {

namespace {

// Publish the reverse map over the entries that are currently in the workset.
//
// `WorkItemId::key()` is the dense half of all three identities — the weld
// class, the vertex slot, the level vertex — which is what lets one spelling
// serve every representation. See `work_item.h` for why the encoding puts it
// there.
void publish_slots(SculptWorkset& r, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i)
        r.slot[r.items[i].key()] = static_cast<std::uint32_t>(i);
}

// THE FIVE FACTORS IN THE ONE FIXED ORDER, and the zero drop.
//
// THE WALK DECIDES WHAT IS REACHED; THE STRAIGHT LINE DECIDES HOW MUCH.
// Weighing by the walk's own distance was tried first and looks wrong: an edge
// path overestimates geodesic distance by a direction-dependent amount, so on
// an irregular triangulation the falloff picks up a visible herringbone banding
// that a render shows and the numbers do not. The straight-line distance
// carries no such bias and is bounded by the walk's, so weighing by it costs
// nothing — and the property the walk exists for survives untouched, because a
// class the walk did not reach is not in the candidate list at all and its
// weight never comes up.
std::size_t compose_weights(const WorkComposeInputs& in, SculptWorkset& r) {
    const MeshBrushSettings& settings = *in.settings;
    const std::size_t candidates = r.items.size();
    r.weights.resize(candidates);
    r.normals.resize(candidates);

    std::size_t kept = 0;
    for (std::size_t i = 0; i < candidates; ++i) {
        const WorkItemId item = r.items[i];
        const kernel::cfloat3 p = r.positions[i];

        WeightFactors f;
        f.falloff = falloff_weight(settings.falloff,
                                   kernel::clength(p - settings.center) / settings.radius);
        // ...and fade out over the last stretch of the walk's path budget, so
        // the rim is smooth even where the two bounds disagree. A ball
        // footprint has no walk, so it passes 1 and the multiplication is the
        // identity.
        if (in.geodesic)
            f.path_taper = path_taper(in.path_distance[i] / settings.radius, in.taper_start,
                                      in.path_budget);
        if (in.gate && *in.gate) f.gate = (*in.gate)(p);
        // The freeze the representation carries on the item itself composes
        // with the caller's gate by taking whichever is STRONGER rather than
        // multiplying, because two independent freezes are not half a freeze
        // each. Skipped entirely where there is none — see `WorkItemReader`.
        if (in.reader.mask_at)
            f.gate = std::max(f.gate, in.reader.mask_at(in.reader.context, item));
        // The alpha multiplies the WEIGHT, which is why it needs no per-verb
        // code: every verb already scales by this, and every falloff already
        // shaped it.
        f.alpha = alpha_at(settings, *in.alpha, p);

        const float w = compose_weight(f);
        if (w <= 0.0f) continue;
        r.items[kept] = item;
        r.weights[kept] = w;
        r.positions[kept] = p;
        // Only now, and only for a survivor: the fixed mesh's normal is an
        // angle-weighted pass over the class's incident triangles, and a rim
        // entry that was about to be dropped must not pay for one.
        r.normals[kept] = in.reader.normal_at(in.reader.context, item);
        ++kept;
    }
    r.items.resize(kept);
    r.weights.resize(kept);
    r.positions.resize(kept);
    r.normals.resize(kept);
    return kept;
}

// THE AUTOMASK IS A SECOND PASS, and it has to be: two of its five factors —
// the boundary fade and the connectivity flood — spread over the workset's own
// neighbourhood, so they cannot be answered one entry at a time while the
// workset is still being built. It multiplies into the weight LAST, which is
// what keeps a stamp with no automask on exactly the bits it had before
// automasking existed.
std::size_t apply_automask(const WorkComposeInputs& in, BrushScratchArena& arena,
                           SculptWorkset& r, std::size_t kept) {
    const AutomaskSettings& settings = in.settings->automask;
    r.automask.clear();
    if (!settings.any() || !in.topology || !in.automask_inputs || kept == 0) return kept;

    r.automask.resize(kept);
    // The seed becomes a slot only now, because only now is the map published.
    // A seed the falloff dropped stays `resolved` with a `kNoClass` slot, which
    // is not the same as no seed at all.
    ConnectivitySeed seed;
    if (in.automask_seed) {
        seed.resolved = true;
        const std::uint32_t key = in.automask_seed->key();
        seed.slot = key < r.slot.size() ? r.slot[key] : kNoClass;
    }
    compute_automask(*in.topology, r, settings, *in.automask_inputs, in.automask_reference, seed,
                     arena, r.automask.data());

    std::size_t survived = 0;
    for (std::size_t i = 0; i < kept; ++i) {
        const float w = r.weights[i] * r.automask[i];
        if (w <= 0.0f) {
            // A fully automasked entry leaves the workset ENTIRELY, which is
            // what makes it bit-identical to its input rather than merely
            // close: nothing writes it at all.
            r.slot[r.items[i].key()] = kNoClass;
            continue;
        }
        r.items[survived] = r.items[i];
        r.weights[survived] = w;
        r.positions[survived] = r.positions[i];
        r.normals[survived] = r.normals[i];
        r.automask[survived] = r.automask[i];
        ++survived;
    }
    if (survived == kept) return kept;

    r.items.resize(survived);
    r.weights.resize(survived);
    r.positions.resize(survived);
    r.normals.resize(survived);
    r.automask.resize(survived);
    publish_slots(r, survived);
    return survived;
}

// The plane and the shared direction, taken from the snapshot and never from
// what the stamp is about to deposit.
void resolve_frame(const WorkComposeInputs& in, SculptWorkset& r, std::size_t kept) {
    const MeshBrushSettings& settings = *in.settings;
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

}  // namespace

std::size_t compose_workset(const WorkComposeInputs& in, BrushScratchArena& arena,
                            SculptWorkset* workset) {
    SculptWorkset& r = *workset;
    const std::size_t kept = compose_weights(in, r);
    // Published before the automask runs, because its topology adapter reads a
    // ring neighbour's membership through this map. Republished inside
    // `apply_automask` only if it dropped something.
    publish_slots(r, kept);
    const std::size_t survived = apply_automask(in, arena, r, kept);
    resolve_frame(in, r, survived);
    return survived;
}

}  // namespace mesh
}  // namespace clay
