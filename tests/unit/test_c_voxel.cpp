#include <doctest/doctest.h>

#include <cmath>

#include <cstdio>

#include <cstring>
#include <limits>
#include <vector>

#include "clay.h"
#include "clay/io/clayspace.h"
#include "clay/voxel/grid.h"

// The C ABI voxel surface (c-abi spec: voxel grids across the ABI). Each case
// performs the same edit twice — once through the C boundary, once on
// voxel::VoxelGrid the way the Python bindings do — and requires the two grids
// to agree cell for cell. That is the parity standard the spec states: the
// falloff dither in particular has to land on the same cells through both, or
// a soft brush means something different depending on which binding drove it.

using namespace clay;

namespace {

// A caller-owned grid with the destroy the spec requires of it.
struct CGrid {
    clay_voxel_grid* grid = nullptr;
    explicit CGrid(float voxel_size = 0.1f) : grid(clay_voxel_grid_create(voxel_size)) {
        REQUIRE(grid != nullptr);
    }
    ~CGrid() { clay_voxel_grid_destroy(grid); }
    CGrid(const CGrid&) = delete;
    CGrid& operator=(const CGrid&) = delete;
};

clay_brush_params brush(std::int32_t size, std::int32_t shape, std::int32_t falloff,
                        float strength, std::uint32_t seed) {
    clay_brush_params b;
    std::memset(&b, 0, sizeof b);
    b.struct_size = static_cast<std::uint32_t>(sizeof b);
    b.size = size;
    b.shape = shape;
    b.falloff = falloff;
    b.strength = strength;
    b.seed = seed;
    return b;
}

voxel::BrushParams ref_brush(const clay_brush_params& b) {
    voxel::BrushParams p;
    p.size = b.size;
    p.shape = static_cast<voxel::BrushShape>(b.shape);
    p.falloff = static_cast<voxel::BrushFalloff>(b.falloff);
    p.strength = b.strength;
    p.seed = b.seed;
    return p;
}

// Cell-for-cell comparison over the union of the two grids' bounds, so a cell
// one of them set and the other did not is a divergence rather than a cell
// neither iterates. Reports the first few divergences with their coordinates.
int compare_grids(const clay_voxel_grid* got, const voxel::VoxelGrid& want) {
    std::int32_t lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    std::int32_t has_bounds = 0;
    REQUIRE(clay_voxel_bounds(got, lo, hi, &has_bounds) == CLAY_OK);
    std::optional<voxel::VoxelCoord> ref_lo = want.bounds_min();
    std::optional<voxel::VoxelCoord> ref_hi = want.bounds_max();
    CHECK((has_bounds != 0) == ref_lo.has_value());
    if (!has_bounds || !ref_lo) return 0;
    voxel::VoxelCoord a{std::min(lo[0], ref_lo->x), std::min(lo[1], ref_lo->y),
                        std::min(lo[2], ref_lo->z)};
    voxel::VoxelCoord b{std::max(hi[0], ref_hi->x), std::max(hi[1], ref_hi->y),
                        std::max(hi[2], ref_hi->z)};
    int diverged = 0;
    for (std::int32_t z = a.z; z <= b.z; ++z)
        for (std::int32_t y = a.y; y <= b.y; ++y)
            for (std::int32_t x = a.x; x <= b.x; ++x) {
                const std::int32_t cell[3] = {x, y, z};
                std::int32_t through_c = -1;
                REQUIRE(clay_voxel_get(got, cell, &through_c) == CLAY_OK);
                std::uint8_t reference = want.get({x, y, z});
                if (through_c == reference) continue;
                if (++diverged <= 5)
                    FAIL_CHECK("cell (" << x << ", " << y << ", " << z << ") reads "
                                        << through_c << " through C and "
                                        << static_cast<int>(reference) << " on the engine");
            }
    return diverged;
}

void check_same_grid(const clay_voxel_grid* got, const voxel::VoxelGrid& want) {
    std::size_t occupied = 0;
    REQUIRE(clay_voxel_occupied_count(got, &occupied) == CLAY_OK);
    CHECK(occupied == want.occupied_count());
    int diverged = compare_grids(got, want);
    INFO("diverging cells: " << diverged);
    CHECK(diverged == 0);
}

// Every buffer of a greedy mesh, not only how many vertices came out: a mesh
// that merged the same quads but coloured, oriented or wound them differently
// is a divergence a count comparison waves through. Reports the first few.
int compare_meshes(const clay_mesh* got, const mesh::Mesh& want) {
    CHECK(clay_mesh_vertex_count(got) == want.positions.size());
    CHECK(clay_mesh_index_count(got) == want.indices.size());
    if (clay_mesh_vertex_count(got) != want.positions.size()) return 1;
    const float* buffers[3] = {clay_mesh_positions(got), clay_mesh_normals(got),
                               clay_mesh_colors(got)};
    const std::vector<kernel::cfloat3>* reference[3] = {&want.positions, &want.normals,
                                                        &want.colors};
    const char* names[3] = {"position", "normal", "color"};
    int diverged = 0;
    for (int b = 0; b < 3; ++b) {
        REQUIRE((buffers[b] != nullptr) == !reference[b]->empty());
        if (!buffers[b]) continue;
        for (std::size_t i = 0; i < reference[b]->size(); ++i) {
            const kernel::cfloat3& v = (*reference[b])[i];
            const float* p = buffers[b] + i * 3;
            if (p[0] == v.x && p[1] == v.y && p[2] == v.z) continue;
            if (++diverged <= 5)
                FAIL_CHECK("vertex " << i << " " << names[b] << " is (" << p[0] << ", " << p[1]
                                     << ", " << p[2] << ") through C and (" << v.x << ", " << v.y
                                     << ", " << v.z << ") on the engine");
        }
    }
    const std::uint32_t* indices = clay_mesh_indices(got);
    REQUIRE(indices != nullptr);
    for (std::size_t i = 0; i < want.indices.size(); ++i)
        if (indices[i] != want.indices[i] && ++diverged <= 5)
            FAIL_CHECK("index " << i << " is " << indices[i] << " through C and "
                                << want.indices[i] << " on the engine");
    return diverged;
}

const std::int32_t kOrigin[3] = {0, 0, 0};

}  // namespace

TEST_CASE("a C brush stamp fills the cells the engine fills") {
    CGrid c(0.1f);
    voxel::VoxelGrid ref(0.1f);
    const float white[3] = {1.0f, 1.0f, 1.0f};
    std::int32_t index = 0;
    REQUIRE(clay_voxel_palette_add(c.grid, white, &index) == CLAY_OK);
    std::uint8_t ref_index = ref.palette_add(kernel::cf3(1, 1, 1));
    REQUIRE(index == ref_index);

    clay_brush_params b = brush(6, CLAY_BRUSH_SHAPE_SPHERE, CLAY_BRUSH_FALLOFF_CONSTANT, 1.0f, 0);
    REQUIRE(clay_voxel_set_brush(c.grid, kOrigin, &b, index) == CLAY_OK);
    ref.set_brush({0, 0, 0}, ref_brush(b), ref_index);
    check_same_grid(c.grid, ref);
}

TEST_CASE("a falloff brush dithers onto the same cells through both bindings") {
    for (std::int32_t falloff : {CLAY_BRUSH_FALLOFF_LINEAR, CLAY_BRUSH_FALLOFF_SMOOTH,
                                 CLAY_BRUSH_FALLOFF_GAUSSIAN}) {
        CGrid c(0.1f);
        voxel::VoxelGrid ref(0.1f);
        const float grey[3] = {0.5f, 0.5f, 0.5f};
        std::int32_t index = 0;
        REQUIRE(clay_voxel_palette_add(c.grid, grey, &index) == CLAY_OK);
        std::uint8_t ref_index = ref.palette_add(kernel::cf3(0.5f, 0.5f, 0.5f));

        clay_brush_params b = brush(9, CLAY_BRUSH_SHAPE_SPHERE, falloff, 0.7f, 20250804u);
        const std::int32_t centre[3] = {3, -2, 5};
        REQUIRE(clay_voxel_set_brush(c.grid, centre, &b, index) == CLAY_OK);
        ref.set_brush({3, -2, 5}, ref_brush(b), ref_index);
        // A soft stamp must actually be soft, or the case would pass on a
        // brush that never dithered at all.
        std::size_t occupied = 0;
        REQUIRE(clay_voxel_occupied_count(c.grid, &occupied) == CLAY_OK);
        CHECK(occupied > 0);
        CHECK(occupied < 9u * 9u * 9u);
        check_same_grid(c.grid, ref);
    }
}

TEST_CASE("the sculpting verbs reshape what the engine reshapes") {
    clay_brush_params stamp =
        brush(9, CLAY_BRUSH_SHAPE_SPHERE, CLAY_BRUSH_FALLOFF_CONSTANT, 1.0f, 0);
    // The verb footprint has to REACH THE SURFACE. At size 7 inside a size-9
    // solid stamp it sat entirely in the interior, where a majority filter and
    // a dilation both have nothing to do: smooth, inflate and pinch changed
    // zero cells and the parity comparison below held for any implementation.
    clay_brush_params verb =
        brush(11, CLAY_BRUSH_SHAPE_SPHERE, CLAY_BRUSH_FALLOFF_CONSTANT, 1.0f, 0);
    const float normal[3] = {0.0f, 1.0f, 0.0f};

    CGrid c(0.1f);
    voxel::VoxelGrid ref(0.1f);
    const float clay_color[3] = {0.8f, 0.4f, 0.2f};
    std::int32_t index = 0;
    REQUIRE(clay_voxel_palette_add(c.grid, clay_color, &index) == CLAY_OK);
    std::uint8_t ref_index = ref.palette_add(kernel::cf3(0.8f, 0.4f, 0.2f));
    REQUIRE(clay_voxel_set_brush(c.grid, kOrigin, &stamp, index) == CLAY_OK);
    ref.set_brush({0, 0, 0}, ref_brush(stamp), ref_index);
    // A spur for smooth to dissolve, so the majority filter has a decision to
    // make rather than a uniform interior to agree with.
    const std::int32_t spur[3] = {6, 0, 0};
    REQUIRE(clay_voxel_set(c.grid, spur, index) == CLAY_OK);
    ref.set({6, 0, 0}, ref_index);

    const std::vector<std::uint8_t> before = ref.serialize();

    SUBCASE("smooth") {
        REQUIRE(clay_voxel_sculpt_smooth(c.grid, kOrigin, &verb) == CLAY_OK);
        ref.sculpt_smooth({0, 0, 0}, ref_brush(verb));
    }
    SUBCASE("inflate grows for a positive amount") {
        REQUIRE(clay_voxel_sculpt_inflate(c.grid, kOrigin, &verb, 2) == CLAY_OK);
        ref.sculpt_inflate({0, 0, 0}, ref_brush(verb), 2);
    }
    SUBCASE("inflate erodes for a negative amount") {
        REQUIRE(clay_voxel_sculpt_inflate(c.grid, kOrigin, &verb, -2) == CLAY_OK);
        ref.sculpt_inflate({0, 0, 0}, ref_brush(verb), -2);
    }
    SUBCASE("flatten measures its offset in cells") {
        REQUIRE(clay_voxel_sculpt_flatten(c.grid, kOrigin, &verb, normal, 1.5f) == CLAY_OK);
        ref.sculpt_flatten({0, 0, 0}, ref_brush(verb), kernel::cf3(0, 1, 0), 1.5f);
    }
    SUBCASE("pinch") {
        REQUIRE(clay_voxel_sculpt_pinch(c.grid, kOrigin, &verb) == CLAY_OK);
        ref.sculpt_pinch({0, 0, 0}, ref_brush(verb));
    }
    // Every verb must have DONE something, or the parity check below is
    // comparing two untouched grids and would pass whatever the verb does.
    // Compared on the whole grid rather than the occupied COUNT: a verb that
    // moves material without adding any (magnify, grab) leaves the count alone.
    CHECK(ref.serialize() != before);
    check_same_grid(c.grid, ref);
}

TEST_CASE("magnify and grab cross the ABI like the other verbs") {
    // These two were the only ABI sculpt verbs with no test at all: magnify is
    // pinch's inverse and shares its walk, and grab drives the same map the SDF
    // grab deformer uses, so a drift in either is invisible without parity.
    clay_brush_params stamp =
        brush(9, CLAY_BRUSH_SHAPE_SPHERE, CLAY_BRUSH_FALLOFF_CONSTANT, 1.0f, 0);
    clay_brush_params verb =
        brush(11, CLAY_BRUSH_SHAPE_SPHERE, CLAY_BRUSH_FALLOFF_CONSTANT, 1.0f, 0);

    CGrid c(0.1f);
    voxel::VoxelGrid ref(0.1f);
    const float clay_color[3] = {0.8f, 0.4f, 0.2f};
    std::int32_t index = 0;
    REQUIRE(clay_voxel_palette_add(c.grid, clay_color, &index) == CLAY_OK);
    std::uint8_t ref_index = ref.palette_add(kernel::cf3(0.8f, 0.4f, 0.2f));
    REQUIRE(clay_voxel_set_brush(c.grid, kOrigin, &stamp, index) == CLAY_OK);
    ref.set_brush({0, 0, 0}, ref_brush(stamp), ref_index);
    const std::vector<std::uint8_t> before = ref.serialize();

    SUBCASE("magnify pushes the surface out") {
        REQUIRE(clay_voxel_sculpt_magnify(c.grid, kOrigin, &verb) == CLAY_OK);
        ref.sculpt_magnify({0, 0, 0}, ref_brush(verb));
        CHECK(ref.serialize() != before);
    }
    SUBCASE("grab translates occupancy") {
        const float displacement[3] = {0.25f, 0.0f, 0.0f};
        REQUIRE(clay_voxel_sculpt_grab(c.grid, kOrigin, &verb, displacement, 0) == CLAY_OK);
        ref.sculpt_grab({0, 0, 0}, ref_brush(verb), kernel::cf3(0.25f, 0, 0), false);
        CHECK(ref.serialize() != before);
    }
    SUBCASE("grab front_only is not the same edit") {
        const float displacement[3] = {0.25f, 0.0f, 0.0f};
        REQUIRE(clay_voxel_sculpt_grab(c.grid, kOrigin, &verb, displacement, 1) == CLAY_OK);
        ref.sculpt_grab({0, 0, 0}, ref_brush(verb), kernel::cf3(0.25f, 0, 0), true);
    }
    SUBCASE("a sub-cell drag succeeds and reports that it moved nothing") {
        // The dead zone the header documents. Rounding is per axis, so 0.02 at
        // a voxel size of 0.1 is a fifth of a cell on each of the three and
        // resolves every cell to itself. CLAY_OK either way, which is why the
        // count is the only thing that separates the two calls below.
        std::uint64_t start = 0, after_dead = 0, after_live = 0;
        REQUIRE(clay_voxel_change_count(c.grid, &start) == CLAY_OK);
        CHECK(start == ref.change_count());

        const float sub_cell[3] = {0.02f, 0.02f, 0.02f};
        REQUIRE(clay_voxel_sculpt_grab(c.grid, kOrigin, &verb, sub_cell, 0) == CLAY_OK);
        ref.sculpt_grab({0, 0, 0}, ref_brush(verb), kernel::cf3(0.02f, 0.02f, 0.02f), false);
        REQUIRE(clay_voxel_change_count(c.grid, &after_dead) == CLAY_OK);
        CHECK(after_dead == start);
        CHECK(ref.serialize() == before);

        const float whole_cells[3] = {0.25f, 0.0f, 0.0f};
        REQUIRE(clay_voxel_sculpt_grab(c.grid, kOrigin, &verb, whole_cells, 0) == CLAY_OK);
        ref.sculpt_grab({0, 0, 0}, ref_brush(verb), kernel::cf3(0.25f, 0, 0), false);
        REQUIRE(clay_voxel_change_count(c.grid, &after_live) == CLAY_OK);
        CHECK(after_live > start);
        CHECK(after_live == ref.change_count());
    }
    SUBCASE("refusals") {
        const float displacement[3] = {0.25f, 0.0f, 0.0f};
        std::uint64_t changes = 0;
        CHECK(clay_voxel_change_count(nullptr, &changes) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_voxel_change_count(c.grid, nullptr) == CLAY_OK);
        CHECK(clay_voxel_sculpt_magnify(nullptr, kOrigin, &verb) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_voxel_sculpt_magnify(c.grid, kOrigin, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_voxel_sculpt_grab(nullptr, kOrigin, &verb, displacement, 0) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_voxel_sculpt_grab(c.grid, kOrigin, &verb, nullptr, 0) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        return;  // the refusals changed nothing, so there is nothing to compare
    }
    check_same_grid(c.grid, ref);
}

TEST_CASE("fills, batches and mirrored edits land where the engine puts them") {
    CGrid c(0.1f);
    voxel::VoxelGrid ref(0.1f);
    const float red[3] = {1.0f, 0.0f, 0.0f};
    std::int32_t index = 0;
    REQUIRE(clay_voxel_palette_add(c.grid, red, &index) == CLAY_OK);
    std::uint8_t ref_index = ref.palette_add(kernel::cf3(1, 0, 0));

    const std::int32_t a[3] = {-3, 0, -3}, b[3] = {3, 0, 3};
    REQUIRE(clay_voxel_fill_box(c.grid, a, b, index) == CLAY_OK);
    ref.fill_box({-3, 0, -3}, {3, 0, 3}, ref_index);

    const std::int32_t from[3] = {-6, 4, -2}, to[3] = {6, 9, 5};
    REQUIRE(clay_voxel_fill_line(c.grid, from, to, index) == CLAY_OK);
    ref.fill_line({-6, 4, -2}, {6, 9, 5}, ref_index);

    const std::int32_t cells[9] = {8, 8, 8, 9, 8, 8, 10, 8, 8};
    REQUIRE(clay_voxel_set_many(c.grid, cells, 3, index) == CLAY_OK);
    for (int i = 0; i < 3; ++i) ref.set({cells[i * 3], cells[i * 3 + 1], cells[i * 3 + 2]}, ref_index);
    REQUIRE(clay_voxel_erase_many(c.grid, cells + 6, 1) == CLAY_OK);
    ref.erase({10, 8, 8});

    const std::int32_t mirrored[3] = {2, 3, 4};
    REQUIRE(clay_voxel_set_mirrored(c.grid, mirrored, index,
                                    CLAY_MIRROR_X | CLAY_MIRROR_Z) == CLAY_OK);
    ref.set_mirrored({2, 3, 4}, ref_index, voxel::kVoxMirrorX | voxel::kVoxMirrorZ);

    check_same_grid(c.grid, ref);
}

TEST_CASE("a flood selection crosses the boundary in the order the engine found it") {
    CGrid c(0.1f);
    voxel::VoxelGrid ref(0.1f);
    clay_brush_params b = brush(5, CLAY_BRUSH_SHAPE_CUBE, CLAY_BRUSH_FALLOFF_CONSTANT, 1.0f, 0);
    REQUIRE(clay_voxel_set_brush(c.grid, kOrigin, &b, 1) == CLAY_OK);
    ref.set_brush({0, 0, 0}, ref_brush(b), 1);

    std::vector<voxel::VoxelCoord> want = ref.flood_select({0, 0, 0}, true);
    std::size_t count = 0;
    REQUIRE(clay_voxel_flood_select(c.grid, kOrigin, 1, nullptr, &count) == CLAY_OK);
    REQUIRE(count == want.size());

    std::vector<std::int32_t> got(count * 3, 0);
    std::size_t capacity = count;
    REQUIRE(clay_voxel_flood_select(c.grid, kOrigin, 1, got.data(), &capacity) == CLAY_OK);
    REQUIRE(capacity == count);
    for (std::size_t i = 0; i < count; ++i)
        REQUIRE(voxel::VoxelCoord{got[i * 3], got[i * 3 + 1], got[i * 3 + 2]} == want[i]);

    // A buffer one cell short reports what it needs and writes nothing.
    std::vector<std::int32_t> guarded(count * 3, -777);
    capacity = count - 1;
    REQUIRE(clay_voxel_flood_select(c.grid, kOrigin, 1, guarded.data(), &capacity) ==
            CLAY_ERROR_BUFFER_TOO_SMALL);
    CHECK(capacity == count);
    CHECK(guarded[0] == -777);
}

TEST_CASE("the greedy mesh and the step field are the engine's own") {
    CGrid c(0.2f);
    voxel::VoxelGrid ref(0.2f);
    const float green[3] = {0.0f, 1.0f, 0.0f};
    std::int32_t index = 0;
    REQUIRE(clay_voxel_palette_add(c.grid, green, &index) == CLAY_OK);
    std::uint8_t ref_index = ref.palette_add(kernel::cf3(0, 1, 0));
    clay_brush_params b = brush(4, CLAY_BRUSH_SHAPE_CUBE, CLAY_BRUSH_FALLOFF_CONSTANT, 1.0f, 0);
    REQUIRE(clay_voxel_set_brush(c.grid, kOrigin, &b, index) == CLAY_OK);
    ref.set_brush({0, 0, 0}, ref_brush(b), ref_index);

    clay_mesh* got = nullptr;
    REQUIRE(clay_voxel_mesh(c.grid, &got) == CLAY_OK);
    mesh::Mesh want = ref.mesh_greedy();
    CHECK(clay_mesh_vertex_count(got) == want.positions.size());
    CHECK(clay_mesh_index_count(got) == want.indices.size());
    const float* positions = clay_mesh_positions(got);
    REQUIRE(positions != nullptr);
    for (std::size_t i = 0; i < want.positions.size(); ++i) {
        CHECK(positions[i * 3 + 0] == want.positions[i].x);
        CHECK(positions[i * 3 + 1] == want.positions[i].y);
        CHECK(positions[i * 3 + 2] == want.positions[i].z);
    }
    clay_mesh_destroy(got);

    const float points[6] = {0.1f, 0.1f, 0.1f, 9.0f, 9.0f, 9.0f};
    float values[2] = {0.0f, 0.0f};
    REQUIRE(clay_voxel_sample_step_field(c.grid, points, 2, values) == CLAY_OK);
    CHECK(values[0] == ref.sample_step_field(kernel::cf3(0.1f, 0.1f, 0.1f)));
    CHECK(values[1] == ref.sample_step_field(kernel::cf3(9.0f, 9.0f, 9.0f)));
}

TEST_CASE("an empty grid meshes to a mesh whose every buffer is absent") {
    // clay_voxel_mesh is the only call that hands back a mesh with nothing in
    // it, so it is the only way to reach an accessor with both vectors empty.
    // Taking &v[0].x there is undefined even though nobody dereferences the
    // result, which the sanitizer preset now fails on rather than logs.
    CGrid c(0.1f);
    clay_mesh* empty = nullptr;
    REQUIRE(clay_voxel_mesh(c.grid, &empty) == CLAY_OK);
    REQUIRE(empty != nullptr);
    CHECK(clay_mesh_vertex_count(empty) == 0);
    CHECK(clay_mesh_index_count(empty) == 0);
    CHECK(clay_mesh_positions(empty) == nullptr);
    CHECK(clay_mesh_normals(empty) == nullptr);
    CHECK(clay_mesh_colors(empty) == nullptr);
    CHECK(clay_mesh_indices(empty) == nullptr);
    clay_mesh_destroy(empty);
}

TEST_CASE("a borrowed layer handle is the document's, not the caller's") {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    clay_voxel_grid* grid = nullptr;
    REQUIRE(clay_document_add_voxel_layer(doc, "sculpt", 0.05f, &layer, &grid) == CLAY_OK);
    REQUIRE(grid != nullptr);

    // Refused, and refused without freeing anything: the document keeps
    // working through the same handle afterwards.
    REQUIRE(clay_voxel_grid_destroy(grid) == CLAY_ERROR_INVALID_ARGUMENT);
    const std::int32_t cell[3] = {4, 5, 6};
    REQUIRE(clay_voxel_set(grid, cell, 3) == CLAY_OK);
    std::int32_t read = 0;
    REQUIRE(clay_voxel_get(grid, cell, &read) == CLAY_OK);
    CHECK(read == 3);

    // The edit is the document's, so a second borrow of the same layer sees
    // it and so does anything that walks the document.
    clay_voxel_grid* again = nullptr;
    REQUIRE(clay_document_voxel_layer(doc, "sculpt", nullptr, &again) == CLAY_OK);
    CHECK(again == grid);
    float voxel_size = 0.0f;
    REQUIRE(clay_voxel_size(again, &voxel_size) == CLAY_OK);
    CHECK(voxel_size == 0.05f);

    clay_document_destroy(doc);
}

TEST_CASE("a document without the layer reports not-found, and borrows do not cross") {
    // Named for what it checks. The header used to promise that a borrowed
    // handle survives its layer being removed; nothing in this ABI removes a
    // layer, so that promise was untestable and the branch behind it
    // unreachable. What is reachable, and what a host actually does, is look a
    // layer up on the wrong document — including one loaded from a file.
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    clay_voxel_grid* grid = nullptr;
    REQUIRE(clay_document_add_voxel_layer(doc, "sculpt", 0.05f, &layer, &grid) == CLAY_OK);
    const std::int32_t cell[3] = {0, 0, 0};
    REQUIRE(clay_voxel_set(grid, cell, 1) == CLAY_OK);

    clay_document* fresh = clay_document_create();
    REQUIRE(clay_document_save(fresh, "c_voxel_stale.clayspace") == CLAY_OK);
    clay_document_destroy(fresh);
    clay_document* reloaded = nullptr;
    REQUIRE(clay_document_load("c_voxel_stale.clayspace", &reloaded) == CLAY_OK);
    clay_voxel_grid* missing = grid;
    CHECK(clay_document_voxel_layer(reloaded, "sculpt", nullptr, &missing) ==
          CLAY_ERROR_NOT_FOUND);
    CHECK(missing == grid);  // a failed lookup writes nothing
    std::remove("c_voxel_stale.clayspace");  // tests leave no artifacts behind

    // The first document is untouched by any of that: its handle still
    // resolves to its own layer, with the edit that was made through it.
    std::int32_t read = 0;
    REQUIRE(clay_voxel_get(grid, cell, &read) == CLAY_OK);
    CHECK(read == 1);
    clay_document_destroy(reloaded);
    clay_document_destroy(doc);
}

TEST_CASE("the brush descriptor reads only the prefix the caller declared") {
    CGrid c(0.1f);
    clay_brush_params b = brush(3, CLAY_BRUSH_SHAPE_CUBE, CLAY_BRUSH_FALLOFF_CONSTANT, 1.0f, 0);

    // A caller compiled against a later header: the tail this build cannot
    // name is ignored rather than read as a field it knows.
    struct Grown {
        clay_brush_params brush;
        float appended[2];
    };
    Grown grown{};
    grown.brush = b;
    grown.brush.struct_size = static_cast<std::uint32_t>(sizeof grown);
    grown.appended[0] = 1234.5f;
    grown.appended[1] = -1.0f;
    REQUIRE(clay_voxel_set_brush(c.grid, kOrigin, &grown.brush, 1) == CLAY_OK);
    std::size_t occupied = 0;
    REQUIRE(clay_voxel_occupied_count(c.grid, &occupied) == CLAY_OK);
    CHECK(occupied == 27);

    // Below the original layout, and absurdly above it: both rejected rather
    // than read one slot off a shorter object. The short one lives on the heap
    // so a misread is an overflow the sanitizer catches.
    std::vector<unsigned char> storage(sizeof(clay_brush_params), 0);
    auto* truncated = reinterpret_cast<clay_brush_params*>(storage.data());
    *truncated = b;
    truncated->struct_size = 4;
    CHECK(clay_voxel_set_brush(c.grid, kOrigin, truncated, 1) == CLAY_ERROR_INVALID_ARGUMENT);
    truncated->struct_size = 0x3D23D70A;  // the bits of 0.04f, not a struct size
    CHECK(clay_voxel_set_brush(c.grid, kOrigin, truncated, 1) == CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("the brush rejects what the Python bindings reject, in the same order") {
    CGrid c(0.1f);
    clay_brush_params b = brush(3, CLAY_BRUSH_SHAPE_CUBE, CLAY_BRUSH_FALLOFF_CONSTANT, 1.0f, 0);

    clay_brush_params bad = b;
    bad.size = 0;
    CHECK(clay_voxel_set_brush(c.grid, kOrigin, &bad, 1) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(std::string(clay_last_error()) == "brush size must be > 0");

    // A bad size wins over a bad shape, which wins over a bad falloff — the
    // order make_brush checks them in, so the same mistake reads the same way.
    bad = b;
    bad.size = -4;
    bad.shape = 42;
    bad.falloff = 42;
    CHECK(clay_voxel_set_brush(c.grid, kOrigin, &bad, 1) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(std::string(clay_last_error()) == "brush size must be > 0");
    bad.size = 3;
    CHECK(clay_voxel_set_brush(c.grid, kOrigin, &bad, 1) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(std::string(clay_last_error()).find("shape") != std::string::npos);
    bad.shape = CLAY_BRUSH_SHAPE_CUBE;
    CHECK(clay_voxel_set_brush(c.grid, kOrigin, &bad, 1) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(std::string(clay_last_error()).find("falloff") != std::string::npos);

    // strength is checked after the three the Python bindings check, because
    // it is the one this boundary adds. A strength that is not > 0 covers no
    // cell at all, so it is a zero-initialized descriptor rather than a
    // request — and taking it for full coverage, as this used to, made
    // clay_voxel_erase_brush take the whole footprint away at the 0 end of a
    // strength slider.
    std::size_t before = 0, after = 0;
    REQUIRE(clay_voxel_set_brush(c.grid, kOrigin, &b, 1) == CLAY_OK);
    REQUIRE(clay_voxel_occupied_count(c.grid, &before) == CLAY_OK);
    REQUIRE(before == 27);
    for (float strength : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN()}) {
        bad = b;
        bad.strength = strength;
        CHECK(clay_voxel_set_brush(c.grid, kOrigin, &bad, 1) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(std::string(clay_last_error()) == "brush strength must be > 0");
        CHECK(clay_voxel_erase_brush(c.grid, kOrigin, &bad) == CLAY_ERROR_INVALID_ARGUMENT);
    }
    REQUIRE(clay_voxel_occupied_count(c.grid, &after) == CLAY_OK);
    CHECK(after == before);

    // and a strength in (0, 1) reaches the engine untouched, so it dithers
    // onto the cells the same stamp dithers onto through the Python bindings
    CGrid soft(0.1f);
    voxel::VoxelGrid ref(0.1f);
    clay_brush_params faded = b;
    faded.falloff = CLAY_BRUSH_FALLOFF_LINEAR;
    faded.strength = 0.35f;
    faded.seed = 11;
    REQUIRE(clay_voxel_set_brush(soft.grid, kOrigin, &faded, 1) == CLAY_OK);
    ref.palette_add(kernel::cf3(1, 1, 1));
    ref.set_brush({0, 0, 0}, ref_brush(faded), 1);
    check_same_grid(soft.grid, ref);

    // Unlike the Python bindings, a zero-length flatten normal is rejected
    // rather than normalized into NaN.
    const float zero[3] = {0.0f, 0.0f, 0.0f};
    CHECK(clay_voxel_sculpt_flatten(c.grid, kOrigin, &b, zero, 0.0f) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    // A palette index is widened to int32_t at the boundary and range-checked,
    // never truncated back into the engine's byte.
    CHECK(clay_voxel_set(c.grid, kOrigin, 256) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_set_mirrored(c.grid, kOrigin, 1, CLAY_MIRROR_Z << 1) ==
          CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("rasterizing a document fills the cells the engine fills") {
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
    clay_item_desc sphere{};
    sphere.struct_size = static_cast<std::uint32_t>(sizeof sphere);
    sphere.prim = CLAY_PRIM_SPHERE;
    sphere.params[0] = 0.6f;
    sphere.color[0] = 0.9f;
    sphere.color[1] = 0.3f;
    sphere.color[2] = 0.1f;
    REQUIRE(clay_add_item(doc, layer, &sphere, nullptr) == CLAY_OK);

    clay_voxel_grid* grid = nullptr;
    REQUIRE(clay_document_add_voxel_layer(doc, "voxels", 0.1f, nullptr, &grid) == CLAY_OK);
    const float lo[3] = {-1.0f, -1.0f, -1.0f}, hi[3] = {1.0f, 1.0f, 1.0f};
    REQUIRE(clay_voxel_rasterize(grid, doc, lo, hi) == CLAY_OK);

    scene::Document reference;
    scene::Layer& ref_layer = reference.add_sdf_layer("body");
    scene::Node node;
    node.prim.type = scene::PrimType::Sphere;
    node.prim.params[0] = 0.6f;
    node.color = kernel::cf3(0.9f, 0.3f, 0.1f);
    ref_layer.sdf->insert(node);
    voxel::VoxelGrid want(0.1f);
    want.rasterize_tape(scene::compile_document(reference),
                        math::Aabb{kernel::cf3(-1, -1, -1), kernel::cf3(1, 1, 1)});

    check_same_grid(grid, want);
    clay_document_destroy(doc);
}

TEST_CASE("a non-finite rasterize region is rejected before the grid is touched") {
    // Aabb::empty() and Aabb::is_infinite() are comparisons, and every
    // comparison against NaN is false, so a NaN region passes both and reaches
    // the engine's float-to-int cell conversion — undefined, and it writes.
    // A region a camera frustum or a degenerate selection produced is exactly
    // where a NaN comes from.
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
    clay_item_desc sphere{};
    sphere.struct_size = static_cast<std::uint32_t>(sizeof sphere);
    sphere.prim = CLAY_PRIM_SPHERE;
    sphere.params[0] = 0.6f;
    REQUIRE(clay_add_item(doc, layer, &sphere, nullptr) == CLAY_OK);
    CGrid c(0.1f);

    const float nan_v = std::numeric_limits<float>::quiet_NaN();
    const float inf_v = std::numeric_limits<float>::infinity();
    const float ok_lo[3] = {-1.0f, -1.0f, -1.0f}, ok_hi[3] = {1.0f, 1.0f, 1.0f};
    const float nan_lo[3] = {nan_v, nan_v, nan_v}, nan_hi[3] = {nan_v, nan_v, nan_v};
    const float inf_hi[3] = {inf_v, inf_v, inf_v};
    std::size_t occupied = 1;

    CHECK(clay_voxel_rasterize(c.grid, doc, nan_lo, nan_hi) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_voxel_occupied_count(c.grid, &occupied) == CLAY_OK);
    CHECK(occupied == 0);
    CHECK(clay_voxel_rasterize(c.grid, doc, ok_lo, nan_hi) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_rasterize(c.grid, doc, ok_lo, inf_hi) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_voxel_occupied_count(c.grid, &occupied) == CLAY_OK);
    CHECK(occupied == 0);

    // and the finite region the rejected ones bracket still works
    REQUIRE(clay_voxel_rasterize(c.grid, doc, ok_lo, ok_hi) == CLAY_OK);
    REQUIRE(clay_voxel_occupied_count(c.grid, &occupied) == CLAY_OK);
    CHECK(occupied > 0);
    clay_document_destroy(doc);
}

TEST_CASE("a count that is not a count is rejected, not allocated from") {
    // The batch entry points size a working buffer from a count only the
    // caller can vouch for, and the throw an over-large one provokes cannot be
    // caught here: the core builds -fno-exceptions, so std::bad_alloc would
    // reach std::terminate and take the host process down. A byte length where
    // an element count belongs, or a negative Swift Int widened to size_t, is
    // how one arrives.
    const std::size_t absurd = static_cast<std::size_t>(1) << 61;
    const std::size_t over = static_cast<std::size_t>(CLAY_MAX_BATCH) + 1;
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
    clay_item_desc sphere{};
    sphere.struct_size = static_cast<std::uint32_t>(sizeof sphere);
    sphere.prim = CLAY_PRIM_SPHERE;
    sphere.params[0] = 0.6f;
    clay_node_id node = 0;
    REQUIRE(clay_add_item(doc, layer, &sphere, &node) == CLAY_OK);

    float scratch[8] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float box_lo[3] = {0.0f, 0.0f, 0.0f}, box_hi[3] = {0.0f, 0.0f, 0.0f};
    std::int32_t has_bounds = 0;
    CHECK(clay_layer_selection_bounds(doc, layer, &node, absurd, box_lo, box_hi, &has_bounds) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_raycast_many(doc, scratch, absurd, nullptr, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_eval_gradients(doc, nullptr, scratch, absurd, scratch) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_eval_points(doc, nullptr, scratch, over, scratch, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_snap_to_surface(doc, scratch, over, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    CGrid c(0.1f);
    const std::int32_t cell[3] = {0, 0, 0};
    CHECK(clay_voxel_set_many(c.grid, cell, absurd, 1) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_sample_step_field(c.grid, scratch, absurd, scratch) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    std::size_t occupied = 1;
    REQUIRE(clay_voxel_occupied_count(c.grid, &occupied) == CLAY_OK);
    CHECK(occupied == 0);

    // the limit itself is inclusive, so a caller can use every bit of it
    CHECK(clay_eval_points(doc, nullptr, scratch, 1, scratch, nullptr) == CLAY_OK);
    clay_document_destroy(doc);
}

TEST_CASE("the spec's sculpting sequence lands cell for cell and buffer for buffer") {
    // The scenario as the spec words it: a grid, palette entries, a sphere
    // stamp, a sculpting verb, a greedy mesh. Checked on the cells first so a
    // divergence names one, and then on every buffer the mesh carries.
    CGrid c(0.15f);
    voxel::VoxelGrid ref(0.15f);
    const float orange[3] = {0.9f, 0.35f, 0.2f};
    const float blue[3] = {0.2f, 0.5f, 0.9f};
    std::int32_t hot = 0, cold = 0;
    REQUIRE(clay_voxel_palette_add(c.grid, orange, &hot) == CLAY_OK);
    REQUIRE(clay_voxel_palette_add(c.grid, blue, &cold) == CLAY_OK);
    std::uint8_t ref_hot = ref.palette_add(kernel::cf3(0.9f, 0.35f, 0.2f));
    std::uint8_t ref_cold = ref.palette_add(kernel::cf3(0.2f, 0.5f, 0.9f));
    REQUIRE(hot == ref_hot);
    REQUIRE(cold == ref_cold);
    std::size_t palette = 0;
    REQUIRE(clay_voxel_palette_size(c.grid, &palette) == CLAY_OK);
    CHECK(palette == ref.palette_size());

    clay_brush_params ball =
        brush(7, CLAY_BRUSH_SHAPE_SPHERE, CLAY_BRUSH_FALLOFF_CONSTANT, 1.0f, 0);
    clay_brush_params block =
        brush(5, CLAY_BRUSH_SHAPE_CUBE, CLAY_BRUSH_FALLOFF_CONSTANT, 1.0f, 0);
    const std::int32_t beside[3] = {5, 1, 0};
    const std::int32_t between[3] = {3, 0, 0};
    REQUIRE(clay_voxel_set_brush(c.grid, kOrigin, &ball, hot) == CLAY_OK);
    REQUIRE(clay_voxel_set_brush(c.grid, beside, &block, cold) == CLAY_OK);
    REQUIRE(clay_voxel_sculpt_smooth(c.grid, between, &ball) == CLAY_OK);
    REQUIRE(clay_voxel_sculpt_inflate(c.grid, kOrigin, &ball, -1) == CLAY_OK);
    ref.set_brush({0, 0, 0}, ref_brush(ball), ref_hot);
    ref.set_brush({5, 1, 0}, ref_brush(block), ref_cold);
    ref.sculpt_smooth({3, 0, 0}, ref_brush(ball));
    ref.sculpt_inflate({0, 0, 0}, ref_brush(ball), -1);

    // the sequence has to leave something of both colors behind, or the
    // comparison below would hold for an empty grid too
    std::size_t occupied = 0;
    REQUIRE(clay_voxel_occupied_count(c.grid, &occupied) == CLAY_OK);
    REQUIRE(occupied > 0);
    check_same_grid(c.grid, ref);

    clay_mesh* got = nullptr;
    REQUIRE(clay_voxel_mesh(c.grid, &got) == CLAY_OK);
    mesh::Mesh want = ref.mesh_greedy();
    REQUIRE(!want.empty());
    int diverged = compare_meshes(got, want);
    INFO("diverging mesh values: " << diverged);
    CHECK(diverged == 0);
    clay_mesh_destroy(got);
}

TEST_CASE("c abi: a mesh rasterizes straight to cells, region optional") {
    // Twelve triangles bounding a box, wound outward — built by hand so the
    // fixture carries no meshing error into a test about sampling.
    const float positions[24] = {-0.3f, -0.2f, -0.25f, 0.3f,  -0.2f, -0.25f,
                                 0.3f,  0.2f,  -0.25f, -0.3f, 0.2f,  -0.25f,
                                 -0.3f, -0.2f, 0.25f,  0.3f,  -0.2f, 0.25f,
                                 0.3f,  0.2f,  0.25f,  -0.3f, 0.2f,  0.25f};
    const std::uint32_t faces[36] = {0, 3, 2, 0, 2, 1, 4, 5, 6, 4, 6, 7,
                                     0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2,
                                     0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
    clay_mesh* m = nullptr;
    REQUIRE(clay_mesh_from_triangles(positions, 8, faces, 36, &m) == CLAY_OK);

    clay_voxel_grid* grid = clay_voxel_grid_create(0.02f);
    REQUIRE(grid != nullptr);
    // NULL region: a mesh always has bounds, which is the whole difference
    // from clay_voxel_rasterize.
    REQUIRE(clay_voxel_rasterize_mesh(grid, m, nullptr, nullptr) == CLAY_OK);
    size_t filled = 0;
    REQUIRE(clay_voxel_occupied_count(grid, &filled) == CLAY_OK);
    CHECK(filled > 0);

    // An explicit region bounds the work.
    clay_voxel_grid* half = clay_voxel_grid_create(0.02f);
    const float lo[3] = {0.0f, -0.4f, -0.4f}, hi[3] = {0.4f, 0.4f, 0.4f};
    REQUIRE(clay_voxel_rasterize_mesh(half, m, lo, hi) == CLAY_OK);
    size_t half_filled = 0;
    REQUIRE(clay_voxel_occupied_count(half, &half_filled) == CLAY_OK);
    CHECK(half_filled > 0);
    CHECK(half_filled < filled);

    // The refusals, each leaving the grid as it was.
    clay_voxel_grid* untouched = clay_voxel_grid_create(0.02f);
    const float nan_hi[3] = {std::nanf(""), 0.4f, 0.4f};
    CHECK(clay_voxel_rasterize_mesh(untouched, m, lo, nan_hi) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_rasterize_mesh(untouched, m, lo, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_rasterize_mesh(untouched, nullptr, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_rasterize_mesh(nullptr, m, nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    size_t none = 1;
    REQUIRE(clay_voxel_occupied_count(untouched, &none) == CLAY_OK);
    CHECK(none == 0);

    clay_voxel_grid_destroy(untouched);
    clay_voxel_grid_destroy(half);
    clay_voxel_grid_destroy(grid);
    clay_mesh_destroy(m);
}

TEST_CASE("c abi: sculpt layers record, dial and survive a round trip") {
    clay_voxel_grid* grid = clay_voxel_grid_create(0.1f);
    REQUIRE(grid != nullptr);
    int32_t idx = 0;
    const float rgb[3] = {0.6f, 0.6f, 0.6f};
    REQUIRE(clay_voxel_palette_add(grid, rgb, &idx) == CLAY_OK);
    for (int32_t x = 0; x < 8; ++x) {
        const int32_t cell[3] = {x, 0, 0};
        REQUIRE(clay_voxel_set(grid, cell, idx) == CLAY_OK);
    }
    size_t base = 0;
    REQUIRE(clay_voxel_occupied_count(grid, &base) == CLAY_OK);

    size_t count = 1;
    CHECK(clay_voxel_sculpt_layer_count(grid, &count) == CLAY_OK);
    CHECK(count == 0);

    size_t layer = 99;
    CHECK(clay_voxel_begin_sculpt_layer(grid, "a pass", &layer) == CLAY_OK);
    CHECK(layer == 0);
    int32_t recording = 0;
    CHECK(clay_voxel_recording_sculpt_layer(grid, &recording) == CLAY_OK);
    CHECK(recording == 1);
    // Nesting has no meaning, and the refusal says so rather than silently
    // starting a second one.
    CHECK(clay_voxel_begin_sculpt_layer(grid, "nested", nullptr) != CLAY_OK);

    for (int32_t x = 0; x < 8; ++x) {
        const int32_t cell[3] = {x, 1, 0};
        REQUIRE(clay_voxel_set(grid, cell, idx) == CLAY_OK);
    }
    CHECK(clay_voxel_end_sculpt_layer(grid) == CLAY_OK);
    CHECK(clay_voxel_recording_sculpt_layer(grid, &recording) == CLAY_OK);
    CHECK(recording == 0);
    // Ending twice is a no-op, not an error.
    CHECK(clay_voxel_end_sculpt_layer(grid) == CLAY_OK);

    CHECK(clay_voxel_sculpt_layer_count(grid, &count) == CLAY_OK);
    CHECK(count == 1);
    size_t cells = 0;
    CHECK(clay_voxel_sculpt_layer_cell_count(grid, 0, &cells) == CLAY_OK);
    CHECK(cells == 8);

    // The name, by the size-query pattern.
    size_t need = 0;
    CHECK(clay_voxel_sculpt_layer_name(grid, 0, nullptr, &need) == CLAY_OK);
    CHECK(need == 7);  // "a pass" + NUL
    std::vector<char> buf(need);
    size_t have = need;
    CHECK(clay_voxel_sculpt_layer_name(grid, 0, buf.data(), &have) == CLAY_OK);
    CHECK(std::string(buf.data()) == "a pass");
    size_t tiny = 2;
    char small[2];
    CHECK(clay_voxel_sculpt_layer_name(grid, 0, small, &tiny) == CLAY_ERROR_BUFFER_TOO_SMALL);
    CHECK(tiny == need);

    // Dialling, from both ends.
    size_t occupied = 0;
    CHECK(clay_voxel_set_sculpt_layer_strength(grid, 0, 0.0f) == CLAY_OK);
    CHECK(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK);
    CHECK(occupied == base);
    CHECK(clay_voxel_set_sculpt_layer_strength(grid, 0, 1.0f) == CLAY_OK);
    CHECK(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK);
    CHECK(occupied == base + 8);
    float strength = 0.0f;
    CHECK(clay_voxel_sculpt_layer_strength(grid, 0, &strength) == CLAY_OK);
    CHECK(strength == doctest::Approx(1.0f));
    // Clamped, not refused.
    CHECK(clay_voxel_set_sculpt_layer_strength(grid, 0, 4.0f) == CLAY_OK);
    CHECK(clay_voxel_sculpt_layer_strength(grid, 0, &strength) == CLAY_OK);
    CHECK(strength == doctest::Approx(1.0f));

    int32_t visible = 0;
    CHECK(clay_voxel_sculpt_layer_visible(grid, 0, &visible) == CLAY_OK);
    CHECK(visible == 1);
    CHECK(clay_voxel_set_sculpt_layer_visible(grid, 0, 0) == CLAY_OK);
    CHECK(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK);
    CHECK(occupied == base);
    CHECK(clay_voxel_set_sculpt_layer_visible(grid, 0, 1) == CLAY_OK);

    // The bottom layer has nothing to merge into, and an index that does not
    // exist is NOT_FOUND rather than a crash.
    CHECK(clay_voxel_merge_sculpt_layer_down(grid, 0) != CLAY_OK);
    CHECK(clay_voxel_sculpt_layer_strength(grid, 7, &strength) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_voxel_set_sculpt_layer_visible(grid, 7, 0) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_voxel_remove_sculpt_layer(grid, 7) == CLAY_ERROR_NOT_FOUND);

    CHECK(clay_voxel_remove_sculpt_layer(grid, 0) == CLAY_OK);
    CHECK(clay_voxel_sculpt_layer_count(grid, &count) == CLAY_OK);
    CHECK(count == 0);
    CHECK(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK);
    CHECK(occupied == base);

    CHECK(clay_voxel_grid_destroy(grid) == CLAY_OK);
}

// -- clay_layer_bounds over a voxel layer (issue #318) ------------------------
//
// Reported for MESH layers; the voxel arm had the identical defect and the
// issue did not know it. Worth its own case rather than a second assertion in
// the mesh one: the two read different side tables, and a cell is a BOX where a
// vertex is a point, which is the part a shared test would not have caught.
TEST_CASE("c voxel layer: clay_layer_bounds answers from the occupied cells") {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);

    clay_layer_id layer = 0;
    clay_voxel_grid* grid = nullptr;
    const float vs = 0.05f;
    REQUIRE(clay_document_add_voxel_layer(doc, "grid", vs, &layer, &grid) == CLAY_OK);

    float bmin[3] = {0, 0, 0};
    float bmax[3] = {0, 0, 0};
    std::int32_t has = 1;

    // An EMPTY grid is genuinely nowhere, and that is a different answer from
    // "this kind of layer cannot say". The fix must not turn it into a box.
    REQUIRE(clay_layer_bounds(doc, layer, bmin, bmax, &has) == CLAY_OK);
    CHECK(has == 0);

    // One cell at the origin. Its extent is the CELL, not the point: a single
    // occupied cell spans [0,vs] on every axis, so a bound that took the near
    // corner twice would report an empty box and read as "nowhere" again.
    const std::int32_t cell[3] = {0, 0, 0};
    REQUIRE(clay_voxel_set(grid, cell, 1) == CLAY_OK);
    has = 0;
    REQUIRE(clay_layer_bounds(doc, layer, bmin, bmax, &has) == CLAY_OK);
    REQUIRE(has == 1);
    for (int i = 0; i < 3; ++i) {
        CHECK(bmin[i] == doctest::Approx(0.0f));
        CHECK(bmax[i] == doctest::Approx(vs));
    }

    // A second cell away from the first, so the box has to grow rather than
    // merely exist. Cell 3 spans [3*vs, 4*vs], hence the +1 on the far corner.
    const std::int32_t far_cell[3] = {3, 2, 1};
    REQUIRE(clay_voxel_set(grid, far_cell, 1) == CLAY_OK);
    REQUIRE(clay_layer_bounds(doc, layer, bmin, bmax, &has) == CLAY_OK);
    REQUIRE(has == 1);
    CHECK(bmin[0] == doctest::Approx(0.0f));
    CHECK(bmax[0] == doctest::Approx(4 * vs));
    CHECK(bmax[1] == doctest::Approx(3 * vs));
    CHECK(bmax[2] == doctest::Approx(2 * vs));

    // World space, like the SDF and mesh arms: the layer transform applies.
    const float offset[3] = {1.0f, 0.0f, -2.0f};
    const float axis[3] = {0.0f, 1.0f, 0.0f};
    REQUIRE(clay_document_set_layer_transform(doc, layer, offset, axis, 0.0f, 1.0f) == CLAY_OK);
    REQUIRE(clay_layer_bounds(doc, layer, bmin, bmax, &has) == CLAY_OK);
    REQUIRE(has == 1);
    CHECK(bmin[0] == doctest::Approx(1.0f));
    CHECK(bmin[2] == doctest::Approx(-2.0f));
    CHECK(bmax[0] == doctest::Approx(1.0f + 4 * vs));

    clay_document_destroy(doc);
}

// The conversion issue #318 says is unreachable: a host cannot supply a region
// for a mesh layer without reading every vertex back, so CLAY_PRIM-side
// mesh-to-voxel could not be driven through the ordinary path at all.
TEST_CASE("c voxel: a mesh layer's bounds are a region rasterize_mesh accepts") {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);

    const float positions[12] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    const std::uint32_t indices[12] = {0, 1, 2, 0, 1, 3, 0, 2, 3, 1, 2, 3};
    clay_mesh* source = nullptr;
    REQUIRE(clay_mesh_from_triangles(positions, 4, indices, 12, &source) == CLAY_OK);

    clay_mesh_layer_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.struct_size = static_cast<std::uint32_t>(sizeof desc);
    desc.name = "scan";
    clay_layer_id mesh_layer = 0;
    clay_mesh* borrowed = nullptr;
    REQUIRE(clay_document_add_mesh_layer(doc, source, &desc, &mesh_layer, &borrowed) == CLAY_OK);
    clay_mesh_destroy(source);

    float bmin[3] = {0, 0, 0};
    float bmax[3] = {0, 0, 0};
    std::int32_t has = 0;
    REQUIRE(clay_layer_bounds(doc, mesh_layer, bmin, bmax, &has) == CLAY_OK);
    REQUIRE(has == 1);

    clay_layer_id voxel_layer = 0;
    clay_voxel_grid* grid = nullptr;
    REQUIRE(clay_document_add_voxel_layer(doc, "from_mesh", 0.05f, &voxel_layer, &grid) == CLAY_OK);
    REQUIRE(clay_voxel_rasterize_mesh(grid, borrowed, bmin, bmax) == CLAY_OK);

    std::size_t occupied = 0;
    REQUIRE(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK);
    // The assertion that matters is that it rasterized ANYTHING: before the fix
    // the region could not be obtained, and the call refused with "this layer
    // has no bounds, so a region is needed to say where the rasterization
    // stops".
    CHECK(occupied > 0);

    clay_document_destroy(doc);
}

// #365: the id-addressed route back to a grid. The refusals are the design —
// an id that names no layer, an id that names a layer of another
// representation, and an id whose layer holds no grid are all NOT_FOUND, and a
// borrow of the wrong kind would be worse than any of them.
TEST_CASE("a voxel grid is reachable by its layer id, and refuses what it cannot answer") {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id voxel = 0;
    clay_voxel_grid* grid = nullptr;
    REQUIRE(clay_document_add_voxel_layer(doc, "sculpt", 0.05f, &voxel, &grid) == CLAY_OK);
    const std::int32_t cell[3] = {4, 5, 6};
    REQUIRE(clay_voxel_set(grid, cell, 3) == CLAY_OK);

    // The same borrow the creation and the by-name lookup hand back: a handle
    // names its layer rather than caching a pointer, so all three are one.
    clay_voxel_grid* by_id = nullptr;
    REQUIRE(clay_document_voxel_layer_by_id(doc, voxel, &by_id) == CLAY_OK);
    CHECK(by_id == grid);
    std::int32_t read = 0;
    REQUIRE(clay_voxel_get(by_id, cell, &read) == CLAY_OK);
    CHECK(read == 3);

    clay_layer_id sdf = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &sdf) == CLAY_OK);

    clay_voxel_grid* untouched = grid;
    // An SDF layer's id is a layer, and is still not a voxel layer.
    CHECK(clay_document_voxel_layer_by_id(doc, sdf, &untouched) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_document_voxel_layer_by_id(doc, voxel + 9999, &untouched) == CLAY_ERROR_NOT_FOUND);
    CHECK(untouched == grid);  // a refused lookup writes nothing

    CHECK(clay_document_voxel_layer_by_id(nullptr, voxel, &untouched) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    // Required here and optional in the by-name form: there a NULL still leaves
    // the call something to answer with, here it asks nothing at all.
    CHECK(clay_document_voxel_layer_by_id(doc, voxel, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(untouched == grid);

    clay_document_destroy(doc);
}

TEST_CASE("a voxel layer whose grid did not come with the file is not found by id") {
    // The third refusal, and the only one that needs a document built by hand:
    // the loader drops a grid whose layer is gone but keeps a layer whose grid
    // is gone, so a file that lost its voxel chunk comes back as a voxel layer
    // with nothing behind it. The by-name lookup already reports NOT_FOUND
    // there and the by-id lookup has to agree, or an id would reach a payload
    // the name cannot.
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id voxel = 0;
    clay_voxel_grid* grid = nullptr;
    REQUIRE(clay_document_add_voxel_layer(doc, "sculpt", 0.05f, &voxel, &grid) == CLAY_OK);
    const std::int32_t cell[3] = {0, 0, 0};
    REQUIRE(clay_voxel_set(grid, cell, 1) == CLAY_OK);

    clay_blob* blob = nullptr;
    REQUIRE(clay_document_save_memory(doc, &blob) == CLAY_OK);
    const std::uint8_t* bytes = clay_blob_data(blob);
    const std::size_t size = clay_blob_size(blob);

    // Container layout: "CLAY", major, minor, then (fourcc, u64 length,
    // payload) repeated. Copy every chunk but the voxel ones.
    const std::uint32_t kVoxelChunk = 0x4C584F56u;  // "VOXL" little-endian
    std::vector<std::uint8_t> stripped(bytes, bytes + 8);
    std::size_t at = 8, dropped = 0;
    while (at + 12 <= size) {
        std::uint32_t cc = 0;
        std::uint64_t len = 0;
        std::memcpy(&cc, bytes + at, 4);
        std::memcpy(&len, bytes + at + 4, 8);
        const std::size_t end = at + 12 + static_cast<std::size_t>(len);
        REQUIRE(end <= size);
        if (cc == kVoxelChunk)
            ++dropped;
        else
            stripped.insert(stripped.end(), bytes + at, bytes + end);
        at = end;
    }
    CHECK(at == size);
    REQUIRE(dropped == 1);  // the surgery found something, so the load is a real case
    clay_blob_destroy(blob);
    clay_document_destroy(doc);

    clay_document* back = nullptr;
    REQUIRE(clay_document_load_memory(stripped.data(), stripped.size(), &back) == CLAY_OK);
    // The layer is there — this is a layer with no grid, not a missing layer.
    clay_layer_info info;
    std::memset(&info, 0, sizeof info);
    info.struct_size = static_cast<std::uint32_t>(sizeof info);
    REQUIRE(clay_document_layer_info(back, voxel, &info) == CLAY_OK);
    CHECK(info.id == voxel);
    CHECK(info.representation == CLAY_LAYER_VOXEL);

    clay_voxel_grid* missing = nullptr;
    CHECK(clay_document_voxel_layer_by_id(back, voxel, &missing) == CLAY_ERROR_NOT_FOUND);
    CHECK(missing == nullptr);
    CHECK(clay_document_voxel_layer(back, "sculpt", nullptr, &missing) == CLAY_ERROR_NOT_FOUND);
    clay_document_destroy(back);
}
