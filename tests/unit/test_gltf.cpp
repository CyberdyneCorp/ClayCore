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
