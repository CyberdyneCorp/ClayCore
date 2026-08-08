#include "clay/mesh/decimate.h"

#include <meshoptimizer.h>

#include <cstring>
#include <unordered_map>
#include <vector>

namespace clay {
namespace mesh {

namespace {

// Weld bit-identical positions (the mesher can emit distinct vertices at the
// same lattice point when crossings land exactly on it). meshoptimizer
// simplifies by position, so index-level cracks would otherwise appear in
// its output.
Mesh weld_positions(const Mesh& m) {
    struct Key {
        std::uint32_t x, y, z;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            return (static_cast<std::size_t>(k.x) * 73856093u) ^
                   (static_cast<std::size_t>(k.y) * 19349663u) ^
                   (static_cast<std::size_t>(k.z) * 83492791u);
        }
    };
    std::unordered_map<Key, std::uint32_t, KeyHash> seen;
    std::vector<std::uint32_t> remap(m.positions.size());
    Mesh out;
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        Key k;
        std::memcpy(&k.x, &m.positions[i].x, 4);
        std::memcpy(&k.y, &m.positions[i].y, 4);
        std::memcpy(&k.z, &m.positions[i].z, 4);
        auto it = seen.find(k);
        if (it != seen.end()) {
            remap[i] = it->second;
            continue;
        }
        std::uint32_t idx = static_cast<std::uint32_t>(out.positions.size());
        seen.emplace(k, idx);
        remap[i] = idx;
        out.positions.push_back(m.positions[i]);
        // Size equality, not emptiness: a short attribute array indexes out of
        // bounds here, and the second pass below already compares sizes.
        if (m.normals.size() == m.positions.size()) out.normals.push_back(m.normals[i]);
        if (m.colors.size() == m.positions.size()) out.colors.push_back(m.colors[i]);
        if (m.uvs.size() == m.positions.size()) out.uvs.push_back(m.uvs[i]);
    }
    out.indices.reserve(m.indices.size());
    for (std::size_t t = 0; t < m.triangle_count(); ++t) {
        std::uint32_t i0 = remap[m.indices[t * 3]];
        std::uint32_t i1 = remap[m.indices[t * 3 + 1]];
        std::uint32_t i2 = remap[m.indices[t * 3 + 2]];
        if (i0 == i1 || i1 == i2 || i0 == i2) continue;  // collapsed sliver
        out.indices.push_back(i0);
        out.indices.push_back(i1);
        out.indices.push_back(i2);
    }
    return out;
}

}  // namespace

Mesh decimate(const Mesh& input, const DecimateOptions& options) {
    if (input.empty()) return input;
    Mesh m = weld_positions(input);
    const std::size_t vcount = m.positions.size();
    const bool with_colors = options.color_weight > 0.0f && m.colors.size() == vcount;

    std::size_t target_indices =
        static_cast<std::size_t>(static_cast<float>(m.indices.size()) * options.target_ratio);
    target_indices -= target_indices % 3;
    if (target_indices < 3) target_indices = 3;

    std::vector<unsigned int> simplified(m.indices.size());
    float result_error = 0.0f;
    std::size_t out_count;
    if (with_colors) {
        const float weights[3] = {options.color_weight, options.color_weight,
                                  options.color_weight};
        out_count = meshopt_simplifyWithAttributes(
            simplified.data(), m.indices.data(), m.indices.size(), &m.positions[0].x, vcount,
            sizeof(kernel::cfloat3), &m.colors[0].x, sizeof(kernel::cfloat3), weights, 3,
            nullptr, target_indices, options.target_error, 0, &result_error);
    } else {
        out_count = meshopt_simplify(simplified.data(), m.indices.data(), m.indices.size(),
                                     &m.positions[0].x, vcount, sizeof(kernel::cfloat3),
                                     target_indices, options.target_error, 0, &result_error);
    }
    simplified.resize(out_count);

    // compact: remap referenced vertices only
    std::vector<std::uint32_t> remap(vcount, UINT32_MAX);
    Mesh out;
    out.indices.reserve(out_count);
    for (unsigned int idx : simplified) {
        if (remap[idx] == UINT32_MAX) {
            remap[idx] = static_cast<std::uint32_t>(out.positions.size());
            out.positions.push_back(m.positions[idx]);
            if (m.normals.size() == vcount) out.normals.push_back(m.normals[idx]);
            if (m.colors.size() == vcount) out.colors.push_back(m.colors[idx]);
            if (m.uvs.size() == vcount) out.uvs.push_back(m.uvs[idx]);
        }
        out.indices.push_back(remap[idx]);
    }
    return out;
}

}  // namespace mesh
}  // namespace clay
