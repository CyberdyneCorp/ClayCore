#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "clay/brick/cache.h"
#include "clay/eval/backend.h"
#include "clay/kernel/field.h"
#include "clay/mesh/dual_contouring.h"
#include "clay/mesh/marching.h"
#include "clay/scene/bounds.h"
#include "clay/scene/tape.h"
#include "scene_utils.h"

// eval_points_batch (evaluation-backends spec): many point runs, each against
// its own tape, in one call. The CPU backend dispatches the flattened batch
// across its pool; the Backend default loops eval_points per run. Both must
// produce byte-identical results — per-point work is independent of how the
// batch is split — and the brick-mesh attribute pass that rides on it must
// leave the mesh byte-identical to the serial per-group evaluation it
// replaced.

using namespace clay;
using kernel::cf3;
using kernel::cfloat3;
using clay_test::gnarly_document;

namespace {

// Runs the Backend DEFAULT eval_points_batch on top of the CPU backend's
// eval_points: the parity reference for the CPU override.
class DefaultBatch final : public eval::Backend {
  public:
    explicit DefaultBatch(eval::Backend* cpu) : cpu_(cpu) {}
    const char* name() const override { return "default-batch"; }
    eval::BackendCaps caps() const override { return cpu_->caps(); }
    eval::Status eval_points(const scene::Tape& tape, const eval::PointQuery& q,
                             const eval::PointResults& out) override {
        return cpu_->eval_points(tape, q, out);
    }
    eval::Status eval_grid(const scene::Tape& tape, const eval::GridQuery& q, float* out_values,
                           float* out_colors_rgb) override {
        return cpu_->eval_grid(tape, q, out_values, out_colors_rgb);
    }
    eval::Status raycast(const scene::Tape& tape, const eval::RayQuery& q,
                         eval::RayHit* hits) override {
        return cpu_->raycast(tape, q, hits);
    }

  private:
    eval::Backend* cpu_;
};

std::vector<float> lattice_points(std::size_t count, cfloat3 center, float extent) {
    std::vector<float> pts(count * 3);
    std::uint64_t state = 424242;
    for (std::size_t i = 0; i < count * 3; ++i) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        const float u = static_cast<float>((state >> 40) & 0xFFFFFF) / 16777216.0f;
        const float c = (i % 3 == 0) ? center.x : (i % 3 == 1 ? center.y : center.z);
        pts[i] = c + (u * 2.0f - 1.0f) * extent;
    }
    return pts;
}

struct Batch {
    std::vector<scene::Tape> tapes;
    std::vector<const scene::Tape*> ptrs;
    std::vector<std::size_t> offsets;
    std::vector<float> points;
    eval::PointBatchQuery query() const {
        eval::PointBatchQuery q;
        q.tapes = ptrs.data();
        q.offsets = offsets.data();
        q.points_xyz = points.data();
        q.count = tapes.size();
        q.gradient_eps = 1e-4f;
        return q;
    }
};

// Culled tapes over four regions of the gnarly scene, with run lengths that
// exercise chunk boundaries: empty runs, one-point runs, and runs far larger
// than a pool chunk.
Batch gnarly_batch() {
    Batch b;
    const scene::Document doc = gnarly_document();
    const cfloat3 centers[4] = {cf3(0, 0, 0), cf3(0.8f, 0.3f, 0), cf3(-0.7f, -0.5f, 0.3f),
                                cf3(0, 0.8f, 0)};
    const std::size_t lens[4] = {0, 1, 700, 130};
    b.offsets.push_back(0);
    for (int i = 0; i < 4; ++i) {
        const scene::CullRegion cull{
            math::Aabb{centers[i] - cf3(0.5f, 0.5f, 0.5f), centers[i] + cf3(0.5f, 0.5f, 0.5f)}};
        b.tapes.push_back(scene::compile_document(doc, &cull));
        std::vector<float> pts = lattice_points(lens[i], centers[i], 0.45f);
        b.points.insert(b.points.end(), pts.begin(), pts.end());
        b.offsets.push_back(b.offsets.back() + lens[i]);
    }
    for (const scene::Tape& t : b.tapes) b.ptrs.push_back(&t);
    return b;
}

struct Outputs {
    std::vector<float> d, g, c;
    explicit Outputs(std::size_t n) : d(n, -1e9f), g(n * 3, -1e9f), c(n * 3, -1e9f) {}
    eval::PointResults results() {
        eval::PointResults out;
        out.distances = d.data();
        out.gradients_xyz = g.data();
        out.colors_rgb = c.data();
        return out;
    }
};

}  // namespace

TEST_CASE("eval_points_batch: the CPU override matches the default byte for byte") {
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    REQUIRE(cpu != nullptr);
    const Batch batch = gnarly_batch();
    const std::size_t total = batch.offsets.back();
    REQUIRE(total > 0);

    Outputs got(total), want(total);
    REQUIRE(cpu->eval_points_batch(batch.query(), got.results()) == eval::Status::Ok);
    DefaultBatch reference(cpu);
    REQUIRE(reference.eval_points_batch(batch.query(), want.results()) == eval::Status::Ok);

    CHECK(std::memcmp(got.d.data(), want.d.data(), total * sizeof(float)) == 0);
    CHECK(std::memcmp(got.g.data(), want.g.data(), total * 3 * sizeof(float)) == 0);
    CHECK(std::memcmp(got.c.data(), want.c.data(), total * 3 * sizeof(float)) == 0);

    // And per-run eval_points agrees: the batch is exactly its runs.
    Outputs per(total);
    eval::PointResults out = per.results();
    for (std::size_t i = 0; i < batch.tapes.size(); ++i) {
        eval::PointQuery q;
        q.points_xyz = batch.points.data() + batch.offsets[i] * 3;
        q.count = batch.offsets[i + 1] - batch.offsets[i];
        q.gradient_eps = 1e-4f;
        if (q.count == 0) continue;
        eval::PointResults slice;
        slice.distances = out.distances + batch.offsets[i];
        slice.gradients_xyz = out.gradients_xyz + batch.offsets[i] * 3;
        slice.colors_rgb = out.colors_rgb + batch.offsets[i] * 3;
        REQUIRE(cpu->eval_points(*batch.ptrs[i], q, slice) == eval::Status::Ok);
    }
    CHECK(std::memcmp(got.d.data(), per.d.data(), total * sizeof(float)) == 0);
    CHECK(std::memcmp(got.g.data(), per.g.data(), total * 3 * sizeof(float)) == 0);
    CHECK(std::memcmp(got.c.data(), per.c.data(), total * 3 * sizeof(float)) == 0);
}

TEST_CASE("eval_points_batch: input validation") {
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    REQUIRE(cpu != nullptr);
    const Batch batch = gnarly_batch();
    Outputs out(batch.offsets.back());

    SUBCASE("an empty batch is Ok") {
        eval::PointBatchQuery q = batch.query();
        q.count = 0;
        CHECK(cpu->eval_points_batch(q, out.results()) == eval::Status::Ok);
    }
    SUBCASE("null buffers are refused") {
        eval::PointBatchQuery q = batch.query();
        q.tapes = nullptr;
        CHECK(cpu->eval_points_batch(q, out.results()) == eval::Status::InvalidInput);
        q = batch.query();
        q.offsets = nullptr;
        CHECK(cpu->eval_points_batch(q, out.results()) == eval::Status::InvalidInput);
        q = batch.query();
        q.points_xyz = nullptr;
        CHECK(cpu->eval_points_batch(q, out.results()) == eval::Status::InvalidInput);
        eval::PointResults no_dist = out.results();
        no_dist.distances = nullptr;
        CHECK(cpu->eval_points_batch(batch.query(), no_dist) == eval::Status::InvalidInput);
    }
    SUBCASE("a null tape is refused") {
        std::vector<const scene::Tape*> ptrs = batch.ptrs;
        ptrs[2] = nullptr;
        eval::PointBatchQuery q = batch.query();
        q.tapes = ptrs.data();
        CHECK(cpu->eval_points_batch(q, out.results()) == eval::Status::InvalidInput);
    }
    SUBCASE("decreasing offsets are refused") {
        std::vector<std::size_t> offsets = batch.offsets;
        std::swap(offsets[1], offsets[2]);
        eval::PointBatchQuery q = batch.query();
        q.offsets = offsets.data();
        CHECK(cpu->eval_points_batch(q, out.results()) == eval::Status::InvalidInput);
    }
}

// Regression for the batched brick-mesh attribute pass: the mesh must be
// byte-identical to what the serial per-group loop it replaced produced —
// same per-brick culled tapes, same points, same taps, one at a time. This
// reference IS that loop: group vertices by the brick owning their position,
// compile one culled tape per group (compile_document without index or plan
// emits byte-identical tapes, per the cull-index contract), evaluate
// serially, and memcmp every copied-out buffer.
TEST_CASE("brick-mesh attributes: batched evaluation leaves the mesh byte-identical") {
    scene::Document doc = gnarly_document();
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    REQUIRE(cpu != nullptr);
    brick::BrickCache cache(brick::BrickConfig{8, 0.08f, 3, 0});
    cache.mark_dirty(scene::layer_influence_bound(doc.layers[0]));
    for (const brick::BrickRequest& req : cache.take_dirty()) {
        scene::CullRegion cull{cache.cull_region(req.key)};
        scene::Tape tape = scene::compile_document(doc, &cull);
        std::vector<float> values(static_cast<std::size_t>(req.grid.nx) * req.grid.ny *
                                  req.grid.nz);
        REQUIRE(cpu->eval_grid(tape, req.grid, values.data()) == eval::Status::Ok);
        cache.submit(req, values.data());
    }

    mesh::MeshingOptions options;
    options.normals = mesh::NormalMode::Gradient;
    options.colors = true;
    mesh::Mesh m = mesh::mesh_bricks(cache, &doc, options);
    REQUIRE(!m.empty());
    REQUIRE(m.normals.size() == m.positions.size());
    REQUIRE(m.colors.size() == m.positions.size());

    // The serial reference over the identical geometry.
    const float brick_width = static_cast<float>(cache.config().dim) * cache.config().voxel_size;
    std::unordered_map<brick::BrickKey, std::vector<std::size_t>, brick::BrickKeyHash> groups;
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        const cfloat3 p = m.positions[i];
        groups[brick::BrickKey{static_cast<int>(std::floor(p.x / brick_width)),
                               static_cast<int>(std::floor(p.y / brick_width)),
                               static_cast<int>(std::floor(p.z / brick_width))}]
            .push_back(i);
    }
    std::vector<cfloat3> want_normals(m.positions.size());
    std::vector<cfloat3> want_colors(m.positions.size());
    for (const auto& [key, verts] : groups) {
        const scene::CullRegion cull{cache.cull_region(key).dilated(options.gradient_eps)};
        const scene::Tape tape = scene::compile_document(doc, &cull);
        auto field = [&](cfloat3 p) { return tape.eval(p).d; };
        for (std::size_t i : verts) {
            want_colors[i] = tape.eval(m.positions[i]).color;
            want_normals[i] = kernel::cnormal(field, m.positions[i], options.gradient_eps);
        }
    }
    CHECK(std::memcmp(m.normals.data(), want_normals.data(),
                      m.normals.size() * sizeof(cfloat3)) == 0);
    CHECK(std::memcmp(m.colors.data(), want_colors.data(), m.colors.size() * sizeof(cfloat3)) ==
          0);
}

TEST_CASE("mesh_tape attributes: the batched pass is the serial pass, bit for bit") {
    // The non-brick meshers' half of the same fix. `mesh_tape`, `mesh_tape_dc`
    // and the dual-grid path all reach `apply_tape_attributes`, which walked
    // the tape once per vertex for the colour and four more times for the
    // gradient, on one thread — 96% of a coloured mesh, and 53x was sitting in
    // the backend the rest of the engine already used (#302).
    //
    // memcmp rather than a tolerance: the batched gradient is the same four
    // taps summed and normalized in the same order, so anything short of
    // identity means a tap moved.
    scene::Document doc = gnarly_document();
    REQUIRE(eval::Registry::instance().find("cpu") != nullptr);
    const scene::Tape tape = scene::compile_document(doc);
    REQUIRE(!tape.empty());
    const cfloat3 pad = cf3(0.2f, 0.2f, 0.2f);
    const math::Aabb region{tape.bounds.min - pad, tape.bounds.max + pad};

    mesh::MeshingOptions options;
    options.normals = mesh::NormalMode::Gradient;
    options.colors = true;
    const mesh::Mesh m = mesh::mesh_tape(tape, region, 0.06f, options);
    REQUIRE(!m.empty());
    REQUIRE(m.normals.size() == m.positions.size());
    REQUIRE(m.colors.size() == m.positions.size());

    // The serial reference, over the identical geometry.
    std::vector<cfloat3> want_normals(m.positions.size());
    std::vector<cfloat3> want_colors(m.positions.size());
    auto field = [&](cfloat3 p) { return tape.eval(p).d; };
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        want_colors[i] = tape.eval(m.positions[i]).color;
        want_normals[i] = kernel::cnormal(field, m.positions[i], options.gradient_eps);
    }
    CHECK(std::memcmp(m.normals.data(), want_normals.data(), m.normals.size() * sizeof(cfloat3)) ==
          0);
    CHECK(std::memcmp(m.colors.data(), want_colors.data(), m.colors.size() * sizeof(cfloat3)) == 0);

    SUBCASE("and the dual-contouring mesher lands on the same pass") {
        // DC ships behind the experimental flag and returns {} without it.
        mesh::DualContouringOptions dc;
        dc.enable_experimental = true;
        mesh::Mesh d = mesh::mesh_tape_dc(tape, region, 0.06f, dc);
        mesh::apply_tape_attributes(d, tape, options);
        REQUIRE(!d.empty());
        for (std::size_t i = 0; i < d.positions.size(); ++i) {
            CAPTURE(i);
            REQUIRE(d.colors[i].x == tape.eval(d.positions[i]).color.x);
            REQUIRE(d.normals[i].x ==
                    kernel::cnormal(field, d.positions[i], options.gradient_eps).x);
        }
    }
}

TEST_CASE("apply_tape_attributes: every combination of what was asked for") {
    // The branch it used to be was `if (colors) ... ; if (Gradient) ... else if
    // (Face) ...`, so face normals AND colours had to keep working together,
    // and NormalMode::None had to leave the normals alone. Batching the two
    // asks into one call rearranged that branch; this holds it.
    scene::Document doc = gnarly_document();
    const scene::Tape tape = scene::compile_document(doc);
    REQUIRE(!tape.empty());
    const cfloat3 pad = cf3(0.2f, 0.2f, 0.2f);
    const math::Aabb region{tape.bounds.min - pad, tape.bounds.max + pad};

    mesh::MeshingOptions bare;
    bare.normals = mesh::NormalMode::None;
    bare.colors = false;
    const mesh::Mesh base = mesh::mesh_tape(tape, region, 0.09f, bare);
    REQUIRE(!base.empty());
    CHECK(base.colors.empty());
    CHECK(base.normals.empty());

    auto attributed = [&](mesh::NormalMode normals, bool colors) {
        mesh::Mesh m = base;
        mesh::MeshingOptions o;
        o.normals = normals;
        o.colors = colors;
        mesh::apply_tape_attributes(m, tape, o);
        return m;
    };

    SUBCASE("colour alone leaves the normals untouched") {
        const mesh::Mesh m = attributed(mesh::NormalMode::None, true);
        CHECK(m.colors.size() == m.positions.size());
        CHECK(m.normals.empty());
    }
    SUBCASE("gradient alone leaves the colours untouched") {
        const mesh::Mesh m = attributed(mesh::NormalMode::Gradient, false);
        CHECK(m.normals.size() == m.positions.size());
        CHECK(m.colors.empty());
    }
    SUBCASE("face normals and colour are both filled") {
        const mesh::Mesh m = attributed(mesh::NormalMode::Face, true);
        REQUIRE(m.normals.size() == m.positions.size());
        REQUIRE(m.colors.size() == m.positions.size());
        // Face normals come from the triangles, not the tape.
        mesh::Mesh faces = base;
        mesh::compute_face_normals(faces);
        CHECK(std::memcmp(m.normals.data(), faces.normals.data(),
                          m.normals.size() * sizeof(cfloat3)) == 0);
        for (std::size_t i = 0; i < m.positions.size(); ++i)
            REQUIRE(m.colors[i].x == tape.eval(m.positions[i]).color.x);
    }
    SUBCASE("asking for nothing does nothing") {
        const mesh::Mesh m = attributed(mesh::NormalMode::None, false);
        CHECK(m.colors.empty());
        CHECK(m.normals.empty());
    }
    SUBCASE("an empty mesh is not a batch of zero points") {
        mesh::Mesh empty;
        mesh::MeshingOptions o;
        o.normals = mesh::NormalMode::Gradient;
        o.colors = true;
        mesh::apply_tape_attributes(empty, tape, o);  // must not reach the backend
        CHECK(empty.colors.empty());
        CHECK(empty.normals.empty());
    }
}
