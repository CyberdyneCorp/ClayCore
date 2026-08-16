#include <doctest/doctest.h>

#include <filesystem>

#include "clay/io/clayspace.h"
#include "clay/io/mesh_io.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/validate.h"
#include "kernel_utils.h"
#include "scene_utils.h"

using namespace clay;
using namespace clay::kernel;
using clay_test::gnarly_document;
using clay_test::item;

namespace {

mesh::Mesh sample_mesh() {
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

io::ClaySpaceDoc sample_clayspace() {
    io::ClaySpaceDoc cs;
    cs.document = gnarly_document();
    scene::Layer& vl = cs.document.add_sdf_layer("blocks");
    vl.kind = scene::LayerKind::Voxel;
    vl.sdf.reset();
    voxel::VoxelGrid grid(0.1f);
    grid.fill_box({0, 0, 0}, {5, 3, 2}, grid.palette_add(cf3(1, 0.5f, 0)));
    cs.voxel_layers.emplace(vl.id, std::move(grid));
    cs.thumbnail_png = {0x89, 'P', 'N', 'G', 1, 2, 3};
    return cs;
}

}  // namespace

TEST_CASE("clayspace: bit-identical round trip with voxel layers and extras") {
    io::ClaySpaceDoc cs = sample_clayspace();
    std::vector<std::uint8_t> bytes = io::save_clayspace(cs);
    io::ClaySpaceDoc back;
    io::IoStatus s = io::load_clayspace(bytes.data(), bytes.size(), &back);
    REQUIRE(s.ok());
    CHECK(io::save_clayspace(back) == bytes);  // canonical bit-identity
    CHECK(back.voxel_layers.size() == 1);
    CHECK(back.thumbnail_png == cs.thumbnail_png);
    // the reloaded document evaluates identically
    scene::Tape a = scene::compile_document(cs.document);
    scene::Tape b = scene::compile_document(back.document);
    clay_test::Lcg rng(901);
    for (int i = 0; i < 200; ++i) {
        cfloat3 p = rng.vec3(-3, 3);
        CHECK(a.eval(p).d == b.eval(p).d);  // exact
    }
}

TEST_CASE("clayspace: a voxel layer's sculpt layers survive the file") {
    // The voxel payload is opaque to the container, so this is the test that
    // says the two actually travel together — the format bump to minor 10 is
    // otherwise a number nothing checks.
    io::ClaySpaceDoc cs = sample_clayspace();
    REQUIRE(cs.voxel_layers.size() == 1);
    voxel::VoxelGrid& grid = cs.voxel_layers.begin()->second;
    const std::uint8_t idx = grid.palette_add(cf3(0.4f, 0.7f, 0.3f));

    grid.begin_sculpt_layer("a dialable pass");
    for (int i = 0; i < 12; ++i) grid.set({i, 1, 1}, idx);
    grid.end_sculpt_layer();
    grid.set_sculpt_layer_strength(0, 0.35f);
    const std::size_t composed = grid.occupied_count();

    std::vector<std::uint8_t> bytes = io::save_clayspace(cs);
    io::ClaySpaceDoc back;
    REQUIRE(io::load_clayspace(bytes.data(), bytes.size(), &back).ok());
    CHECK(io::save_clayspace(back) == bytes);  // still canonical

    REQUIRE(back.voxel_layers.size() == 1);
    voxel::VoxelGrid& reloaded = back.voxel_layers.begin()->second;
    REQUIRE(reloaded.sculpt_layer_count() == 1);
    CHECK(reloaded.sculpt_layer_name(0) == "a dialable pass");
    CHECK(reloaded.sculpt_layer_strength(0) == doctest::Approx(0.35f));
    CHECK(reloaded.occupied_count() == composed);

    // Still dialable after the round trip, which is the whole reason the diff
    // is stored rather than the result.
    CHECK(reloaded.set_sculpt_layer_strength(0, 1.0f));
    CHECK(grid.set_sculpt_layer_strength(0, 1.0f));
    CHECK(reloaded.occupied_count() == grid.occupied_count());
    CHECK(reloaded.serialize() == grid.serialize());
}

TEST_CASE("clayspace: forward-refuse, truncation, unknown chunks") {
    std::vector<std::uint8_t> bytes = io::save_clayspace(sample_clayspace());
    io::ClaySpaceDoc out;

    // newer major version refused with no partial document
    std::vector<std::uint8_t> newer = bytes;
    newer[4] = 99;
    io::IoStatus s = io::load_clayspace(newer.data(), newer.size(), &out);
    CHECK(s.error == io::IoError::ForwardVersion);

    // truncation at every prefix fails cleanly (never crashes)
    for (std::size_t cut = 0; cut < bytes.size(); cut += 97)
        CHECK_FALSE(io::load_clayspace(bytes.data(), cut, &out).ok());

    // unknown chunk appended -> still loads (backward-open)
    std::vector<std::uint8_t> extended = bytes;
    const char cc[4] = {'F', 'U', 'T', 'R'};
    extended.insert(extended.end(), cc, cc + 4);
    for (int i = 0; i < 8; ++i) extended.push_back(i == 0 ? 4 : 0);  // u64 size = 4
    for (int i = 0; i < 4; ++i) extended.push_back(0xAB);
    CHECK(io::load_clayspace(extended.data(), extended.size(), &out).ok());
}

TEST_CASE("OBJ: vertex-color round trip") {
    mesh::Mesh m = sample_mesh();
    std::string text = io::save_obj(m, "clay", "clay.mtl");
    CHECK(text.find("mtllib clay.mtl") != std::string::npos);
    mesh::Mesh back;
    REQUIRE(io::load_obj(text, &back).ok());
    REQUIRE(back.positions.size() == m.positions.size());
    REQUIRE(back.indices == m.indices);
    REQUIRE(back.colors.size() == m.colors.size());
    for (std::size_t i = 0; i < m.positions.size(); i += 13) {
        CHECK(clength(back.positions[i] - m.positions[i]) < 1e-5f);
        CHECK(clength(back.colors[i] - m.colors[i]) < 1e-5f);
    }
    CHECK(io::save_mtl().find("newmtl") == 0);
}

TEST_CASE("PLY: binary and ascii round trips with colors") {
    mesh::Mesh m = sample_mesh();
    for (bool binary : {true, false}) {
        CAPTURE(binary);
        std::vector<std::uint8_t> bytes = io::save_ply(m, binary);
        mesh::Mesh back;
        REQUIRE(io::load_ply(bytes.data(), bytes.size(), &back).ok());
        REQUIRE(back.positions.size() == m.positions.size());
        REQUIRE(back.indices == m.indices);
        for (std::size_t i = 0; i < m.positions.size(); i += 13) {
            CHECK(clength(back.positions[i] - m.positions[i]) < (binary ? 1e-7f : 1e-5f));
            // colors quantized to 8 bits: sqrt(3) * half-step L2 bound
            CHECK(clength(back.colors[i] - m.colors[i]) < 1.7320508f * 0.5f / 255.0f + 1e-4f);
        }
    }
}

TEST_CASE("FBX: writer output round-trips through ufbx with geometry intact") {
    mesh::Mesh m = sample_mesh();
    std::vector<std::uint8_t> bytes = io::save_fbx(m, "clay_export");
    REQUIRE(bytes.size() > 200);
    mesh::Mesh back;
    io::IoStatus s = io::load_fbx(bytes.data(), bytes.size(), &back);
    REQUIRE(s.ok());
    CHECK(back.triangle_count() == m.triangle_count());
    // geometry preserved: volume and area agree
    CHECK(mesh::signed_volume(back) == doctest::Approx(mesh::signed_volume(m)).epsilon(1e-4));
    CHECK(mesh::surface_area(back) == doctest::Approx(mesh::surface_area(m)).epsilon(1e-4));
    // colors survived
    REQUIRE(back.colors.size() == back.positions.size());
    bool found_red = false, found_blue = false;
    for (const cfloat3& c : back.colors) {
        if (c.x > 0.7f && c.z < 0.3f) found_red = true;
        if (c.z > 0.7f && c.x < 0.3f) found_blue = true;
    }
    CHECK(found_red);
    CHECK(found_blue);
}

TEST_CASE("FBX: import welds, rather than emitting a vertex per triangle corner") {
    // Issue #38. load_fbx appended a vertex for every triangle CORNER and
    // shared none of them, so an import carried triangle_count * 3 vertices
    // whatever the file stored: six times the memory of the welded mesh here,
    // and no two triangles sharing a vertex for anything downstream that walks
    // adjacency. ufbx addresses each attribute by corner, which makes appending
    // per corner the straightforward reading of its API and is how this got in.
    //
    // The surface was never wrong, so area and volume could not catch it. What
    // pins it is the vertex COUNT, checked against the two readers that had it
    // right all along — the same model through three formats should not depend
    // on the extension.
    const mesh::Mesh m = sample_mesh();
    REQUIRE(m.triangle_count() > 100);  // enough sharing for the difference to bite

    std::vector<std::uint8_t> bytes = io::save_fbx(m, "clay_export");
    mesh::Mesh back;
    REQUIRE(io::load_fbx(bytes.data(), bytes.size(), &back).ok());

    mesh::Mesh via_obj, via_ply;
    REQUIRE(io::load_obj(io::save_obj(m, "clay", "clay.mtl"), &via_obj).ok());
    const std::vector<std::uint8_t> ply = io::save_ply(m);
    REQUIRE(io::load_ply(ply.data(), ply.size(), &via_ply).ok());

    CHECK(back.triangle_count() == m.triangle_count());
    CHECK(back.positions.size() == m.positions.size());
    CHECK(back.positions.size() == via_obj.positions.size());
    CHECK(back.positions.size() == via_ply.positions.size());
    // The thing the count is a proxy for: corners genuinely share storage.
    CHECK(back.positions.size() < back.indices.size());

    // Welding must not move the surface, and must keep the attributes lined up
    // with the vertices they now describe.
    CHECK(mesh::signed_volume(back) == doctest::Approx(mesh::signed_volume(m)).epsilon(1e-4));
    CHECK(mesh::surface_area(back) == doctest::Approx(mesh::surface_area(m)).epsilon(1e-4));
    CHECK(back.colors.size() == back.positions.size());
    for (std::uint32_t i : back.indices) REQUIRE(i < back.positions.size());
}

TEST_CASE("import guardrails: budgets") {
    mesh::Mesh m = sample_mesh();
    io::ImportBudget tiny;
    tiny.max_vertices = 10;
    mesh::Mesh out;
    CHECK(io::load_obj(io::save_obj(m), &out, tiny).error == io::IoError::BudgetExceeded);
    std::vector<std::uint8_t> ply = io::save_ply(m);
    CHECK(io::load_ply(ply.data(), ply.size(), &out, tiny).error ==
          io::IoError::BudgetExceeded);
    std::vector<std::uint8_t> fbx = io::save_fbx(m);
    CHECK(io::load_fbx(fbx.data(), fbx.size(), &out, tiny).error ==
          io::IoError::BudgetExceeded);
}

TEST_CASE("import guardrails: PLY allocation bomb rejected before allocating") {
    // header declares 100M vertices with a 10-byte payload
    std::string bomb =
        "ply\nformat binary_little_endian 1.0\nelement vertex 100000000\n"
        "property float x\nproperty float y\nproperty float z\n"
        "element face 0\nproperty list uchar int vertex_indices\nend_header\n"
        "0123456789";
    mesh::Mesh out;
    io::IoStatus s = io::load_ply(reinterpret_cast<const std::uint8_t*>(bomb.data()),
                                  bomb.size(), &out, io::ImportBudget{});
    CHECK_FALSE(s.ok());
    CHECK(out.positions.capacity() < 1000000);  // no giant reservation happened
}

TEST_CASE("ply: a header with no trailing newline stays inside the buffer") {
    // The header scan stepped over the newline after "end_header" without
    // checking there was one, leaving header_end at size + 1: the header string
    // was built from one byte past the buffer (an ASan heap-buffer-overflow
    // READ), and the ascii body then wrapped size - header_end to SIZE_MAX and
    // aborted in std::string. A PLY with CR-only line endings, or one truncated
    // just past its header, is enough to reach it.
    for (const char* fmt : {"ascii", "binary_little_endian"}) {
        std::string s = std::string("ply\nformat ") + fmt + " 1.0\nend_headerX";
        mesh::Mesh out;
        // The assertion is that this returns at all, rather than reading out of
        // bounds or terminating; an empty declaration is legitimately an empty
        // mesh, so either status is acceptable.
        io::IoStatus r = io::load_ply(reinterpret_cast<const std::uint8_t*>(s.data()), s.size(),
                                      &out, io::ImportBudget{});
        CHECK(out.positions.empty());
        CHECK((r.ok() || r.error == io::IoError::Malformed));
    }
}

TEST_CASE("ply: a vertex element with no properties cannot fake a payload fit") {
    // vstride was zero, so "declared counts exceed payload" compared
    // vertex_count * 0 against the file size and passed for any count.
    std::string bomb =
        "ply\nformat ascii 1.0\nelement vertex 50000000\n"
        "element face 0\nproperty list uchar int vertex_indices\nend_header\n";
    mesh::Mesh out;
    io::IoStatus s = io::load_ply(reinterpret_cast<const std::uint8_t*>(bomb.data()),
                                  bomb.size(), &out, io::ImportBudget{});
    CHECK_FALSE(s.ok());
    CHECK(s.error == io::IoError::Malformed);
    CHECK(out.positions.capacity() < 1000000);
}

TEST_CASE("ply: the payload-fit guard covers ascii, not only binary") {
    // The guardrail was gated on `binary`, so an ascii header could declare
    // tens of millions of vertices behind a 160-byte file and get as far as
    // reserving for them.
    std::string over =
        "ply\nformat ascii 1.0\nelement vertex 40000000\n"
        "property float x\nproperty float y\nproperty float z\n"
        "element face 0\nproperty list uchar int vertex_indices\nend_header\n";
    mesh::Mesh out;
    io::IoStatus s = io::load_ply(reinterpret_cast<const std::uint8_t*>(over.data()),
                                  over.size(), &out, io::ImportBudget{});
    CHECK_FALSE(s.ok());
    CHECK(s.error == io::IoError::Malformed);
    CHECK(out.positions.capacity() < 1000000);
}

TEST_CASE("ply: a well-formed ascii file need not end with a newline") {
    // The ascii payload floor is exactly the cost of a newline-terminated
    // vertex line, so an exact comparison refuses a file that is one byte
    // under — and the PLY spec does not require the final line to be
    // terminated. A guard that rejects well-formed files is worse than the
    // over-declaration it was added to catch.
    for (int n : {1, 2, 3}) {
        std::string body;
        for (int i = 0; i < n; ++i) {
            body += std::to_string(i) + " 0 0";
            if (i + 1 < n) body += "\n";  // deliberately no trailing newline
        }
        std::string s = "ply\nformat ascii 1.0\nelement vertex " + std::to_string(n) +
                        "\nproperty float x\nproperty float y\nproperty float z\n"
                        "element face 0\nproperty list uchar int vertex_indices\nend_header\n" +
                        body;
        mesh::Mesh out;
        io::IoStatus r = io::load_ply(reinterpret_cast<const std::uint8_t*>(s.data()), s.size(),
                                      &out, io::ImportBudget{});
        INFO("vertex count " << n);
        CHECK(r.ok());
        CHECK(out.positions.size() == static_cast<std::size_t>(n));
    }
}

TEST_CASE("clayspace: the read ceiling is the caller's to raise") {
    // A document carrying sampled volumes is large by nature and nothing caps
    // what save_clayspace_file writes, so a fixed reader ceiling would make a
    // document this library had just written permanently unopenable.
    io::ClaySpaceDoc cs = sample_clayspace();
    const std::string path = "clay_budget_probe.clayspace";
    REQUIRE(io::save_clayspace_file(cs, path).ok());

    io::ImportBudget tiny;
    tiny.max_file_bytes = 16;
    io::ClaySpaceDoc back;
    io::IoStatus refused = io::load_clayspace_file(path, &back, tiny);
    CHECK_FALSE(refused.ok());
    CHECK(refused.error == io::IoError::BudgetExceeded);

    CHECK(io::load_clayspace_file(path, &back).ok());  // the default admits it
    std::filesystem::remove(path);
}

TEST_CASE("ply: an element this reader does not read is refused, not skipped") {
    // Only vertex and face are read, but every declared element has a payload
    // in the stream. One declared before vertex used to leave its bytes in
    // front of the vertex data, so every vertex was read from the wrong offset
    // and the mesh came back silently wrong.
    std::string with_camera =
        "ply\nformat ascii 1.0\n"
        "element camera 1\nproperty float view_px\n"
        "element vertex 1\nproperty float x\nproperty float y\nproperty float z\n"
        "element face 0\nproperty list uchar int vertex_indices\nend_header\n"
        "0.5\n1 2 3\n";
    mesh::Mesh out;
    io::IoStatus s = io::load_ply(reinterpret_cast<const std::uint8_t*>(with_camera.data()),
                                  with_camera.size(), &out, io::ImportBudget{});
    CHECK_FALSE(s.ok());
    CHECK(s.error == io::IoError::Unsupported);

    // a vertex+face file is still read, and an empty extra element is harmless
    std::string plain =
        "ply\nformat ascii 1.0\nelement vertex 3\n"
        "property float x\nproperty float y\nproperty float z\n"
        "element face 1\nproperty list uchar int vertex_indices\nend_header\n"
        "0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n";
    CHECK(io::load_ply(reinterpret_cast<const std::uint8_t*>(plain.data()), plain.size(), &out,
                       io::ImportBudget{})
              .ok());
    CHECK(out.positions.size() == 3);
    CHECK(out.triangle_count() == 1);
}

TEST_CASE("loaders refuse a path that is not a regular file") {
    // fopen("rb") succeeds on a directory and glibc then tells LONG_MAX rather
    // than failing, so every *_file loader sized a buffer from it and took the
    // process down with std::bad_alloc (the library builds -fno-exceptions).
    const std::string dir = "clay_not_a_file_dir";
    std::filesystem::create_directory(dir);

    mesh::Mesh m;
    for (auto load : {&io::load_ply_file, &io::load_obj_file, &io::load_fbx_file}) {
        io::IoStatus s = load(dir, &m, io::ImportBudget{});
        CHECK_FALSE(s.ok());
    }
    io::ClaySpaceDoc cs;
    CHECK_FALSE(io::load_clayspace_file(dir, &cs).ok());

    std::filesystem::remove(dir);
}

TEST_CASE("import fuzz: mutated files never crash the loaders") {
    mesh::Mesh m = sample_mesh();
    std::string obj = io::save_obj(m);
    std::vector<std::uint8_t> ply = io::save_ply(m);
    std::vector<std::uint8_t> fbx = io::save_fbx(m);
    clay_test::Lcg rng(902);
    mesh::Mesh out;
    for (int i = 0; i < 60; ++i) {
        auto mutate = [&](std::vector<std::uint8_t> data) {
            for (int k = 0; k < 8; ++k)
                data[static_cast<std::size_t>(rng.range(0.0f, 0.999f) *
                                              static_cast<float>(data.size()))] =
                    static_cast<std::uint8_t>(rng.range(0, 255.99f));
            if (i % 3 == 0) data.resize(data.size() / 2 + 1);
            return data;
        };
        std::vector<std::uint8_t> pm = mutate(ply);
        (void)io::load_ply(pm.data(), pm.size(), &out, io::ImportBudget{});
        std::vector<std::uint8_t> fm = mutate(fbx);
        (void)io::load_fbx(fm.data(), fm.size(), &out, io::ImportBudget{});
        std::vector<std::uint8_t> om = mutate({obj.begin(), obj.end()});
        (void)io::load_obj(std::string(om.begin(), om.end()), &out, io::ImportBudget{});
    }
    CHECK(true);  // surviving without a crash IS the assertion (ASan job hardens it)
}

TEST_CASE("platform buffer view exposes contiguous typed arrays") {
    mesh::Mesh m = sample_mesh();
    io::MeshBufferView v = io::buffer_view(m);
    REQUIRE(v.vertex_count == m.positions.size());
    REQUIRE(v.index_count == m.indices.size());
    CHECK(v.positions == &m.positions[0].x);
    CHECK(v.positions[4] == m.positions[1].y);  // stride-3 packing
    CHECK(v.colors != nullptr);
    CHECK(v.normals != nullptr);
    CHECK(v.indices == m.indices.data());
}

TEST_CASE("io: file round trips through disk") {
    mesh::Mesh m = sample_mesh();
    std::string base = "io_sample";
    REQUIRE(io::save_obj_file(m, base + ".obj").ok());
    REQUIRE(io::save_ply_file(m, base + ".ply").ok());
    REQUIRE(io::save_fbx_file(m, base + ".fbx").ok());
    io::ClaySpaceDoc cs = sample_clayspace();
    REQUIRE(io::save_clayspace_file(cs, base + ".clayspace").ok());

    mesh::Mesh back;
    CHECK(io::load_obj_file(base + ".obj", &back).ok());
    CHECK(io::load_ply_file(base + ".ply", &back).ok());
    CHECK(io::load_fbx_file(base + ".fbx", &back).ok());
    io::ClaySpaceDoc cs_back;
    CHECK(io::load_clayspace_file(base + ".clayspace", &cs_back).ok());
    CHECK_FALSE(io::load_obj_file("does_not_exist.obj", &back).ok());
}
