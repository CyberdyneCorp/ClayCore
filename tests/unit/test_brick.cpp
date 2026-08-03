#include <doctest/doctest.h>

#include <map>
#include <vector>

#include "clay/brick/cache.h"
#include "clay/scene/bounds.h"
#include "kernel_utils.h"
#include "scene_utils.h"

using namespace clay;
using namespace clay::kernel;
using clay_test::gnarly_document;
using clay_test::item;
using brick::BrickCache;
using brick::BrickConfig;
using brick::BrickKey;
using brick::BrickRequest;
using brick::BrickState;
using brick::SubmitResult;

namespace {

// Fill every dirty brick from the tape with per-brick CULLED evaluation —
// the production pattern: cull region from the cache, eval_grid via the
// backend, submit.
int fill_cache(BrickCache& cache, const scene::Document& doc, eval::Backend* backend) {
    int filled = 0;
    for (const BrickRequest& req : cache.take_dirty()) {
        scene::CullRegion cull{cache.cull_region(req.key)};
        scene::Tape tape = scene::compile_document(doc, &cull);
        std::size_t n = static_cast<std::size_t>(req.grid.nx) * req.grid.ny * req.grid.nz;
        std::vector<float> values(n);
        REQUIRE(backend->eval_grid(tape, req.grid, values.data()) == eval::Status::Ok);
        if (cache.submit(req, values.data()) == SubmitResult::Accepted) ++filled;
    }
    return filled;
}

}  // namespace

TEST_CASE("fp16 conversion: exact round trips and monotone quantization") {
    clay_test::Lcg rng(601);
    // half-representable values round-trip exactly
    for (int i = -2048; i <= 2048; ++i) {
        float f = static_cast<float>(i) * 0.25f;
        CHECK(brick::half_to_float(brick::float_to_half(f)) == f);
    }
    // arbitrary values quantize within half precision (rel 2^-11)
    for (int i = 0; i < 5000; ++i) {
        float f = rng.range(-64.0f, 64.0f);
        float q = brick::half_to_float(brick::float_to_half(f));
        CHECK(cabs(q - f) <= cmax(cabs(f) * 0.0005f, 1e-5f));
    }
    // determinism: same bits every time
    CHECK(brick::float_to_half(0.123456f) == brick::float_to_half(0.123456f));
}

TEST_CASE("sparse storage: only surface bricks allocate") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::sphere(0.4f), cf3(0, 0, 0)));

    BrickCache cache(BrickConfig{8, 0.05f, 3, 0});
    // large domain vs a small object
    cache.mark_dirty(math::Aabb{cf3(-2, -2, -2), cf3(2, 2, 2)});
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    fill_cache(cache, doc, cpu);

    std::vector<BrickKey> surface = cache.surface_bricks();
    CHECK(surface.size() > 0);
    // sphere r=0.4 in a 4x4x4 domain: far fewer surface bricks than total
    float bs = 8 * 0.05f;  // 0.4 world units per brick
    int total = static_cast<int>((4.0f / bs) * (4.0f / bs) * (4.0f / bs));
    CHECK(static_cast<int>(surface.size()) < total / 4);
    CHECK(cache.memory_usage() == surface.size() * cache.config().brick_bytes());

    // implicit inside brick: center of the sphere
    const brick::Brick* center = cache.find(BrickKey{-1, -1, -1});
    REQUIRE(center != nullptr);
    // whatever its state, non-surface bricks hold no storage
    if (center->state != BrickState::Surface) CHECK(center->values.empty());
}

TEST_CASE("dirty tracking: local edit dirties only intersecting bricks") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::sphere(0.5f), cf3(0, 0, 0)));

    BrickCache cache(BrickConfig{8, 0.05f, 3, 0});
    cache.mark_dirty(math::Aabb{cf3(-2, -2, -2), cf3(2, 2, 2)});
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    fill_cache(cache, doc, cpu);
    CHECK(cache.dirty_count() == 0);

    // a small edit far in a corner
    scene::NodeId id =
        l.sdf->insert(item(scene::Prim::sphere(0.1f), cf3(1.5f, 1.5f, 1.5f)));
    math::Aabb bound = scene::item_influence_bound(*l.sdf->find(id), l);
    cache.mark_dirty(bound);
    std::size_t dirty = cache.dirty_count();
    CHECK(dirty > 0);
    CHECK(dirty < 30);  // a handful of bricks, not the whole domain
}

TEST_CASE("locality regression: distant edit leaves bricks bit-identical") {
    // incremental path: fill, edit far away, re-eval only dirty bricks
    scene::Document doc = gnarly_document();
    eval::Backend* cpu = eval::Registry::instance().find("cpu");

    BrickCache incremental(BrickConfig{8, 0.1f, 3, 0});
    incremental.mark_dirty(scene::layer_influence_bound(doc.layers[0]));
    fill_cache(incremental, doc, cpu);

    // snapshot all brick payloads
    std::map<std::tuple<int, int, int>, std::vector<std::uint16_t>> before;
    for (BrickKey k : incremental.surface_bricks())
        before[{k.x, k.y, k.z}] = incremental.find(k)->values;

    // distant edit: item far outside every existing brick
    scene::Layer& layer = doc.layers[0];
    scene::NodeId id = layer.sdf->insert(
        item(scene::Prim::sphere(0.3f), cf3(20, 20, 20), scene::Op::Add,
             scene::Blend{scene::BlendProfile::Quadratic, 0.1f}));
    math::Aabb new_bound = scene::item_influence_bound(*layer.sdf->find(id), layer);
    incremental.mark_dirty(new_bound);
    int refilled = fill_cache(incremental, doc, cpu);
    CHECK(refilled > 0);  // the new region was evaluated

    // every pre-existing brick untouched by the dirty set is bit-identical
    int compared = 0;
    for (auto& [kt, values] : before) {
        BrickKey k{std::get<0>(kt), std::get<1>(kt), std::get<2>(kt)};
        if (incremental.brick_bounds(k).dilated(incremental.config().band())
                .intersects(new_bound))
            continue;
        const brick::Brick* b = incremental.find(k);
        REQUIRE(b != nullptr);
        CHECK(b->values == values);  // fp16 payload bit-identity
        ++compared;
    }
    CHECK(compared > 10);

    // and a from-scratch cache over the NEW scene agrees bit-identically on
    // those bricks too (culled re-eval == incremental state)
    BrickCache fresh(BrickConfig{8, 0.1f, 3, 0});
    fresh.mark_dirty(scene::layer_influence_bound(doc.layers[0]));
    fill_cache(fresh, doc, cpu);
    for (auto& [kt, values] : before) {
        BrickKey k{std::get<0>(kt), std::get<1>(kt), std::get<2>(kt)};
        if (fresh.brick_bounds(k).dilated(fresh.config().band()).intersects(new_bound)) continue;
        const brick::Brick* b = fresh.find(k);
        REQUIRE(b != nullptr);
        CHECK(b->values == values);
    }
}

TEST_CASE("generation counters reject stale in-flight results") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::sphere(0.5f), cf3(0, 0, 0)));
    scene::Tape tape = scene::compile_document(doc);
    eval::Backend* cpu = eval::Registry::instance().find("cpu");

    BrickCache cache(BrickConfig{8, 0.1f, 3, 0});
    cache.mark_dirty(math::Aabb{cf3(-1, -1, -1), cf3(1, 1, 1)});
    std::vector<BrickRequest> inflight = cache.take_dirty();
    REQUIRE(!inflight.empty());

    // brick re-dirtied while the request is in flight
    cache.mark_dirty(cache.brick_bounds(inflight[0].key));

    std::size_t n = static_cast<std::size_t>(cache.config().dim);
    std::vector<float> values(n * n * n, 0.0f);
    REQUIRE(cpu->eval_grid(tape, inflight[0].grid, values.data()) == eval::Status::Ok);
    CHECK(cache.submit(inflight[0], values.data()) == SubmitResult::Stale);

    // the NEW request for the same brick is accepted
    for (const BrickRequest& req : cache.take_dirty()) {
        if (req.key == inflight[0].key) {
            REQUIRE(cpu->eval_grid(tape, req.grid, values.data()) == eval::Status::Ok);
            CHECK(cache.submit(req, values.data()) == SubmitResult::Accepted);
        }
    }
}

TEST_CASE("LOD mips subsample up-to-date children and invalidate on dirty") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::sphere(0.6f), cf3(0.4f, 0.4f, 0.4f)));
    eval::Backend* cpu = eval::Registry::instance().find("cpu");

    BrickCache cache(BrickConfig{8, 0.05f, 3, 0});
    cache.mark_dirty(math::Aabb{cf3(-1, -1, -1), cf3(1.6f, 1.6f, 1.6f)});
    fill_cache(cache, doc, cpu);

    BrickKey coarse{0, 0, 0};  // children (0..1)^3
    REQUIRE(cache.build_mip(coarse));
    const brick::Brick* mip = cache.find_mip(coarse);
    REQUIRE(mip != nullptr);
    CHECK(cache.current_lod(coarse) == 1);

    // mip lattice equals subsampled fine lattice
    int dim = cache.config().dim;
    for (int k = 0; k < dim; k += 3)
        for (int j = 0; j < dim; j += 3)
            for (int i = 0; i < dim; i += 3) {
                int fi = i * 2, fj = j * 2, fk = k * 2;
                BrickKey child{fi / dim, fj / dim, fk / dim};
                float fine = cache.sample(child, fi % dim, fj % dim, fk % dim);
                float coarse_v =
                    brick::half_to_float(mip->values[(static_cast<std::size_t>(k) * dim + j) * dim + i]);
                CHECK(coarse_v == fine);
            }

    // dirtying a child invalidates the mip (no stale LOD data)
    cache.mark_dirty(cache.brick_bounds(BrickKey{0, 0, 0}));
    CHECK(cache.current_lod(coarse) == 0);
    // and it cannot be rebuilt until the child is re-evaluated
    CHECK_FALSE(cache.build_mip(coarse));
}

TEST_CASE("memory budget: predictable failure, usage query, data intact") {
    scene::Document doc = gnarly_document();
    eval::Backend* cpu = eval::Registry::instance().find("cpu");

    BrickConfig config{8, 0.1f, 3, 0};
    std::size_t budget = config.brick_bytes() * 10;  // room for only 10 surface bricks
    config.memory_budget = budget;

    BrickCache cache(config);
    cache.mark_dirty(scene::layer_influence_bound(doc.layers[0]));
    int accepted = 0, over_budget = 0;
    for (const BrickRequest& req : cache.take_dirty()) {
        scene::CullRegion cull{cache.cull_region(req.key)};
        scene::Tape tape = scene::compile_document(doc, &cull);
        std::size_t n = static_cast<std::size_t>(req.grid.nx) * req.grid.ny * req.grid.nz;
        std::vector<float> values(n);
        REQUIRE(cpu->eval_grid(tape, req.grid, values.data()) == eval::Status::Ok);
        SubmitResult r = cache.submit(req, values.data());
        if (r == SubmitResult::Accepted) ++accepted;
        if (r == SubmitResult::BudgetExceeded) ++over_budget;
        CHECK(cache.memory_usage() <= budget);  // ceiling never breached
    }
    CHECK(accepted > 0);
    CHECK(over_budget > 0);  // the scene needs more than 10 surface bricks
    CHECK(cache.surface_bricks().size() <= 10);
    // existing surface bricks remain valid and sampleable
    for (BrickKey k : cache.surface_bricks()) {
        const brick::Brick* b = cache.find(k);
        REQUIRE(b != nullptr);
        CHECK(b->values.size() == static_cast<std::size_t>(8 * 8 * 8));
    }
}
