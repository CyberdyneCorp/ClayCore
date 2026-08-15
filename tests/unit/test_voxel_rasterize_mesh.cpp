// Triangles straight to cells (voxel-engine spec, rasterize-mesh).
//
// The claim worth testing is not "it fills cells" — it is that ONE sampling
// beats two. The four-step detour an importer had to take (triangles -> narrow
// band -> document -> cells) quantises a field that was itself quantised, so
// the tests below compare against that chain rather than against nothing, and
// the thin-feature case is the one where the difference is the point.

#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <optional>
#include <vector>

#include "clay/field/volume.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/to_field.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"
#include "clay/voxel/grid.h"
#include "scene_utils.h"

using namespace clay;
using namespace clay::kernel;
using clay_test::item;
using mesh::Mesh;
using voxel::VoxelGrid;

namespace {

// An axis-aligned box as twelve triangles, wound outward. Built by hand rather
// than meshed, so the fixture carries no meshing error into a test about
// sampling error.
Mesh box_mesh(cfloat3 lo, cfloat3 hi) {
    Mesh m;
    m.positions = {cf3(lo.x, lo.y, lo.z), cf3(hi.x, lo.y, lo.z), cf3(hi.x, hi.y, lo.z),
                   cf3(lo.x, hi.y, lo.z), cf3(lo.x, lo.y, hi.z), cf3(hi.x, lo.y, hi.z),
                   cf3(hi.x, hi.y, hi.z), cf3(lo.x, hi.y, hi.z)};
    const std::uint32_t faces[36] = {0, 3, 2, 0, 2, 1,   // -Z
                                     4, 5, 6, 4, 6, 7,   // +Z
                                     0, 1, 5, 0, 5, 4,   // -Y
                                     3, 7, 6, 3, 6, 2,   // +Y
                                     0, 4, 7, 0, 7, 3,   // -X
                                     1, 2, 6, 1, 6, 5};  // +X
    m.indices.assign(faces, faces + 36);
    return m;
}

// The same box with one face's triangles removed: dirty input, which is the
// input. A parity ray cast would flip a whole half-space on this.
Mesh holed_box(cfloat3 lo, cfloat3 hi) {
    Mesh m = box_mesh(lo, hi);
    m.indices.resize(m.indices.size() - 6);  // drop +X
    return m;
}

// The cell a world point falls in, by the same floor-division rasterize_mesh
// uses. VoxelGrid works in cell coordinates and leaves this to its callers.
voxel::VoxelCoord at(const VoxelGrid& grid, cfloat3 p) {
    const float vs = grid.voxel_size();
    return {static_cast<std::int32_t>(std::floor(p.x / vs)),
            static_cast<std::int32_t>(std::floor(p.y / vs)),
            static_cast<std::int32_t>(std::floor(p.z / vs))};
}

// The chain an importer had to take before this existed: triangles into a
// narrow band, the band into a document, the document rasterized.
VoxelGrid via_the_detour(const Mesh& m, float cell, const math::Aabb& region) {
    mesh::ImportSettings settings;
    settings.cell_size = cell;
    std::optional<field::FieldVolume> volume = mesh::to_field(m, settings);
    VoxelGrid grid(cell);
    if (!volume) return grid;
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("import");
    scene::Node n = item(scene::Prim::volume(), cf3(0, 0, 0), scene::Op::Add);
    n.volume = std::make_shared<field::FieldVolume>(std::move(*volume));
    l.sdf->insert(n);
    grid.rasterize_tape(scene::compile_document(doc), region);
    return grid;
}

}  // namespace

TEST_CASE("rasterize_mesh fills the solid the triangles bound") {
    const Mesh m = box_mesh(cf3(-0.3f, -0.2f, -0.25f), cf3(0.3f, 0.2f, 0.25f));
    VoxelGrid grid(0.02f);
    grid.rasterize_mesh(m);

    CHECK(grid.occupied_count() > 0);
    // Inside is set, outside is not, and the boundary is allowed one cell.
    CHECK(grid.get(at(grid, cf3(0, 0, 0))) != 0);
    CHECK(grid.get(at(grid, cf3(0.20f, 0.10f, 0.10f))) != 0);
    CHECK(grid.get(at(grid, cf3(0.5f, 0, 0))) == 0);
    CHECK(grid.get(at(grid, cf3(0, 0.5f, 0))) == 0);

    // The occupied volume is the box's, to within the surface's own half-cell.
    const float cell = 0.02f;
    const float measured = static_cast<float>(grid.occupied_count()) * cell * cell * cell;
    CHECK(measured == doctest::Approx(0.6f * 0.4f * 0.5f).epsilon(0.06));
}

TEST_CASE("rasterize_mesh defaults its region to the mesh's own bounds") {
    const Mesh m = box_mesh(cf3(-0.3f, -0.2f, -0.25f), cf3(0.3f, 0.2f, 0.25f));
    VoxelGrid unbounded(0.02f), bounded(0.02f);
    unbounded.rasterize_mesh(m);
    bounded.rasterize_mesh(m, math::Aabb{cf3(-0.4f, -0.4f, -0.4f), cf3(0.4f, 0.4f, 0.4f)});
    // A region that contains the mesh changes nothing; a mesh cannot be
    // unbounded, so the default exists where rasterize_tape's cannot.
    CHECK(unbounded.occupied_count() == bounded.occupied_count());
}

TEST_CASE("an explicit region bounds the work and says nothing about the rest") {
    const Mesh m = box_mesh(cf3(-0.3f, -0.2f, -0.25f), cf3(0.3f, 0.2f, 0.25f));
    VoxelGrid whole(0.02f), half(0.02f);
    whole.rasterize_mesh(m);
    half.rasterize_mesh(m, math::Aabb{cf3(0.0f, -0.4f, -0.4f), cf3(0.4f, 0.4f, 0.4f)});
    CHECK(half.occupied_count() > 0);
    CHECK(half.occupied_count() < whole.occupied_count());
    // The half outside the region is simply not rasterized.
    CHECK(half.get(at(half, cf3(-0.2f, 0, 0))) == 0);
    CHECK(half.get(at(half, cf3(0.2f, 0, 0))) != 0);
}

TEST_CASE("a hole does not flip a half-space") {
    // The whole point of the winding number over a parity ray cast. One missing
    // face and a parity count is wrong for everything behind it.
    const Mesh m = holed_box(cf3(-0.3f, -0.2f, -0.25f), cf3(0.3f, 0.2f, 0.25f));
    VoxelGrid grid(0.02f);
    grid.rasterize_mesh(m);

    CHECK(grid.get(at(grid, cf3(0, 0, 0))) != 0);          // still solid inside
    CHECK(grid.get(at(grid, cf3(-0.2f, 0, 0))) != 0);      // away from the hole
    CHECK(grid.get(at(grid, cf3(0.9f, 0, 0))) == 0);       // and not outside
    CHECK(grid.get(at(grid, cf3(0, 0, 0.9f))) == 0);

    // Most of the solid survives losing one face of six.
    VoxelGrid whole(0.02f);
    whole.rasterize_mesh(box_mesh(cf3(-0.3f, -0.2f, -0.25f), cf3(0.3f, 0.2f, 0.25f)));
    CHECK(static_cast<float>(grid.occupied_count()) >
          static_cast<float>(whole.occupied_count()) * 0.8f);
}

TEST_CASE("vertex colours reach the palette, and a colourless mesh takes one entry") {
    Mesh m = box_mesh(cf3(-0.3f, -0.3f, -0.3f), cf3(0.3f, 0.3f, 0.3f));
    // Red at -X, blue at +X, so the colour a cell takes depends on where it is
    // rather than on which vertex happened to be first.
    m.colors.resize(m.positions.size());
    for (std::size_t v = 0; v < m.positions.size(); ++v)
        m.colors[v] = m.positions[v].x < 0.0f ? cf3(1, 0, 0) : cf3(0, 0, 1);

    VoxelGrid grid(0.03f);
    grid.rasterize_mesh(m);
    const cfloat3 left = grid.palette_color(grid.get(at(grid, cf3(-0.25f, 0, 0))));
    const cfloat3 right = grid.palette_color(grid.get(at(grid, cf3(0.25f, 0, 0))));
    CHECK(left.x > left.z);   // reads red
    CHECK(right.z > right.x); // reads blue

    // With no colours there is nothing to read, and the grid says so with one
    // entry rather than inventing a colour per cell.
    VoxelGrid plain(0.03f);
    plain.rasterize_mesh(box_mesh(cf3(-0.3f, -0.3f, -0.3f), cf3(0.3f, 0.3f, 0.3f)));
    CHECK(plain.occupied_count() > 0);
    CHECK(plain.palette_size() == 2);  // index 0 is "empty", plus the one neutral
}

TEST_CASE("one sampling beats two: rasterize_mesh against the document detour") {
    // A THIN slab, thinner than two cells. The detour samples it into a band and
    // then samples the band, and the feature can fall between centres on the
    // second pass; one sampling asks the triangles directly.
    const float cell = 0.02f;
    const math::Aabb region{cf3(-0.5f, -0.5f, -0.5f), cf3(0.5f, 0.5f, 0.5f)};
    const Mesh slab = box_mesh(cf3(-0.30f, -0.014f, -0.30f), cf3(0.30f, 0.014f, 0.30f));

    VoxelGrid direct(cell);
    direct.rasterize_mesh(slab, region);
    const VoxelGrid detoured = via_the_detour(slab, cell, region);

    CHECK(direct.occupied_count() > 0);
    // The direct path keeps the slab; whatever the detour does, it cannot be
    // BETTER, and the test states the ordering rather than a magic number.
    CHECK(direct.occupied_count() >= detoured.occupied_count());

    // And on a thick shape the two agree to within about a cell of surface.
    const Mesh box = box_mesh(cf3(-0.25f, -0.25f, -0.25f), cf3(0.25f, 0.25f, 0.25f));
    VoxelGrid thick_direct(cell);
    thick_direct.rasterize_mesh(box, region);
    const VoxelGrid thick_detour = via_the_detour(box, cell, region);
    const float a = static_cast<float>(thick_direct.occupied_count());
    const float b = static_cast<float>(thick_detour.occupied_count());
    CHECK(std::fabs(a - b) / std::max(a, b) < 0.10f);
}

TEST_CASE("rasterize_mesh refuses what it cannot mean, and changes nothing") {
    VoxelGrid grid(0.02f);
    const Mesh empty;
    grid.rasterize_mesh(empty);
    CHECK(grid.occupied_count() == 0);

    const Mesh m = box_mesh(cf3(-0.2f, -0.2f, -0.2f), cf3(0.2f, 0.2f, 0.2f));
    grid.rasterize_mesh(m, math::Aabb{});  // an empty region
    CHECK(grid.occupied_count() == 0);

    // A mesh whose every index is out of range has no triangles to bound
    // anything with, and is not an error.
    Mesh bad = m;
    for (std::uint32_t& i : bad.indices) i += 1000;
    grid.rasterize_mesh(bad);
    CHECK(grid.occupied_count() == 0);
}

TEST_CASE("rasterize_mesh reports its writes like every other verb") {
    const Mesh m = box_mesh(cf3(-0.2f, -0.2f, -0.2f), cf3(0.2f, 0.2f, 0.2f));
    VoxelGrid grid(0.02f);
    const std::size_t before = grid.change_count();
    grid.rasterize_mesh(m);
    CHECK(grid.change_count() > before);
    // Rasterizing the same mesh again writes the same cells to the same
    // values, so nothing CHANGES the second time.
    const std::size_t after = grid.change_count();
    grid.rasterize_mesh(m);
    CHECK(grid.change_count() == after);
}

TEST_CASE("rasterizing is deterministic, palette indices included") {
    // Both rasterize paths evaluate in parallel and write serially. What a
    // race would show up as is an answer that VARIES — and here it would vary
    // in the palette too, because `palette_add` assigns indices in first-seen
    // order and an index is what the grid stores. So the check is the whole
    // serialised document, eight times, not just the occupancy.
    Mesh m = box_mesh(cf3(-0.3f, -0.3f, -0.3f), cf3(0.3f, 0.3f, 0.3f));
    m.colors.resize(m.positions.size());
    for (std::size_t v = 0; v < m.positions.size(); ++v)
        m.colors[v] = m.positions[v].x < 0.0f ? cf3(1, 0, 0)
                                              : (m.positions[v].y < 0.0f ? cf3(0, 1, 0)
                                                                        : cf3(0, 0, 1));

    std::vector<std::uint8_t> first;
    for (int run = 0; run < 8; ++run) {
        VoxelGrid g(0.01f);  // fine enough that the region spans many planes
        g.rasterize_mesh(m);
        const std::vector<std::uint8_t> bytes = g.serialize();
        if (run == 0)
            first = bytes;
        else {
            CAPTURE(run);
            CHECK(bytes == first);
        }
    }
    REQUIRE(!first.empty());

    // ...and the same for a tape, whose colours also reach the palette.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node a = item(scene::Prim::sphere(0.4f), cf3(-0.15f, 0, 0));
    a.color = cf3(0.9f, 0.2f, 0.1f);
    l.sdf->insert(a);
    scene::Node b = item(scene::Prim::sphere(0.4f), cf3(0.15f, 0, 0));
    b.color = cf3(0.1f, 0.3f, 0.9f);
    b.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.1f};
    l.sdf->insert(b);
    const scene::Tape tape = scene::compile_document(doc);
    const math::Aabb region{cf3(-0.7f, -0.5f, -0.5f), cf3(0.7f, 0.5f, 0.5f)};

    std::vector<std::uint8_t> tape_first;
    for (int run = 0; run < 8; ++run) {
        VoxelGrid g(0.01f);
        g.rasterize_tape(tape, region);
        const std::vector<std::uint8_t> bytes = g.serialize();
        if (run == 0)
            tape_first = bytes;
        else {
            CAPTURE(run);
            CHECK(bytes == tape_first);
        }
    }
    REQUIRE(!tape_first.empty());
    // Both colours made it, so the palette really was exercised.
    VoxelGrid probe(0.01f);
    probe.rasterize_tape(tape, region);
    CHECK(probe.palette_size() >= 3);  // empty + two
}

TEST_CASE("a rasterize spanning many waves is the same as one spanning few") {
    // The work is done in WAVES of planes so the pending list stays bounded.
    // A wave boundary must not be visible in the result: the same region
    // rasterized at a cell size that makes it one wave and at one that makes
    // it many has to agree about the solid it produces.
    const Mesh m = box_mesh(cf3(-0.25f, -0.25f, -0.25f), cf3(0.25f, 0.25f, 0.25f));
    VoxelGrid coarse(0.05f);   // ~10 planes: one wave
    VoxelGrid fine(0.01f);     // ~50 planes: several
    coarse.rasterize_mesh(m);
    fine.rasterize_mesh(m);

    // Volume agrees to within the surface's own half-cell on each.
    const float cv = static_cast<float>(coarse.occupied_count()) * 0.05f * 0.05f * 0.05f;
    const float fv = static_cast<float>(fine.occupied_count()) * 0.01f * 0.01f * 0.01f;
    CHECK(cv == doctest::Approx(0.5f * 0.5f * 0.5f).epsilon(0.15));
    CHECK(fv == doctest::Approx(0.5f * 0.5f * 0.5f).epsilon(0.05));
}
