// Merging coincident vertices and dropping what that collapses (add-mesh-weld).

#include "clay/mesh/weld.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "clay/math/geom.h"
#include "clay/mesh/quad_mesh.h"

namespace clay {
namespace mesh {

namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFu;

bool close_enough(kernel::cfloat2 a, kernel::cfloat2 b, float eps) {
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps;
}
bool close_enough(kernel::cfloat3 a, kernel::cfloat3 b, float eps) {
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps &&
           std::fabs(a.z - b.z) <= eps;
}

// Whether two vertices may merge on their ATTRIBUTES, having already agreed on
// position. A UV seam is duplicated positions carrying different uvs, so this is
// what keeps welding from destroying one.
bool attributes_agree(const Mesh& m, std::uint32_t a, std::uint32_t b, float eps) {
    if (m.uvs.size() == m.positions.size() && !close_enough(m.uvs[a], m.uvs[b], eps))
        return false;
    if (m.colors.size() == m.positions.size() && !close_enough(m.colors[a], m.colors[b], eps))
        return false;
    return true;
}

}  // namespace

WeldReport weld(Mesh* m, const WeldOptions& options) {
    WeldReport report;
    if (!m) return report;
    report.vertices_before = m->positions.size();
    report.triangles_before = m->triangle_count();
    if (m->positions.empty() || m->indices.empty()) return report;

    // THROUGH `Adjacency`, not beside it. The spatial hash, the 27-cell search
    // and the exact-bit path all live there already, and two copies of that
    // arithmetic would be two answers to "are these the same vertex" — which is
    // exactly the question a caller welding before building an Adjacency needs
    // to have answered the same way twice.
    const Adjacency adjacency = Adjacency::build(*m, options.epsilon);
    // The relative epsilon resolved against this mesh's own size, computed the
    // same way Adjacency computes it so the reported number is the one that was
    // actually applied.
    if (options.epsilon > 0.0f) {
        math::Aabb box;
        for (const kernel::cfloat3& p : m->positions) box.expand(p);
        report.epsilon = options.epsilon * kernel::clength(box.max - box.min);
    }

    // The survivor of each class: the lowest-indexed vertex in it that this
    // vertex may actually merge with. Lowest rather than first-seen so the
    // result depends on the mesh and not on a traversal order.
    const std::size_t vertices = m->positions.size();
    std::vector<std::uint32_t> survivor(vertices, kNone);
    std::vector<std::uint32_t> class_first(adjacency.class_count(), kNone);
    for (std::size_t v = 0; v < vertices; ++v) {
        const std::uint32_t cls = adjacency.class_of(static_cast<std::uint32_t>(v));
        const std::uint32_t first = class_first[cls];
        if (first == kNone) {
            class_first[cls] = static_cast<std::uint32_t>(v);
            survivor[v] = static_cast<std::uint32_t>(v);
            continue;
        }
        // A class can hold vertices this one may not merge with — a seam's two
        // corners share a position and differ in uv. Merge only into the first
        // member that agrees; otherwise stand alone and become a survivor
        // ourselves, so a third vertex matching THIS one still finds it.
        if (options.preserve_attribute_splits &&
            !attributes_agree(*m, first, static_cast<std::uint32_t>(v),
                              options.attribute_epsilon)) {
            survivor[v] = static_cast<std::uint32_t>(v);
            continue;
        }
        survivor[v] = first;
        ++report.vertices_merged;
    }

    // A triangle naming a vertex that does not exist has to go, and counts as
    // work — otherwise the fast path below would hand a malformed mesh straight
    // back whenever nothing else needed merging, which would make "every index
    // is in range afterwards" true only sometimes.
    std::size_t invalid = 0;
    for (std::size_t t = 0; t + 2 < m->indices.size(); t += 3)
        if (m->indices[t] >= vertices || m->indices[t + 1] >= vertices ||
            m->indices[t + 2] >= vertices)
            ++invalid;
    // A trailing partial triangle is malformed too, and the rebuild below drops
    // it by walking whole triples.
    const bool ragged = m->indices.size() % 3 != 0;

    // Nothing merged and nothing malformed: leave the mesh exactly as it was,
    // quads and all. A weld that changed nothing must not renumber anything, or
    // every caller pays a rewrite for the common case where there was nothing
    // to do.
    if (report.vertices_merged == 0 && invalid == 0 && !ragged) {
        report.vertices_after = report.vertices_before;
        report.triangles_after = report.triangles_before;
        std::vector<std::uint8_t> seen(vertices, 0);
        for (std::uint32_t index : m->indices)
            if (index < vertices) seen[index] = 1;
        for (std::uint8_t s : seen) report.vertices_unreferenced += s ? 0 : 1;
        return report;
    }

    // Rebuild the triangle list against the survivors, dropping the collapsed.
    std::vector<std::uint32_t> kept;
    kept.reserve(m->indices.size());
    for (std::size_t t = 0; t + 2 < m->indices.size(); t += 3) {
        const std::uint32_t raw[3] = {m->indices[t], m->indices[t + 1], m->indices[t + 2]};
        if (raw[0] >= vertices || raw[1] >= vertices || raw[2] >= vertices) {
            ++report.triangles_invalid;
            continue;
        }
        const std::uint32_t s[3] = {survivor[raw[0]], survivor[raw[1]], survivor[raw[2]]};
        if (s[0] == s[1] || s[1] == s[2] || s[0] == s[2]) {
            ++report.triangles_collapsed;
            continue;
        }
        kept.insert(kept.end(), {s[0], s[1], s[2]});
    }

    // Compact: survivors that a surviving triangle still references, in their
    // original order, so the output is the input with things removed rather
    // than the input reordered.
    std::vector<std::uint8_t> referenced(vertices, 0);
    for (std::uint32_t index : kept) referenced[index] = 1;
    std::vector<std::uint32_t> remap(vertices, kNone);
    Mesh out;
    const bool has_normals = m->normals.size() == vertices;
    const bool has_colors = m->colors.size() == vertices;
    const bool has_uvs = m->uvs.size() == vertices;
    for (std::size_t v = 0; v < vertices; ++v) {
        if (!referenced[v]) {
            if (survivor[v] == v) ++report.vertices_unreferenced;
            continue;
        }
        remap[v] = static_cast<std::uint32_t>(out.positions.size());
        out.positions.push_back(m->positions[v]);
        if (has_normals) out.normals.push_back(m->normals[v]);
        if (has_colors) out.colors.push_back(m->colors[v]);
        if (has_uvs) out.uvs.push_back(m->uvs[v]);
    }
    out.indices.reserve(kept.size());
    for (std::uint32_t index : kept) out.indices.push_back(remap[index]);

    // `quads` is deliberately not carried: this rewrote `indices`, and
    // mesh_data.h's rule is that a rewrite must clear them rather than leave a
    // quad list describing triangles that no longer exist.
    report.quads_dropped = m->has_quads();
    report.vertices_after = out.positions.size();
    report.triangles_after = out.triangle_count();
    *m = std::move(out);
    return report;
}

}  // namespace mesh
}  // namespace clay
