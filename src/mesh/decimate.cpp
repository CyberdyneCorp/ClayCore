#include "clay/mesh/decimate.h"

#include <meshoptimizer.h>

#include <cstring>
#include <unordered_map>
#include <vector>

#include "clay/mesh/quad_mesh.h"  // drop_quads

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

// Does any edge carry more than two triangles?
//
// The same question `validate` answers, asked on its own because the answer is
// wanted on the HOT path: every decimation checks its own result, and the rest
// of a validation report -- orientation, Euler, slivers -- is not consulted here.
bool edge_manifold(const Mesh& m) {
    std::unordered_map<std::uint64_t, int> incidence;
    incidence.reserve(m.triangle_count() * 3);
    for (std::size_t t = 0; t < m.triangle_count(); ++t) {
        const std::uint32_t* idx = &m.indices[t * 3];
        if (idx[0] == idx[1] || idx[1] == idx[2] || idx[0] == idx[2]) continue;
        for (int e = 0; e < 3; ++e) {
            const std::uint32_t a = idx[e], b = idx[(e + 1) % 3];
            const std::uint64_t key = a < b ? (static_cast<std::uint64_t>(a) << 32) | b
                                            : (static_cast<std::uint64_t>(b) << 32) | a;
            if (++incidence[key] > 2) return false;
        }
    }
    return true;
}

// One meshoptimizer pass, compacted to the vertices it actually references.
Mesh simplify_to(const Mesh& m, std::size_t target_indices, const DecimateOptions& options,
                 bool with_colors, unsigned int flags) {
    const std::size_t vcount = m.positions.size();
    std::vector<unsigned int> simplified(m.indices.size());
    float result_error = 0.0f;
    std::size_t out_count;
    if (with_colors) {
        const float weights[3] = {options.color_weight, options.color_weight,
                                  options.color_weight};
        out_count = meshopt_simplifyWithAttributes(
            simplified.data(), m.indices.data(), m.indices.size(), &m.positions[0].x, vcount,
            sizeof(kernel::cfloat3), &m.colors[0].x, sizeof(kernel::cfloat3), weights, 3,
            nullptr, target_indices, options.target_error, flags, &result_error);
    } else {
        out_count = meshopt_simplify(simplified.data(), m.indices.data(), m.indices.size(),
                                     &m.positions[0].x, vcount, sizeof(kernel::cfloat3),
                                     target_indices, options.target_error, flags, &result_error);
    }
    simplified.resize(out_count);

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

std::size_t aligned_target(std::size_t indices, float ratio) {
    std::size_t target = static_cast<std::size_t>(static_cast<float>(indices) * ratio);
    target -= target % 3;
    return target < 3 ? 3 : target;
}

// What to try when the requested simplification lands on a pinch.
//
// EVERY RETRY KEEPS THE REQUESTED SIZE. `meshopt_SimplifyRegularize` picks
// different collapses for the same target, and every recovery measured came
// from that and not from asking for more geometry: 4 of 20 configurations
// pinched, all 4 cleared here, within two triangles of the count requested.
//
// An earlier version also retried at 1.05x, 1.25x and 1.6x the target. It is
// gone because a silent size increase is the wrong trade for the callers that
// decimate at all: examples/_render.py exports at a ratio of 0.12 against a
// 400 KiB budget for committed models. A caller picks a ratio because it needs
// that size.
//
// Asking for the same target is NOT enough to hold a retry to the same size,
// which is the part that had to be measured rather than assumed. Regularize
// weighs triangle shape and will stop short of a target the plain pass reaches:
// on 04_repeat_radial it answered 1,744 triangles where 834 were asked for and
// 834 were delivered without it. So a retry is accepted only if it did not grow
// the result materially -- the recoveries that matter came in within two
// triangles, so the allowance is not what makes them work.
constexpr float kRetryGrowthAllowance = 1.05f;
//
// Neither flag is a guarantee. Regularize is not a manifold-preserving mode --
// measured over 21 shape-and-ratio combinations it both fixed cases and broke
// ones that were clean without it -- so it is another thing to try and then
// check, never a thing to trust.
constexpr unsigned int kRetryFlags[] = {meshopt_SimplifyRegularize,
                                        meshopt_SimplifyRegularizeLight};

void report_to(DecimateReport* report, bool manifold, bool input_manifold, int attempts) {
    if (!report) return;
    report->manifold = manifold;
    report->input_manifold = input_manifold;
    report->attempts = attempts;
}

}  // namespace

Mesh decimate(const Mesh& input, const DecimateOptions& options, DecimateReport* report) {
    // A decimated mesh is a TRIANGLE mesh: an edge collapse breaks the quad
    // pairing the first time it fires, so `out` below is built without quads
    // and there is nothing to carry over. The empty passthrough is the one
    // path that returns the input itself, and it drops them too rather than
    // leaving "empty" as the exception to a rule the rest of the tree obeys.
    if (input.empty()) {
        Mesh passthrough = input;
        drop_quads(passthrough);
        report_to(report, true, true, 0);
        return passthrough;
    }
    Mesh m = weld_positions(input);
    const std::size_t vcount = m.positions.size();
    const bool with_colors = options.color_weight > 0.0f && m.colors.size() == vcount;

    const std::size_t target_indices = aligned_target(m.indices.size(), options.target_ratio);
    Mesh out = simplify_to(m, target_indices, options, with_colors, 0);

    // meshoptimizer decides its own collapses and does not apply the link
    // condition `collapse_edge` refuses on (mesh/topology_ops.h), so a mesh that
    // arrives watertight and 2-manifold can leave with edges carrying four
    // incident triangles. Measured on an unmodified tree: a two-torus document
    // meshed at 0.035 and decimated to a quarter produced two such edges from an
    // input with none, and the Euler characteristic moved from -4 to -2 -- the
    // collapse closed a handle.
    //
    // Retry rather than repair. The pinches are FLAT -- the four triangles at
    // the two measured edges sit at 0, 178.7, 178.7 and -177.3 degrees around
    // the edge -- so which pair belongs to which sheet is numerically
    // undecidable, and the two answers differ in the genus they leave behind.
    if (edge_manifold(out)) {
        report_to(report, true, true, 1);
        return out;
    }
    if (!edge_manifold(m)) {
        report_to(report, false, false, 1);  // it arrived pinched; not this pass's doing
        return out;
    }

    const std::size_t size_ceiling = static_cast<std::size_t>(
        static_cast<float>(out.triangle_count()) * kRetryGrowthAllowance);
    int attempts = 1;
    for (unsigned int flags : kRetryFlags) {
        Mesh alt = simplify_to(m, target_indices, options, with_colors, flags);
        ++attempts;
        if (alt.triangle_count() > size_ceiling) continue;  // clean but not what was asked for
        if (edge_manifold(alt)) {
            report_to(report, true, true, attempts);
            return alt;
        }
    }

    // NO COLLAPSE ORDER CLEARS IT, so return the size that was asked for and say
    // it is pinched.
    //
    // The alternative -- hand back the undecimated input -- was written first
    // and is worse. On examples/37_groups it turned a requested 12,418 triangles
    // into 155,388, a twelvefold file, because at one triangle in twelve a
    // grouped model merges sheets by definition rather than by accident. A
    // caller that asked for 8% is not served by 100%, and one that cannot use a
    // pinched mesh needs to be told rather than handed a different problem.
    report_to(report, false, true, attempts);
    return out;
}

}  // namespace mesh
}  // namespace clay
