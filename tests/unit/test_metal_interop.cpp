#include <doctest/doctest.h>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "clay.h"
#include "clay/eval/backend.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"
#include "scene_utils.h"

// Metal device interop (evaluation-backends spec: batched grid evaluation
// amortizes device dispatch — the device-buffer form). The zero-copy refill
// (clay_brick_cache_eval_requests_device) is the call an iPad renderer makes
// every frame, and until the batched device form existed it dispatched one
// command buffer per brick — 25-165x behind the host-memory route on the
// same GPU. Speed is the benchmark harness's to hold; what this suite pins
// is the CONTRACT the batching must not bend:
//
//   1. The batched device form computes exactly what the host-memory batch
//      computes, BIT-identical — same backend, same device, same kernel;
//      anything but equality is a plumbing bug.
//   2. Brick i lands at the documented fixed slot, a caller's base offset is
//      honoured, and bytes outside the slots are never touched.
//   3. The C ABI's device path agrees with the host-memory path a renderer
//      would otherwise use, over a real cache's requests — far-empty bricks
//      (empty culled tapes, the zero-length slice in the concatenated
//      upload) included.
//
// The test creates a Metal device the way a HOST would and lends it to
// claycore; adopting a device claycore made would not exercise the case this
// feature exists for. Skips when no Metal device is present.

using namespace clay;
using clay_test::item;

namespace {

// A device the TEST owns, standing in for the host's renderer.
struct HostDevice {
    MTL::Device* device = nullptr;
    MTL::CommandQueue* queue = nullptr;
    bool ok = false;

    HostDevice() {
        device = MTL::CreateSystemDefaultDevice();
        if (!device) return;
        queue = device->newCommandQueue();
        ok = queue != nullptr;
    }
    ~HostDevice() {
        if (queue) queue->release();
        if (device) device->release();
    }
    HostDevice(const HostDevice&) = delete;
    HostDevice& operator=(const HostDevice&) = delete;

    eval::DeviceHandles handles() const {
        eval::DeviceHandles d;
        d.api = eval::DeviceApi::Metal;
        d.handles[0] = device;
        d.handles[1] = queue;
        return d;
    }

    MTL::Buffer* alloc(std::size_t bytes) const {
        return device->newBuffer(bytes, MTL::ResourceStorageModeShared);
    }
};

scene::Document scene_doc() {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::sphere(0.4f), kernel::cf3(-0.2f, 0.0f, 0.0f)));
    l.sdf->insert(item(scene::Prim::box(kernel::cf3(0.3f, 0.25f, 0.2f)),
                       kernel::cf3(0.3f, 0.1f, 0.0f)));
    return doc;
}

float rel_err(float a, float b) {
    const float scale = std::max(std::max(std::fabs(a), std::fabs(b)), 1.0f);
    return std::fabs(a - b) / scale;
}

// A dab, kept so the oracle document can be rebuilt item for item.
struct Dab {
    float radius;
    float pos[3];
};

clay_document* build(const std::vector<Dab>& dabs, clay_layer_id* out_layer) {
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
    for (const Dab& d : dabs) {
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &d.radius, 1);
        REQUIRE(it != nullptr);
        REQUIRE(clay_item_set_position(it, d.pos) == CLAY_OK);
        REQUIRE(clay_layer_add_item(doc, layer, it, nullptr) == CLAY_OK);
        clay_item_destroy(it);
    }
    if (out_layer) *out_layer = layer;
    return doc;
}

// A window of bricks over the sphere's equator, where every brick either
// straddles the surface or lies inside it. A window that runs off the shape
// holds bricks whose culled prefix produced nothing at all, which are
// correctly walked in full for ever — a legitimate refusal that would sit in
// the counter below pretending to be the defect it is gating.
std::vector<clay_brick_request> equator_window(int n) {
    std::vector<clay_brick_request> reqs(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        std::memset(&reqs[static_cast<std::size_t>(i)], 0, sizeof(clay_brick_request));
        clay_brick_request& r = reqs[static_cast<std::size_t>(i)];
        r.key[0] = i - 1;
        r.key[1] = 0;
        r.key[2] = 0;
        for (int a = 0; a < 3; ++a)
            r.origin[a] = static_cast<float>(r.key[a]) * 8 * 0.05f;
        r.spacing = 0.05f;
        r.dims[0] = r.dims[1] = r.dims[2] = 8;
        r.band = 0.15f;
    }
    return reqs;
}

}  // namespace

TEST_CASE("metal interop: the batched device form is the host-memory batch, bit for bit") {
    HostDevice host;
    if (!host.ok) {
        MESSAGE("no Metal device; skipping");
        return;
    }
    std::unique_ptr<eval::Backend> adopted = eval::make_backend("metal", host.handles());
    REQUIRE(static_cast<bool>(adopted));

    const scene::Document doc = scene_doc();
    // Per-brick culled tapes, the refill shape — including one culled to a
    // region far from every item, whose tape is EMPTY: the zero-length slice
    // in the concatenated upload.
    const float spacing = 0.05f;
    const int n = 8;
    const std::size_t per = static_cast<std::size_t>(n) * n * n;
    std::vector<kernel::cfloat3> origins = {
        kernel::cf3(-0.4f, -0.2f, -0.2f), kernel::cf3(0.0f, -0.2f, -0.2f),
        kernel::cf3(0.2f, 0.0f, -0.1f),   kernel::cf3(5.0f, 5.0f, 5.0f),
        kernel::cf3(-0.2f, 0.0f, 0.0f),
    };
    std::vector<scene::Tape> tapes;
    std::vector<const scene::Tape*> tape_ptrs;
    tapes.reserve(origins.size());  // the pointers below must survive growth
    for (const kernel::cfloat3& o : origins) {
        const float side = spacing * static_cast<float>(n);
        scene::CullRegion cull{math::Aabb{o, o + kernel::cf3(side, side, side)}.dilated(0.15f)};
        tapes.push_back(scene::compile_document(doc, &cull));
        tape_ptrs.push_back(&tapes.back());
    }
    CHECK(tapes[3].instrs.empty());  // the far brick really is the empty-tape case

    eval::GridBatchQuery q;
    q.tapes = tape_ptrs.data();
    q.origins = origins.data();
    q.spacing = spacing;
    q.nx = q.ny = q.nz = n;
    q.count = origins.size();
    const std::size_t total = per * q.count;

    // The reference: the host-memory batch through the SAME adopted backend.
    std::vector<float> host_values(total), host_colors(total * 3);
    REQUIRE(adopted->eval_grid_batch(q, host_values.data(), host_colors.data()) ==
            eval::Status::Ok);

    // The caller packs results into a larger allocation at an offset — the
    // normal case for an atlas, not an edge one.
    const std::size_t base = 64 * sizeof(float);
    MTL::Buffer* values = host.alloc(base + total * sizeof(float));
    MTL::Buffer* colors = host.alloc(base + total * 3 * sizeof(float));
    REQUIRE(values != nullptr);
    REQUIRE(colors != nullptr);
    std::memset(values->contents(), 0xCD, base + total * sizeof(float));
    std::memset(colors->contents(), 0xCD, base + total * 3 * sizeof(float));

    eval::DeviceBuffer dst{values, base, total * sizeof(float)};
    eval::DeviceBuffer dst_colors{colors, base, total * 3 * sizeof(float)};
    REQUIRE(adopted->eval_grid_batch_device(q, dst, dst_colors) == eval::Status::Ok);

    const auto* raw = static_cast<const unsigned char*>(values->contents());
    const auto* raw_colors = static_cast<const unsigned char*>(colors->contents());
    const float* got = reinterpret_cast<const float*>(raw + base);
    const float* got_colors = reinterpret_cast<const float*>(raw_colors + base);
    for (std::size_t i = 0; i < total; ++i) CHECK(got[i] == host_values[i]);
    for (std::size_t i = 0; i < total * 3; ++i) CHECK(got_colors[i] == host_colors[i]);
    // and nothing before the offset was touched
    for (std::size_t b = 0; b < base; ++b) {
        CHECK(raw[b] == 0xCD);
        CHECK(raw_colors[b] == 0xCD);
    }

    SUBCASE("an undersized destination is refused with nothing written") {
        MTL::Buffer* small = host.alloc(total * sizeof(float));
        REQUIRE(small != nullptr);
        std::memset(small->contents(), 0xCD, total * sizeof(float));
        eval::DeviceBuffer short_dst{small, 0, total * sizeof(float) - 4};
        CHECK(adopted->eval_grid_batch_device(q, short_dst, eval::DeviceBuffer{}) ==
              eval::Status::InvalidInput);
        const auto* bytes = static_cast<const unsigned char*>(small->contents());
        for (std::size_t b = 0; b < total * sizeof(float); ++b) CHECK(bytes[b] == 0xCD);
        // colours too: a colour buffer sized for distances is three times short
        eval::DeviceBuffer short_colors{small, 0, total * sizeof(float)};
        CHECK(adopted->eval_grid_batch_device(q, dst, short_colors) ==
              eval::Status::InvalidInput);
        small->release();
    }

    SUBCASE("an empty batch is Ok") {
        eval::GridBatchQuery empty = q;
        empty.count = 0;
        CHECK(adopted->eval_grid_batch_device(empty, dst, eval::DeviceBuffer{}) ==
              eval::Status::Ok);
    }

    SUBCASE("a backend that owns its device refuses: an MTLBuffer belongs to its maker") {
        if (eval::Backend* owned = eval::Registry::instance().find("metal"))
            CHECK(owned->eval_grid_batch_device(q, dst, eval::DeviceBuffer{}) ==
                  eval::Status::Unsupported);
    }

    values->release();
    colors->release();
}

TEST_CASE("metal interop: the C ABI device refill agrees with the host-memory refill") {
    HostDevice host;
    if (!host.ok) {
        MESSAGE("no Metal device; skipping");
        return;
    }

    clay_device_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.api = CLAY_DEVICE_API_METAL;
    desc.handles[0] = host.device;
    desc.handles[1] = host.queue;
    clay_device* cdev = clay_device_adopt(&desc);
    REQUIRE(cdev != nullptr);
    REQUIRE(std::string(clay_device_backend_name(cdev)) == "metal");

    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
    for (int i = 0; i < 3; ++i) {
        float r = 0.25f;
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        REQUIRE(it != nullptr);
        const float pos[3] = {0.2f * static_cast<float>(i), 0.0f, 0.0f};
        REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
        REQUIRE(clay_layer_add_item(doc, layer, it, nullptr) == CLAY_OK);
        clay_item_destroy(it);
    }

    clay_brick_config cfg;
    cfg.struct_size = sizeof(cfg);
    REQUIRE(clay_brick_config_defaults(&cfg) == CLAY_OK);
    cfg.dim = 8;
    cfg.voxel_size = 0.05f;
    clay_brick_cache* cache = clay_brick_cache_create(&cfg);
    REQUIRE(cache != nullptr);
    // Wider than the geometry, so the batch spans surface, inside, outside
    // AND far-empty bricks — the empty culled tape rides in the same batch.
    const float lo[3] = {-0.6f, -0.6f, -0.6f}, hi[3] = {1.2f, 0.6f, 0.6f};
    REQUIRE(clay_brick_cache_mark_dirty(cache, lo, hi) == CLAY_OK);

    std::vector<clay_brick_request> requests(4096);
    std::size_t count = requests.size(), remaining = 0;
    REQUIRE(clay_brick_cache_take_dirty(cache, requests.data(), &count, &remaining) == CLAY_OK);
    REQUIRE(remaining == 0);
    REQUIRE(count > 30);  // enough bricks that the batch is a real batch
    const std::size_t per = 8 * 8 * 8;
    const std::size_t total = count * per;

    // The reference: the registered "metal" backend's host-memory refill —
    // the route a renderer takes today, and the values the zero-copy path
    // promises to reproduce exactly.
    std::vector<float> host_values(total), host_colors(total * 3);
    REQUIRE(clay_brick_cache_eval_requests(doc, "metal", requests.data(), count,
                                           host_values.data(), total, host_colors.data(),
                                           total * 3) == CLAY_OK);

    MTL::Buffer* values = host.alloc(total * sizeof(float));
    MTL::Buffer* colors = host.alloc(total * 3 * sizeof(float));
    REQUIRE(values != nullptr);
    REQUIRE(colors != nullptr);

    clay_device_buffer dst;
    std::memset(&dst, 0, sizeof dst);
    dst.struct_size = sizeof dst;
    dst.handle = values;
    dst.offset = 0;
    dst.size = total * sizeof(float);
    clay_device_buffer dst_colors = dst;
    dst_colors.handle = colors;
    dst_colors.size = total * 3 * sizeof(float);

    REQUIRE(clay_brick_cache_eval_requests_device(doc, cdev, requests.data(), count, &dst,
                                                  &dst_colors) == CLAY_OK);

    const float* got = static_cast<const float*>(values->contents());
    const float* got_colors = static_cast<const float*>(colors->contents());
    std::size_t value_diffs = 0, color_diffs = 0;
    for (std::size_t i = 0; i < total; ++i) value_diffs += got[i] != host_values[i];
    for (std::size_t i = 0; i < total * 3; ++i) color_diffs += got_colors[i] != host_colors[i];
    CHECK(value_diffs == 0);
    CHECK(color_diffs == 0);

    values->release();
    colors->release();
    clay_brick_cache_destroy(cache);
    clay_document_destroy(doc);
    clay_device_release(cdev);
}

// Issue #350. The resumed device refill landed on Vulkan (#345) and fell back to
// the full walk on Metal, because `caps().device_copy` was false there: the two
// transfers the resume needs — a host-computed brick INTO the caller's slot,
// and a fully walked brick back OUT so it seeds the next dab — had no Metal
// implementation. Everything else about the resume is in the C ABI and is
// backend agnostic, so this case is the Vulkan one written for the platform the
// zero-copy path exists for.
//
// Written the same way deliberately: the two paths are value-identical by
// contract, so nothing about the VALUES can witness that the fast route fired.
// clay_document_resume_stats is the witness, and without it these checks pass
// just as happily on the fallback they did before this change.
TEST_CASE("metal interop: a stroke resumes into the caller's buffer and matches a full walk") {
    HostDevice host;
    if (!host.ok) {
        MESSAGE("no Metal device; skipping");
        return;
    }
    clay_device_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.api = CLAY_DEVICE_API_METAL;
    desc.handles[0] = host.device;
    desc.handles[1] = host.queue;
    clay_device* dev = clay_device_adopt(&desc);
    REQUIRE(dev != nullptr);

    // A sculpt with enough history that a full walk and a suffix are visibly
    // different amounts of work, then a stroke of dabs on top of it.
    std::vector<Dab> dabs;
    dabs.push_back(Dab{1.0f, {0.0f, 0.0f, 0.0f}});
    for (int i = 0; i < 200; ++i) {
        const float t = static_cast<float>(i) * 0.031f;
        dabs.push_back(Dab{0.08f, {std::cos(t), std::sin(t) * 0.4f, std::sin(t * 1.7f) * 0.4f}});
    }

    clay_layer_id layer = 0;
    clay_document* doc = build(dabs, &layer);
    const std::vector<clay_brick_request> reqs = equator_window(6);
    const std::size_t count = reqs.size();
    const std::size_t per = 8 * 8 * 8;
    const std::size_t total = count * per;

    MTL::Buffer* values = host.alloc(total * sizeof(float));
    MTL::Buffer* colors = host.alloc(total * 3 * sizeof(float));
    REQUIRE(values != nullptr);
    REQUIRE(colors != nullptr);
    clay_device_buffer dst;
    std::memset(&dst, 0, sizeof dst);
    dst.struct_size = sizeof dst;
    dst.handle = values;
    dst.size = total * sizeof(float);
    clay_device_buffer dst_colors = dst;
    dst_colors.handle = colors;
    dst_colors.size = total * 3 * sizeof(float);

    // Primed in the MIDDLE, and through the host-memory entry point, so the
    // first device call has to hold three cases apart at once: a run of
    // resumable bricks that does NOT start at brick 0, and un-resumable bricks
    // on both sides of it. Every destination here is a fixed slot, so a run
    // that lands at the run's own offset and a run that lands at the buffer's
    // start differ only when the run does not start at 0 — which is the shape a
    // moving dirty window is in every dab but the first. The seed store is the
    // DOCUMENT's, not either entry point's, so priming through the host path is
    // also the check that one refill's seeds serve the other's.
    std::vector<float> prime(4 * per), prime_rgb(4 * per * 3);
    REQUIRE(clay_brick_cache_eval_requests(doc, "cpu", reqs.data() + 1, 4, prime.data(),
                                           prime.size(), prime_rgb.data(),
                                           prime_rgb.size()) == CLAY_OK);

    std::vector<float> oracle(total), oracle_rgb(total * 3);
    float worst = 0.0f, worst_rgb = 0.0f;
    for (int step = 0; step < 6; ++step) {
        // One dab, appended — which is the case a suffix exists for.
        const float t = static_cast<float>(step) * 0.05f;
        dabs.push_back(Dab{0.07f, {0.95f, t, -0.1f}});
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &dabs.back().radius, 1);
        REQUIRE(it != nullptr);
        REQUIRE(clay_item_set_position(it, dabs.back().pos) == CLAY_OK);
        REQUIRE(clay_layer_add_item(doc, layer, it, nullptr) == CLAY_OK);
        clay_item_destroy(it);

        REQUIRE(clay_brick_cache_eval_requests_device(doc, dev, reqs.data(), count, &dst,
                                                      &dst_colors) == CLAY_OK);

        // The oracle: the same items in a document that has never been
        // refilled, so its CPU refill walks the whole edit list.
        clay_document* fresh = build(dabs, nullptr);
        REQUIRE(clay_brick_cache_eval_requests(fresh, "cpu", reqs.data(), count, oracle.data(),
                                               total, oracle_rgb.data(), total * 3) == CLAY_OK);
        clay_document_destroy(fresh);

        const float* got = static_cast<const float*>(values->contents());
        const float* got_rgb = static_cast<const float*>(colors->contents());
        for (std::size_t i = 0; i < total; ++i) worst = std::max(worst, rel_err(got[i], oracle[i]));
        for (std::size_t i = 0; i < total * 3; ++i)
            worst_rgb = std::max(worst_rgb, rel_err(got_rgb[i], oracle_rgb[i]));
    }
    // The parity suite's standard for a GPU backend against the CPU scalar
    // reference (test_parity.cpp): 1e-4 relative. Not bit-identity, because the
    // half of each answer that was NOT resumed came off the GPU and the oracle
    // is the CPU; identity is the wrong bar for that pair, and it is the bar the
    // device path already had to meet before it resumed anything.
    CHECK(worst <= 1e-4f);
    CHECK(worst_rgb <= 1e-4f);

    // And it fired. Before this change `caps().device_copy` was false on Metal
    // and this counter stayed at zero while every check above still passed.
    clay_resume_stats rs;
    std::memset(&rs, 0, sizeof rs);
    rs.struct_size = sizeof rs;
    REQUIRE(clay_document_resume_stats(doc, &rs) == CLAY_OK);
    CHECK(rs.resumed_bricks > 0);
    // The bricks outside the primed middle were walked in full on the first
    // device call, which is what left them a seed — and a seed can only have
    // been left by read_device_buffer, the other half of this change.
    CHECK(rs.refilled_bricks >= 2);
    // and carried the stroke rather than firing once. Not "every brick after
    // the first call": the cull pad GROWS as a stroke reaches outward, and a
    // seed taken under a smaller one is correctly refused and refilled, which is
    // a legitimate refusal the host path makes too.
    CHECK(rs.resumed_bricks > rs.refilled_bricks);
    CHECK(rs.entries >= count);

    values->release();
    colors->release();
    clay_document_destroy(doc);
    clay_device_release(dev);
}

// The capability itself, and its one restriction: device_copy names a buffer
// the CALLER lent us, so a backend that made its own device must not claim it —
// an MTLBuffer belongs to the device that allocated it, and the registered
// "metal" backend's device is not the caller's.
TEST_CASE("metal interop: only an adopted backend serves a device copy") {
    HostDevice host;
    if (!host.ok) {
        MESSAGE("no Metal device; skipping");
        return;
    }
    std::unique_ptr<eval::Backend> adopted = eval::make_backend("metal", host.handles());
    REQUIRE(adopted != nullptr);
    CHECK(adopted->caps().device_copy);

    eval::Backend* registered = eval::Registry::instance().find("metal");
    REQUIRE(registered != nullptr);
    CHECK(registered->caps().device_copy == false);

    // A round trip through the caller's own buffer, at a non-zero offset, so
    // the slice arithmetic is exercised rather than only the whole-buffer case
    // — a refill writes every run at the run's own offset.
    constexpr std::size_t kFloats = 512;      // one 8^3 brick
    constexpr std::size_t kSkip = 64;         // the run does not start at 0
    MTL::Buffer* buf = host.alloc((kFloats + kSkip) * sizeof(float));
    REQUIRE(buf != nullptr);
    std::memset(buf->contents(), 0, (kFloats + kSkip) * sizeof(float));

    std::vector<float> sent(kFloats);
    for (std::size_t i = 0; i < kFloats; ++i) sent[i] = static_cast<float>(i) * 0.25f - 3.0f;

    eval::DeviceBuffer slice;
    slice.handle = buf;
    slice.offset = kSkip * sizeof(float);
    slice.size = kFloats * sizeof(float);
    REQUIRE(adopted->write_device_buffer(slice, sent.data(), kFloats * sizeof(float)) ==
            eval::Status::Ok);

    std::vector<float> got(kFloats, 0.0f);
    REQUIRE(adopted->read_device_buffer(got.data(), slice, kFloats * sizeof(float)) ==
            eval::Status::Ok);
    CHECK(got == sent);

    // Bytes BEFORE the slice were not touched: a copy that ignored the offset
    // would land at 0 and still pass the round trip above.
    const float* raw = static_cast<const float*>(buf->contents());
    std::size_t before_touched = 0;
    for (std::size_t i = 0; i < kSkip; ++i) before_touched += raw[i] != 0.0f;
    CHECK(before_touched == 0);

    // Refused rather than truncated: past what the slice says it holds, and a
    // partial float. Both would otherwise be silent corruption.
    CHECK(adopted->write_device_buffer(slice, sent.data(), (kFloats + 1) * sizeof(float)) ==
          eval::Status::InvalidInput);
    CHECK(adopted->read_device_buffer(got.data(), slice, kFloats * sizeof(float) - 1) ==
          eval::Status::InvalidInput);
    // Zero bytes is a no-op, not an error.
    CHECK(adopted->write_device_buffer(slice, sent.data(), 0) == eval::Status::Ok);

    // The registered backend refuses a foreign buffer outright, on the same
    // terms its eval_grid_device does.
    float one = 0.0f;
    CHECK(registered->write_device_buffer(slice, &one, sizeof(float)) ==
          eval::Status::Unsupported);
    CHECK(registered->read_device_buffer(&one, slice, sizeof(float)) ==
          eval::Status::Unsupported);

    buf->release();
}
