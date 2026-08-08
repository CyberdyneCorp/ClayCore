// GLB (glTF 2.0 binary) writer, dependency-free by design: the JSON document
// is assembled with a local string builder and the container is three raw
// little-endian chunks. Full glTF-validator runs happen in CI at integration
// time; the unit tests parse the container back structurally and semantically.

#include <cstdio>
#include <cstring>
#include <string>

#include "clay/io/mesh_io.h"

#include "file_bytes.h"

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

}  // namespace io
}  // namespace clay
