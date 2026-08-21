// GLB writer tests: structural container checks, a minimal in-test GLB/JSON
// reader that decodes the BIN chunk back through the JSON offsets, and a
// golden-scene determinism check. Full glTF-validator conformance runs in CI
// at integration time.

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "clay/io/mesh_io.h"
#include "clay/mesh/marching.h"
#include "scene_utils.h"

using namespace clay;
using namespace clay::kernel;
using clay_test::item;

namespace {

mesh::Mesh quad_mesh() {
    mesh::Mesh m;
    m.positions = {cf3(-1, 0, -2), cf3(1.5f, 0, -2), cf3(1.5f, 0.25f, 3), cf3(-1, 0.25f, 3)};
    m.normals = {cf3(0, 1, 0), cf3(0, 1, 0), cf3(0, 1, 0), cf3(0, 1, 0)};
    m.colors = {cf3(1, 0, 0), cf3(0, 1, 0), cf3(0, 0, 1), cf3(0.25f, 0.5f, 0.75f)};
    m.uvs = {cf2(0, 0), cf2(1, 0), cf2(1, 1), cf2(0, 1)};
    m.indices = {0, 1, 2, 0, 2, 3};
    return m;
}

mesh::Mesh golden_scene_mesh() {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node a = item(scene::Prim::sphere(0.6f), cf3(-0.3f, 0, 0));
    a.color = cf3(0.9f, 0.2f, 0.1f);
    l.sdf->insert(a);
    scene::Node b = item(scene::Prim::box(cf3(0.4f, 0.4f, 0.4f)), cf3(0.4f, 0.1f, 0),
                         scene::Op::Add, scene::Blend{scene::BlendProfile::Quadratic, 0.1f});
    b.color = cf3(0.1f, 0.3f, 0.9f);
    l.sdf->insert(b);
    scene::Tape tape = scene::compile_document(doc);
    return mesh::mesh_tape(tape, math::Aabb{cf3(-1.2f, -1.2f, -1.2f), cf3(1.2f, 1.2f, 1.2f)},
                           0.08f);
}

// -- minimal GLB reader (test-only) ------------------------------------------

std::uint32_t u32at(const std::vector<std::uint8_t>& b, std::size_t off) {
    std::uint32_t v;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

struct GlbParts {
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t total = 0;
    std::uint32_t json_length = 0;
    std::uint32_t bin_length = 0;
    std::string json;  // includes chunk padding
    std::vector<std::uint8_t> bin;
};

GlbParts split_glb(const std::vector<std::uint8_t>& b) {
    GlbParts g;
    REQUIRE(b.size() >= 12);
    g.magic = u32at(b, 0);
    g.version = u32at(b, 4);
    g.total = u32at(b, 8);
    std::size_t off = 12;
    while (off + 8 <= b.size()) {
        std::uint32_t len = u32at(b, off);
        std::uint32_t type = u32at(b, off + 4);
        REQUIRE(off + 8 + len <= b.size());
        if (type == 0x4E4F534Au) {
            g.json_length = len;
            g.json.assign(b.begin() + static_cast<long>(off + 8),
                          b.begin() + static_cast<long>(off + 8 + len));
        } else if (type == 0x004E4942u) {
            g.bin_length = len;
            g.bin.assign(b.begin() + static_cast<long>(off + 8),
                         b.begin() + static_cast<long>(off + 8 + len));
        }
        off += 8 + len;
    }
    REQUIRE(off == b.size());
    return g;
}

// enough JSON poking to read back what the writer emits: integer after a key
long json_int_after(const std::string& j, const std::string& key) {
    std::size_t p = j.find(key);
    REQUIRE(p != std::string::npos);
    return std::strtol(j.c_str() + p + key.size(), nullptr, 10);
}

// i-th top-level object of the array named `name`
std::string json_array_item(const std::string& j, const std::string& name, long index) {
    std::size_t p = j.find("\"" + name + "\":[");
    REQUIRE(p != std::string::npos);
    p = j.find('[', p) + 1;
    int depth = 0;
    long count = -1;
    std::size_t start = 0;
    for (std::size_t i = p; i < j.size(); ++i) {
        char c = j[i];
        if (c == '{') {
            if (depth == 0) {
                ++count;
                start = i;
            }
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0 && count == index) return j.substr(start, i - start + 1);
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
    FAIL("array item not found: ", name, "[", index, "]");
    return {};
}

struct AccessorData {
    std::size_t offset = 0;
    std::size_t length = 0;
    long count = 0;
    long component_type = 0;
    long target = 0;
};

AccessorData read_accessor(const GlbParts& g, long accessor_index) {
    std::string acc = json_array_item(g.json, "accessors", accessor_index);
    AccessorData a;
    a.count = json_int_after(acc, "\"count\":");
    a.component_type = json_int_after(acc, "\"componentType\":");
    std::string view = json_array_item(g.json, "bufferViews", json_int_after(acc, "\"bufferView\":"));
    a.offset = static_cast<std::size_t>(json_int_after(view, "\"byteOffset\":"));
    a.length = static_cast<std::size_t>(json_int_after(view, "\"byteLength\":"));
    a.target = json_int_after(view, "\"target\":");
    REQUIRE(a.offset + a.length <= g.bin.size());
    return a;
}

std::vector<float> decode_floats(const GlbParts& g, const AccessorData& a) {
    std::vector<float> f(a.length / 4);
    std::memcpy(f.data(), g.bin.data() + a.offset, a.length);
    return f;
}

int count_occurrences(const std::string& s, const std::string& needle) {
    int n = 0;
    for (std::size_t p = s.find(needle); p != std::string::npos; p = s.find(needle, p + 1)) ++n;
    return n;
}

}  // namespace

TEST_CASE("glb: container structure, accessor counts, position min/max") {
    mesh::Mesh m = quad_mesh();
    std::vector<std::uint8_t> bytes = io::save_glb(m);
    GlbParts g = split_glb(bytes);

    CHECK(g.magic == 0x46546C67u);
    CHECK(g.version == 2);
    CHECK(g.total == bytes.size());
    CHECK(g.json_length % 4 == 0);
    CHECK(g.bin_length % 4 == 0);
    // JSON chunk padding is spaces, so trimming trailing spaces ends at '}'
    CHECK(g.json[g.json.find_last_not_of(' ')] == '}');

    CHECK(g.json.find("\"asset\":{\"version\":\"2.0\",\"generator\":\"claycore\"}") !=
          std::string::npos);
    CHECK(g.json.find("\"scene\":0") != std::string::npos);
    CHECK(g.json.find("\"mode\":4") != std::string::npos);
    // one accessor per attribute (POSITION, NORMAL, COLOR_0, TEXCOORD_0) + indices
    CHECK(count_occurrences(g.json, "\"componentType\":5126") == 4);
    CHECK(count_occurrences(g.json, "\"componentType\":5125") == 1);
    CHECK(count_occurrences(g.json, "\"target\":34962") == 4);
    CHECK(count_occurrences(g.json, "\"target\":34963") == 1);
    CHECK(json_int_after(g.json, "\"buffers\":[{\"byteLength\":") ==
          static_cast<long>(4 * (12 + 12 + 12 + 8) + 6 * 4));

    // position accessor min/max match the computed bounds, %.9g formatted
    char expect[128];
    std::snprintf(expect, sizeof expect, "\"min\":[%.9g,%.9g,%.9g]", -1.0, 0.0, -2.0);
    CHECK(g.json.find(expect) != std::string::npos);
    std::snprintf(expect, sizeof expect, "\"max\":[%.9g,%.9g,%.9g]", 1.5, 0.25, 3.0);
    CHECK(g.json.find(expect) != std::string::npos);
}

TEST_CASE("glb: BIN chunk decodes back to the exact source mesh") {
    mesh::Mesh m = quad_mesh();
    GlbParts g = split_glb(io::save_glb(m));

    AccessorData pos = read_accessor(g, json_int_after(g.json, "\"POSITION\":"));
    AccessorData nrm = read_accessor(g, json_int_after(g.json, "\"NORMAL\":"));
    AccessorData col = read_accessor(g, json_int_after(g.json, "\"COLOR_0\":"));
    AccessorData uv = read_accessor(g, json_int_after(g.json, "\"TEXCOORD_0\":"));
    AccessorData idx = read_accessor(g, json_int_after(g.json, "\"indices\":"));

    CHECK(pos.count == 4);
    CHECK(pos.component_type == 5126);
    CHECK(pos.target == 34962);
    CHECK(idx.count == 6);
    CHECK(idx.component_type == 5125);
    CHECK(idx.target == 34963);

    std::vector<float> p = decode_floats(g, pos);
    std::vector<float> n = decode_floats(g, nrm);
    std::vector<float> c = decode_floats(g, col);
    std::vector<float> t = decode_floats(g, uv);
    REQUIRE(p.size() == m.positions.size() * 3);
    REQUIRE(n.size() == m.normals.size() * 3);
    REQUIRE(c.size() == m.colors.size() * 3);
    REQUIRE(t.size() == m.uvs.size() * 2);
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        CHECK(p[i * 3] == m.positions[i].x);
        CHECK(p[i * 3 + 1] == m.positions[i].y);
        CHECK(p[i * 3 + 2] == m.positions[i].z);
        CHECK(n[i * 3] == m.normals[i].x);
        CHECK(n[i * 3 + 1] == m.normals[i].y);
        CHECK(n[i * 3 + 2] == m.normals[i].z);
        CHECK(c[i * 3] == m.colors[i].x);
        CHECK(c[i * 3 + 1] == m.colors[i].y);
        CHECK(c[i * 3 + 2] == m.colors[i].z);
        CHECK(t[i * 2] == m.uvs[i].x);
        CHECK(t[i * 2 + 1] == m.uvs[i].y);
    }
    REQUIRE(idx.length == m.indices.size() * 4);
    for (std::size_t i = 0; i < m.indices.size(); ++i)
        CHECK(u32at(g.bin, idx.offset + i * 4) == m.indices[i]);
}

TEST_CASE("glb: attributes absent from the mesh are omitted") {
    mesh::Mesh m = quad_mesh();
    m.normals.clear();
    m.uvs.clear();
    GlbParts g = split_glb(io::save_glb(m));
    CHECK(g.json.find("\"NORMAL\"") == std::string::npos);
    CHECK(g.json.find("\"TEXCOORD_0\"") == std::string::npos);
    CHECK(g.json.find("\"COLOR_0\"") != std::string::npos);
    CHECK(count_occurrences(g.json, "\"componentType\":5126") == 2);
}

TEST_CASE("glb: golden scene export is non-empty and deterministic") {
    mesh::Mesh m = golden_scene_mesh();
    REQUIRE_FALSE(m.empty());
    std::vector<std::uint8_t> first = io::save_glb(m);
    std::vector<std::uint8_t> second = io::save_glb(m);
    CHECK_FALSE(first.empty());
    CHECK(first == second);

    GlbParts g = split_glb(first);
    CHECK(g.magic == 0x46546C67u);
    AccessorData pos = read_accessor(g, json_int_after(g.json, "\"POSITION\":"));
    CHECK(pos.count == static_cast<long>(m.positions.size()));
    AccessorData idx = read_accessor(g, json_int_after(g.json, "\"indices\":"));
    CHECK(idx.count == static_cast<long>(m.indices.size()));

    // file variant writes the same bytes
    std::string path = "test_gltf_golden.glb";
    REQUIRE(io::save_glb_file(m, path).ok());
    std::FILE* f = std::fopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);
    std::vector<std::uint8_t> from_disk(first.size() + 1);
    std::size_t read = std::fread(from_disk.data(), 1, from_disk.size(), f);
    std::fclose(f);
    std::remove(path.c_str());
    from_disk.resize(read);
    CHECK(from_disk == first);
}

// -- the reader (add-glb-import) ----------------------------------------------
//
// GLB was the only format this library could write and not read. These are the
// tests that matter for a reader of UNTRUSTED input: the round trip, and then
// every way a file can lie about itself.

TEST_CASE("glb: a mesh survives the round trip") {
    const mesh::Mesh src = quad_mesh();
    const std::vector<std::uint8_t> glb = io::save_glb(src);

    mesh::Mesh back;
    const io::IoStatus s = io::load_glb(glb.data(), glb.size(), &back);
    REQUIRE(s.ok());
    REQUIRE(back.positions.size() == src.positions.size());
    REQUIRE(back.indices.size() == src.indices.size());
    REQUIRE(back.normals.size() == src.normals.size());
    REQUIRE(back.colors.size() == src.colors.size());
    REQUIRE(back.uvs.size() == src.uvs.size());

    // Bit-identical, not merely close: every attribute is float32 both ways and
    // the identity transform must not perturb one.
    for (std::size_t i = 0; i < src.positions.size(); ++i) {
        CHECK(back.positions[i].x == src.positions[i].x);
        CHECK(back.positions[i].y == src.positions[i].y);
        CHECK(back.positions[i].z == src.positions[i].z);
        CHECK(back.colors[i].x == src.colors[i].x);
        CHECK(back.uvs[i].x == src.uvs[i].x);
        CHECK(back.uvs[i].y == src.uvs[i].y);
    }
    for (std::size_t i = 0; i < src.indices.size(); ++i)
        CHECK(back.indices[i] == src.indices[i]);
}

TEST_CASE("glb: a real meshed scene round trips") {
    const mesh::Mesh src = golden_scene_mesh();
    REQUIRE(src.positions.size() > 100);
    const std::vector<std::uint8_t> glb = io::save_glb(src);
    mesh::Mesh back;
    REQUIRE(io::load_glb(glb.data(), glb.size(), &back).ok());
    CHECK(back.positions.size() == src.positions.size());
    CHECK(back.indices.size() == src.indices.size());
    for (std::size_t i = 0; i < src.positions.size(); i += 37) {
        CHECK(back.positions[i].x == src.positions[i].x);
        CHECK(back.positions[i].z == src.positions[i].z);
    }
}

TEST_CASE("glb: a file that lies about itself is refused, not read") {
    const std::vector<std::uint8_t> good = io::save_glb(quad_mesh());
    mesh::Mesh out;

    SUBCASE("too short, or not a GLB at all") {
        CHECK(io::load_glb(nullptr, 0, &out).error == io::IoError::Malformed);
        CHECK(io::load_glb(good.data(), 8, &out).error == io::IoError::Malformed);
        std::vector<std::uint8_t> bad = good;
        bad[0] ^= 0xFF;  // magic
        CHECK(io::load_glb(bad.data(), bad.size(), &out).error == io::IoError::Malformed);
    }

    SUBCASE("a version this reader does not know") {
        std::vector<std::uint8_t> bad = good;
        bad[4] = 3;
        CHECK(io::load_glb(bad.data(), bad.size(), &out).error == io::IoError::ForwardVersion);
    }

    SUBCASE("truncated after the header") {
        // The declared total length still says the file is whole. Trusting it
        // over the real length is exactly how a truncated file becomes an
        // out-of-bounds read, so the reader takes the smaller of the two.
        for (std::size_t cut : {13u, 24u, 40u, 96u}) {
            if (cut >= good.size()) continue;
            const io::IoStatus s = io::load_glb(good.data(), cut, &out);
            CAPTURE(cut);
            CHECK((!s.ok() || out.positions.empty()));  // refused, or honestly empty
        }
    }

    SUBCASE("a chunk claiming to be longer than the file") {
        std::vector<std::uint8_t> bad = good;
        const std::uint32_t huge = 0xFFFFFF00u;
        std::memcpy(bad.data() + 12, &huge, 4);  // first chunk's length
        CHECK_FALSE(io::load_glb(bad.data(), bad.size(), &out).ok());
    }

    SUBCASE("malformed JSON") {
        std::vector<std::uint8_t> bad = good;
        // The JSON chunk starts at byte 20; corrupt its first character.
        bad[20] = '#';
        const io::IoStatus s = io::load_glb(bad.data(), bad.size(), &out);
        CHECK(s.error == io::IoError::Malformed);
        CHECK(s.detail.find("JSON") != std::string::npos);
    }
}

TEST_CASE("glb: the import budget is enforced before allocating") {
    const std::vector<std::uint8_t> glb = io::save_glb(quad_mesh());
    mesh::Mesh out;
    io::ImportBudget tiny;
    tiny.max_vertices = 2;  // the quad has four
    CHECK(io::load_glb(glb.data(), glb.size(), &out, tiny).error == io::IoError::BudgetExceeded);

    io::ImportBudget few_tris;
    few_tris.max_triangles = 1;  // the quad is two
    CHECK(io::load_glb(glb.data(), glb.size(), &out, few_tris).error ==
          io::IoError::BudgetExceeded);
}

namespace {

// A GLB assembled by hand, using conventions this library's WRITER never
// emits: a node transform, uint16 indices, an interleaved bufferView with a
// byteStride, COLOR_0 as normalized unsigned bytes, and two primitives in one
// mesh. Round-tripping our own output proves the reader agrees with our
// writer; this is what proves it reads somebody else's file.
std::vector<std::uint8_t> hand_built_glb(const std::string& json,
                                         const std::vector<std::uint8_t>& bin) {
    auto pad4 = [](std::vector<std::uint8_t>* v, std::uint8_t with) {
        while (v->size() % 4 != 0) v->push_back(with);
    };
    std::vector<std::uint8_t> j(json.begin(), json.end());
    pad4(&j, ' ');
    std::vector<std::uint8_t> b = bin;
    pad4(&b, 0);

    std::vector<std::uint8_t> out;
    auto u32 = [&out](std::uint32_t v) {
        std::uint8_t t[4];
        std::memcpy(t, &v, 4);
        out.insert(out.end(), t, t + 4);
    };
    u32(0x46546C67u);
    u32(2);
    u32(static_cast<std::uint32_t>(12 + 8 + j.size() + 8 + b.size()));
    u32(static_cast<std::uint32_t>(j.size()));
    u32(0x4E4F534Au);
    out.insert(out.end(), j.begin(), j.end());
    u32(static_cast<std::uint32_t>(b.size()));
    u32(0x004E4942u);
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

void put_f32(std::vector<std::uint8_t>* v, float f) {
    std::uint8_t t[4];
    std::memcpy(t, &f, 4);
    v->insert(v->end(), t, t + 4);
}

void put_u16(std::vector<std::uint8_t>* v, std::uint16_t u) {
    std::uint8_t t[2];
    std::memcpy(t, &u, 2);
    v->insert(v->end(), t, t + 2);
}

}  // namespace

TEST_CASE("glb: a file written by somebody else's exporter reads") {
    // One triangle, positions interleaved with an unsigned-byte colour at a
    // stride of 16, uint16 indices, under a node that translates by +10 on x
    // and scales by 2.
    std::vector<std::uint8_t> bin;
    const float xs[3] = {0.0f, 1.0f, 0.0f};
    const float ys[3] = {0.0f, 0.0f, 1.0f};
    const std::uint8_t reds[3] = {255, 0, 0};
    for (int i = 0; i < 3; ++i) {
        put_f32(&bin, xs[i]);
        put_f32(&bin, ys[i]);
        put_f32(&bin, 0.0f);
        bin.push_back(reds[i]);
        bin.push_back(0);
        bin.push_back(0);
        bin.push_back(255);
    }
    const std::size_t index_offset = bin.size();
    put_u16(&bin, 0);
    put_u16(&bin, 1);
    put_u16(&bin, 2);

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0,\"translation\":[10,0,0],\"scale\":[2,2,2]}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"COLOR_0\":1},"
        "\"indices\":2}]}],"
        "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48,\"byteStride\":16},"
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(index_offset) + ",\"byteLength\":6}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"byteOffset\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":0,\"byteOffset\":12,\"componentType\":5121,\"normalized\":true,"
        "\"count\":3,\"type\":\"VEC4\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}]}";

    const std::vector<std::uint8_t> glb = hand_built_glb(json, bin);
    mesh::Mesh out;
    const io::IoStatus s = io::load_glb(glb.data(), glb.size(), &out);
    REQUIRE_MESSAGE(s.ok(), s.detail);
    REQUIRE(out.positions.size() == 3);
    REQUIRE(out.indices.size() == 3);

    // The node transform was applied: scaled by 2 then translated by +10.
    CHECK(out.positions[0].x == doctest::Approx(10.0f));
    CHECK(out.positions[1].x == doctest::Approx(12.0f));
    CHECK(out.positions[2].y == doctest::Approx(2.0f));

    // COLOR_0 came in as normalized bytes, and the vec4's alpha was dropped
    // because the mesh has nowhere to put it.
    REQUIRE(out.colors.size() == 3);
    CHECK(out.colors[0].x == doctest::Approx(1.0f));
    CHECK(out.colors[1].x == doctest::Approx(0.0f));
}

TEST_CASE("glb: a non-triangle primitive is refused rather than silently dropped") {
    std::vector<std::uint8_t> bin;
    for (int i = 0; i < 2; ++i) {
        put_f32(&bin, static_cast<float>(i));
        put_f32(&bin, 0.0f);
        put_f32(&bin, 0.0f);
    }
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"mode\":1}]}],"
        "\"buffers\":[{\"byteLength\":24}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":24}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":2,"
        "\"type\":\"VEC3\"}]}";
    const std::vector<std::uint8_t> glb = hand_built_glb(json, bin);
    mesh::Mesh out;
    const io::IoStatus s = io::load_glb(glb.data(), glb.size(), &out);
    CHECK(s.error == io::IoError::Unsupported);
    CHECK(s.detail.find("TRIANGLES") != std::string::npos);
}

TEST_CASE("glb: an accessor that reads past its bufferView is refused") {
    // The commonest real-world corruption, and the one that would be an
    // out-of-bounds read: a count larger than the data behind it.
    std::vector<std::uint8_t> bin(36, 0);
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
        "\"buffers\":[{\"byteLength\":36}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":9999,"
        "\"type\":\"VEC3\"}]}";
    const std::vector<std::uint8_t> glb = hand_built_glb(json, bin);
    mesh::Mesh out;
    const io::IoStatus s = io::load_glb(glb.data(), glb.size(), &out);
    CHECK(s.error == io::IoError::Malformed);
    CHECK(out.positions.empty());
}

TEST_CASE("glb: an index pointing past the vertices is refused") {
    std::vector<std::uint8_t> bin;
    for (int i = 0; i < 3; ++i) {
        put_f32(&bin, static_cast<float>(i));
        put_f32(&bin, 0.0f);
        put_f32(&bin, 0.0f);
    }
    const std::size_t io_off = bin.size();
    put_u16(&bin, 0);
    put_u16(&bin, 1);
    put_u16(&bin, 7);  // there is no vertex 7
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
        "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(io_off) + ",\"byteLength\":6}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}]}";
    const std::vector<std::uint8_t> glb = hand_built_glb(json, bin);
    mesh::Mesh out;
    CHECK(io::load_glb(glb.data(), glb.size(), &out).error == io::IoError::Malformed);
}

TEST_CASE("glb: a node cycle terminates instead of recursing forever") {
    // Two nodes naming each other as children. Malformed, and a recursive
    // walker would not come back.
    std::vector<std::uint8_t> bin;
    for (int i = 0; i < 3; ++i) {
        put_f32(&bin, static_cast<float>(i));
        put_f32(&bin, 0.0f);
        put_f32(&bin, 0.0f);
    }
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"children\":[1]},{\"children\":[0],\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
        "\"buffers\":[{\"byteLength\":36}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
        "\"type\":\"VEC3\"}]}";
    const std::vector<std::uint8_t> glb = hand_built_glb(json, bin);
    mesh::Mesh out;
    REQUIRE(io::load_glb(glb.data(), glb.size(), &out).ok());
    CHECK(out.positions.size() == 3);  // the mesh appears once, not endlessly
}

TEST_CASE("glb: mutating a valid file never reads out of bounds") {
    // A parser of untrusted input is only as good as what it does to a file it
    // has never seen. This flips bytes through a valid GLB deterministically —
    // no RNG, so a failure reproduces exactly — and requires only that the
    // reader returns rather than reads past the end. Under ASan, "returns" is
    // the whole assertion.
    const std::vector<std::uint8_t> good = io::save_glb(quad_mesh());
    REQUIRE(good.size() > 64);

    std::size_t ok = 0, refused = 0;
    for (std::size_t at = 0; at < good.size(); at += 3) {
        for (std::uint8_t xor_with : {std::uint8_t{0x01}, std::uint8_t{0x80}, std::uint8_t{0xFF}}) {
            std::vector<std::uint8_t> bad = good;
            bad[at] ^= xor_with;
            mesh::Mesh out;
            const io::IoStatus s = io::load_glb(bad.data(), bad.size(), &out);
            if (s.ok()) {
                ++ok;
                // Whatever it accepted must still be self-consistent: no index
                // may point past the vertices it was read with.
                for (std::uint32_t i : out.indices) REQUIRE(i < out.positions.size());
            } else {
                ++refused;
            }
        }
    }
    // Both outcomes are legitimate — a flipped byte in a float is still a valid
    // float — so this only records that the corpus exercised both paths.
    CHECK(ok + refused > 0);
    MESSAGE("mutations accepted: " << ok << ", refused: " << refused);
}
