#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

#include "clay/io/mesh_io.h"

#include "file_bytes.h"

namespace clay {
namespace io {

using kernel::cf3;

std::vector<std::uint8_t> save_ply(const mesh::Mesh& m, bool binary) {
    const bool colors = m.colors.size() == m.positions.size();
    const bool normals = m.normals.size() == m.positions.size();
    std::string header = "ply\nformat " +
                         std::string(binary ? "binary_little_endian" : "ascii") +
                         " 1.0\ncomment claycore export\n";
    header += "element vertex " + std::to_string(m.positions.size()) + "\n";
    header += "property float x\nproperty float y\nproperty float z\n";
    if (normals) header += "property float nx\nproperty float ny\nproperty float nz\n";
    if (colors)
        header += "property uchar red\nproperty uchar green\nproperty uchar blue\n";
    header += "element face " + std::to_string(m.triangle_count()) + "\n";
    header += "property list uchar int vertex_indices\nend_header\n";

    std::vector<std::uint8_t> out(header.begin(), header.end());
    auto putf = [&](float f) {
        std::uint8_t b[4];
        std::memcpy(b, &f, 4);
        out.insert(out.end(), b, b + 4);
    };
    auto puti = [&](std::int32_t v) {
        std::uint8_t b[4];
        std::memcpy(b, &v, 4);
        out.insert(out.end(), b, b + 4);
    };
    auto to_u8 = [](float c) {
        return static_cast<std::uint8_t>(kernel::cclamp(c, 0.0f, 1.0f) * 255.0f + 0.5f);
    };

    if (binary) {
        for (std::size_t i = 0; i < m.positions.size(); ++i) {
            putf(m.positions[i].x);
            putf(m.positions[i].y);
            putf(m.positions[i].z);
            if (normals) {
                putf(m.normals[i].x);
                putf(m.normals[i].y);
                putf(m.normals[i].z);
            }
            if (colors) {
                out.push_back(to_u8(m.colors[i].x));
                out.push_back(to_u8(m.colors[i].y));
                out.push_back(to_u8(m.colors[i].z));
            }
        }
        for (std::size_t t = 0; t < m.triangle_count(); ++t) {
            out.push_back(3);
            puti(static_cast<std::int32_t>(m.indices[t * 3]));
            puti(static_cast<std::int32_t>(m.indices[t * 3 + 1]));
            puti(static_cast<std::int32_t>(m.indices[t * 3 + 2]));
        }
    } else {
        char line[256];
        std::string body;
        for (std::size_t i = 0; i < m.positions.size(); ++i) {
            std::snprintf(line, sizeof line, "%.9g %.9g %.9g", m.positions[i].x,
                          m.positions[i].y, m.positions[i].z);
            body += line;
            if (normals) {
                std::snprintf(line, sizeof line, " %.9g %.9g %.9g", m.normals[i].x,
                              m.normals[i].y, m.normals[i].z);
                body += line;
            }
            if (colors) {
                std::snprintf(line, sizeof line, " %d %d %d", to_u8(m.colors[i].x),
                              to_u8(m.colors[i].y), to_u8(m.colors[i].z));
                body += line;
            }
            body += "\n";
        }
        for (std::size_t t = 0; t < m.triangle_count(); ++t) {
            std::snprintf(line, sizeof line, "3 %u %u %u\n", m.indices[t * 3],
                          m.indices[t * 3 + 1], m.indices[t * 3 + 2]);
            body += line;
        }
        out.insert(out.end(), body.begin(), body.end());
    }
    return out;
}

namespace {

struct PlyProperty {
    std::string name;
    std::string type;
    bool is_list = false;
    std::string list_count_type;
};

std::size_t scalar_size(const std::string& type) {
    if (type == "char" || type == "uchar" || type == "int8" || type == "uint8") return 1;
    if (type == "short" || type == "ushort" || type == "int16" || type == "uint16") return 2;
    if (type == "int" || type == "uint" || type == "int32" || type == "uint32" ||
        type == "float" || type == "float32")
        return 4;
    if (type == "double" || type == "float64") return 8;
    return 0;
}

double read_scalar(const std::uint8_t* p, const std::string& type) {
    if (type == "float" || type == "float32") {
        float f;
        std::memcpy(&f, p, 4);
        return f;
    }
    if (type == "double" || type == "float64") {
        double d;
        std::memcpy(&d, p, 8);
        return d;
    }
    if (type == "uchar" || type == "uint8") return *p;
    if (type == "char" || type == "int8") return static_cast<std::int8_t>(*p);
    if (type == "ushort" || type == "uint16") {
        std::uint16_t v;
        std::memcpy(&v, p, 2);
        return v;
    }
    if (type == "short" || type == "int16") {
        std::int16_t v;
        std::memcpy(&v, p, 2);
        return v;
    }
    if (type == "uint" || type == "uint32") {
        std::uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }
    std::int32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

}  // namespace

IoStatus load_ply(const std::uint8_t* data, std::size_t size, mesh::Mesh* out,
                  const ImportBudget& budget) {
    // parse header text
    const char* text = reinterpret_cast<const char*>(data);
    std::size_t header_end = 0;
    for (std::size_t i = 0; i + 10 < size; ++i)
        if (std::memcmp(text + i, "end_header", 10) == 0) {
            header_end = i + 10;
            while (header_end < size && text[header_end] != '\n') ++header_end;
            // Only step over the newline if there IS one. A file whose header
            // is not newline-terminated used to leave header_end at size + 1,
            // which over-read the buffer building the header string below and
            // then wrapped `size - header_end` to SIZE_MAX in the ascii body.
            if (header_end < size) ++header_end;
            break;
        }
    if (header_end == 0) return IoStatus::fail(IoError::Malformed, "no end_header");

    std::istringstream header(std::string(text, header_end));
    std::string line;
    bool binary = false;
    std::size_t vertex_count = 0, face_count = 0;
    std::vector<PlyProperty> vprops;
    std::string current;
    std::string unsupported_element;
    while (std::getline(header, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ls(line);
        std::string tok;
        ls >> tok;
        if (tok == "format") {
            std::string fmt;
            ls >> fmt;
            if (fmt == "binary_little_endian")
                binary = true;
            else if (fmt != "ascii")
                return IoStatus::fail(IoError::Unsupported, "big-endian ply");
        } else if (tok == "element") {
            std::string name;
            std::size_t count;
            ls >> name >> count;
            current = name;
            if (name == "vertex") vertex_count = count;
            else if (name == "face") face_count = count;
            // Only vertex and face are read, but EVERY declared element has a
            // payload in the stream. One this reader skips is not skipped in
            // the bytes: the vertex data would be read from the wrong offset
            // and the mesh would come out silently wrong. Refuse instead.
            else if (count > 0) unsupported_element = name;
        } else if (tok == "property" && current == "vertex") {
            PlyProperty p;
            std::string t;
            ls >> t;
            if (t == "list") {
                ls >> p.list_count_type >> p.type >> p.name;
                p.is_list = true;
            } else {
                p.type = t;
                ls >> p.name;
            }
            vprops.push_back(p);
        }
    }
    if (!unsupported_element.empty())
        return IoStatus::fail(IoError::Unsupported,
                              "ply element '" + unsupported_element +
                                  "' is not read, and its payload would displace the vertices");
    if (vertex_count > budget.max_vertices)
        return IoStatus::fail(IoError::BudgetExceeded, "vertex budget");
    if (face_count > budget.max_triangles)
        return IoStatus::fail(IoError::BudgetExceeded, "face budget");

    // allocation-bomb guardrail: declared counts must fit the payload
    std::size_t vstride = 0;
    for (const PlyProperty& p : vprops) {
        if (p.is_list) return IoStatus::fail(IoError::Unsupported, "list vertex property");
        std::size_t s = scalar_size(p.type);
        if (s == 0) return IoStatus::fail(IoError::Malformed, "unknown property type");
        vstride += s;
    }
    // A vertex element with no properties has a stride of zero, which made the
    // payload check below vacuous: any vertex_count "fit" in any file.
    if (vertex_count > 0 && vstride == 0)
        return IoStatus::fail(IoError::Malformed, "vertex element declares no properties");
    // Each vertex costs vstride bytes when binary; in ascii it still costs at
    // least one byte of digit plus one separator per property, so both forms
    // have a floor the declared count has to fit under.
    //
    // The ascii floor is exactly tight (n digits + n-1 spaces + 1 newline), so
    // it is relaxed by one byte: the PLY spec does not require the final line
    // to be newline-terminated, and an otherwise well-formed file that ends
    // without one would be refused by an exact comparison.
    const std::size_t per_vertex = binary ? vstride : 2 * vprops.size();
    const std::size_t available = size - header_end + (binary ? 0 : 1);
    if (per_vertex > 0 && vertex_count > available / per_vertex)
        return IoStatus::fail(IoError::Malformed, "declared counts exceed payload");
    if (binary && header_end + vertex_count * vstride + face_count > size)
        return IoStatus::fail(IoError::Malformed, "declared counts exceed payload");

    mesh::Mesh m;
    m.positions.reserve(vertex_count);
    bool has_colors = false, has_normals = false;
    for (const PlyProperty& p : vprops) {
        if (p.name == "red") has_colors = true;
        if (p.name == "nx") has_normals = true;
    }

    if (binary) {
        const std::uint8_t* p = data + header_end;
        const std::uint8_t* end = data + size;
        for (std::size_t v = 0; v < vertex_count; ++v) {
            if (p + vstride > end) return IoStatus::fail(IoError::Malformed, "truncated verts");
            float x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0, r = 1, g = 1, b = 1;
            std::size_t off = 0;
            for (const PlyProperty& prop : vprops) {
                double val = read_scalar(p + off, prop.type);
                off += scalar_size(prop.type);
                float fval = static_cast<float>(val);
                if (prop.name == "x") x = fval;
                else if (prop.name == "y") y = fval;
                else if (prop.name == "z") z = fval;
                else if (prop.name == "nx") nx = fval;
                else if (prop.name == "ny") ny = fval;
                else if (prop.name == "nz") nz = fval;
                else if (prop.name == "red") r = fval / 255.0f;
                else if (prop.name == "green") g = fval / 255.0f;
                else if (prop.name == "blue") b = fval / 255.0f;
            }
            p += vstride;
            m.positions.push_back(cf3(x, y, z));
            if (has_normals) m.normals.push_back(cf3(nx, ny, nz));
            if (has_colors) m.colors.push_back(cf3(r, g, b));
        }
        for (std::size_t f = 0; f < face_count; ++f) {
            if (p + 1 > end) return IoStatus::fail(IoError::Malformed, "truncated faces");
            std::uint8_t n = *p++;
            if (p + static_cast<std::size_t>(n) * 4 > end)
                return IoStatus::fail(IoError::Malformed, "truncated face indices");
            std::vector<std::uint32_t> face(n);
            for (std::uint8_t i = 0; i < n; ++i) {
                std::int32_t idx;
                std::memcpy(&idx, p, 4);
                p += 4;
                if (idx < 0 || static_cast<std::size_t>(idx) >= vertex_count)
                    return IoStatus::fail(IoError::Malformed, "face index out of range");
                face[i] = static_cast<std::uint32_t>(idx);
            }
            for (std::uint8_t i = 1; i + 1 < n; ++i) {
                m.indices.push_back(face[0]);
                m.indices.push_back(face[i]);
                m.indices.push_back(face[i + 1]);
            }
        }
    } else {
        std::istringstream body(std::string(text + header_end, size - header_end));
        for (std::size_t v = 0; v < vertex_count; ++v) {
            float x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0, r = 1, g = 1, b = 1;
            for (const PlyProperty& prop : vprops) {
                double val;
                if (!(body >> val)) return IoStatus::fail(IoError::Malformed, "truncated verts");
                float fval = static_cast<float>(val);
                if (prop.name == "x") x = fval;
                else if (prop.name == "y") y = fval;
                else if (prop.name == "z") z = fval;
                else if (prop.name == "nx") nx = fval;
                else if (prop.name == "ny") ny = fval;
                else if (prop.name == "nz") nz = fval;
                else if (prop.name == "red") r = fval / 255.0f;
                else if (prop.name == "green") g = fval / 255.0f;
                else if (prop.name == "blue") b = fval / 255.0f;
            }
            m.positions.push_back(cf3(x, y, z));
            if (has_normals) m.normals.push_back(cf3(nx, ny, nz));
            if (has_colors) m.colors.push_back(cf3(r, g, b));
        }
        for (std::size_t f = 0; f < face_count; ++f) {
            std::size_t n;
            if (!(body >> n) || n > 255)
                return IoStatus::fail(IoError::Malformed, "bad face count");
            std::vector<std::uint32_t> face(n);
            for (std::size_t i = 0; i < n; ++i) {
                long idx;
                if (!(body >> idx) || idx < 0 ||
                    static_cast<std::size_t>(idx) >= vertex_count)
                    return IoStatus::fail(IoError::Malformed, "face index out of range");
                face[i] = static_cast<std::uint32_t>(idx);
            }
            for (std::size_t i = 1; i + 1 < n; ++i) {
                m.indices.push_back(face[0]);
                m.indices.push_back(face[i]);
                m.indices.push_back(face[i + 1]);
            }
        }
    }
    *out = std::move(m);
    return IoStatus::success();
}

IoStatus save_ply_file(const mesh::Mesh& m, const std::string& path, bool binary) {
    return detail::write_whole_file(path, save_ply(m, binary));
}

IoStatus load_ply_file(const std::string& path, mesh::Mesh* out, const ImportBudget& budget) {
    std::vector<std::uint8_t> bytes;
    IoStatus s = detail::read_whole_file(path, &bytes, budget.max_file_bytes);
    if (!s.ok()) return s;
    return load_ply(bytes.data(), bytes.size(), out, budget);
}

}  // namespace io
}  // namespace clay
