// FBX interchange (file-io spec): import via ufbx (MIT, battle-tested),
// export via a minimal FBX 7.4 binary writer (mesh, ByVertice normals and
// colors, Y-up, meters via UnitScaleFactor=100). Exports are validated by
// round-tripping through ufbx (an independent implementation) in tests and
// through assimp in CI.

#include <ufbx.h>

#include <cstring>
#include <string>

#include "clay/io/mesh_io.h"

namespace clay {
namespace io {

using kernel::cf3;

// ---------------------------------------------------------------------------
// minimal binary writer
// ---------------------------------------------------------------------------

namespace {

struct FbxProp {
    char type = 'I';
    std::int64_t i = 0;
    double d = 0;
    std::string s;
    std::vector<double> darr;
    std::vector<std::int32_t> iarr;

    static FbxProp I32(std::int32_t v) {
        FbxProp p;
        p.type = 'I';
        p.i = v;
        return p;
    }
    static FbxProp I64(std::int64_t v) {
        FbxProp p;
        p.type = 'L';
        p.i = v;
        return p;
    }
    static FbxProp F64(double v) {
        FbxProp p;
        p.type = 'D';
        p.d = v;
        return p;
    }
    static FbxProp Str(std::string v) {
        FbxProp p;
        p.type = 'S';
        p.s = std::move(v);
        return p;
    }
    static FbxProp DArr(std::vector<double> v) {
        FbxProp p;
        p.type = 'd';
        p.darr = std::move(v);
        return p;
    }
    static FbxProp IArr(std::vector<std::int32_t> v) {
        FbxProp p;
        p.type = 'i';
        p.iarr = std::move(v);
        return p;
    }
};

struct FbxNode {
    std::string name;
    std::vector<FbxProp> props;
    std::vector<FbxNode> children;

    FbxNode& child(std::string n) {
        children.push_back(FbxNode{std::move(n), {}, {}});
        return children.back();
    }
};

void put_u8(std::vector<std::uint8_t>& out, std::uint8_t v) { out.push_back(v); }
void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}
void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}
void put_f64(std::vector<std::uint8_t>& out, double v) {
    std::uint64_t bits;
    std::memcpy(&bits, &v, 8);
    put_u64(out, bits);
}

void write_prop(std::vector<std::uint8_t>& out, const FbxProp& p) {
    out.push_back(static_cast<std::uint8_t>(p.type));
    switch (p.type) {
        case 'I': put_u32(out, static_cast<std::uint32_t>(p.i)); break;
        case 'L': put_u64(out, static_cast<std::uint64_t>(p.i)); break;
        case 'D': put_f64(out, p.d); break;
        case 'S':
            put_u32(out, static_cast<std::uint32_t>(p.s.size()));
            out.insert(out.end(), p.s.begin(), p.s.end());
            break;
        case 'd':
            put_u32(out, static_cast<std::uint32_t>(p.darr.size()));
            put_u32(out, 0);  // no compression
            put_u32(out, static_cast<std::uint32_t>(p.darr.size() * 8));
            for (double v : p.darr) put_f64(out, v);
            break;
        case 'i':
            put_u32(out, static_cast<std::uint32_t>(p.iarr.size()));
            put_u32(out, 0);
            put_u32(out, static_cast<std::uint32_t>(p.iarr.size() * 4));
            for (std::int32_t v : p.iarr) put_u32(out, static_cast<std::uint32_t>(v));
            break;
        default: break;
    }
}

void write_node(std::vector<std::uint8_t>& out, const FbxNode& node) {
    std::size_t header_at = out.size();
    put_u32(out, 0);  // EndOffset fixup
    put_u32(out, static_cast<std::uint32_t>(node.props.size()));
    put_u32(out, 0);  // PropertyListLen fixup
    put_u8(out, static_cast<std::uint8_t>(node.name.size()));
    out.insert(out.end(), node.name.begin(), node.name.end());

    std::size_t props_at = out.size();
    for (const FbxProp& p : node.props) write_prop(out, p);
    std::uint32_t prop_len = static_cast<std::uint32_t>(out.size() - props_at);
    std::memcpy(out.data() + header_at + 8, &prop_len, 4);

    if (!node.children.empty()) {
        for (const FbxNode& c : node.children) write_node(out, c);
        out.insert(out.end(), 13, 0);  // null sentinel
    }
    std::uint32_t end_offset = static_cast<std::uint32_t>(out.size());
    std::memcpy(out.data() + header_at, &end_offset, 4);
}

FbxNode p70(const char* name, const char* type, const char* subtype, const char* flags,
            FbxProp value) {
    FbxNode n{"P", {}, {}};
    n.props = {FbxProp::Str(name), FbxProp::Str(type), FbxProp::Str(subtype),
               FbxProp::Str(flags), std::move(value)};
    return n;
}

std::string fbx_name(const std::string& name, const char* clazz) {
    return name + std::string("\x00\x01", 2) + clazz;
}

}  // namespace

std::vector<std::uint8_t> save_fbx(const mesh::Mesh& m, const std::string& name) {
    const std::int64_t geom_id = 1000001;
    const std::int64_t model_id = 2000001;

    FbxNode root;

    FbxNode& header = root.child("FBXHeaderExtension");
    header.child("FBXHeaderVersion").props = {FbxProp::I32(1003)};
    header.child("FBXVersion").props = {FbxProp::I32(7400)};
    header.child("Creator").props = {FbxProp::Str("claycore")};

    FbxNode& gs = root.child("GlobalSettings");
    gs.child("Version").props = {FbxProp::I32(1000)};
    FbxNode& gsp = gs.child("Properties70");
    gsp.children.push_back(p70("UpAxis", "int", "Integer", "", FbxProp::I32(1)));
    gsp.children.push_back(p70("UpAxisSign", "int", "Integer", "", FbxProp::I32(1)));
    gsp.children.push_back(p70("FrontAxis", "int", "Integer", "", FbxProp::I32(2)));
    gsp.children.push_back(p70("FrontAxisSign", "int", "Integer", "", FbxProp::I32(1)));
    gsp.children.push_back(p70("CoordAxis", "int", "Integer", "", FbxProp::I32(0)));
    gsp.children.push_back(p70("CoordAxisSign", "int", "Integer", "", FbxProp::I32(1)));
    // file unit = 100 cm = 1 m: vertex values are authored in meters
    gsp.children.push_back(p70("UnitScaleFactor", "double", "Number", "", FbxProp::F64(100.0)));

    FbxNode& docs = root.child("Documents");
    docs.child("Count").props = {FbxProp::I32(1)};
    FbxNode& docn = docs.child("Document");
    docn.props = {FbxProp::I64(999901), FbxProp::Str("Scene"), FbxProp::Str("Scene")};
    docn.child("RootNode").props = {FbxProp::I64(0)};

    FbxNode& defs = root.child("Definitions");
    defs.child("Version").props = {FbxProp::I32(100)};
    defs.child("Count").props = {FbxProp::I32(2)};
    defs.child("ObjectType").props = {FbxProp::Str("Model")};
    defs.children.back().child("Count").props = {FbxProp::I32(1)};
    defs.child("ObjectType").props = {FbxProp::Str("Geometry")};
    defs.children.back().child("Count").props = {FbxProp::I32(1)};

    FbxNode& objects = root.child("Objects");
    FbxNode& geom = objects.child("Geometry");
    geom.props = {FbxProp::I64(geom_id), FbxProp::Str(fbx_name(name, "Geometry")),
                  FbxProp::Str("Mesh")};
    {
        std::vector<double> verts;
        verts.reserve(m.positions.size() * 3);
        for (const kernel::cfloat3& p : m.positions) {
            verts.push_back(p.x);
            verts.push_back(p.y);
            verts.push_back(p.z);
        }
        geom.child("Vertices").props = {FbxProp::DArr(std::move(verts))};
        std::vector<std::int32_t> poly;
        poly.reserve(m.indices.size());
        for (std::size_t t = 0; t < m.triangle_count(); ++t) {
            poly.push_back(static_cast<std::int32_t>(m.indices[t * 3]));
            poly.push_back(static_cast<std::int32_t>(m.indices[t * 3 + 1]));
            poly.push_back(~static_cast<std::int32_t>(m.indices[t * 3 + 2]));  // end marker
        }
        geom.child("PolygonVertexIndex").props = {FbxProp::IArr(std::move(poly))};
        geom.child("GeometryVersion").props = {FbxProp::I32(124)};

        if (m.normals.size() == m.positions.size()) {
            FbxNode& ln = geom.child("LayerElementNormal");
            ln.props = {FbxProp::I32(0)};
            ln.child("Version").props = {FbxProp::I32(101)};
            ln.child("Name").props = {FbxProp::Str("")};
            ln.child("MappingInformationType").props = {FbxProp::Str("ByVertice")};
            ln.child("ReferenceInformationType").props = {FbxProp::Str("Direct")};
            std::vector<double> normals;
            normals.reserve(m.normals.size() * 3);
            for (const kernel::cfloat3& n : m.normals) {
                normals.push_back(n.x);
                normals.push_back(n.y);
                normals.push_back(n.z);
            }
            ln.child("Normals").props = {FbxProp::DArr(std::move(normals))};
        }
        if (m.colors.size() == m.positions.size()) {
            FbxNode& lc = geom.child("LayerElementColor");
            lc.props = {FbxProp::I32(0)};
            lc.child("Version").props = {FbxProp::I32(101)};
            lc.child("Name").props = {FbxProp::Str("clay_colors")};
            lc.child("MappingInformationType").props = {FbxProp::Str("ByVertice")};
            lc.child("ReferenceInformationType").props = {FbxProp::Str("Direct")};
            std::vector<double> colors;
            colors.reserve(m.colors.size() * 4);
            for (const kernel::cfloat3& c : m.colors) {
                colors.push_back(c.x);
                colors.push_back(c.y);
                colors.push_back(c.z);
                colors.push_back(1.0);
            }
            lc.child("Colors").props = {FbxProp::DArr(std::move(colors))};
        }
        FbxNode& layer = geom.child("Layer");
        layer.props = {FbxProp::I32(0)};
        layer.child("Version").props = {FbxProp::I32(100)};
        if (m.normals.size() == m.positions.size()) {
            FbxNode& le = layer.child("LayerElement");
            le.child("Type").props = {FbxProp::Str("LayerElementNormal")};
            le.child("TypedIndex").props = {FbxProp::I32(0)};
        }
        if (m.colors.size() == m.positions.size()) {
            FbxNode& le = layer.child("LayerElement");
            le.child("Type").props = {FbxProp::Str("LayerElementColor")};
            le.child("TypedIndex").props = {FbxProp::I32(0)};
        }
    }

    FbxNode& model = objects.child("Model");
    model.props = {FbxProp::I64(model_id), FbxProp::Str(fbx_name(name, "Model")),
                   FbxProp::Str("Mesh")};
    model.child("Version").props = {FbxProp::I32(232)};
    model.child("Properties70");

    FbxNode& conns = root.child("Connections");
    {
        FbxNode& c1 = conns.child("C");
        c1.props = {FbxProp::Str("OO"), FbxProp::I64(model_id), FbxProp::I64(0)};
        FbxNode& c2 = conns.child("C");
        c2.props = {FbxProp::Str("OO"), FbxProp::I64(geom_id), FbxProp::I64(model_id)};
    }

    // serialize
    std::vector<std::uint8_t> out;
    const char magic[] = "Kaydara FBX Binary  ";
    out.insert(out.end(), magic, magic + 20);
    out.push_back(0x00);
    out.push_back(0x1A);
    out.push_back(0x00);
    put_u32(out, 7400);
    for (const FbxNode& n : root.children) write_node(out, n);
    out.insert(out.end(), 13, 0);  // top-level null sentinel

    // footer: id bytes, alignment padding, version, reserved, magic
    const std::uint8_t footid[16] = {0xfa, 0xbc, 0xab, 0x09, 0xd0, 0xc8, 0xd4, 0x66,
                                     0xb1, 0x76, 0xfb, 0x83, 0x1c, 0xf7, 0x26, 0x7e};
    out.insert(out.end(), footid, footid + 16);
    out.insert(out.end(), 4, 0);
    while (out.size() % 16 != 0) out.push_back(0);
    put_u32(out, 7400);
    out.insert(out.end(), 120, 0);
    const std::uint8_t footmagic[16] = {0xf8, 0x5a, 0x8c, 0x6a, 0xde, 0xf5, 0xd9, 0x7e,
                                        0xec, 0xe9, 0x0c, 0xe3, 0x75, 0x8f, 0x29, 0x0b};
    out.insert(out.end(), footmagic, footmagic + 16);
    return out;
}

// ---------------------------------------------------------------------------
// import via ufbx
// ---------------------------------------------------------------------------

IoStatus load_fbx(const std::uint8_t* data, std::size_t size, mesh::Mesh* out,
                  const ImportBudget& budget) {
    ufbx_load_opts opts = {};
    opts.target_unit_meters = 1.0f;
    opts.target_axes = ufbx_axes_right_handed_y_up;
    ufbx_error error;
    ufbx_scene* scene = ufbx_load_memory(data, size, &opts, &error);
    if (!scene) return IoStatus::fail(IoError::Malformed, error.description.data);

    mesh::Mesh m;
    bool any_colors = false;
    IoStatus status = IoStatus::success();
    std::vector<std::uint32_t> tri_indices;

    for (std::size_t ni = 0; ni < scene->nodes.count && status.ok(); ++ni) {
        ufbx_node* node = scene->nodes.data[ni];
        ufbx_mesh* fm = node->mesh;
        if (!fm) continue;
        tri_indices.resize(fm->max_face_triangles * 3);
        for (std::size_t fi = 0; fi < fm->faces.count && status.ok(); ++fi) {
            ufbx_face face = fm->faces.data[fi];
            std::size_t tris =
                ufbx_triangulate_face(tri_indices.data(), tri_indices.size(), fm, face);
            for (std::size_t t = 0; t < tris * 3; ++t) {
                std::uint32_t index = tri_indices[t];
                if (m.positions.size() >= budget.max_vertices) {
                    status = IoStatus::fail(IoError::BudgetExceeded, "vertex budget");
                    break;
                }
                ufbx_vec3 vp = ufbx_get_vertex_vec3(&fm->vertex_position, index);
                ufbx_vec3 wp = ufbx_transform_position(&node->geometry_to_world, vp);
                m.indices.push_back(static_cast<std::uint32_t>(m.positions.size()));
                m.positions.push_back(cf3(static_cast<float>(wp.x), static_cast<float>(wp.y),
                                          static_cast<float>(wp.z)));
                if (fm->vertex_normal.exists) {
                    ufbx_vec3 vn = ufbx_get_vertex_vec3(&fm->vertex_normal, index);
                    m.normals.push_back(cf3(static_cast<float>(vn.x), static_cast<float>(vn.y),
                                            static_cast<float>(vn.z)));
                }
                if (fm->vertex_color.exists) {
                    ufbx_vec4 vc = ufbx_get_vertex_vec4(&fm->vertex_color, index);
                    m.colors.push_back(cf3(static_cast<float>(vc.x), static_cast<float>(vc.y),
                                           static_cast<float>(vc.z)));
                    any_colors = true;
                }
            }
        }
    }
    ufbx_free_scene(scene);
    if (!status.ok()) return status;
    if (!any_colors) m.colors.clear();
    if (m.normals.size() != m.positions.size()) m.normals.clear();
    if (m.triangle_count() > budget.max_triangles)
        return IoStatus::fail(IoError::BudgetExceeded, "triangle budget");
    *out = std::move(m);
    return IoStatus::success();
}

// ---------------------------------------------------------------------------
// files
// ---------------------------------------------------------------------------

namespace {

IoStatus write_bytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return IoStatus::fail(IoError::WriteFailed, path);
    std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return written == bytes.size() ? IoStatus::success()
                                   : IoStatus::fail(IoError::WriteFailed, path);
}

}  // namespace

IoStatus save_fbx_file(const mesh::Mesh& m, const std::string& path) {
    return write_bytes(path, save_fbx(m));
}

IoStatus load_fbx_file(const std::string& path, mesh::Mesh* out, const ImportBudget& budget) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return IoStatus::fail(IoError::FileNotFound, path);
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size > 0 ? size : 0));
    std::size_t read = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (read != bytes.size()) return IoStatus::fail(IoError::ReadFailed, path);
    return load_fbx(bytes.data(), bytes.size(), out, budget);
}

}  // namespace io
}  // namespace clay
