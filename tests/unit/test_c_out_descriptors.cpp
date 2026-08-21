#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "clay.h"

// Output descriptors are filled BOUNDED by the size the caller declared
// (c-abi spec: the struct_size prefix rule).
//
// The rule cuts both ways and only one half was ever tested. read_desc checks
// an INCOMING struct_size; nothing checked what we write back, and the natural
// spelling — `*out = clay_thing{}` then assign the fields — writes sizeof as
// THIS build defines it. A host compiled against an older clay.h allocated the
// older, smaller struct, so every byte a later field added lands past the end
// of its buffer. That is not a style point: growing clay_brick_stats by two
// fields segfaulted the ctypes ABI check, and clay_brick_config, clay_quad_report,
// clay_repair_report and clay_consolidation_cost had all already grown the same
// way without anyone noticing.
//
// So each call below is made the way an OLD host makes it: declaring the
// ORIGINAL layout, into a buffer with room to spare, and checking that not one
// byte past that layout was touched. For a descriptor that has not grown yet
// the check passes trivially — which is the point. It starts failing the day
// somebody appends a field without using write_desc, instead of shipping.

namespace {

constexpr unsigned char kFill = 0xAB;

// A caller's buffer, over-allocated so an overrun has somewhere to land
// instead of corrupting a neighbour and going unnoticed.
template <typename Desc>
struct OldHostBuffer {
    alignas(alignof(std::max_align_t)) unsigned char bytes[sizeof(Desc) + 64];
    std::size_t declared;

    explicit OldHostBuffer(std::size_t original) : declared(original) {
        std::memset(bytes, kFill, sizeof bytes);
        const std::uint32_t s = static_cast<std::uint32_t>(original);
        std::memcpy(bytes, &s, sizeof s);  // struct_size leads every descriptor
    }

    Desc* ptr() { return reinterpret_cast<Desc*>(bytes); }

    std::size_t overrun() const {
        std::size_t n = 0;
        for (std::size_t i = declared; i < sizeof bytes; ++i)
            if (bytes[i] != kFill) ++n;
        return n;
    }

    // The declared size must come back unchanged: it describes the CALLER's
    // buffer, and handing back ours would advertise fields never written.
    std::uint32_t reported_size() const {
        std::uint32_t s = 0;
        std::memcpy(&s, bytes, sizeof s);
        return s;
    }

    // Something inside the declared span changed, so the call actually filled
    // the struct rather than passing by doing nothing.
    bool was_filled() const {
        for (std::size_t i = sizeof(std::uint32_t); i < declared; ++i)
            if (bytes[i] != kFill) return true;
        return false;
    }
};

#define ORIGINAL_OF(Desc, last) (offsetof(Desc, last) + sizeof(((Desc*)nullptr)->last))

struct Doc {
    clay_document* d = nullptr;
    clay_layer_id layer = 0;
    Doc() {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "body", &layer) == CLAY_OK);
        const float r = 0.5f;
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        REQUIRE(it != nullptr);
        clay_node_id id = 0;
        REQUIRE(clay_layer_add_item(d, layer, it, &id) == CLAY_OK);
        clay_item_destroy(it);
    }
    ~Doc() { clay_document_destroy(d); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
    operator clay_document*() const { return d; }
};

}  // namespace

TEST_CASE("c abi: an output descriptor is never filled past the size the caller declared") {
    SUBCASE("clay_document_layer_info") {
        Doc doc;
        OldHostBuffer<clay_layer_info> buf(ORIGINAL_OF(clay_layer_info, locked));
        REQUIRE(clay_document_layer_info(doc, doc.layer, buf.ptr()) == CLAY_OK);
        CHECK(buf.overrun() == 0);
        CHECK(buf.reported_size() == buf.declared);
        CHECK(buf.was_filled());
    }

    SUBCASE("clay_layer_field_report") {
        Doc doc;
        OldHostBuffer<clay_field_report> buf(
            ORIGINAL_OF(clay_field_report, advises_consolidation));
        REQUIRE(clay_layer_field_report(doc, doc.layer, 0.25f, buf.ptr()) == CLAY_OK);
        CHECK(buf.overrun() == 0);
        CHECK(buf.reported_size() == buf.declared);
        CHECK(buf.was_filled());
    }

    SUBCASE("clay_layer_consolidation_cost") {
        Doc doc;
        clay_consolidation_params p{};
        p.struct_size = sizeof(p);
        p.cell_size = 0.05f;
        p.band = 3.0f;
        OldHostBuffer<clay_consolidation_cost> buf(
            ORIGINAL_OF(clay_consolidation_cost, bounds_max));
        REQUIRE(clay_layer_consolidation_cost(doc, doc.layer, &p, nullptr, nullptr, buf.ptr()) ==
                CLAY_OK);
        CHECK(buf.overrun() == 0);
        CHECK(buf.reported_size() == buf.declared);
        CHECK(buf.was_filled());
    }

    SUBCASE("clay_mesh_quad_report") {
        Doc doc;
        clay_quad_params p{};
        p.struct_size = sizeof(p);
        p.cell_size = 0.08f;
        clay_mesh* mesh = nullptr;
        REQUIRE(clay_document_mesh_quads(doc, &p, &mesh) == CLAY_OK);
        REQUIRE(mesh != nullptr);
        OldHostBuffer<clay_quad_report> buf(ORIGINAL_OF(clay_quad_report, clamped));
        REQUIRE(clay_mesh_quad_report(mesh, buf.ptr()) == CLAY_OK);
        CHECK(buf.overrun() == 0);
        CHECK(buf.reported_size() == buf.declared);
        CHECK(buf.was_filled());
        clay_mesh_destroy(mesh);
    }

    SUBCASE("clay_voxel_repair_report") {
        clay_voxel_grid* g = clay_voxel_grid_create(0.1f);
        REQUIRE(g != nullptr);
        OldHostBuffer<clay_repair_report> buf(ORIGINAL_OF(clay_repair_report, airtight));
        REQUIRE(clay_voxel_repair_report(g, buf.ptr()) == CLAY_OK);
        CHECK(buf.overrun() == 0);
        CHECK(buf.reported_size() == buf.declared);
        clay_voxel_grid_destroy(g);
    }

    SUBCASE("clay_brick_cache_config and clay_brick_cache_stats") {
        clay_brick_config cfg{};
        cfg.struct_size = sizeof(cfg);
        REQUIRE(clay_brick_config_defaults(&cfg) == CLAY_OK);
        clay_brick_cache* cache = clay_brick_cache_create(&cfg);
        REQUIRE(cache != nullptr);

        OldHostBuffer<clay_brick_config> conf(ORIGINAL_OF(clay_brick_config, memory_budget));
        REQUIRE(clay_brick_cache_config(cache, conf.ptr()) == CLAY_OK);
        CHECK(conf.overrun() == 0);
        CHECK(conf.reported_size() == conf.declared);
        CHECK(conf.was_filled());

        OldHostBuffer<clay_brick_stats> stats(ORIGINAL_OF(clay_brick_stats, memory_budget));
        REQUIRE(clay_brick_cache_stats(cache, stats.ptr()) == CLAY_OK);
        CHECK(stats.overrun() == 0);
        CHECK(stats.reported_size() == stats.declared);

        clay_brick_cache_destroy(cache);
    }
}

TEST_CASE("c abi: a caller declaring the current layout still gets every field") {
    // The bound must not become a truncation for callers who are up to date:
    // the fields appended after each original layout are the ones a current
    // host is asking for, and silently leaving them zero would be the same bug
    // wearing the opposite sign.
    clay_brick_config cfg{};
    cfg.struct_size = sizeof(cfg);
    REQUIRE(clay_brick_config_defaults(&cfg) == CLAY_OK);
    cfg.colors = 1;
    clay_brick_cache* cache = clay_brick_cache_create(&cfg);
    REQUIRE(cache != nullptr);

    clay_brick_config back{};
    back.struct_size = sizeof(back);
    REQUIRE(clay_brick_cache_config(cache, &back) == CLAY_OK);
    CHECK(back.struct_size == sizeof(back));
    CHECK(back.colors == 1);  // the field appended after the original layout
    CHECK(back.dim == cfg.dim);
    CHECK(back.voxel_size == doctest::Approx(cfg.voxel_size));

    clay_brick_stats stats{};
    stats.struct_size = sizeof(stats);
    REQUIRE(clay_brick_cache_stats(cache, &stats) == CLAY_OK);
    CHECK(stats.struct_size == sizeof(stats));
    CHECK(stats.brick_bytes > 0);  // also appended after the original layout

    clay_brick_cache_destroy(cache);
}

TEST_CASE("c abi: a defaults call declares its size like every other descriptor") {
    // These two used to SET struct_size rather than read it, which reads as a
    // convenience and is the one case the prefix rule cannot cover: with
    // nothing declared there is no size to bound the fill against, so it wrote
    // sizeof as the library defines it into whatever the caller allocated.
    // clay_brick_config had already grown a field, so that was 8 real bytes
    // past the end of an older host's struct.
    //
    // Requiring struct_size does not rescue an already-compiled old host — it
    // declares nothing, so it now gets a refusal instead of corruption. That
    // is the whole of the improvement, and it is worth having.
    SUBCASE("a descriptor declaring no size is refused, not filled") {
        clay_brick_config cfg{};  // struct_size 0, as a caller predating the rule leaves it
        CHECK(clay_brick_config_defaults(&cfg) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(cfg.dim == 0);  // and nothing was written

        clay_stroke_preset preset{};
        CHECK(clay_stroke_preset_defaults(&preset) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(preset.radius == 0.0f);
    }

    SUBCASE("a size below the original layout is refused") {
        clay_brick_config cfg{};
        cfg.struct_size = 4;
        CHECK(clay_brick_config_defaults(&cfg) == CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("an old host's declared layout is filled and not exceeded") {
        OldHostBuffer<clay_brick_config> buf(ORIGINAL_OF(clay_brick_config, memory_budget));
        REQUIRE(clay_brick_config_defaults(buf.ptr()) == CLAY_OK);
        CHECK(buf.overrun() == 0);
        CHECK(buf.reported_size() == buf.declared);
        CHECK(buf.was_filled());

        OldHostBuffer<clay_stroke_preset> preset(ORIGINAL_OF(clay_stroke_preset, accumulation));
        REQUIRE(clay_stroke_preset_defaults(preset.ptr()) == CLAY_OK);
        CHECK(preset.overrun() == 0);
        CHECK(preset.reported_size() == preset.declared);
        CHECK(preset.was_filled());
    }

    SUBCASE("a current host still gets the appended fields") {
        clay_brick_config cfg{};
        cfg.struct_size = sizeof(cfg);
        REQUIRE(clay_brick_config_defaults(&cfg) == CLAY_OK);
        CHECK(cfg.struct_size == sizeof(cfg));
        CHECK(cfg.dim > 0);
        CHECK(cfg.voxel_size > 0.0f);
        // `colors` is the field that was appended; its default is off, so the
        // check that matters is that the cache accepts what defaults produced.
        clay_brick_cache* cache = clay_brick_cache_create(&cfg);
        CHECK(cache != nullptr);
        clay_brick_cache_destroy(cache);
    }
}

TEST_CASE("c abi: deserializing a preset is an output descriptor too") {
    // This site filled its descriptor by delegating to the defaults call, so
    // it matched no `*out = clay_thing{}` grep and was missed by the first
    // sweep. The header walk in tools/check_c_abi.py is what found it.
    clay_stroke_preset p{};
    p.struct_size = sizeof(p);
    REQUIRE(clay_stroke_preset_defaults(&p) == CLAY_OK);
    p.radius = 0.42f;
    p.seed = 77;

    std::size_t size = 0;
    REQUIRE(clay_stroke_preset_serialize(&p, nullptr, &size) == CLAY_OK);
    std::vector<std::uint8_t> bytes(size);
    std::size_t capacity = size;
    REQUIRE(clay_stroke_preset_serialize(&p, bytes.data(), &capacity) == CLAY_OK);

    SUBCASE("an old host's buffer is not overrun") {
        OldHostBuffer<clay_stroke_preset> buf(ORIGINAL_OF(clay_stroke_preset, accumulation));
        REQUIRE(clay_stroke_preset_deserialize(bytes.data(), bytes.size(), buf.ptr()) == CLAY_OK);
        CHECK(buf.overrun() == 0);
        CHECK(buf.reported_size() == buf.declared);
        CHECK(buf.was_filled());
    }

    SUBCASE("a current host round-trips every field") {
        clay_stroke_preset back{};
        back.struct_size = sizeof(back);
        REQUIRE(clay_stroke_preset_deserialize(bytes.data(), bytes.size(), &back) == CLAY_OK);
        CHECK(back.struct_size == sizeof(back));
        CHECK(back.radius == doctest::Approx(0.42f));
        CHECK(back.seed == 77);
    }

    SUBCASE("a descriptor declaring no size is refused") {
        clay_stroke_preset back{};
        CHECK(clay_stroke_preset_deserialize(bytes.data(), bytes.size(), &back) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }
}
