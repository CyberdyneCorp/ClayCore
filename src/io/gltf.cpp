// GLB (glTF 2.0 binary) writer, dependency-free by design: the JSON document
// is assembled with a local string builder and the container is three raw
// little-endian chunks. Full glTF-validator runs happen in CI at integration
// time; the unit tests parse the container back structurally and semantically.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "clay/io/mesh_io.h"

#include "file_bytes.h"
#include "json.h"

namespace clay {
namespace io {

namespace {

constexpr std::uint32_t kGlbMagic = 0x46546C67u;   // 'glTF'
constexpr std::uint32_t kChunkJson = 0x4E4F534Au;  // 'JSON'
constexpr std::uint32_t kChunkBin = 0x004E4942u;   // 'BIN\0'
constexpr int kTargetArrayBuffer = 34962;
constexpr int kTargetElementArrayBuffer = 34963;
constexpr int kComponentFloat = 5126;
constexpr int kComponentUint32 = 5125;

// %.9g keeps float32 round-trip exact and the output byte-deterministic.
void append_float(std::string* s, float f) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(f));
    *s += buf;
}

void append_u32le(std::vector<std::uint8_t>* out, std::uint32_t v) {
    std::uint8_t b[4];
    std::memcpy(b, &v, 4);
    out->insert(out->end(), b, b + 4);
}

void append_f32le(std::vector<std::uint8_t>* out, float f) {
    std::uint8_t b[4];
    std::memcpy(b, &f, 4);
    out->insert(out->end(), b, b + 4);
}

struct BinBlock {
    std::size_t offset = 0;
    std::size_t length = 0;
};

}  // namespace

std::vector<std::uint8_t> save_glb(const mesh::Mesh& m) {
    const std::size_t vcount = m.positions.size();
    const bool has_normals = vcount > 0 && m.normals.size() == vcount;
    const bool has_colors = vcount > 0 && m.colors.size() == vcount;
    const bool has_uvs = vcount > 0 && m.uvs.size() == vcount;

    // BIN payload: one tightly packed block per attribute, indices last.
    // Every component is 4 bytes so all block offsets stay 4-byte aligned.
    std::vector<std::uint8_t> bin;
    BinBlock bpos, bnrm, bcol, buv, bidx;
    auto block = [&bin](BinBlock* b, auto&& fill) {
        b->offset = bin.size();
        fill();
        b->length = bin.size() - b->offset;
    };
    block(&bpos, [&] {
        for (const kernel::cfloat3& p : m.positions) {
            append_f32le(&bin, p.x);
            append_f32le(&bin, p.y);
            append_f32le(&bin, p.z);
        }
    });
    if (has_normals)
        block(&bnrm, [&] {
            for (const kernel::cfloat3& n : m.normals) {
                append_f32le(&bin, n.x);
                append_f32le(&bin, n.y);
                append_f32le(&bin, n.z);
            }
        });
    if (has_colors)
        block(&bcol, [&] {
            for (const kernel::cfloat3& c : m.colors) {
                append_f32le(&bin, c.x);
                append_f32le(&bin, c.y);
                append_f32le(&bin, c.z);
            }
        });
    if (has_uvs)
        block(&buv, [&] {
            for (const kernel::cfloat2& t : m.uvs) {
                append_f32le(&bin, t.x);
                append_f32le(&bin, t.y);
            }
        });
    block(&bidx, [&] {
        for (std::uint32_t i : m.indices) append_u32le(&bin, i);
    });
    const std::size_t buffer_length = bin.size();  // before chunk padding

    // bufferView i and accessor i are created pairwise, so indices coincide.
    std::string views, accessors;
    int next_index = 0;
    auto add_accessor = [&](const BinBlock& b, int target, int component_type, std::size_t count,
                            const char* type, const std::string& extra) {
        if (!views.empty()) views += ',';
        views += "{\"buffer\":0,\"byteOffset\":" + std::to_string(b.offset) +
                 ",\"byteLength\":" + std::to_string(b.length) +
                 ",\"target\":" + std::to_string(target) + "}";
        if (!accessors.empty()) accessors += ',';
        accessors += "{\"bufferView\":" + std::to_string(next_index) +
                     ",\"componentType\":" + std::to_string(component_type) +
                     ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"" +
                     extra + "}";
        return next_index++;
    };

    // min/max are REQUIRED on the POSITION accessor by the glTF 2.0 spec.
    kernel::cfloat3 mn = kernel::cf3(0, 0, 0);
    kernel::cfloat3 mx = mn;
    if (vcount > 0) {
        mn = mx = m.positions[0];
        for (const kernel::cfloat3& p : m.positions) {
            mn = kernel::cmin(mn, p);
            mx = kernel::cmax(mx, p);
        }
    }
    std::string minmax = ",\"min\":[";
    append_float(&minmax, mn.x);
    minmax += ',';
    append_float(&minmax, mn.y);
    minmax += ',';
    append_float(&minmax, mn.z);
    minmax += "],\"max\":[";
    append_float(&minmax, mx.x);
    minmax += ',';
    append_float(&minmax, mx.y);
    minmax += ',';
    append_float(&minmax, mx.z);
    minmax += ']';

    const int acc_pos =
        add_accessor(bpos, kTargetArrayBuffer, kComponentFloat, vcount, "VEC3", minmax);
    std::string attributes = "\"POSITION\":" + std::to_string(acc_pos);
    if (has_normals)
        attributes += ",\"NORMAL\":" + std::to_string(add_accessor(
                          bnrm, kTargetArrayBuffer, kComponentFloat, vcount, "VEC3", {}));
    if (has_colors)
        attributes += ",\"COLOR_0\":" + std::to_string(add_accessor(
                          bcol, kTargetArrayBuffer, kComponentFloat, vcount, "VEC3", {}));
    if (has_uvs)
        attributes += ",\"TEXCOORD_0\":" + std::to_string(add_accessor(
                          buv, kTargetArrayBuffer, kComponentFloat, vcount, "VEC2", {}));
    const int acc_idx = add_accessor(bidx, kTargetElementArrayBuffer, kComponentUint32,
                                     m.indices.size(), "SCALAR", {});

    std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"claycore\"},"
                       "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
                       "\"meshes\":[{\"primitives\":[{\"attributes\":{" +
                       attributes + "},\"indices\":" + std::to_string(acc_idx) +
                       ",\"mode\":4}]}],\"accessors\":[" + accessors + "],\"bufferViews\":[" +
                       views + "],\"buffers\":[{\"byteLength\":" +
                       std::to_string(buffer_length) + "}]}";

    // GLB container: JSON chunk space-padded, BIN chunk zero-padded, both to
    // 4-byte boundaries as the container spec requires.
    while (json.size() % 4 != 0) json += ' ';
    while (bin.size() % 4 != 0) bin.push_back(0);

    std::vector<std::uint8_t> out;
    out.reserve(12 + 8 + json.size() + 8 + bin.size());
    append_u32le(&out, kGlbMagic);
    append_u32le(&out, 2);
    append_u32le(&out, static_cast<std::uint32_t>(12 + 8 + json.size() + 8 + bin.size()));
    append_u32le(&out, static_cast<std::uint32_t>(json.size()));
    append_u32le(&out, kChunkJson);
    out.insert(out.end(), json.begin(), json.end());
    append_u32le(&out, static_cast<std::uint32_t>(bin.size()));
    append_u32le(&out, kChunkBin);
    out.insert(out.end(), bin.begin(), bin.end());
    return out;
}

IoStatus save_glb_file(const mesh::Mesh& m, const std::string& path) {
    return detail::write_whole_file(path, save_glb(m));
}


// -- the reader ---------------------------------------------------------------
//
// GLB is three little-endian chunks: a 12-byte header, a JSON chunk describing
// the scene, and a BIN chunk holding the vertex data the JSON's accessors point
// into. Everything below is bounds-checked against BOTH the declared chunk
// length and the real file length, because an asset is untrusted input and a
// declared length is a claim rather than a fact.

namespace {

struct Accessor {
    std::size_t count = 0;
    int component = 0;
    int components_per_element = 0;  // 1 for SCALAR, 2 VEC2, 3 VEC3, 4 VEC4
    bool normalized = false;
    std::size_t offset = 0;  // absolute byte offset into the BIN chunk
    std::size_t stride = 0;  // bytes between elements
    bool valid = false;
};

int components_of(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    return 0;
}

int component_size(int component_type) {
    switch (component_type) {
        case 5120: case 5121: return 1;  // byte, unsigned byte
        case 5122: case 5123: return 2;  // short, unsigned short
        case 5125: case 5126: return 4;  // unsigned int, float
        default: return 0;
    }
}

std::uint32_t read_u32le(const std::uint8_t* p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

float read_f32le(const std::uint8_t* p) {
    float v = 0.0f;
    std::memcpy(&v, p, 4);
    return v;
}

// One component, as a float, normalized when the accessor says so. The
// normalization rules are the spec's: unsigned types map to [0,1] and signed
// to [-1,1] with the most negative value clamped.
float component_as_float(const std::uint8_t* p, int component_type, bool normalized) {
    switch (component_type) {
        case 5126: return read_f32le(p);
        case 5125: {
            std::uint32_t v = read_u32le(p);
            return static_cast<float>(v);
        }
        case 5123: {
            std::uint16_t v = 0;
            std::memcpy(&v, p, 2);
            return normalized ? static_cast<float>(v) / 65535.0f : static_cast<float>(v);
        }
        case 5122: {
            std::int16_t v = 0;
            std::memcpy(&v, p, 2);
            return normalized ? std::max(static_cast<float>(v) / 32767.0f, -1.0f)
                              : static_cast<float>(v);
        }
        case 5121: {
            std::uint8_t v = *p;
            return normalized ? static_cast<float>(v) / 255.0f : static_cast<float>(v);
        }
        case 5120: {
            std::int8_t v = static_cast<std::int8_t>(*p);
            return normalized ? std::max(static_cast<float>(v) / 127.0f, -1.0f)
                              : static_cast<float>(v);
        }
        default: return 0.0f;
    }
}

}  // namespace

namespace {

// A plain 4x4, column-major as glTF stores it. `math::Transform` cannot stand
// in: a glTF node matrix may carry shear, which a rotation-scale-translation
// type has nowhere to put, and silently dropping it would move geometry.
struct Mat4 {
    float m[16];  // m[col * 4 + row]

    static Mat4 identity() {
        Mat4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }

    Mat4 operator*(const Mat4& b) const {
        Mat4 r{};
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) sum += m[k * 4 + row] * b.m[c * 4 + k];
                r.m[c * 4 + row] = sum;
            }
        return r;
    }

    kernel::cfloat3 point(kernel::cfloat3 p) const {
        return kernel::cf3(m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                           m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                           m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]);
    }

    // Normals go through the inverse transpose of the upper 3x3, or a
    // non-uniform scale tilts them off the surface.
    kernel::cfloat3 normal(kernel::cfloat3 n) const {
        const float a = m[0], b = m[4], c = m[8];
        const float d = m[1], e = m[5], f = m[9];
        const float g = m[2], h = m[6], i = m[10];
        const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
        if (std::abs(det) < 1e-20f) return n;  // degenerate: leave it alone
        const float inv = 1.0f / det;
        // (M^-1)^T, written out: row r of the result is column r of M^-1.
        const float t[9] = {(e * i - f * h) * inv, (c * h - b * i) * inv, (b * f - c * e) * inv,
                            (f * g - d * i) * inv, (a * i - c * g) * inv, (c * d - a * f) * inv,
                            (d * h - e * g) * inv, (b * g - a * h) * inv, (a * e - b * d) * inv};
        return kernel::cf3(t[0] * n.x + t[3] * n.y + t[6] * n.z,
                           t[1] * n.x + t[4] * n.y + t[7] * n.z,
                           t[2] * n.x + t[5] * n.y + t[8] * n.z);
    }
};

Mat4 trs_of(const JsonDoc& doc, const JsonNode& node) {
    const JsonNode& matrix = doc.member(node, "matrix");
    if (matrix.type == JsonType::Array && matrix.children.size() == 16) {
        Mat4 r{};
        for (std::size_t k = 0; k < 16; ++k)
            r.m[k] = static_cast<float>(doc.element(matrix, k).number);
        return r;
    }
    float tx = 0, ty = 0, tz = 0, qx = 0, qy = 0, qz = 0, qw = 1, sx = 1, sy = 1, sz = 1;
    const JsonNode& t = doc.member(node, "translation");
    if (t.children.size() == 3) {
        tx = static_cast<float>(doc.element(t, 0).number);
        ty = static_cast<float>(doc.element(t, 1).number);
        tz = static_cast<float>(doc.element(t, 2).number);
    }
    const JsonNode& q = doc.member(node, "rotation");
    if (q.children.size() == 4) {
        qx = static_cast<float>(doc.element(q, 0).number);
        qy = static_cast<float>(doc.element(q, 1).number);
        qz = static_cast<float>(doc.element(q, 2).number);
        qw = static_cast<float>(doc.element(q, 3).number);
    }
    const JsonNode& s = doc.member(node, "scale");
    if (s.children.size() == 3) {
        sx = static_cast<float>(doc.element(s, 0).number);
        sy = static_cast<float>(doc.element(s, 1).number);
        sz = static_cast<float>(doc.element(s, 2).number);
    }
    Mat4 r = Mat4::identity();
    const float xx = qx * qx, yy = qy * qy, zz = qz * qz;
    const float xy = qx * qy, xz = qx * qz, yz = qy * qz;
    const float wx = qw * qx, wy = qw * qy, wz = qw * qz;
    r.m[0] = (1 - 2 * (yy + zz)) * sx;
    r.m[1] = (2 * (xy + wz)) * sx;
    r.m[2] = (2 * (xz - wy)) * sx;
    r.m[4] = (2 * (xy - wz)) * sy;
    r.m[5] = (1 - 2 * (xx + zz)) * sy;
    r.m[6] = (2 * (yz + wx)) * sy;
    r.m[8] = (2 * (xz + wy)) * sz;
    r.m[9] = (2 * (yz - wx)) * sz;
    r.m[10] = (1 - 2 * (xx + yy)) * sz;
    r.m[12] = tx;
    r.m[13] = ty;
    r.m[14] = tz;
    return r;
}

}  // namespace

namespace {

// Resolves accessor `index` against the bufferViews, bounds-checked against the
// BIN chunk. An accessor is a claim about where data lives; this is where the
// claim is checked, once, so the readers below can index without re-checking.
bool resolve_accessor(const JsonDoc& doc, const JsonNode& root, std::size_t index,
                      std::size_t bin_size, Accessor* out, std::string* why) {
    const JsonNode& accessors = doc.member(root, "accessors");
    const JsonNode& a = doc.element(accessors, index);
    if (a.type != JsonType::Object) {
        *why = "accessor " + std::to_string(index) + " does not exist";
        return false;
    }
    if (doc.has(a, "sparse")) {
        *why = "sparse accessors are not supported";
        return false;
    }
    Accessor r;
    r.count = static_cast<std::size_t>(doc.number_or(a, "count", 0));
    r.component = static_cast<int>(doc.number_or(a, "componentType", 0));
    r.components_per_element = components_of(doc.string_or(a, "type", ""));
    const JsonNode& norm = doc.member(a, "normalized");
    r.normalized = norm.type == JsonType::Bool && norm.boolean;
    const int csize = component_size(r.component);
    if (csize == 0 || r.components_per_element == 0) {
        *why = "accessor " + std::to_string(index) + " has an unknown componentType or type";
        return false;
    }
    if (r.count == 0) {  // legal, and nothing to read
        r.valid = true;
        *out = r;
        return true;
    }

    const JsonNode& views = doc.member(root, "bufferViews");
    const JsonNode& v = doc.element(views, static_cast<std::size_t>(
                                               doc.number_or(a, "bufferView", -1)));
    if (v.type != JsonType::Object) {
        *why = "accessor " + std::to_string(index) + " names no bufferView";
        return false;
    }
    if (static_cast<int>(doc.number_or(v, "buffer", 0)) != 0) {
        *why = "only buffer 0 (the BIN chunk) is supported; this file references another";
        return false;
    }
    const std::size_t view_offset = static_cast<std::size_t>(doc.number_or(v, "byteOffset", 0));
    const std::size_t view_length = static_cast<std::size_t>(doc.number_or(v, "byteLength", 0));
    const std::size_t acc_offset = static_cast<std::size_t>(doc.number_or(a, "byteOffset", 0));
    const std::size_t element = static_cast<std::size_t>(csize) *
                                static_cast<std::size_t>(r.components_per_element);
    std::size_t stride = static_cast<std::size_t>(doc.number_or(v, "byteStride", 0));
    if (stride == 0) stride = element;
    if (stride < element) {
        *why = "a bufferView's byteStride is shorter than one element";
        return false;
    }

    // The span this accessor actually touches: (count - 1) strides plus one
    // whole element. Using count * stride would reject a tightly packed final
    // element that is legal.
    if (r.count > (SIZE_MAX - element) / stride) {
        *why = "accessor " + std::to_string(index) + " declares a count that cannot be addressed";
        return false;
    }
    const std::size_t span = (r.count - 1) * stride + element;
    if (view_offset > bin_size || view_length > bin_size - view_offset) {
        *why = "a bufferView lies outside the BIN chunk";
        return false;
    }
    if (acc_offset > view_length || span > view_length - acc_offset) {
        *why = "accessor " + std::to_string(index) + " reads past its bufferView";
        return false;
    }
    r.offset = view_offset + acc_offset;
    r.stride = stride;
    r.valid = true;
    *out = r;
    return true;
}

const std::uint8_t* element_at(const std::uint8_t* bin, const Accessor& a, std::size_t i) {
    return bin + a.offset + i * a.stride;
}

}  // namespace

IoStatus load_glb(const std::uint8_t* data, std::size_t size, mesh::Mesh* out,
                  const ImportBudget& budget) {
    if (!out) return IoStatus::fail(IoError::Malformed, "null output mesh");
    *out = mesh::Mesh{};
    if (!data || size < 12) return IoStatus::fail(IoError::Malformed, "not a GLB: too short");
    if (read_u32le(data) != kGlbMagic)
        return IoStatus::fail(IoError::Malformed, "not a GLB: bad magic");
    const std::uint32_t version = read_u32le(data + 4);
    if (version != 2)
        return IoStatus::fail(IoError::ForwardVersion,
                              "GLB version " + std::to_string(version) + "; this reads version 2");
    const std::uint32_t declared = read_u32le(data + 8);
    // The declared total is a claim. Trusting it over the real length is how a
    // truncated file becomes an out-of-bounds read.
    const std::size_t total = std::min<std::size_t>(declared, size);

    const std::uint8_t* json = nullptr;
    std::size_t json_len = 0;
    const std::uint8_t* bin = nullptr;
    std::size_t bin_len = 0;
    std::size_t at = 12;
    while (at + 8 <= total) {
        const std::uint32_t len = read_u32le(data + at);
        const std::uint32_t type = read_u32le(data + at + 4);
        at += 8;
        if (len > total - at)
            return IoStatus::fail(IoError::Malformed, "a GLB chunk runs past the end of the file");
        if (type == kChunkJson && !json) {
            json = data + at;
            json_len = len;
        } else if (type == kChunkBin && !bin) {
            bin = data + at;
            bin_len = len;
        }
        at += len;
        // Chunks are 4-byte aligned; the padding is part of the container.
        at = (at + 3u) & ~std::size_t{3};
    }
    if (!json) return IoStatus::fail(IoError::Malformed, "the GLB has no JSON chunk");

    JsonDoc doc;
    if (!doc.parse(reinterpret_cast<const char*>(json), json_len))
        return IoStatus::fail(IoError::Malformed, "the GLB's JSON is malformed: " + doc.error());
    const JsonNode& root = doc.root();
    if (root.type != JsonType::Object)
        return IoStatus::fail(IoError::Malformed, "the GLB's JSON is not an object");

    const JsonNode& meshes = doc.member(root, "meshes");
    if (meshes.type != JsonType::Array || meshes.children.empty()) return IoStatus::success();

    // Walk the node hierarchy so each primitive arrives with its world
    // transform. An exported scene otherwise imports as a pile of pieces at the
    // origin, which looks like a broken reader rather than a missing feature.
    struct Instance {
        std::size_t mesh = 0;
        Mat4 world;
    };
    std::vector<Instance> instances;
    const JsonNode& nodes = doc.member(root, "nodes");

    if (nodes.type == JsonType::Array && !nodes.children.empty()) {
        // Iterative, with an explicit stack and a visited set: a node list that
        // names itself as a child is malformed, and recursion on it would not
        // return.
        std::vector<std::uint8_t> seen(nodes.children.size(), 0);
        std::vector<std::pair<std::size_t, Mat4>> stack;

        const JsonNode& scenes = doc.member(root, "scenes");
        const JsonNode& scene =
            doc.element(scenes, static_cast<std::size_t>(doc.number_or(root, "scene", 0)));
        const JsonNode& roots = doc.member(scene, "nodes");
        if (roots.type == JsonType::Array && !roots.children.empty()) {
            for (std::size_t k = roots.children.size(); k-- > 0;)
                stack.push_back({static_cast<std::size_t>(doc.element(roots, k).number),
                                 Mat4::identity()});
        } else {
            // No scene, or an empty one: glTF allows it, and the geometry is
            // still there. Every node is a root.
            for (std::size_t k = nodes.children.size(); k-- > 0;)
                stack.push_back({k, Mat4::identity()});
        }

        while (!stack.empty()) {
            const std::size_t index = stack.back().first;
            const Mat4 parent = stack.back().second;
            stack.pop_back();
            if (index >= nodes.children.size() || seen[index]) continue;
            seen[index] = 1;
            const JsonNode& node = doc.element(nodes, index);
            if (node.type != JsonType::Object) continue;
            const Mat4 world = parent * trs_of(doc, node);
            if (doc.has(node, "mesh")) {
                const std::size_t m = static_cast<std::size_t>(doc.number_or(node, "mesh", 0));
                if (m < meshes.children.size()) instances.push_back({m, world});
            }
            const JsonNode& children = doc.member(node, "children");
            if (children.type == JsonType::Array)
                for (std::size_t k = children.children.size(); k-- > 0;)
                    stack.push_back(
                        {static_cast<std::size_t>(doc.element(children, k).number), world});
        }
    }
    if (instances.empty()) {
        // A file with meshes and no nodes referencing them: import the geometry
        // rather than nothing, untransformed.
        for (std::size_t m = 0; m < meshes.children.size(); ++m)
            instances.push_back({m, Mat4::identity()});
    }

    for (const Instance& inst : instances) {
        const JsonNode& mesh_node = doc.element(meshes, inst.mesh);
        const JsonNode& primitives = doc.member(mesh_node, "primitives");
        if (primitives.type != JsonType::Array) continue;
        for (std::size_t pi = 0; pi < primitives.children.size(); ++pi) {
            const JsonNode& prim = doc.element(primitives, pi);
            if (prim.type != JsonType::Object) continue;
            const int mode = static_cast<int>(doc.number_or(prim, "mode", 4));
            if (mode != 4)
                return IoStatus::fail(IoError::Unsupported,
                                      "primitive mode " + std::to_string(mode) +
                                          " is not TRIANGLES; importing it as geometry would "
                                          "silently drop it");
            const JsonNode& attrs = doc.member(prim, "attributes");
            if (!doc.has(attrs, "POSITION")) continue;

            std::string why;
            Accessor pos;
            if (!resolve_accessor(doc, root,
                                  static_cast<std::size_t>(doc.number_or(attrs, "POSITION", 0)),
                                  bin_len, &pos, &why))
                return IoStatus::fail(IoError::Malformed, why);
            if (pos.components_per_element != 3 || pos.component != 5126)
                return IoStatus::fail(IoError::Malformed, "POSITION must be a float VEC3");
            if (pos.count > 0 && !bin)
                return IoStatus::fail(IoError::Malformed, "the GLB has vertices and no BIN chunk");

            const std::size_t base = out->positions.size();
            if (pos.count > budget.max_vertices - std::min(base, budget.max_vertices))
                return IoStatus::fail(IoError::BudgetExceeded,
                                      "the file declares more vertices than the import budget");

            const auto attribute = [&](const char* name, Accessor* a) -> bool {
                if (!doc.has(attrs, name)) return false;
                if (!resolve_accessor(doc, root,
                                      static_cast<std::size_t>(doc.number_or(attrs, name, 0)),
                                      bin_len, a, &why))
                    return false;
                return a->count == pos.count;
            };
            Accessor nrm, col, uv;
            const bool has_nrm = attribute("NORMAL", &nrm) && nrm.components_per_element == 3;
            const bool has_col = attribute("COLOR_0", &col) &&
                                 (col.components_per_element == 3 ||
                                  col.components_per_element == 4);
            const bool has_uv = attribute("TEXCOORD_0", &uv) && uv.components_per_element == 2;

            // Attributes are vertex-aligned or absent. A mesh that gathered
            // normals for some primitives and not others would have a normals
            // array shorter than its positions, which every consumer reads as
            // "no normals" — so the arrays are padded to stay aligned.
            const bool want_nrm = has_nrm || !out->normals.empty();
            const bool want_col = has_col || !out->colors.empty();
            const bool want_uv = has_uv || !out->uvs.empty();
            if (want_nrm) out->normals.resize(base);
            if (want_col) out->colors.resize(base);
            if (want_uv) out->uvs.resize(base);

            for (std::size_t v = 0; v < pos.count; ++v) {
                const std::uint8_t* p = element_at(bin, pos, v);
                out->positions.push_back(
                    inst.world.point(kernel::cf3(read_f32le(p), read_f32le(p + 4),
                                                 read_f32le(p + 8))));
                if (want_nrm) {
                    kernel::cfloat3 n = kernel::cf3(0, 0, 0);
                    if (has_nrm) {
                        const int ns = component_size(nrm.component);
                        const std::uint8_t* q = element_at(bin, nrm, v);
                        const auto c = [&](int k) {
                            return component_as_float(q + k * ns, nrm.component, nrm.normalized);
                        };
                        n = inst.world.normal(kernel::cf3(c(0), c(1), c(2)));
                        const float len = kernel::clength(n);
                        if (len > 0.0f) n = n / len;
                    }
                    out->normals.push_back(n);
                }
                if (want_col) {
                    kernel::cfloat3 c = kernel::cf3(1, 1, 1);
                    if (has_col) {
                        const int cs = component_size(col.component);
                        const std::uint8_t* q = element_at(bin, col, v);
                        c = kernel::cf3(component_as_float(q, col.component, col.normalized),
                                        component_as_float(q + cs, col.component, col.normalized),
                                        component_as_float(q + 2 * cs, col.component,
                                                           col.normalized));
                    }
                    out->colors.push_back(c);
                }
                if (want_uv) {
                    kernel::cfloat2 t{0.0f, 0.0f};
                    if (has_uv) {
                        const int cs = component_size(uv.component);
                        const std::uint8_t* q = element_at(bin, uv, v);
                        t.x = component_as_float(q, uv.component, uv.normalized);
                        t.y = component_as_float(q + cs, uv.component, uv.normalized);
                    }
                    out->uvs.push_back(t);
                }
            }

            if (doc.has(prim, "indices")) {
                Accessor idx;
                if (!resolve_accessor(doc, root,
                                      static_cast<std::size_t>(doc.number_or(prim, "indices", 0)),
                                      bin_len, &idx, &why))
                    return IoStatus::fail(IoError::Malformed, why);
                if (idx.components_per_element != 1)
                    return IoStatus::fail(IoError::Malformed, "an index accessor must be SCALAR");
                if (idx.count % 3 != 0)
                    return IoStatus::fail(IoError::Malformed,
                                          "a TRIANGLES primitive's index count is not a multiple "
                                          "of three");
                if (idx.count / 3 > budget.max_triangles - std::min(out->indices.size() / 3,
                                                                    budget.max_triangles))
                    return IoStatus::fail(IoError::BudgetExceeded,
                                          "the file declares more triangles than the import "
                                          "budget");
                for (std::size_t k = 0; k < idx.count; ++k) {
                    const std::uint8_t* q = element_at(bin, idx, k);
                    std::uint32_t v = 0;
                    switch (idx.component) {
                        case 5121: v = *q; break;
                        case 5123: { std::uint16_t h = 0; std::memcpy(&h, q, 2); v = h; break; }
                        case 5125: v = read_u32le(q); break;
                        default:
                            return IoStatus::fail(IoError::Malformed,
                                                  "an index accessor must be an unsigned integer");
                    }
                    if (v >= pos.count)
                        return IoStatus::fail(IoError::Malformed,
                                              "an index points past the primitive's vertices");
                    out->indices.push_back(static_cast<std::uint32_t>(base) + v);
                }
            } else {
                // Non-indexed: the vertices are the triangles, in order.
                if (pos.count % 3 != 0)
                    return IoStatus::fail(IoError::Malformed,
                                          "a non-indexed TRIANGLES primitive's vertex count is "
                                          "not a multiple of three");
                for (std::size_t k = 0; k < pos.count; ++k)
                    out->indices.push_back(static_cast<std::uint32_t>(base + k));
            }
        }
    }
    return IoStatus::success();
}

IoStatus load_glb_file(const std::string& path, mesh::Mesh* out, const ImportBudget& budget) {
    std::vector<std::uint8_t> bytes;
    IoStatus s = detail::read_whole_file(path, &bytes, budget.max_file_bytes);
    if (!s.ok()) return s;
    return load_glb(bytes.data(), bytes.size(), out, budget);
}

}  // namespace io
}  // namespace clay
