// A backend that provides most of what it could provide (#63, second half).
//
// The failure this covers appears on one device class — Apple Paravirtual
// GPUs, where clay_raycast would not compile while the evaluation kernels did
// — and neither CI nor this machine has one. So the partial backend here is a
// test double: it delegates to the CPU backend and refuses raycast, which is
// exactly the shape metal_backend reports when a pipeline is missing. That
// makes the registry, caps and ABI behaviour testable everywhere, and leaves
// only the Metal wiring to the device.
#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "clay.h"
#include "clay/eval/backend.h"
#include "clay/scene/document.h"
#include "kernel_utils.h"
#include "scene_utils.h"

using namespace clay;

namespace {

// Registered under its own name rather than shadowing "metal": the registry
// holds one entry per name and a second "metal" would make find() answer
// whichever came first, which would make this test's result depend on whether
// the build has Metal compiled in.
constexpr const char* kName = "partial-test";

class PartialBackend : public eval::Backend {
  public:
    const char* name() const override { return kName; }

    eval::BackendCaps caps() const override {
        eval::BackendCaps c;
        c.raycast = false;  // the pipeline that would not build
        return c;
    }

    eval::Status eval_points(const scene::Tape& tape, const eval::PointQuery& q,
                             const eval::PointResults& out) override {
        return cpu()->eval_points(tape, q, out);
    }
    eval::Status eval_grid(const scene::Tape& tape, const eval::GridQuery& q, float* out_values,
                           float* out_colors_rgb) override {
        return cpu()->eval_grid(tape, q, out_values, out_colors_rgb);
    }
    eval::Status raycast(const scene::Tape&, const eval::RayQuery&, eval::RayHit*) override {
        return eval::Status::Unsupported;
    }

  private:
    static eval::Backend* cpu() { return eval::Registry::instance().find("cpu"); }
};

// Registered on first use, NOT at static-initialization time.
//
// A namespace-scope object doing this ran Registry's constructor before main,
// which on a Metal build creates an MTLDevice before the process is ready for
// one: the whole binary segfaulted in 0.02 s, before a single test executed,
// on macOS only. Registry itself is lazy for this reason and a test has no
// business making it eager.
//
// The registry has no remove(), which is deliberate — nothing in the library
// removes a backend — so this leans on the double being harmless to leave in
// place: every other suite either names the backend it wants or skips one
// whose raycast is Unsupported.
eval::Backend* partial_backend() {
    eval::Registry& registry = eval::Registry::instance();
    if (eval::Backend* existing = registry.find(kName)) return existing;
    registry.add(std::make_unique<PartialBackend>());
    return registry.find(kName);
}

scene::Document sphere_document() {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("body");
    l.sdf->insert(clay_test::item(scene::Prim::sphere(1.0f), kernel::cf3(0, 0, 0)));
    return doc;
}

}  // namespace

TEST_CASE("a backend missing one pipeline still registers") {
    REQUIRE(partial_backend() != nullptr);

    // The assertion the issue is about: before this change a backend whose
    // raycast pipeline failed was DISCARDED, and clay_list_backends answered
    // exactly what a build with no such backend answers.
    size_t size = 0;
    REQUIRE(clay_list_backends(nullptr, &size) == CLAY_OK);
    std::vector<char> names(size);
    REQUIRE(clay_list_backends(names.data(), &size) == CLAY_OK);
    CHECK(std::string(names.data()).find(kName) != std::string::npos);
}

TEST_CASE("a partial backend reports the operation it cannot run") {
    REQUIRE(partial_backend() != nullptr);

    int32_t supported = -1;
    REQUIRE(clay_backend_supports(kName, CLAY_BACKEND_OP_EVAL_POINTS, &supported) == CLAY_OK);
    CHECK(supported == 1);
    REQUIRE(clay_backend_supports(kName, CLAY_BACKEND_OP_EVAL_GRID, &supported) == CLAY_OK);
    CHECK(supported == 1);
    REQUIRE(clay_backend_supports(kName, CLAY_BACKEND_OP_RAYCAST, &supported) == CLAY_OK);
    CHECK(supported == 0);
}

TEST_CASE("the operations a partial backend kept still agree with the reference") {
    // A backend that registers partially has to be WORTH registering: the
    // point of not discarding it is that what remains works.
    scene::Document doc = sphere_document();
    scene::Tape tape = scene::compile_document(doc);
    const float points[] = {0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.5f, 0.5f, 0.0f};
    const std::size_t count = 3;

    float mine[count] = {0, 0, 0}, reference[count] = {0, 0, 0};
    eval::PointQuery q{points, count, 1e-4f};
    eval::Backend* partial = partial_backend();
    REQUIRE(partial != nullptr);
    REQUIRE(partial->eval_points(tape, q, eval::PointResults{mine, nullptr, nullptr}) ==
            eval::Status::Ok);
    eval::eval_points_reference(tape, q, eval::PointResults{reference, nullptr, nullptr});
    for (std::size_t i = 0; i < count; ++i) CHECK(mine[i] == doctest::Approx(reference[i]));
}

TEST_CASE("the operation it cannot run refuses rather than failing") {
    // Unsupported, not DeviceError: a caller distinguishes "not from me, ask
    // someone else" from "this went wrong", and only the first is a fallback.
    scene::Document doc = sphere_document();
    scene::Tape tape = scene::compile_document(doc);
    float ray[6] = {0, 0, -5, 0, 0, 1};
    eval::RayQuery q{ray, 1, 0.0f, 1e6f, 1e-4f, 256};
    eval::RayHit hit{};
    eval::Backend* partial = partial_backend();
    REQUIRE(partial != nullptr);
    CHECK(partial->raycast(tape, q, &hit) == eval::Status::Unsupported);
}

TEST_CASE("clay_raycast is unaffected by a partial backend") {
    // It asks the registry for "cpu" by name, which is why a backend that
    // cannot raycast costs the ABI nothing. Stated as a test because it is the
    // fact the registration rule rests on.
    clay_document* doc = clay_document_create();
    REQUIRE(doc != nullptr);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
    float params[1] = {1.0f};
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, params, 1);
    REQUIRE(item != nullptr);
    clay_node_id node = 0;
    REQUIRE(clay_layer_add_item(doc, layer, item, &node) == CLAY_OK);
    clay_item_destroy(item);

    const float origin[3] = {0, 0, -5}, dir[3] = {0, 0, 1};
    int32_t did_hit = 0;
    float t = 0;
    CHECK(clay_raycast(doc, origin, dir, &did_hit, &t, nullptr, nullptr) == CLAY_OK);
    CHECK(did_hit == 1);
    clay_document_destroy(doc);
}

TEST_CASE("backend queries refuse what they cannot answer") {
    REQUIRE(partial_backend() != nullptr);

    int32_t supported = -1;
    CHECK(clay_backend_supports("no-such-backend", CLAY_BACKEND_OP_RAYCAST, &supported) ==
          CLAY_ERROR_NOT_FOUND);
    CHECK(clay_backend_supports(nullptr, CLAY_BACKEND_OP_RAYCAST, &supported) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_backend_supports(kName, CLAY_BACKEND_OP_RAYCAST, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_backend_supports(kName, static_cast<clay_backend_op>(99), &supported) ==
          CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("a healthy backend has nothing to say, and says it successfully") {
    size_t size = 0;
    REQUIRE(clay_backend_diagnostic("cpu", nullptr, &size) == CLAY_OK);
    CHECK(size == 1);  // just the NUL
    std::vector<char> buffer(size);
    REQUIRE(clay_backend_diagnostic("cpu", buffer.data(), &size) == CLAY_OK);
    CHECK(std::string(buffer.data()).empty());
}

TEST_CASE("a backend this build does not contain reads differently from a failed one") {
    // The distinction the issue asked for: "cpu" from clay_list_backends means
    // either "no GPU compiled in" or "one was discarded", and a host acts on
    // those differently.
    size_t size = 0;
    REQUIRE(clay_backend_diagnostic("no-such-backend", nullptr, &size) == CLAY_OK);
    std::vector<char> buffer(size);
    REQUIRE(clay_backend_diagnostic("no-such-backend", buffer.data(), &size) == CLAY_OK);
    const std::string text(buffer.data());
    CHECK(text.find("not compiled into this build") != std::string::npos);
}

TEST_CASE("the diagnostic follows the size-query pattern") {
    size_t size = 0;
    REQUIRE(clay_backend_diagnostic("no-such-backend", nullptr, &size) == CLAY_OK);
    REQUIRE(size > 1);
    size_t too_small = size - 1;
    std::vector<char> buffer(size);
    CHECK(clay_backend_diagnostic("no-such-backend", buffer.data(), &too_small) ==
          CLAY_ERROR_BUFFER_TOO_SMALL);
    CHECK(too_small == size);  // the needed size comes back
    CHECK(clay_backend_diagnostic(nullptr, buffer.data(), &size) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_backend_diagnostic("cpu", buffer.data(), nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
}
