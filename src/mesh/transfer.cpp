#include "clay/mesh/transfer.h"

#include <cmath>

#include "clay/math/geom.h"
#include "clay/mesh/bvh.h"

namespace clay {
namespace mesh {

namespace {

using kernel::cfloat3;

// The barycentric read `Bvh::ClosestPoint` exists to make possible. One
// function for every attribute so the three cannot interpolate differently.
template <typename T>
T interpolate(const std::vector<T>& attr, const std::vector<std::uint32_t>& indices,
              std::uint32_t triangle, float u, float v) {
    const std::uint32_t i0 = indices[triangle * 3];
    const std::uint32_t i1 = indices[triangle * 3 + 1];
    const std::uint32_t i2 = indices[triangle * 3 + 2];
    const float w = 1.0f - u - v;
    // A query that lands on a corner has to return THAT corner's value, bit for
    // bit — it is what makes an identity transfer exact, and a transfer that
    // reproduces a vertex ALMOST exactly is not a transfer anyone can chain.
    //
    // The weighted sum below delivers that only when the corner's weight is
    // exactly 1, and `Bvh::closest` gives exactly 1 on x86 but not on Apple
    // silicon, where the dot products behind it contract to fma and leave ~1e-8
    // on the other two corners. So the corner is taken, not hoped for. The
    // threshold is ~100 ulps at magnitude 1: far above the rounding it absorbs,
    // far below 8-bit colour quantisation (4e-3), so nothing visible snaps.
    constexpr float kCorner = 1.0f - 1e-5f;
    if (w >= kCorner) return attr[i0];
    if (u >= kCorner) return attr[i1];
    if (v >= kCorner) return attr[i2];
    // A weighted sum rather than a lerp chain, so the three attributes cannot
    // pick up different rounding from each other.
    return attr[i0] * w + attr[i1] * u + attr[i2] * v;
}

float derived_threshold(const Mesh& source) {
    math::Aabb box;
    for (const cfloat3& p : source.positions) box.expand(p);
    if (box.empty()) return 0.0f;
    return kernel::clength(box.max - box.min) * 0.05f;
}

}  // namespace

kernel::cfloat3 sample_color(const Mesh& m, std::uint32_t triangle, float u, float v,
                             kernel::cfloat3 fallback) {
    if (m.colors.size() != m.positions.size()) return fallback;
    if (static_cast<std::size_t>(triangle) * 3 + 2 >= m.indices.size()) return fallback;
    return interpolate(m.colors, m.indices, triangle, u, v);
}

kernel::cfloat2 sample_uv(const Mesh& m, std::uint32_t triangle, float u, float v,
                          kernel::cfloat2 fallback) {
    if (m.uvs.size() != m.positions.size()) return fallback;
    if (static_cast<std::size_t>(triangle) * 3 + 2 >= m.indices.size()) return fallback;
    return interpolate(m.uvs, m.indices, triangle, u, v);
}

TransferReport transfer_attributes(const Mesh& source, Mesh* target,
                                   const TransferOptions& options) {
    TransferReport report;
    if (!target || target->positions.empty() || source.positions.empty() ||
        source.indices.size() < 3)
        return report;

    const std::size_t sv = source.positions.size();
    const bool has_colors = options.colors && source.colors.size() == sv;
    const bool has_uvs = options.uvs && source.uvs.size() == sv;
    const bool has_normals = options.normals && source.normals.size() == sv;
    if (!has_colors && !has_uvs && !has_normals) return report;

    report.max_distance =
        options.max_distance > 0.0f ? options.max_distance : derived_threshold(source);

    // A channel the target does not carry yet is created and filled with a
    // documented default, so a vertex that falls back has a defined value
    // rather than whatever the allocation held.
    const std::size_t tv = target->positions.size();
    if (has_colors && target->colors.size() != tv)
        target->colors.assign(tv, kernel::cf3(1, 1, 1));
    if (has_uvs && target->uvs.size() != tv) target->uvs.assign(tv, kernel::cf2(0, 0));
    if (has_normals && target->normals.size() != tv)
        target->normals.assign(tv, kernel::cf3(0, 1, 0));

    const Bvh bvh = Bvh::build(source);
    const std::size_t triangles = source.indices.size() / 3;

    for (std::size_t i = 0; i < tv; ++i) {
        const Bvh::ClosestPoint hit = bvh.closest(target->positions[i]);
        if (!hit.found || hit.triangle >= triangles || hit.distance > report.max_distance) {
            ++report.fell_back;
            continue;
        }
        if (has_colors)
            target->colors[i] =
                sample_color(source, hit.triangle, hit.u, hit.v, target->colors[i]);
        if (has_uvs)
            target->uvs[i] = sample_uv(source, hit.triangle, hit.u, hit.v, target->uvs[i]);
        if (has_normals) {
            cfloat3 n = interpolate(source.normals, source.indices, hit.triangle, hit.u, hit.v);
            const float len = kernel::clength(n);
            // Interpolating three unit normals gives a shorter one; renormalise
            // so a transferred normal is still a normal.
            if (len > 1e-20f) n = n / len;
            target->normals[i] = n;
        }
        ++report.transferred;
    }

    report.colors = has_colors;
    report.uvs = has_uvs;
    report.normals = has_normals;
    return report;
}

}  // namespace mesh
}  // namespace clay
