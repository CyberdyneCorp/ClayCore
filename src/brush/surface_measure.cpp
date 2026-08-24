#include "clay/brush/surface_measure.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#include "clay/kernel/field.h"
#include "clay/parallel/thread_pool.h"

namespace clay {
namespace brush {

using kernel::cfloat3;

namespace {

// The Laplacian of a distance field, by the six-point stencil.
//
// For f = |p| - R, a sphere of radius R, this is 2/R at the surface — POSITIVE
// for a convex surface, and the magnitude is the curvature. That unambiguous
// sign is why cavity and convexity are one subtraction apart here and an
// error-prone vertex-ring estimate in a mesh engine.
float laplacian(const std::function<float(cfloat3)>& f, cfloat3 p, float h) {
    const float c = f(p);
    const float sum = f(kernel::cf3(p.x + h, p.y, p.z)) + f(kernel::cf3(p.x - h, p.y, p.z)) +
                      f(kernel::cf3(p.x, p.y + h, p.z)) + f(kernel::cf3(p.x, p.y - h, p.z)) +
                      f(kernel::cf3(p.x, p.y, p.z + h)) + f(kernel::cf3(p.x, p.y, p.z - h));
    return (sum - 6.0f * c) / (h * h);
}

float saturate(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// An integer hash, so a point's sample rotation is a pure function of where it
// is and what seed was asked for. NOT a random number generator: the whole
// determinism guarantee rests on this being reproducible across runs, backends
// and thread schedules, and an RNG's state would depend on all three.
std::uint32_t hash_u32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

std::uint32_t hash_point(cfloat3 p, std::uint32_t seed) {
    // Quantised before hashing, so two points a float-epsilon apart get the
    // same rotation rather than two unrelated ones — otherwise neighbouring
    // texels would differ by sample noise rather than by occlusion, and the
    // bake would look like film grain.
    const auto q = [](float v) {
        return static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(v * 1024.0f)));
    };
    return hash_u32(q(p.x) ^ hash_u32(q(p.y) ^ hash_u32(q(p.z) ^ hash_u32(seed))));
}

// Hammersley on the hemisphere, cosine-weighted. Low-discrepancy rather than
// random, so N rays cover the hemisphere evenly instead of clumping — which is
// what lets 16 rays look like far more.
cfloat3 hemisphere_dir(int i, int n, std::uint32_t rotation, cfloat3 normal) {
    // Van der Corput radical inverse, XOR-scrambled by the point's hash. The
    // scramble is what decorrelates neighbouring points without making either
    // of them non-reproducible.
    std::uint32_t bits = static_cast<std::uint32_t>(i);
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
    bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
    bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
    bits ^= rotation;
    const float u1 = (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
    const float u2 = static_cast<float>(bits) * 2.3283064365386963e-10f;  // 2^-32

    // Cosine-weighted: rays near the normal carry more of the hemisphere's
    // solid angle, so weighting the DIRECTIONS means the average needs no
    // per-ray cosine term.
    const float r = std::sqrt(u1);
    const float phi = 6.2831853071795864f * u2;
    const float x = r * std::cos(phi);
    const float y = r * std::sin(phi);
    const float z = std::sqrt(std::max(0.0f, 1.0f - u1));

    // An orthonormal basis around the normal, branchless — Duff et al. The
    // usual "cross with an arbitrary axis" degenerates when the normal happens
    // to be that axis, which on an axis-aligned model is most of the surface.
    const float sign = normal.z >= 0.0f ? 1.0f : -1.0f;
    const float a = -1.0f / (sign + normal.z);
    const float b = normal.x * normal.y * a;
    const cfloat3 t1 = kernel::cf3(1.0f + sign * normal.x * normal.x * a, sign * b,
                                   -sign * normal.x);
    const cfloat3 t2 = kernel::cf3(b, sign + normal.y * normal.y * a, -normal.y);
    return kernel::cf3(t1.x * x + t2.x * y + normal.x * z, t1.y * x + t2.y * y + normal.y * z,
                       t1.z * x + t2.z * y + normal.z * z);
}

// March along a ray and report where it first met the surface, or a negative t.
// Bounded by construction: an unbounded probe is not a measure of anything
// local, and both callers here are asking a local question.
float march_hit(const std::function<float(cfloat3)>& f, cfloat3 origin, cfloat3 dir, float t_start,
                float t_max) {
    float t = t_start;
    for (int i = 0; i < 128 && t < t_max; ++i) {
        const float d = f(kernel::cf3(origin.x + dir.x * t, origin.y + dir.y * t,
                                      origin.z + dir.z * t));
        if (d < 1e-5f) return t;
        t += std::max(d, 1e-5f);
    }
    return -1.0f;
}

float occlusion_at(const std::function<float(cfloat3)>& f, cfloat3 p, cfloat3 normal,
                   const MeasureSettings& s, float length) {
    const int n = s.ray_count > 0 ? s.ray_count : 16;
    const std::uint32_t rot = hash_point(p, s.seed);
    // Start a little off the surface, or every ray immediately hits the point
    // it started from and everything reads fully occluded.
    const float bias = length * 0.01f;
    const cfloat3 start = kernel::cf3(p.x + normal.x * bias, p.y + normal.y * bias,
                                      p.z + normal.z * bias);
    float blocked = 0.0f;
    for (int i = 0; i < n; ++i) {
        const cfloat3 dir = hemisphere_dir(i, n, rot, normal);
        const float t = march_hit(f, start, dir, bias, length);
        if (t < 0.0f) continue;
        // A near blocker counts for more than a far one, which is what makes a
        // crevice read darker than a point beside a distant wall.
        blocked += s.falloff > 0.0f ? 1.0f / (1.0f + s.falloff * (t / length)) : 1.0f;
    }
    return saturate(blocked / static_cast<float>(n));
}

float thickness_at(const std::function<float(cfloat3)>& f, cfloat3 p, cfloat3 normal,
                   float length) {
    // INWARD, along -normal, until the field turns positive again: that is the
    // far wall, and the distance to it is how much material is behind the
    // point. One ray, not a hemisphere — thickness is a direction, not a
    // gathering.
    const cfloat3 dir = kernel::cf3(-normal.x, -normal.y, -normal.z);
    const float bias = length * 0.01f;
    float t = bias;
    for (int i = 0; i < 256 && t < length; ++i) {
        const float d = f(kernel::cf3(p.x + dir.x * t, p.y + dir.y * t, p.z + dir.z * t));
        if (d > 0.0f) return saturate(t / length);
        // Inside the solid the distance is negative and its magnitude is a safe
        // step toward the boundary, exactly as it is outside.
        t += std::max(-d, length * 0.005f);
    }
    // Never found the far wall within the probe: thicker than we can say.
    return 1.0f;
}

}  // namespace

float measure_at(const std::function<float(cfloat3)>& f, SurfaceMeasure measure, cfloat3 p,
                 const MeasureSettings& settings) {
    if (!f) return 0.0f;
    const float scale = settings.scale > 0.0f ? settings.scale : 0.05f;
    const float h = settings.h > 0.0f ? settings.h : scale * 0.2f;
    const float length = settings.ray_length > 0.0f ? settings.ray_length : scale * 20.0f;

    switch (measure) {
        case SurfaceMeasure::NormalDirection: {
            const cfloat3 dir = kernel::clength(settings.direction) > 0.0f
                                    ? kernel::cnormalize(settings.direction)
                                    : kernel::cf3(0.0f, 1.0f, 0.0f);
            const cfloat3 n = kernel::cnormal(f, p, h);
            const float agreement = kernel::cdot(n, dir);
            if (agreement <= settings.threshold) return 0.0f;
            // Remap [threshold, 1] onto [0, 1], so raising the threshold
            // NARROWS the cone rather than dimming the whole result — which is
            // what a caller means by a threshold.
            const float span = 1.0f - settings.threshold;
            return saturate(span > 0.0f ? (agreement - settings.threshold) / span : 1.0f);
        }
        case SurfaceMeasure::AmbientOcclusion:
            return occlusion_at(f, p, kernel::cnormal(f, p, h), settings, length);
        case SurfaceMeasure::Thickness:
            return thickness_at(f, p, kernel::cnormal(f, p, h), length);
        default: break;
    }

    // scale is the RADIUS that reads as fully saturated, and curvature is
    // 1/radius, so the product is the fraction.
    const float k = laplacian(f, p, h) * scale;
    switch (measure) {
        case SurfaceMeasure::Curvature: return saturate(std::fabs(k));
        case SurfaceMeasure::Cavity: return saturate(-k);   // concave
        case SurfaceMeasure::Convexity: return saturate(k);  // convex
        default: return 0.0f;
    }
}

void measure_points(const std::function<float(cfloat3)>& f, SurfaceMeasure measure,
                    const cfloat3* points, std::size_t count, const MeasureSettings& settings,
                    float* out_values, parallel::CancelToken* token, bool* out_cancelled) {
    if (out_cancelled) *out_cancelled = false;
    if (!f || !points || !out_values || count == 0) return;

    // ONE PHASE, not one per point. The second argument is a PHASE COUNT —
    // passing the work count both narrows size_t to uint32_t (which MSVC /WX
    // rejects and GCC accepts silently) and means the wrong thing: the
    // per-item figure is what advance() carries.
    parallel::ProgressScope progress(token, 1);
    // Chunked rather than per-point, so the cancel check is one relaxed load
    // per chunk instead of one per point — the same granularity every other
    // cancellable walk in the tree uses.
    constexpr std::size_t kChunk = 256;
    std::atomic<bool> stop{false};
    const std::size_t chunks = (count + kChunk - 1) / kChunk;

    // A cancelled chunk RETURNS NORMALLY and never throws. thread_pool.h's join
    // waits for `done >= num_tasks` and `run()` increments `done` only after
    // `fn` returns, so a chunk that threw would hang the join forever.
    parallel::for_range(chunks, 1, [&](std::size_t first, std::size_t last) {
        for (std::size_t chunk = first; chunk < last; ++chunk) {
            if (stop.load(std::memory_order_relaxed)) return;
            if (parallel::cancelled(token)) {
                stop.store(true, std::memory_order_relaxed);
                return;
            }
            const std::size_t begin = chunk * kChunk;
            const std::size_t end = std::min(begin + kChunk, count);
            for (std::size_t i = begin; i < end; ++i)
                out_values[i] = measure_at(f, measure, points[i], settings);
            progress.advance(end, static_cast<float>(end) / static_cast<float>(count));
        }
    });

    if (stop.load(std::memory_order_relaxed) && out_cancelled) *out_cancelled = true;
}

}  // namespace brush
}  // namespace clay
