// The Magnify/Pinch brush on a layer's assembled surface. See
// include/clay/brush/magnify.h for why it exists, what it shares with the Move
// resolver and why the strength — unlike a drag's displacement — crosses the
// layer's symmetry images untouched.

#include "clay/brush/magnify.h"

#include <utility>

namespace clay {
namespace brush {

namespace {

MoveSettings as_move_settings(const MagnifySettings& settings) {
    MoveSettings out;
    out.radius = settings.radius;
    out.ease = settings.ease;
    // Never gated on a half-space: a radial scale has no direction, and asking
    // `prepare_move` for the front-only reach would shrink which items the
    // gesture reaches for a rule it does not obey.
    out.front_only = false;
    return out;
}

}  // namespace

std::vector<PreparedMove> prepare_magnify(const scene::Layer& layer,
                                          kernel::cfloat3 world_centre,
                                          const MagnifySettings& settings,
                                          MovePrepareStats* out_stats) {
    return prepare_move(layer, world_centre, as_move_settings(settings), out_stats);
}

MoveWarp resolve_prepared_magnify(const PreparedMove& prepared, float strength) {
    MoveWarp warp;
    resolve_prepared_magnify(prepared, strength, &warp);
    return warp;
}

void resolve_prepared_magnify(const PreparedMove& prepared, float strength, MoveWarp* out) {
    MoveWarp& warp = *out;
    warp.node = prepared.node;
    warp.deformers.clear();
    warp.gesture.clear();
    for (const PreparedImage& image : prepared.images) {
        // The strength is the SAME for every image. A reflection or a rotation
        // of a radial scale is a radial scale of equal strength, so unlike a
        // drag's displacement there is nothing per-image to map — and nothing
        // per-image that could round differently between the two halves of a
        // mirrored gesture.
        scene::Deformer magnify = scene::Deformer::magnify(image.local_centre,
                                                           prepared.local_radius, strength,
                                                           prepared.ease);
        // Each one lands in exactly one of the two: the reaching images are the
        // warp, the others only its identity, so `moved_chain` recognises a
        // frame in which an image stopped reaching this item.
        if (image.reaches) {
            warp.deformers.push_back(std::move(magnify));
        } else {
            warp.gesture.push_back(std::move(magnify));
        }
    }
    order_by_value(&warp.deformers);
}

std::vector<MoveWarp> magnify_brush(const scene::Layer& layer, kernel::cfloat3 world_centre,
                                    float strength, const MagnifySettings& settings) {
    std::vector<MoveWarp> out;
    // A strength of zero scales by one: a chain of those costs a tape record
    // per item per evaluation and changes nothing.
    if (strength == 0.0f) return out;
    const std::vector<PreparedMove> prepared = prepare_magnify(layer, world_centre, settings);
    out.reserve(prepared.size());
    for (const PreparedMove& p : prepared)
        out.push_back(resolve_prepared_magnify(p, strength));
    return out;
}

}  // namespace brush
}  // namespace clay
