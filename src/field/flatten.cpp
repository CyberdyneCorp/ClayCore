// Flattening a surface onto a plane (sdf-kernels spec, add-sdf-flatten). See
// include/clay/field/flatten.h for why this is not the Clip brush and why it
// samples rather than rewriting a volume's samples.

#include "clay/field/flatten.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace clay {
namespace field {

using kernel::cf3;
using kernel::cfloat3;

namespace {

// How much of the plane to take at `p`. Outside the region this is zero and
// the source is left exactly as it was.
float region_weight(const FlattenSettings& s, cfloat3 p) {
    const float distance = kernel::clength(p - s.centre);
    if (distance <= s.region_radius) return 1.0f;
    // A hard edge would leave a rim where the flattened field meets the
    // untouched one, so the effect tapers.
    const float t = (distance - s.region_radius) / std::max(s.falloff, 1e-4f);
    if (t >= 1.0f) return 0.0f;
    return 1.0f - t * t * (3.0f - 2.0f * t);  // smoothstep
}

// The freeze: a fully masked sample takes weight zero, so the source's value
// comes back untouched and the surface there stays where the source put it
// rather than being drawn onto the plane.
float mask_gate(const MaskGate& mask, cfloat3 p) {
    if (!mask) return 1.0f;
    return 1.0f - std::clamp(mask(p), 0.0f, 1.0f);
}

// The plane, resolved once per call: normalised, with the strength folded in.
struct Plane {
    cfloat3 normal;
    cfloat3 point;
    float strength;
};

// What one sample becomes: what the source said at `p`, drawn toward the plane
// by the region's weight. Shared by the two source overloads — the
// point-at-a-time one and the batched one — because a blend written twice is a
// blend that drifts, and the two are required to agree sample for sample.
// `src` is a CALLABLE rather than the value, so the weight is computed before
// it is called. That is not a style preference: the source is a std::function,
// which the optimiser must treat as able to touch anything, so reading the
// settings after it means reloading every one of them. Passing `source(p)` as
// an argument -- which evaluates first -- cost 5% of an in-place flatten.
template <typename Src>
float flatten_at(const FlattenSettings& s, const Plane& pl, cfloat3 p, Src&& src) {
    const float weight = region_weight(s, p) * pl.strength * mask_gate(s.mask, p);
    const float here = src(p);
    if (weight <= 0.0f) return here;
    // The plane is itself a signed distance function, so blending toward it is
    // what makes this two-sided: above the plane the value rises and material
    // goes, below it the value falls and a hollow fills. At full weight the
    // field IS the half-space, so the surface IS the plane.
    const float plane = kernel::cdot(pl.normal, p - pl.point);
    // One clamp is the whole difference between the three modes. The term is
    // positive where the plane is further out than the surface — material to
    // remove — and negative where it is further in.
    float toward = plane - here;
    if (s.mode == FlattenMode::CutOnly) toward = kernel::cmax(toward, 0.0f);
    else if (s.mode == FlattenMode::FillOnly) toward = kernel::cmin(toward, 0.0f);
    return here + toward * weight;
}

// What the volume says at one of its own sample positions.
//
// The STORED sample where a brick holds it, and eval() only where none does.
// Not an optimisation -- it is what makes the fill exactly the identity outside
// the brush, which is what resample_region requires and what keeps the two
// copies of a sample shared across a brick face from drifting apart. At weight
// zero flatten_at hands this value straight back, so a sample in a stored brick
// comes back as the identical float. Reaching for eval() instead would be
// betting that `(origin + n*cell - origin) / cell` lands exactly on `n`, which
// is not what a halo invariant can rest on.
//
// It is also the substantive half of why a local flatten does not inflate the
// volume. Away from the band eval() returns the far bound, which steps by BRICK
// rather than by cell; re-recording those steps as samples is what took a
// re-baked ball from 444 stored bricks to 1,246 and declared a Lipschitz of 14
// on a field whose source declared 1. Now only a brick that stores nothing at
// all can contribute one, and only inside the brush.
float source_at(const FieldVolume& v, const int g[3], cfloat3 p) {
    const std::optional<float> stored = v.sample_at(g[0], g[1], g[2]);
    if (stored) return *stored;
    // No brick stores this sample, which is a MEASUREMENT rather than an
    // absence: a brick is skipped only when every SAMPLE in it lies beyond the
    // band, and every brick sharing this coordinate has said so. eval() reports
    // the far BOUND, which is deliberately smaller -- it has to cover the
    // points BETWEEN samples, so far_value() sits 0.87 of a cell inside the
    // band -- and handing that back as a sample is what re-created the shell of
    // bricks just outside the stored ones and filled them with brick-sized
    // steps. That shell is where a Lipschitz of 14 came from, and it is why
    // compact() exists to undo it after a bake.
    //
    // So the bound is floored at what the volume actually guarantees. Just PAST
    // the band, because the near-surface test is inclusive and a sample sitting
    // exactly on it would store the brick this is keeping empty.
    const float bound = v.eval(p);
    const float floor_at = std::nextafter(v.band(), std::numeric_limits<float>::infinity());
    return bound < 0.0f ? std::min(bound, -floor_at) : std::max(bound, floor_at);
}

// The guards both source overloads apply, so that "no plane" and "no region"
// mean the same thing whichever one a caller reached for. Returns false when
// the settings describe no flatten at all and the source should be sampled
// unchanged.
bool resolve_plane(const FlattenSettings& settings, Plane* out) {
    const float length = kernel::clength(settings.plane_normal);
    const float strength = std::clamp(settings.strength, 0.0f, 1.0f);
    // A zero normal describes no plane. Sampling the source unchanged beats
    // shaping the field by an arbitrary direction, which would look like it
    // worked.
    //
    // A missing REGION is refused for a less obvious reason. Flatten is local
    // by nature: where its weight is one the result IS the plane, so blending
    // with no region at full strength does not flatten an object, it REPLACES
    // it with a half-space — a ball comes back as a box. The interesting
    // behaviour all lives in the taper, so the region is what makes this a
    // brush rather than a very slow trim.
    if (!(length > 1e-6f) || strength <= 0.0f || !(settings.region_radius > 0.0f)) return false;
    *out = Plane{settings.plane_normal * (1.0f / length), settings.plane_point, strength};
    return true;
}

}  // namespace

FieldVolume flatten(const std::function<float(cfloat3)>& source, const math::Aabb& region,
                    float cell_size, float band, const FlattenSettings& settings) {
    Plane pl;
    if (!resolve_plane(settings, &pl))
        return FieldVolume::sample(source, region, cell_size, band);

    FieldVolume out = FieldVolume::sample(
        [&source, &settings, &pl](cfloat3 p) { return flatten_at(settings, pl, p, source); },
        region, cell_size, band);

    // Measured, not bounded in advance. Inside the region the result is a
    // convex combination of two 1-Lipschitz fields and so is 1-Lipschitz; it is
    // the TAPER that can steepen it, by the movement times the taper's slope.
    // Reading back what the samples actually do beats guessing an envelope
    // generous enough for every region and falloff a caller might pick.
    //
    // Which `sample` has already done, over these samples and by this rule --
    // sample_blocks ends by measuring what it stored. Measuring again here was
    // a second sweep of every stored sample in the volume for a number already
    // in hand: 29.7 ms of a 629 ms flatten at cell 0.015.
    return out;
}

FieldVolume flatten(const FieldVolume::BrickBlockFill& source, const math::Aabb& region,
                    float cell_size, float band, const FlattenSettings& settings) {
    Plane pl;
    if (!resolve_plane(settings, &pl))
        return FieldVolume::sample_blocks(source, region, cell_size, band);

    // The blend is applied to the block the source filled, IN PLACE, rather
    // than to a volume built from the source and rewritten afterwards. That
    // distinction is the whole reason this takes the source's fill instead of
    // baking first: `sample_blocks` decides which bricks to keep from the
    // values it is handed, and flatten moves the surface by many band widths,
    // so a volume built from the source would have kept the bricks around the
    // surface the SOURCE had. The facet would then sit in a band that was
    // never sampled around it.
    FieldVolume out = FieldVolume::sample_blocks(
        [&source, &settings, &pl](const FieldVolume::BrickGrid& grid, std::size_t first,
                                  std::size_t count, float* block) {
            source(grid, first, count, block);
            for (std::size_t s = 0; s < count; ++s)
                for (int i = 0; i < kBrickSamples; ++i) {
                    const std::size_t at = s * kBrickSamples + static_cast<std::size_t>(i);
                    block[at] = flatten_at(settings, pl, grid.sample_position(first + s, i),
                                           [&](cfloat3) { return block[at]; });
                }
        },
        region, cell_size, band);

    // sample_blocks measured what it stored, and it stored the flattened block
    // -- the blend happens inside the fill, above, before the classification.
    // See the note on the overload above.
    return out;
}

FieldVolume flatten(const FieldVolume& v, const FlattenSettings& settings) {
    if (v.empty()) return v;
    Plane pl;
    // Nothing to do, so nothing is done. This used to resample the volume from
    // its own eval() over its own bounds, which is a costly way to say "the
    // source, unchanged" and not even an accurate one -- see the region note
    // below for what such a resample does to a volume.
    if (!resolve_plane(settings, &pl)) return v;

    // Where the weight can be non-zero: the region plus its taper, and the
    // taper as region_weight CLAMPS it rather than as the caller wrote it. A
    // region measured to a different taper than the operator uses is the one
    // failure mode resample_region cannot detect.
    //
    // No margin for how far the surface MOVES, which is worth being explicit
    // about because flatten moves it by many band widths and it looks as though
    // one is owed. There is not: flatten_at returns the source untouched
    // wherever the weight is zero, so the field outside this ball is unchanged,
    // so the zero set outside it is unchanged. Whatever facet the brush creates
    // lies inside the ball that created it.
    const FieldVolume::Region support = FieldVolume::Region::ball(
        settings.centre, settings.region_radius + std::max(settings.falloff, 1e-4f));

    // Reads come from `v` and writes go to the copy, so no half-written brick
    // is ever a source -- the ordering rule snapshot_region documents, met by
    // construction rather than by a snapshot.
    FieldVolume out = v;
    out.resample_region(support,
                        [&v, &settings, &pl](const FieldVolume::BrickGrid& grid, std::size_t first,
                                             std::size_t count, float* block) {
                            for (std::size_t s = 0; s < count; ++s)
                                for (int i = 0; i < kBrickSamples; ++i) {
                                    int g[3];
                                    grid.sample_cell(first + s, i, g);
                                    block[s * kBrickSamples + static_cast<std::size_t>(i)] =
                                        flatten_at(settings, pl, grid.sample_position(first + s, i),
                                                   [&](cfloat3 p) { return source_at(v, g, p); });
                                }
                        });
    return out;
}

}  // namespace field
}  // namespace clay
