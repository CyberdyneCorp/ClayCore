#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "clay.h"
#include "clay/kernel/tape.h"

// Issue #51 item B: a volume item must be renderable by whichever host preview
// path wins. This is not a hypothetical — ClaySpace's hand-written MSL ends
// with `default: return 1e9`, and CLAY_PRIM_VOLUME falls into that default, so
// every REGIONAL verb (hPolish, flatten, move-topological, smooth) is invisible
// in their live preview until the bake lands. Users report it as "the brush
// does nothing, then a second later it does".
//
// Both paths this release ships remove the host's kernel implementation
// entirely, so a volume ought to come along for free: on the analytic path its
// samples ride in the tape's blob and `ctape_volume` reads them, and on the
// atlas path the bricks were filled by evaluating that same tape. "Ought to" is
// the reason for this file — the claim is cheap to make and was worth checking,
// especially since #35 was a volume-versus-blob-offset bug that only showed
// when a volume did NOT sit at blob offset 0.
//
// So each case puts a blob-carrying item BEFORE the volume, which is the
// arrangement that made #35 visible.

namespace {

constexpr float kVoxel = 0.05f;
constexpr int kDim = 8;
constexpr std::size_t kSamples = 8 * 8 * 8;

struct Doc {
    clay_document* d = nullptr;
    clay_layer_id layer = 0;
    Doc() {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "body", &layer) == CLAY_OK);
    }
    ~Doc() { clay_document_destroy(d); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

// A stroke first — it carries out-of-line points, so the volume that follows
// does NOT sit at blob offset 0. That is the arrangement #35 needed.
void add_stroke(Doc& doc, float x) {
    const float radius = 0.12f;
    // A stroke takes NO params: its points are the out-of-line payload.
    clay_item* it = clay_item_create(CLAY_PRIM_STROKE, nullptr, 0);
    REQUIRE(it != nullptr);
    for (int i = 0; i < 4; ++i) {
        const float p[3] = {x, -0.3f + 0.2f * static_cast<float>(i), 0.0f};
        REQUIRE(clay_item_add_stroke_point(it, p, radius) == CLAY_OK);
    }
    clay_node_id id = 0;
    REQUIRE(clay_layer_add_item(doc.d, doc.layer, it, &id) == CLAY_OK);
    clay_item_destroy(it);
}

// What every regional verb produces: a sampled volume, added as an ordinary
// item. Built by baking a source document, which is clay_item_volume_from_document.
clay_node_id add_volume(Doc& doc, float radius, const float centre[3]) {
    Doc source;
    clay_item* sphere = clay_item_create(CLAY_PRIM_SPHERE, &radius, 1);
    REQUIRE(sphere != nullptr);
    REQUIRE(clay_item_set_position(sphere, centre) == CLAY_OK);
    clay_node_id sid = 0;
    REQUIRE(clay_layer_add_item(source.d, source.layer, sphere, &sid) == CLAY_OK);
    clay_item_destroy(sphere);

    // Zeroed but for struct_size and the cell size: every other field reads
    // "<= 0 means the default" on this descriptor.
    clay_volume_params vp{};
    vp.struct_size = static_cast<std::uint32_t>(sizeof vp);
    vp.cell_size = 0.05f;
    const float lo[3] = {centre[0] - radius - 0.2f, centre[1] - radius - 0.2f,
                         centre[2] - radius - 0.2f};
    const float hi[3] = {centre[0] + radius + 0.2f, centre[1] + radius + 0.2f,
                         centre[2] + radius + 0.2f};
    clay_item* vol = nullptr;
    REQUIRE(clay_item_volume_from_document(source.d, &vp, lo, hi, &vol) == CLAY_OK);
    REQUIRE(vol != nullptr);
    clay_node_id id = 0;
    REQUIRE(clay_layer_add_item(doc.d, doc.layer, vol, &id) == CLAY_OK);
    clay_item_destroy(vol);
    return id;
}

clay::kernel::CTapeValue eval_exported(const clay_tape* tape, const float p[3]) {
    std::size_t ni = 0, np = 0, nb = 0;
    const clay_tape_instr* instrs = clay_tape_instrs(tape, &ni);
    const float* params = clay_tape_params(tape, &np);
    const float* blob = clay_tape_blob(tape, &nb);
    return clay::kernel::ctape_eval(
        reinterpret_cast<const clay::kernel::CTapeInstr*>(instrs), static_cast<int>(ni), params,
        blob, clay::kernel::cf3(p[0], p[1], p[2]));
}

}  // namespace

TEST_CASE("host paths: a volume item is on the analytic path, blob and all") {
    Doc doc;
    add_stroke(doc, -0.6f);  // pushes the volume off blob offset 0 (#35)
    const float centre[3] = {0.3f, 0.0f, 0.0f};
    add_volume(doc, 0.25f, centre);

    clay_tape* tape = nullptr;
    REQUIRE(clay_tape_export(doc.d, nullptr, nullptr, &tape) == CLAY_OK);

    // The blob is where a volume's samples live, so an export that dropped it
    // would leave a host drawing nothing exactly where ClaySpace draws nothing.
    std::size_t nb = 0;
    REQUIRE(clay_tape_blob(tape, &nb) != nullptr);
    CHECK(nb > 0);

    // Inside the volume the field is negative, and the host's own evaluator
    // agrees with the library's — which is the whole claim.
    std::size_t inside_seen = 0;
    for (int i = -2; i <= 2; ++i)
        for (int j = -2; j <= 2; ++j)
            for (int k = -2; k <= 2; ++k) {
                const float p[3] = {centre[0] + static_cast<float>(i) * 0.06f,
                                    centre[1] + static_cast<float>(j) * 0.06f,
                                    centre[2] + static_cast<float>(k) * 0.06f};
                float expect = 0.0f;
                REQUIRE(clay_eval_points(doc.d, nullptr, p, 1, &expect, nullptr) == CLAY_OK);
                CHECK(eval_exported(tape, p).d == doctest::Approx(expect).epsilon(1e-4));
                if (expect < 0.0f) ++inside_seen;
            }
    // The volume is actually there: a host drawing this tape sees a surface,
    // not the 1e9 a missing primitive would contribute.
    CHECK(inside_seen > 0);

    SUBCASE("and removing the volume changes the field, so this is not vacuous") {
        Doc bare;
        add_stroke(bare, -0.6f);
        float without = 0.0f, with = 0.0f;
        REQUIRE(clay_eval_points(bare.d, nullptr, centre, 1, &without, nullptr) == CLAY_OK);
        REQUIRE(clay_eval_points(doc.d, nullptr, centre, 1, &with, nullptr) == CLAY_OK);
        CHECK(with < 0.0f);
        CHECK(without > 0.0f);
    }

    clay_tape_release(tape);
}

TEST_CASE("host paths: a volume item is in the brick atlas, colour included") {
    Doc doc;
    add_stroke(doc, -0.6f);
    const float centre[3] = {0.3f, 0.0f, 0.0f};
    add_volume(doc, 0.25f, centre);

    clay_brick_config cfg;
    cfg.struct_size = sizeof(cfg);
    REQUIRE(clay_brick_config_defaults(&cfg) == CLAY_OK);
    cfg.dim = kDim;
    cfg.voxel_size = kVoxel;
    cfg.colors = 1;
    clay_brick_cache* cache = clay_brick_cache_create(&cfg);
    REQUIRE(cache != nullptr);

    // Dirty only the volume's neighbourhood, so a pass here cannot be the
    // stroke's bricks standing in for the volume's.
    const float lo[3] = {centre[0] - 0.4f, centre[1] - 0.4f, centre[2] - 0.4f};
    const float hi[3] = {centre[0] + 0.4f, centre[1] + 0.4f, centre[2] + 0.4f};
    REQUIRE(clay_brick_cache_mark_dirty(cache, lo, hi) == CLAY_OK);

    constexpr std::size_t kChunk = 64;
    std::vector<clay_brick_request> reqs(kChunk);
    std::vector<float> values(kChunk * kSamples);
    std::vector<float> rgb(kChunk * kSamples * 3);
    std::vector<std::int32_t> results(kChunk);
    for (;;) {
        std::size_t count = kChunk, remaining = 0;
        REQUIRE(clay_brick_cache_take_dirty(cache, reqs.data(), &count, &remaining) == CLAY_OK);
        if (count == 0) break;
        REQUIRE(clay_brick_cache_eval_requests(doc.d, nullptr, reqs.data(), count, values.data(),
                                               count * kSamples, rgb.data(),
                                               count * kSamples * 3) == CLAY_OK);
        std::size_t accepted = 0;
        REQUIRE(clay_brick_cache_submit(cache, reqs.data(), count, values.data(),
                                        count * kSamples, rgb.data(), count * kSamples * 3,
                                        results.data(), &accepted) == CLAY_OK);
        if (remaining == 0) break;
    }

    // The volume's surface produced surface bricks. Without volumes on this
    // path the region would be uniformly outside and there would be none.
    std::size_t n = 0;
    REQUIRE(clay_brick_cache_surface_bricks(cache, nullptr, &n) == CLAY_OK);
    CHECK(n > 0);

    // And the band around the volume's surface is really there: raycast it.
    const float origin[3] = {centre[0] - 2.0f, centre[1], centre[2]};
    const float dir[3] = {1.0f, 0.0f, 0.0f};
    std::int32_t hit = 0;
    float t = 0.0f, pos[3] = {0, 0, 0};
    REQUIRE(clay_brick_cache_raycast(cache, origin, dir, &hit, &t, pos, nullptr) == CLAY_OK);
    CHECK(hit == 1);
    // it hit the volume's near side, not something off in the stroke
    CHECK(pos[0] == doctest::Approx(centre[0] - 0.25f).epsilon(0.15));

    // Meshing the same bricks yields geometry too, so the mesh path a host
    // falls back to for export agrees with what it previews.
    clay_brick_mesh_params mp{};
    mp.struct_size = static_cast<std::uint32_t>(sizeof mp);
    mp.normals = CLAY_NORMAL_FACE;
    mp.colors = 0;
    clay_mesh* mesh = nullptr;
    REQUIRE(clay_brick_cache_mesh(cache, nullptr, &mp, nullptr, 0, nullptr, &mesh) == CLAY_OK);
    CHECK(clay_mesh_vertex_count(mesh) > 0);
    clay_mesh_destroy(mesh);

    clay_brick_cache_destroy(cache);
}

// Issue #51 item C: the fixture must be reachable from a host's own test
// bundle. `clay parity-fixture` writes the same bytes, but an iOS test target
// links the framework and cannot shell out to a CLI that is not in it — so
// "the preview agrees with the field" stayed a property to hope for rather
// than a test to run per PR.
TEST_CASE("host paths: the parity fixture is reachable from the ABI") {
    std::size_t size = 0;
    REQUIRE(clay_parity_fixture_json(nullptr, &size) == CLAY_OK);
    CHECK(size > 1024);  // a real table, not an empty document

    std::vector<char> buf(size);
    std::size_t capacity = size;
    REQUIRE(clay_parity_fixture_json(buf.data(), &capacity) == CLAY_OK);
    CHECK(capacity == size);
    const std::string json(buf.data());
    CHECK(json.size() + 1 == size);

    // What a host needs in it to be able to gate anything: the tapes, the
    // probes, the reference values, the tolerances, and the step scale a
    // sphere tracer needs.
    for (const char* key : {"\"cases\"", "\"instrs\"", "\"params\"", "\"blob\"",
                            "\"points\"", "\"distance\"", "\"color\"",
                            "\"safe_step_scale\"", "\"is_exact\"", "\"tolerance\"",
                            "\"distance_abs\"", "\"color_abs\""}) {
        CAPTURE(key);
        CHECK(json.find(key) != std::string::npos);
    }
    // and the blend cases that catch the drift this fixture exists for
    CHECK(json.find("blend_union_quadratic") != std::string::npos);
    CHECK(json.find("composed_document") != std::string::npos);

    SUBCASE("deterministic, so a host can diff two runs") {
        std::vector<char> again(size);
        std::size_t again_capacity = size;
        REQUIRE(clay_parity_fixture_json(again.data(), &again_capacity) == CLAY_OK);
        CHECK(again_capacity == size);
        CHECK(std::memcmp(buf.data(), again.data(), size) == 0);
    }

    SUBCASE("every refusal") {
        CHECK(clay_parity_fixture_json(nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
        std::size_t small = size - 1;
        std::vector<char> tiny(size);
        CHECK(clay_parity_fixture_json(tiny.data(), &small) == CLAY_ERROR_BUFFER_TOO_SMALL);
        // and a refusal reports what was needed, so the retry is one call
        CHECK(small == size);
    }
}
