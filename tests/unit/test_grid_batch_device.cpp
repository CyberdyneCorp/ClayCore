#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "clay/eval/backend.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"
#include "scene_utils.h"

// The default eval_grid_batch_device (evaluation-backends spec: batched grid
// evaluation amortizes device dispatch, device-buffer form). The default is
// the CONTRACT the Metal override is held to: it defines which slot each grid
// lands in and which batches are refused, so this suite pins it with a fake
// backend that records every per-grid call — no GPU required, so the slot
// arithmetic is checked on every platform, not just where Metal adopts.

using namespace clay;

namespace {

struct Call {
    const scene::Tape* tape;
    kernel::cfloat3 origin;
    void* values_handle;
    std::uint64_t values_offset;
    std::uint64_t values_size;
    void* colors_handle;
    std::uint64_t colors_offset;
    std::uint64_t colors_size;
};

// Answers eval_grid_device by recording the slot it was handed. Everything
// else is the interface's minimum.
class RecordingBackend final : public eval::Backend {
  public:
    std::vector<Call> calls;
    // The call index that fails, or -1 for none: error propagation must stop
    // the batch where the per-grid path stopped.
    int fail_at = -1;
    eval::Status failure = eval::Status::DeviceError;

    const char* name() const override { return "recording"; }
    eval::BackendCaps caps() const override { return {}; }
    eval::Status eval_points(const scene::Tape&, const eval::PointQuery&,
                             const eval::PointResults&) override {
        return eval::Status::Unsupported;
    }
    eval::Status eval_grid(const scene::Tape&, const eval::GridQuery&, float*,
                           float*) override {
        return eval::Status::Unsupported;
    }
    eval::Status raycast(const scene::Tape&, const eval::RayQuery&, eval::RayHit*) override {
        return eval::Status::Unsupported;
    }
    eval::Status eval_grid_device(const scene::Tape& tape, const eval::GridQuery& q,
                                  const eval::DeviceBuffer& values,
                                  const eval::DeviceBuffer& colors) override {
        if (fail_at >= 0 && calls.size() == static_cast<std::size_t>(fail_at)) return failure;
        calls.push_back(Call{&tape, q.origin, values.handle, values.offset, values.size,
                             colors.handle, colors.offset, colors.size});
        return eval::Status::Ok;
    }
};

scene::Tape small_tape() {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(clay_test::item(scene::Prim::sphere(0.5f), kernel::cf3(0, 0, 0)));
    return scene::compile_document(doc);
}

eval::GridBatchQuery batch_of(const std::vector<const scene::Tape*>& tapes,
                              const std::vector<kernel::cfloat3>& origins) {
    eval::GridBatchQuery q;
    q.tapes = tapes.data();
    q.origins = origins.data();
    q.spacing = 0.05f;
    q.nx = q.ny = q.nz = 8;
    q.count = tapes.size();
    return q;
}

}  // namespace

TEST_CASE("eval_grid_batch_device default: grid i lands at the host-memory batch's slot") {
    const scene::Tape tape = small_tape();
    const std::vector<const scene::Tape*> tapes = {&tape, &tape, &tape};
    const std::vector<kernel::cfloat3> origins = {kernel::cf3(0, 0, 0), kernel::cf3(1, 0, 0),
                                                  kernel::cf3(0, 2, 0)};
    const eval::GridBatchQuery q = batch_of(tapes, origins);
    const std::uint64_t per = 8 * 8 * 8;

    int values_mem = 0, colors_mem = 0;  // handles are opaque; identity is the check
    const std::uint64_t base = 256;      // a caller packing into a larger allocation
    eval::DeviceBuffer values{&values_mem, base, 3 * per * sizeof(float)};
    eval::DeviceBuffer colors{&colors_mem, base * 2, 3 * per * 3 * sizeof(float)};

    RecordingBackend b;
    REQUIRE(b.eval_grid_batch_device(q, values, colors) == eval::Status::Ok);
    REQUIRE(b.calls.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        const Call& c = b.calls[i];
        CHECK(c.tape == &tape);
        CHECK(c.origin.x == origins[i].x);
        CHECK(c.origin.y == origins[i].y);
        CHECK(c.origin.z == origins[i].z);
        // Brick i at values.offset + i * dim^3 floats, exactly as
        // clay_brick_cache_eval_requests_device documents its slots.
        CHECK(c.values_handle == &values_mem);
        CHECK(c.values_offset == base + i * per * sizeof(float));
        CHECK(c.values_size == per * sizeof(float));
        CHECK(c.colors_handle == &colors_mem);
        CHECK(c.colors_offset == base * 2 + i * per * 3 * sizeof(float));
        CHECK(c.colors_size == per * 3 * sizeof(float));
    }

    SUBCASE("no colours asked for means no colour slot handed down") {
        b.calls.clear();
        REQUIRE(b.eval_grid_batch_device(q, values, eval::DeviceBuffer{}) == eval::Status::Ok);
        REQUIRE(b.calls.size() == 3);
        for (const Call& c : b.calls) CHECK(c.colors_handle == nullptr);
    }
}

TEST_CASE("eval_grid_batch_device default: refusals leave nothing evaluated") {
    const scene::Tape tape = small_tape();
    const std::vector<const scene::Tape*> tapes = {&tape, &tape};
    const std::vector<kernel::cfloat3> origins = {kernel::cf3(0, 0, 0), kernel::cf3(1, 0, 0)};
    const eval::GridBatchQuery q = batch_of(tapes, origins);
    const std::uint64_t per = 8 * 8 * 8;
    int mem = 0;

    RecordingBackend b;

    SUBCASE("an empty batch is Ok and evaluates nothing") {
        eval::GridBatchQuery empty = q;
        empty.count = 0;
        CHECK(b.eval_grid_batch_device(empty, eval::DeviceBuffer{&mem, 0, 4},
                                       eval::DeviceBuffer{}) == eval::Status::Ok);
        CHECK(b.calls.empty());
    }

    SUBCASE("a null destination is refused") {
        CHECK(b.eval_grid_batch_device(q, eval::DeviceBuffer{}, eval::DeviceBuffer{}) ==
              eval::Status::InvalidInput);
        CHECK(b.calls.empty());
    }

    SUBCASE("a values buffer sized for one brick cannot take two") {
        eval::DeviceBuffer small{&mem, 0, per * sizeof(float)};
        CHECK(b.eval_grid_batch_device(q, small, eval::DeviceBuffer{}) ==
              eval::Status::InvalidInput);
        CHECK(b.calls.empty());
    }

    SUBCASE("a colour buffer sized for distances is three times short") {
        eval::DeviceBuffer values{&mem, 0, 2 * per * sizeof(float)};
        eval::DeviceBuffer colors{&mem, 0, 2 * per * sizeof(float)};
        CHECK(b.eval_grid_batch_device(q, values, colors) == eval::Status::InvalidInput);
        CHECK(b.calls.empty());
    }

    SUBCASE("a per-grid failure stops the batch with that grid's status") {
        eval::DeviceBuffer values{&mem, 0, 2 * per * sizeof(float)};
        b.fail_at = 1;
        b.failure = eval::Status::DeviceError;
        CHECK(b.eval_grid_batch_device(q, values, eval::DeviceBuffer{}) ==
              eval::Status::DeviceError);
        CHECK(b.calls.size() == 1);  // the first grid ran, the second refused
    }
}

TEST_CASE("eval_grid_batch_device default: a backend with no device path says Unsupported") {
    // The CPU backend serves no caller-owned device buffer, so the batched
    // device form must refuse exactly as the per-grid form does — callers
    // fall back to host memory, they do not get silence.
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    REQUIRE(cpu != nullptr);
    const scene::Tape tape = small_tape();
    const std::vector<const scene::Tape*> tapes = {&tape};
    const std::vector<kernel::cfloat3> origins = {kernel::cf3(0, 0, 0)};
    int mem = 0;
    eval::DeviceBuffer values{&mem, 0, 8 * 8 * 8 * sizeof(float)};
    CHECK(cpu->eval_grid_batch_device(batch_of(tapes, origins), values,
                                      eval::DeviceBuffer{}) == eval::Status::Unsupported);
}

TEST_CASE("device copy: a backend that does not move bytes says so, and says it up front") {
    // The device-destination refill keeps a seed only when the backend can
    // write a resumed brick into the caller's buffer and read a refilled one
    // back (#345). A backend without those has to be recognisable BEFORE the
    // refill commits to a plan — a discovery made half way through would leave
    // part of the batch resumed and part not, with no way back — so the
    // capability is REPORTED and the operations refuse. A backend that reports
    // nothing gets the whole-batch full walk, which is correct and is exactly
    // what it did before there was a resumed path at all.
    RecordingBackend b;
    CHECK(b.caps().device_copy == false);  // the default, for every backend
    int mem = 0;
    float host = 0.0f;
    const eval::DeviceBuffer slice{&mem, 0, sizeof(float)};
    CHECK(b.write_device_buffer(slice, &host, sizeof(float)) == eval::Status::Unsupported);
    CHECK(b.read_device_buffer(&host, slice, sizeof(float)) == eval::Status::Unsupported);

    // Including the CPU backend, which is the one every fallback lands on.
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    REQUIRE(cpu != nullptr);
    CHECK(cpu->caps().device_copy == false);
    CHECK(cpu->write_device_buffer(slice, &host, sizeof(float)) == eval::Status::Unsupported);
    CHECK(cpu->read_device_buffer(&host, slice, sizeof(float)) == eval::Status::Unsupported);
}
