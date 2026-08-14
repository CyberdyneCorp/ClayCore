#include <cstdio>
#include <cstring>
#include <sstream>

#include "clay/io/mesh_io.h"

#include "file_bytes.h"

namespace clay {
namespace io {

using kernel::cf2;
using kernel::cf3;

namespace {

// One `f` line of n corners. Lifted out of save_obj rather than left a lambda
// inside it because the four-way corner spelling is the whole of the format's
// complexity here, and leaving it nested charged save_obj for it.
//
// The corner spelling is the same whether n is 3 or 4: the vertex index doubles
// as the vt and vn index, because claycore meshes are vertex-aligned. OBJ is
// 1-based, hence the +1.
void write_face(std::string& out, const std::uint32_t* face, int n, bool normals, bool uvs) {
    out += "f";
    char corner[64];
    for (int i = 0; i < n; ++i) {
        const std::uint32_t a = face[i] + 1;
        if (normals && uvs)
            std::snprintf(corner, sizeof corner, " %u/%u/%u", a, a, a);
        else if (normals)
            std::snprintf(corner, sizeof corner, " %u//%u", a, a);
        else if (uvs)
            std::snprintf(corner, sizeof corner, " %u/%u", a, a);
        else
            std::snprintf(corner, sizeof corner, " %u", a);
        out += corner;
    }
    out += "\n";
}

}  // namespace

std::string save_obj(const mesh::Mesh& m, const std::string& object_name,
                     const std::string& mtl_name) {
    std::string out;
    out.reserve(m.positions.size() * 48);
    char line[256];
    out += "# claycore OBJ export (vertex-color extension: v x y z r g b)\n";
    if (!mtl_name.empty()) {
        out += "mtllib " + mtl_name + "\n";
        out += "usemtl clay_default\n";
    }
    out += "o " + object_name + "\n";
    const bool colors = m.colors.size() == m.positions.size();
    const bool normals = m.normals.size() == m.positions.size();
    const bool uvs = m.uvs.size() == m.positions.size();
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        if (colors)
            std::snprintf(line, sizeof line, "v %.9g %.9g %.9g %.9g %.9g %.9g\n",
                          m.positions[i].x, m.positions[i].y, m.positions[i].z, m.colors[i].x,
                          m.colors[i].y, m.colors[i].z);
        else
            std::snprintf(line, sizeof line, "v %.9g %.9g %.9g\n", m.positions[i].x,
                          m.positions[i].y, m.positions[i].z);
        out += line;
    }
    for (std::size_t i = 0; normals && i < m.normals.size(); ++i) {
        std::snprintf(line, sizeof line, "vn %.9g %.9g %.9g\n", m.normals[i].x, m.normals[i].y,
                      m.normals[i].z);
        out += line;
    }
    for (std::size_t i = 0; uvs && i < m.uvs.size(); ++i) {
        std::snprintf(line, sizeof line, "vt %.9g %.9g\n", m.uvs[i].x, m.uvs[i].y);
        out += line;
    }
    // A quad mesh writes its QUADS — that is the point of carrying them — and a
    // mesh without them writes exactly the triangles it always did, byte for
    // byte.
    if (m.has_quads())
        for (std::size_t q = 0; q < m.quad_count(); ++q)
            write_face(out, &m.quads[q * 4], 4, normals, uvs);
    else
        for (std::size_t t = 0; t < m.triangle_count(); ++t)
            write_face(out, &m.indices[t * 3], 3, normals, uvs);
    return out;
}

std::string save_mtl(const std::string& material_name) {
    return "newmtl " + material_name +
           "\nKa 1.0 1.0 1.0\nKd 0.8 0.8 0.8\nKs 0.1 0.1 0.1\nNs 16\nd 1.0\nillum 2\n";
}

IoStatus load_obj(const std::string& text, mesh::Mesh* out, const ImportBudget& budget) {
    mesh::Mesh m;
    std::vector<kernel::cfloat3> normals_pool;
    std::vector<kernel::cfloat2> uv_pool;
    bool any_colors = false;

    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.size() < 2) continue;
        if (line[0] == 'v' && line[1] == ' ') {
            float x, y, z, r, g, b;
            int n = std::sscanf(line.c_str() + 2, "%f %f %f %f %f %f", &x, &y, &z, &r, &g, &b);
            if (n < 3) return IoStatus::fail(IoError::Malformed, "bad vertex line");
            if (m.positions.size() >= budget.max_vertices)
                return IoStatus::fail(IoError::BudgetExceeded, "vertex budget");
            m.positions.push_back(cf3(x, y, z));
            if (n >= 6) {
                any_colors = true;
                m.colors.push_back(cf3(r, g, b));
            } else {
                m.colors.push_back(cf3(1, 1, 1));
            }
        } else if (line.rfind("vn ", 0) == 0) {
            float x, y, z;
            if (std::sscanf(line.c_str() + 3, "%f %f %f", &x, &y, &z) != 3)
                return IoStatus::fail(IoError::Malformed, "bad normal line");
            normals_pool.push_back(cf3(x, y, z));
        } else if (line.rfind("vt ", 0) == 0) {
            float u, v;
            if (std::sscanf(line.c_str() + 3, "%f %f", &u, &v) != 2)
                return IoStatus::fail(IoError::Malformed, "bad uv line");
            uv_pool.push_back(cf2(u, v));
        } else if (line[0] == 'f' && line[1] == ' ') {
            // parse "v", "v/vt", "v//vn", "v/vt/vn" tuples; triangulate fans
            std::vector<long> verts;
            const char* p = line.c_str() + 2;
            while (*p) {
                while (*p == ' ') ++p;
                if (!*p) break;
                char* end = nullptr;
                long v = std::strtol(p, &end, 10);
                if (end == p) return IoStatus::fail(IoError::Malformed, "bad face index");
                verts.push_back(v);
                p = end;
                while (*p && *p != ' ') ++p;  // skip /vt/vn part
            }
            if (verts.size() < 3) return IoStatus::fail(IoError::Malformed, "face < 3 verts");
            auto resolve = [&](long v) -> long {
                return v > 0 ? v - 1 : static_cast<long>(m.positions.size()) + v;
            };
            for (std::size_t i = 1; i + 1 < verts.size(); ++i) {
                long a = resolve(verts[0]), b = resolve(verts[i]), c = resolve(verts[i + 1]);
                if (a < 0 || b < 0 || c < 0 ||
                    a >= static_cast<long>(m.positions.size()) ||
                    b >= static_cast<long>(m.positions.size()) ||
                    c >= static_cast<long>(m.positions.size()))
                    return IoStatus::fail(IoError::Malformed, "face index out of range");
                if (m.triangle_count() >= budget.max_triangles)
                    return IoStatus::fail(IoError::BudgetExceeded, "triangle budget");
                m.indices.push_back(static_cast<std::uint32_t>(a));
                m.indices.push_back(static_cast<std::uint32_t>(b));
                m.indices.push_back(static_cast<std::uint32_t>(c));
            }
        }
    }
    if (!any_colors) m.colors.clear();
    // OBJ vn indices can differ per corner; claycore meshes are
    // vertex-aligned — accept only the aligned case (our own exports)
    if (normals_pool.size() == m.positions.size()) m.normals = std::move(normals_pool);
    if (uv_pool.size() == m.positions.size()) m.uvs = std::move(uv_pool);
    *out = std::move(m);
    return IoStatus::success();
}

IoStatus save_obj_file(const mesh::Mesh& m, const std::string& path, bool with_mtl) {
    std::string mtl_path;
    std::string mtl_name;
    if (with_mtl) {
        mtl_path = path;
        std::size_t dot = mtl_path.find_last_of('.');
        if (dot != std::string::npos) mtl_path.resize(dot);
        mtl_path += ".mtl";
        std::size_t slash = mtl_path.find_last_of("/\\");
        mtl_name = slash == std::string::npos ? mtl_path : mtl_path.substr(slash + 1);
        IoStatus s = detail::write_whole_file(mtl_path, save_mtl());
        if (!s.ok()) return s;
    }
    return detail::write_whole_file(path, save_obj(m, "claycore", mtl_name));
}

IoStatus load_obj_file(const std::string& path, mesh::Mesh* out, const ImportBudget& budget) {
    std::string text;
    IoStatus s = detail::read_whole_file(path, &text, budget.max_file_bytes);
    if (!s.ok()) return s;
    return load_obj(text, out, budget);
}

MeshBufferView buffer_view(const mesh::Mesh& m) {
    MeshBufferView v;
    v.vertex_count = m.positions.size();
    v.positions = v.vertex_count ? &m.positions[0].x : nullptr;
    v.normals = m.normals.size() == v.vertex_count && v.vertex_count ? &m.normals[0].x : nullptr;
    v.colors = m.colors.size() == v.vertex_count && v.vertex_count ? &m.colors[0].x : nullptr;
    v.uvs = m.uvs.size() == v.vertex_count && v.vertex_count ? &m.uvs[0].x : nullptr;
    v.indices = m.indices.empty() ? nullptr : m.indices.data();
    v.index_count = m.indices.size();
    return v;
}

}  // namespace io
}  // namespace clay
