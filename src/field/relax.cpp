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
    float distance = kernel::clength(p - settings.centre);
    if (distance <= settings.region_radius) return 1.0f;
    // A hard edge would leave a visible rim where the smoothed field meets the
    // untouched one, so the effect tapers.
    float taper = std::max(settings.falloff, 1e-4f);
    float t = (distance - settings.region_radius) / taper;
    if (t >= 1.0f) return 0.0f;
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

FieldVolume relax(const FieldVolume& v, const RelaxSettings& settings,
                  parallel::CancelToken* token) {
    if (v.empty()) return v;

    const float cell = v.cell_size();
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
    std::optional<math::Aabb> region_bounds;
    if (tuned.region_radius > 0.0f) {
        const float reach = tuned.region_radius + tuned.falloff;
        const cfloat3 r = cf3(reach, reach, reach);
        region_bounds = math::Aabb{tuned.centre - r, tuned.centre + r};
    }

    FieldVolume current = v;
    // The checkpoint is the pass boundary, which is the granularity that
    // already exists: relax is `iterations` sweeps of the whole band, so a
    // check per pass costs one relaxed load per sweep. A partially-relaxed
    // volume is discarded by the caller, not returned.
    parallel::ProgressScope progress(token, static_cast<std::uint32_t>(iterations));
    for (int pass = 0; pass < iterations; ++pass) {
        if (parallel::cancelled(token)) return v;  // unchanged: the input copy
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
            float here = tap_at(gx, gy, gz).value_or(old);
            const cfloat3 at = current.cell_position(gx, gy, gz);
            float weight = region_weight(tuned, at) * mask_gate(tuned.mask, at);
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
        if (region_bounds) current.rewrite_region(*region_bounds, blend);
        else current.rewrite(blend);
    }

    // Smoothing MOVES the surface, and the sample-free bricks were classified
    // against where it used to be. Their bounds would otherwise overstate the
    // distance to the surface that is there now, which is the one thing a
    // bound may never do. A pass cannot move it further than the kernel
    // reaches, so that is the margin.
    current.shrink_band(static_cast<float>(radius) * cell * static_cast<float>(iterations));
    return current;
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
