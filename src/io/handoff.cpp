#include "clay/io/handoff.h"

#include <cstring>

#include "file_bytes.h"
#include "clay/kernel/shim.h"

namespace clay {
namespace io {

namespace {

std::uint8_t to_u8(float c) {
    return static_cast<std::uint8_t>(kernel::cclamp(c, 0.0f, 1.0f) * 255.0f + 0.5f);
}

void put_f32(std::vector<std::uint8_t>& out, float f) {
    std::uint8_t b[4];
    std::memcpy(b, &f, 4);
    out.insert(out.end(), b, b + 4);
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    std::uint8_t b[4];
    std::memcpy(b, &v, 4);
    out.insert(out.end(), b, b + 4);
}

}  // namespace

std::vector<float> handoff_material_mix(const mesh::Mesh& m, const voxel::MaskField* mask) {
    std::vector<float> mix(m.positions.size(), 0.0f);
    if (!mask) return mix;  // required payload, honest value
    for (std::size_t i = 0; i < m.positions.size(); ++i)
        mix[i] = kernel::cclamp(mask->sample(m.positions[i]), 0.0f, 1.0f);
    return mix;
}

std::vector<std::uint8_t> save_handoff_ply(const mesh::Mesh& m, const HandoffOptions& options) {
    // TRIANGLES, whatever the mesh carries. Their reader rejects any other
    // arity, and mesh_data.h guarantees `indices` is the triangulation of the
    // quads over the same positions — so this loses nothing but the quad
    // grouping, which their reader would not have accepted anyway.
    const std::size_t vertex_count = m.positions.size();
    const std::size_t face_count = m.indices.size() / 3;

    // Computed into a local, so a caller's mesh is never modified by writing it.
    const std::vector<kernel::cfloat3> computed =
        m.normals.size() == vertex_count ? std::vector<kernel::cfloat3>() : mesh::vertex_normals(m);
    const std::vector<kernel::cfloat3>& normals =
        m.normals.size() == vertex_count ? m.normals : computed;

    const bool has_colors = m.colors.size() == vertex_count;
    const std::vector<float> mix = handoff_material_mix(m, options.material_mask);

    std::string header = "ply\nformat ";
    header += options.binary ? "binary_little_endian 1.0\n" : "ascii 1.0\n";
    // The line that makes this a handoff rather than an ordinary PLY. Without
    // it their reader answers UnsupportedFormat, by design.
    header += "comment cyber_sculpt_handoff " + std::to_string(kHandoffVersionMajor) + " " +
              std::to_string(kHandoffVersionMinor) + "\n";
    if (!options.producer.empty())
        header += "comment cyber_handoff_producer " + options.producer + "\n";
    header += "element vertex " + std::to_string(vertex_count) + "\n";
    header += "property float x\nproperty float y\nproperty float z\n";
    header += "property float nx\nproperty float ny\nproperty float nz\n";
    header += "property uchar red\nproperty uchar green\nproperty uchar blue\n";
    header += "property float material_mix\n";
    header += "element face " + std::to_string(face_count) + "\n";
    header += "property list uchar int vertex_indices\nend_header\n";

    if (!options.binary) {
        std::string body;
        for (std::size_t i = 0; i < vertex_count; ++i) {
            const kernel::cfloat3 p = m.positions[i], n = normals[i];
            const kernel::cfloat3 c = has_colors ? m.colors[i] : kernel::cf3(1, 1, 1);
            body += std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z);
            body += " " + std::to_string(n.x) + " " + std::to_string(n.y) + " " +
                    std::to_string(n.z);
            body += " " + std::to_string(static_cast<int>(to_u8(c.x))) + " " +
                    std::to_string(static_cast<int>(to_u8(c.y))) + " " +
                    std::to_string(static_cast<int>(to_u8(c.z)));
            body += " " + std::to_string(mix[i]) + "\n";
        }
        for (std::size_t f = 0; f < face_count; ++f)
            body += "3 " + std::to_string(m.indices[f * 3]) + " " +
                    std::to_string(m.indices[f * 3 + 1]) + " " +
                    std::to_string(m.indices[f * 3 + 2]) + "\n";
        std::vector<std::uint8_t> out(header.begin(), header.end());
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }

    std::vector<std::uint8_t> out(header.begin(), header.end());
    for (std::size_t i = 0; i < vertex_count; ++i) {
        const kernel::cfloat3 p = m.positions[i], n = normals[i];
        const kernel::cfloat3 c = has_colors ? m.colors[i] : kernel::cf3(1, 1, 1);
        put_f32(out, p.x);
        put_f32(out, p.y);
        put_f32(out, p.z);
        put_f32(out, n.x);
        put_f32(out, n.y);
        put_f32(out, n.z);
        out.push_back(to_u8(c.x));
        out.push_back(to_u8(c.y));
        out.push_back(to_u8(c.z));
        put_f32(out, mix[i]);
    }
    for (std::size_t f = 0; f < face_count; ++f) {
        out.push_back(3);
        put_u32(out, m.indices[f * 3]);
        put_u32(out, m.indices[f * 3 + 1]);
        put_u32(out, m.indices[f * 3 + 2]);
    }
    return out;
}

IoStatus save_handoff_ply_file(const mesh::Mesh& m, const std::string& path,
                               const HandoffOptions& options) {
    return detail::write_whole_file(path, save_handoff_ply(m, options));
}

}  // namespace io
}  // namespace clay
