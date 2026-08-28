// Relaxing a sampled field (sdf-kernels spec, add-sdf-relax). See
// include/clay/field/relax.h for why smoothing keeps the property sphere
// tracing depends on even though it destroys exactness.

#include "clay/field/relax.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <optional>
#include <vector>


namespace clay {
namespace field {

using kernel::cf3;
using kernel::cfloat3;

namespace {

// The cell-aligned taps within `radius` cells. Integer offsets, because relax
// works on the stored samples rather than on world positions.
struct Stencil {
    std::vector<std::array<int, 3>> offsets;
    std::vector<float> weights;
};

Stencil build_stencil(int radius_cells) {
    Stencil s;
    const int r = std::max(1, radius_cells);
    const float limit = static_cast<float>(r) + 0.001f;
    float total = 0.0f;
    for (int z = -r; z <= r; ++z)
        for (int y = -r; y <= r; ++y)
            for (int x = -r; x <= r; ++x) {
                float distance = std::sqrt(static_cast<float>(x * x + y * y + z * z));
                if (distance > limit) continue;  // a ball, not a cube
                s.offsets.push_back({x, y, z});
                // Uniform within the ball. A curvature-weighted variant is
                // additive and deliberately not here.
                s.weights.push_back(1.0f);
                total += 1.0f;
            }
    for (float& w : s.weights) w /= total;
    return s;
}

// How much of the smoothed value to take at `p`. Outside the region this is
// zero and the field is left exactly as it was.
float region_weight(const RelaxSettings& settings, cfloat3 p) {
    if (!(settings.region_radius > 0.0f)) return 1.0f;  // everywhere
    // Squared, because both answers that need no interpolation can be given
    // from the square and they are the overwhelming majority. A brush selects
    // whole bricks from a box around its sphere, so most samples it is handed
    // are outside it entirely -- measured, between 62% and 95% -- and the
    // square root that told them so was paid once per sample.
    const cfloat3 d = p - settings.centre;
    const float d2 = kernel::cdot(d, d);
    const float inner = settings.region_radius;
    if (d2 <= inner * inner) return 1.0f;
    // A hard edge would leave a visible rim where the smoothed field meets the
    // untouched one, so the effect tapers.
    const float taper = std::max(settings.falloff, 1e-4f);
    const float outer = inner + taper;
    if (d2 >= outer * outer) return 0.0f;
    // Only the taper itself needs the distance.
    const float t = (std::sqrt(d2) - inner) / taper;
    return 1.0f - t * t * (3.0f - 2.0f * t);  // smoothstep
}

// The freeze. Scaling by (1 - mask) means a fully masked sample takes weight
// zero and relax returns it verbatim rather than nearly so — a frozen region
// that drifts by a rounding error per iteration is a frozen region that moves.
float mask_gate(const MaskGate& mask, cfloat3 p) {
    if (!mask) return 1.0f;
    return 1.0f - std::clamp(mask(p), 0.0f, 1.0f);
}

}  // namespace

RelaxResult relax_in_place(FieldVolume& volume, const RelaxSettings& settings,
                           parallel::CancelToken* token) {
    RelaxResult result;
    if (volume.empty()) return result;

    const float cell = volume.cell_size();
    const float strength = std::clamp(settings.strength, 0.0f, 1.0f);
    const int iterations = std::max(1, settings.iterations);
    const int radius = std::max(1, settings.radius_cells);
    const Stencil stencil = build_stencil(radius);

    RelaxSettings tuned = settings;
    // A taper narrower than the kernel cannot hide the seam the kernel makes,
    // and one of zero would step. Both are silently widened rather than obeyed.
    tuned.falloff = std::max(settings.falloff, cell * static_cast<float>(radius) * 2.0f);

    // Where the weight can be non-zero: the region plus its taper. A radius of
    // zero means "everywhere", which is a filter rather than a brush, and then
    // there is nothing to bound.
    std::optional<FieldVolume::Region> region_bounds;
    if (tuned.region_radius > 0.0f)
        // A BALL rather than the box around it: the weight is zero outside the
        // taper, so a brick the ball cannot reach holds nothing this pass may
        // change. The box around it holds nearly twice as many.
        region_bounds = FieldVolume::Region::ball(tuned.centre,
                                                  tuned.region_radius + tuned.falloff);

    FieldVolume& current = volume;
    // The checkpoint is the pass boundary, which is the granularity that
    // already exists: relax is `iterations` sweeps of the whole band, so a
    // check per pass costs one relaxed load per sweep. That granularity is now
    // a CONTRACT rather than a convenience -- an in-place operator has no input
    // to hand back, so "whole passes only" is the strongest thing it can
    // promise, and it is what `RelaxResult::cancelled` reports. The copying
    // `relax` below still discards the lot, which is what it always did.
    parallel::ProgressScope progress(token, static_cast<std::uint32_t>(iterations));
    int completed = 0;
    for (int pass = 0; pass < iterations; ++pass) {
        if (parallel::cancelled(token)) {
            result.cancelled = true;
            break;  // whole passes only: nothing is half-written to unwind
        }
        progress.phase(static_cast<std::uint32_t>(pass));
        // The pass's INPUT, for the bricks the pass will overwrite. Copying the
        // whole volume for that -- which is what this was -- costs six
        // megabytes at an interactive cell to protect a few hundred kilobytes,
        // and put a term that scales with the model back into a dab that had
        // just been made to scale with itself. Reads outside the snapshotted
        // bricks come from `current`, which still holds what it held, because
        // those bricks are not written. The region must be the SAME one the
        // rewrite uses, and below it is.
        const std::optional<FieldVolume::RegionSnapshot> snapshot =
            region_bounds ? std::optional<FieldVolume::RegionSnapshot>(
                                current.snapshot_region(*region_bounds))
                          : std::nullopt;
        const FieldVolume previous = region_bounds ? FieldVolume() : current;
        auto blend = [&previous, &snapshot, &current, &stencil, &tuned, strength](
                         int gx, int gy, int gz, float old) {
            auto tap_at = [&](int x, int y, int z) {
                return snapshot ? snapshot->sample_at(x, y, z) : previous.sample_at(x, y, z);
            };
            // `old` is what a lookup of this sample would return, so there is
            // no lookup. rewrite_region hands over the value held in the brick
            // it is writing, and that brick has not been written yet this pass;
            // the snapshot holds the same coordinate's value from before the
            // pass; and the two copies of a sample shared across a brick face
            // cannot disagree, because every writer is a function of the GLOBAL
            // coordinate. So they are the same number.
            //
            // Worth removing rather than tidying: it ran once per sample, and
            // most samples a brush is handed are outside it — the selection is
            // whole bricks from a box around a sphere.
            const float here = old;
            const cfloat3 at = current.cell_position(gx, gy, gz);
            const float weight = region_weight(tuned, at) * mask_gate(tuned.mask, at);
            if (weight <= 0.0f) return here;

            // Only taps that EXIST count. A brick with no samples is not a
            // measurement of zero, it is the absence of one, and renormalizing
            // over the taps that are there smooths with the data rather than
            // dragging the edge of the band inward.
            float averaged = 0.0f;
            float total = 0.0f;
            for (std::size_t i = 0; i < stencil.offsets.size(); ++i) {
                const auto& d = stencil.offsets[i];
                std::optional<float> tap = tap_at(gx + d[0], gy + d[1], gz + d[2]);
                if (!tap) continue;
                averaged += stencil.weights[i] * *tap;
                total += stencil.weights[i];
            }
            if (total <= 0.0f) return here;
            return here + (averaged / total - here) * (strength * weight);
        };
        // A dab should cost what it moves. Outside the region `blend` returns
        // the sample it was handed — that is the `weight <= 0` line above — so
        // walking the rest of the band only to be told so is the whole of what
        // made a five-cell brush cost what the model cost. The region is the
        // sphere the weight is non-zero in, which is the taper's outer edge and
        // not the brush radius; `rewrite_region` rounds it outward to whole
        // bricks and requires exactly the identity `blend` already has.
        //
        // No margin for the STENCIL is needed, and that is worth being explicit
        // about because it looks as though it should be: the taps are read from
        // `previous`, which is the whole volume and is not being written, so a
        // sample inside the region may read neighbours outside it freely. What
        // the region bounds is where values CHANGE, and that is exactly where
        // the weight is non-zero, on this pass and on every later one.
        if (region_bounds) {
            const FieldVolume::RewriteTally tally =
                current.rewrite_region_tallied(*region_bounds, blend);
            result.dirty_bounds.expand(tally.bounds);
            // The same region every pass selects the same bricks, so this is
            // the pass's count rather than a sum over passes -- which is what a
            // host invalidating a preview and a scaling test both want.
            result.touched_bricks = std::max(result.touched_bricks, tally.touched_bricks);
            result.changed = result.changed || tally.changed;
        } else {
            // The unregioned path is a FILTER over the whole volume, and
            // `rewrite` has no tally to give because there is no selection to
            // report: everything stored is written. The dirty region is the
            // volume, and whether anything moved is the one question left.
            bool moved = false;
            auto watched = [&blend, &moved](int gx, int gy, int gz, float old) {
                const float now = blend(gx, gy, gz, old);
                if (now != old) moved = true;
                return now;
            };
            current.rewrite(watched);
            result.dirty_bounds.expand(current.bounds());
            result.touched_bricks = current.brick_count();
            result.changed = result.changed || moved;
        }
        ++completed;
    }

    // Smoothing MOVES the surface, and the sample-free bricks were classified
    // against where it used to be. Their bounds would otherwise overstate the
    // distance to the surface that is there now, which is the one thing a
    // bound may never do. A pass cannot move it further than the kernel
    // reaches, so that is the margin.
    // ...by what the passes that RAN could have moved it. A cancelled relax
    // that narrowed the band for passes it never made would leave the empty
    // bricks claiming less than the truth, and understating a distance is the
    // one direction the bound may not err in.
    current.shrink_band(static_cast<float>(radius) * cell * static_cast<float>(completed));
    return result;
}

FieldVolume relax(const FieldVolume& v, const RelaxSettings& settings,
                  parallel::CancelToken* token) {
    if (v.empty()) return v;
    FieldVolume out = v;
    // A standalone relax still hands back the INPUT on a cancel, which is what
    // it has always done and what its callers are written against: the volume
    // is the return value, so there is no half-relaxed thing for them to hold.
    // The in-place form cannot offer that, and does not have to -- a
    // transaction keeps the passes it paid for. Same algorithm, two ownerships.
    if (relax_in_place(out, settings, token).cancelled) return v;
    return out;
}

FieldVolume relax(const std::function<float(kernel::cfloat3)>& source, const math::Aabb& region,
                  float cell_size, float band, const RelaxSettings& settings) {
    return relax(FieldVolume::sample(source, region, cell_size, band), settings);
}

FieldVolume relax(const FieldVolume::BrickBlockFill& source, const math::Aabb& region,
                  float cell_size, float band, const RelaxSettings& settings) {
    // Still exactly sample-then-relax, which is what the per-point overload
    // above is too — relax averages cell-aligned taps and a fresh bake's taps
    // ARE the source at those lattice points, so there is nothing a fused form
    // could do better. All that changes is who evaluates the source.
    return relax(FieldVolume::sample_blocks(source, region, cell_size, band), settings);
}

}  // namespace field
}  // namespace clay
