// The incremental voxel display path across the C ABI (c-abi spec: a host can
// display a voxel sculpt incrementally, #86 part 2).
//
// The engine side is proved in test_voxel_incremental_mesh.cpp. What is left to
// prove here is the boundary: the drain's capacity/count/remainder loop, the
// refusals the header states, and that the ranges a host would patch a GPU
// buffer with are the engine's own.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#include "clay.h"
#include "clay/voxel/grid.h"

using namespace clay;

namespace {

struct CGrid {
    clay_voxel_grid* grid = nullptr;
    explicit CGrid(float voxel_size = 1.0f) : grid(clay_voxel_grid_create(voxel_size)) {
        REQUIRE(grid != nullptr);
    }
    ~CGrid() { clay_voxel_grid_destroy(grid); }
    CGrid(const CGrid&) = delete;
    CGrid& operator=(const CGrid&) = delete;
};

std::int32_t white(clay_voxel_grid* g) {
    const float rgb[3] = {1.0f, 1.0f, 1.0f};
    std::int32_t index = 0;
    REQUIRE(clay_voxel_palette_add(g, rgb, &index) == CLAY_OK);
    return index;
}

void set_cell(clay_voxel_grid* g, std::int32_t x, std::int32_t y, std::int32_t z,
              std::int32_t index) {
    const std::int32_t c[3] = {x, y, z};
    REQUIRE(clay_voxel_set(g, c, index) == CLAY_OK);
}

// Every key the drain reports, taken in chunks of `capacity` until nothing is
// left — which is exactly the loop the header tells a host to write.
std::vector<std::array<std::int32_t, 3>> drain_all(clay_voxel_grid* g, std::size_t capacity,
                                                   int* out_calls = nullptr) {
    std::vector<std::array<std::int32_t, 3>> keys;
    std::vector<std::int32_t> buf(capacity * 3);
    std::size_t remaining = 1;
    int calls = 0;
    while (remaining > 0) {
        std::size_t count = capacity;
        REQUIRE(clay_voxel_take_dirty_chunks(g, buf.data(), &count, &remaining) == CLAY_OK);
        ++calls;
        for (std::size_t i = 0; i < count; ++i)
            keys.push_back({buf[i * 3], buf[i * 3 + 1], buf[i * 3 + 2]});
        if (count == 0 && remaining == 0) break;
    }
    if (out_calls) *out_calls = calls;
    return keys;
}

}  // namespace

TEST_CASE("a host drains dirty chunks in fixed-size chunks and gets each key once") {
    CGrid c(1.0f);
    std::int32_t idx = white(c.grid);
    const int kDim = voxel::kChunkDim;
    for (int i = 0; i < 7; ++i) set_cell(c.grid, i * kDim + 3, 3, 3, idx);

    int calls = 0;
    std::vector<std::array<std::int32_t, 3>> keys = drain_all(c.grid, 2, &calls);
    CHECK(keys.size() == 7);
    CHECK(calls == 4);  // 2 + 2 + 2 + 1, and the last reports 0 remaining
    std::set<std::array<std::int32_t, 3>> unique(keys.begin(), keys.end());
    CHECK(unique.size() == 7);
    // Deterministic order, so a replay drains the same list.
    for (std::size_t i = 1; i < keys.size(); ++i) CHECK(keys[i - 1] < keys[i]);

    // A second drain with nothing written in between is empty.
    std::int32_t buf[3] = {0, 0, 0};
    std::size_t count = 1, remaining = 7;
    REQUIRE(clay_voxel_take_dirty_chunks(c.grid, buf, &count, &remaining) == CLAY_OK);
    CHECK(count == 0);
    CHECK(remaining == 0);
}

TEST_CASE("a write during a partial drain is queued behind it") {
    CGrid c(1.0f);
    std::int32_t idx = white(c.grid);
    const int kDim = voxel::kChunkDim;
    for (int i = 0; i < 4; ++i) set_cell(c.grid, i * kDim + 3, 3, 3, idx);

    std::int32_t buf[3 * 2] = {0};
    std::size_t count = 2, remaining = 0;
    REQUIRE(clay_voxel_take_dirty_chunks(c.grid, buf, &count, &remaining) == CLAY_OK);
    CHECK(count == 2);
    CHECK(remaining == 2);

    // A fresh write while two keys are still staged: it lands behind them.
    set_cell(c.grid, 9 * kDim + 3, 3, 3, idx);
    CHECK(remaining == 2);
    std::vector<std::array<std::int32_t, 3>> rest = drain_all(c.grid, 8);
    CHECK(rest.size() == 3);
    CHECK(rest.back() == std::array<std::int32_t, 3>{9, 0, 0});
}

TEST_CASE("the drain is capacity-in/count-out, never a size query") {
    CGrid c(1.0f);
    std::int32_t idx = white(c.grid);
    set_cell(c.grid, 1, 1, 1, idx);

    std::size_t count = 0;
    CHECK(clay_voxel_take_dirty_chunks(c.grid, nullptr, &count, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    std::int32_t buf[3] = {0, 0, 0};
    CHECK(clay_voxel_take_dirty_chunks(c.grid, buf, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_take_dirty_chunks(nullptr, buf, &count, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    // The refusals left the set alone.
    count = 4;
    std::size_t remaining = 0;
    REQUIRE(clay_voxel_take_dirty_chunks(c.grid, buf, &count, &remaining) == CLAY_OK);
    CHECK(count == 1);
    CHECK(remaining == 0);
}

TEST_CASE("a regional mesh reports one range per key, in the order given") {
    CGrid c(1.0f);
    std::int32_t idx = white(c.grid);
    const int kDim = voxel::kChunkDim;
    set_cell(c.grid, 1, 1, 1, idx);
    set_cell(c.grid, kDim + 1, 1, 1, idx);

    const std::int32_t keys[6] = {1, 0, 0, 0, 0, 0};
    clay_voxel_chunk_mesh_range ranges[2];
    std::memset(ranges, 0xEE, sizeof ranges);
    clay_mesh* m = nullptr;
    REQUIRE(clay_voxel_mesh_chunks(c.grid, keys, 2, ranges, &m) == CLAY_OK);
    REQUIRE(m != nullptr);

    CHECK(ranges[0].key[0] == 1);
    CHECK(ranges[1].key[0] == 0);
    CHECK(ranges[0].vertex_first == 0);
    CHECK(ranges[0].vertex_count == 24);
    CHECK(ranges[1].vertex_first == 24);
    CHECK(ranges[1].vertex_count == 24);
    CHECK(ranges[0].index_count + ranges[1].index_count == clay_mesh_index_count(m));
    CHECK(clay_mesh_vertex_count(m) == 48);

    // The ranges partition: every index in a key's range points into that
    // key's own vertices, so a host may drop one key's slice on its own.
    const std::uint32_t* indices = clay_mesh_indices(m);
    REQUIRE(indices != nullptr);
    for (int r = 0; r < 2; ++r)
        for (std::uint32_t i = 0; i < ranges[r].index_count; ++i) {
            std::uint32_t v = indices[ranges[r].index_first + i];
            CHECK(v >= ranges[r].vertex_first);
            CHECK(v < ranges[r].vertex_first + ranges[r].vertex_count);
        }
    clay_mesh_destroy(m);
}

TEST_CASE("a stale key is an ordinary key with an empty range") {
    CGrid c(1.0f);
    std::int32_t idx = white(c.grid);
    set_cell(c.grid, 1, 1, 1, idx);
    set_cell(c.grid, 1, 1, 1, 0);  // the chunk is emptied and dropped

    std::vector<std::array<std::int32_t, 3>> keys = drain_all(c.grid, 8);
    REQUIRE(keys.size() == 1);
    std::vector<std::int32_t> flat = {keys[0][0], keys[0][1], keys[0][2]};
    clay_voxel_chunk_mesh_range range;
    std::memset(&range, 0xEE, sizeof range);
    clay_mesh* m = nullptr;
    REQUIRE(clay_voxel_mesh_chunks(c.grid, flat.data(), 1, &range, &m) == CLAY_OK);
    CHECK(clay_mesh_index_count(m) == 0);
    CHECK(range.vertex_count == 0);
    CHECK(range.index_count == 0);
    clay_mesh_destroy(m);
}

TEST_CASE("a regional mesh refuses what it cannot size or read") {
    CGrid c(1.0f);
    clay_mesh* m = nullptr;
    clay_voxel_chunk_mesh_range range;
    const std::int32_t keys[3] = {0, 0, 0};

    // Ranges with no keys: there is no count the caller could have sized from.
    CHECK(clay_voxel_mesh_chunks(c.grid, nullptr, 0, &range, &m) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_mesh_chunks(c.grid, keys, 1, &range, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_mesh_chunks(c.grid, nullptr, 3, &range, &m) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_voxel_mesh_chunks(nullptr, keys, 1, &range, &m) == CLAY_ERROR_INVALID_ARGUMENT);

    // No keys at all is an ordinary empty mesh, as an empty grid is.
    REQUIRE(clay_voxel_mesh_chunks(c.grid, nullptr, 0, nullptr, &m) == CLAY_OK);
    CHECK(clay_mesh_vertex_count(m) == 0);
    CHECK(clay_mesh_positions(m) == nullptr);
    clay_mesh_destroy(m);
}

TEST_CASE("clay_voxel_mesh still means the whole grid, unchanged") {
    CGrid c(1.0f);
    voxel::VoxelGrid ref(1.0f);
    std::int32_t idx = white(c.grid);
    std::uint8_t ref_idx = ref.palette_add(kernel::cf3(1, 1, 1));
    const int kDim = voxel::kChunkDim;
    for (int x = kDim - 3; x <= kDim + 2; ++x) {
        set_cell(c.grid, x, 0, 0, idx);
        ref.set({x, 0, 0}, ref_idx);
    }

    clay_mesh* whole = nullptr;
    REQUIRE(clay_voxel_mesh(c.grid, &whole) == CLAY_OK);
    mesh::Mesh want = ref.mesh_greedy();
    REQUIRE(clay_mesh_vertex_count(whole) == want.positions.size());
    REQUIRE(clay_mesh_index_count(whole) == want.indices.size());
    const float* positions = clay_mesh_positions(whole);
    for (std::size_t i = 0; i < want.positions.size(); ++i) {
        CHECK(positions[i * 3 + 0] == want.positions[i].x);
        CHECK(positions[i * 3 + 1] == want.positions[i].y);
        CHECK(positions[i * 3 + 2] == want.positions[i].z);
    }

    // Draining does not disturb it either: the whole-grid call ignores the set.
    (void)drain_all(c.grid, 8);
    clay_mesh* again = nullptr;
    REQUIRE(clay_voxel_mesh(c.grid, &again) == CLAY_OK);
    CHECK(clay_mesh_vertex_count(again) == clay_mesh_vertex_count(whole));
    CHECK(clay_mesh_index_count(again) == clay_mesh_index_count(whole));

    // The per-chunk mesh over the same chunks splits quads at the seam and so
    // has MORE triangles over the same surface, never fewer.
    const std::int32_t keys[6] = {0, 0, 0, 1, 0, 0};
    clay_mesh* per = nullptr;
    REQUIRE(clay_voxel_mesh_chunks(c.grid, keys, 2, nullptr, &per) == CLAY_OK);
    CHECK(clay_mesh_index_count(per) > clay_mesh_index_count(whole));

    clay_mesh_destroy(per);
    clay_mesh_destroy(again);
    clay_mesh_destroy(whole);
}

TEST_CASE("a borrowed layer grid drains and meshes its own chunks") {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    clay_voxel_grid* grid = nullptr;
    REQUIRE(clay_document_add_voxel_layer(doc, "vox", 1.0f, &layer, &grid) == CLAY_OK);
    std::int32_t idx = white(grid);
    set_cell(grid, 1, 1, 1, idx);

    std::vector<std::array<std::int32_t, 3>> keys = drain_all(grid, 4);
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == std::array<std::int32_t, 3>{0, 0, 0});

    const std::int32_t flat[3] = {0, 0, 0};
    clay_mesh* m = nullptr;
    REQUIRE(clay_voxel_mesh_chunks(grid, flat, 1, nullptr, &m) == CLAY_OK);
    CHECK(clay_mesh_index_count(m) == 36);
    clay_mesh_destroy(m);
    clay_document_destroy(doc);
}
