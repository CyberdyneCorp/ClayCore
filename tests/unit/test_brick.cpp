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

TEST_CASE("dirty everything re-queues bricks that were already waiting") {
    // Regression. The infinite branch of mark_dirty cleared the queue and then
    // re-added through dirty_one, which pushes only a brick that is not already
    // queued — and the queued FLAGS survived the clear. Every brick already
    // waiting was therefore dropped and could never be re-queued by any later
    // call: the cache served its stale samples for the rest of its life, and
    // only destroying it recovered. It is reached by the documented way of
    // saying "this edit's influence is unbounded", which is what a plane or an
    // intersect op produces.
    BrickCache cache(BrickConfig{8, 0.05f, 3, 0});

    // queue a region and deliberately do NOT drain it
    cache.mark_dirty(math::Aabb{cf3(0, 0, 0), cf3(0.1f, 0.1f, 0.1f)});
    const std::size_t tracked = cache.tracked_count();
    REQUIRE(tracked > 0);
    REQUIRE(cache.dirty_count() == tracked);

    cache.mark_dirty(math::Aabb::infinite());
    CHECK(cache.dirty_count() == tracked);  // everything tracked, still queued

    // ...and it stays true however many times it is said
    cache.mark_dirty(math::Aabb::infinite());
    CHECK(cache.dirty_count() == tracked);
    CHECK(cache.take_dirty().size() == tracked);

    // the same after a full round trip, where the flags start clean
    BrickCache drained(BrickConfig{8, 0.05f, 3, 0});
    drained.mark_dirty(math::Aabb{cf3(0, 0, 0), cf3(0.1f, 0.1f, 0.1f)});
    std::vector<float> values(8 * 8 * 8, 1.0f);
    for (const BrickRequest& req : drained.take_dirty())
        REQUIRE(drained.submit(req, values.data()) == SubmitResult::Accepted);
    REQUIRE(drained.dirty_count() == 0);
    drained.mark_dirty(math::Aabb::infinite());
    CHECK(drained.dirty_count() == drained.tracked_count());
}

// -- eviction (add-brick-cache-eviction) --------------------------------------
// The budget was a wall and is now a ceiling. What these defend is that
// reclaiming memory loses no INFORMATION — an evicted brick is one that must be
// re-evaluated if looked at again, which is what the dirty/request/submit cycle
// already does.

namespace {

// A cache filled from a sphere, at a resolution that produces enough surface
// bricks for a trim to have something to choose between.
BrickCache filled_sphere_cache(const scene::Document& doc, eval::Backend* cpu,
                               std::size_t budget = 0) {
    BrickConfig cfg;
    cfg.voxel_size = 0.03f;
    cfg.dim = 8;
    cfg.memory_budget = budget;
    BrickCache cache(cfg);
    cache.mark_dirty(math::Aabb{cf3(-1.2f, -1.2f, -1.2f), cf3(1.2f, 1.2f, 1.2f)});
    fill_cache(cache, doc, cpu);
    return cache;
}

}  // namespace

TEST_CASE("an evicted brick re-evaluates to bit-identical data") {
    // The claim the whole change rests on: eviction is a memory decision, never
    // a data one. Not "within tolerance" — the same document evaluated against
    // the same cull region must quantize to the same fp16 bits.
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    REQUIRE(cpu != nullptr);
    scene::Document doc;
    scene::Node body;
    body.prim = scene::Prim::sphere(1.0f);
    doc.add_sdf_layer("s").sdf->insert(body);

    BrickCache cache = filled_sphere_cache(doc, cpu);
    const std::vector<BrickKey> surface = cache.surface_bricks();
    REQUIRE(surface.size() > 20);

    // Snapshot a brick's decoded lattice, evict it, put it back.
    const BrickKey key = surface[surface.size() / 2];
    const int dim = cache.config().dim;
    std::vector<float> before;
    for (int k = 0; k < dim; ++k)
        for (int j = 0; j < dim; ++j)
            for (int i = 0; i < dim; ++i) before.push_back(cache.sample(key, i, j, k));

    const std::size_t bytes_before = cache.memory_usage();
    REQUIRE(cache.evict(key));
    CHECK(cache.memory_usage() < bytes_before);
    CHECK(cache.find(key) == nullptr);  // back to never-evaluated

    // Re-dirty exactly that brick and refill it from the unchanged document.
    cache.mark_dirty(cache.brick_bounds(key));
    fill_cache(cache, doc, cpu);

    std::vector<float> after;
    for (int k = 0; k < dim; ++k)
        for (int j = 0; j < dim; ++j)
            for (int i = 0; i < dim; ++i) after.push_back(cache.sample(key, i, j, k));
    REQUIRE(after.size() == before.size());
    for (std::size_t i = 0; i < before.size(); ++i) REQUIRE(after[i] == before[i]);
    CHECK(cache.memory_usage() == bytes_before);
}

TEST_CASE("trim reaches its target, and prefers to drop what is far away") {
    // The policy decision, measured rather than asserted: a sculptor works in a
    // NEIGHBOURHOOD, so the bricks they come back to are near where they are
    // working. Evicting near the focus is the worst available choice.
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    REQUIRE(cpu != nullptr);
    scene::Document doc;
    scene::Node body;
    body.prim = scene::Prim::sphere(1.0f);
    doc.add_sdf_layer("s").sdf->insert(body);

    BrickCache cache = filled_sphere_cache(doc, cpu);
    const std::size_t full = cache.memory_usage();
    REQUIRE(full > 0);

    // Work at the +X pole; trim to half.
    const cfloat3 focus = cf3(1.0f, 0, 0);
    const std::size_t target = full / 2;
    const std::size_t dropped = cache.trim_to(target, focus);
    CHECK(dropped > 0);
    CHECK(cache.memory_usage() <= target);

    // What survived is nearer the focus than what went. Compare the mean
    // distance of the survivors against the mean over everything that was there.
    double kept_sum = 0;
    std::size_t kept = 0;
    for (const BrickKey& k : cache.surface_bricks()) {
        const math::Aabb b = cache.brick_bounds(k);
        kept_sum += clength((b.min + b.max) * 0.5f - focus);
        ++kept;
    }
    REQUIRE(kept > 0);
    const double kept_mean = kept_sum / static_cast<double>(kept);
    // On a sphere of radius 1 focused at its +X pole, the mean distance over
    // the whole shell is well past 1; the surviving half must be much nearer.
    CAPTURE(kept_mean);
    CHECK(kept_mean < 1.0);
}

TEST_CASE("trim never drops a brick the host is waiting on") {
    // A dirty brick is already scheduled to be rewritten, so dropping it trades
    // memory for the one thing the host is actively waiting on.
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    REQUIRE(cpu != nullptr);
    scene::Document doc;
    scene::Node body;
    body.prim = scene::Prim::sphere(1.0f);
    doc.add_sdf_layer("s").sdf->insert(body);

    BrickCache cache = filled_sphere_cache(doc, cpu);
    // Dirty a region WITHOUT draining it: those bricks are queued.
    cache.mark_dirty(math::Aabb{cf3(0.8f, -0.2f, -0.2f), cf3(1.2f, 0.2f, 0.2f)});
    const std::size_t queued = cache.dirty_count();
    REQUIRE(queued > 0);

    // Trim hard, from the far side, so the queued bricks are the ones the
    // policy would most like to drop.
    cache.trim_to(0, cf3(-1.0f, 0, 0));
    CHECK(cache.dirty_count() == queued);  // nothing was taken out of the queue

    // The queued work still completes and still lands.
    const int refilled = fill_cache(cache, doc, cpu);
    CHECK(refilled > 0);
}

TEST_CASE("eviction keeps the coarse stand-in that dirtying would have dropped") {
    // The distinction worth having: DIRTYING a child invalidates its mip
    // because the shape changed. EVICTION does not change the shape — it drops
    // a cached copy of it — so the mip survives. That is the whole value of a
    // memory-warning response: the silhouette stays at an eighth of the memory.
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    REQUIRE(cpu != nullptr);
    scene::Document doc;
    scene::Node body;
    body.prim = scene::Prim::sphere(1.0f);
    doc.add_sdf_layer("s").sdf->insert(body);

    BrickCache cache = filled_sphere_cache(doc, cpu);
    // Build every mip that can be built, then find one that took.
    BrickKey coarse{0, 0, 0};
    bool built = false;
    for (const BrickKey& k : cache.surface_bricks()) {
        const BrickKey c{k.x >> 1, k.y >> 1, k.z >> 1};
        if (cache.build_mip(c)) {
            coarse = c;
            built = true;
            break;
        }
    }
    REQUIRE(built);
    REQUIRE(cache.current_lod(coarse) == 1);

    // Evict every child of that coarse key.
    for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx)
                cache.evict(BrickKey{coarse.x * 2 + dx, coarse.y * 2 + dy, coarse.z * 2 + dz});

    // The mip is still there and still answers.
    CHECK(cache.current_lod(coarse) == 1);
    CHECK(cache.find_mip(coarse) != nullptr);
}

TEST_CASE("a cache that is never trimmed behaves exactly as it does today") {
    // The additive claim, including at the budget wall: nothing about eviction
    // exists until a host asks for it.
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    REQUIRE(cpu != nullptr);
    scene::Document doc;
    scene::Node body;
    body.prim = scene::Prim::sphere(1.0f);
    doc.add_sdf_layer("s").sdf->insert(body);

    BrickCache untouched = filled_sphere_cache(doc, cpu);
    const std::size_t bytes = untouched.memory_usage();
    const std::size_t tracked = untouched.tracked_count();
    CHECK(bytes > 0);

    // Trimming to a target already met changes nothing at all.
    CHECK(untouched.trim_to(bytes) == 0);
    CHECK(untouched.trim_to(bytes, cf3(0, 0, 0)) == 0);
    CHECK(untouched.memory_usage() == bytes);
    CHECK(untouched.tracked_count() == tracked);

    // And the budget wall still refuses rather than silently evicting.
    BrickCache tight = filled_sphere_cache(doc, cpu, 4096);
    CHECK(tight.memory_usage() <= 4096);
}

TEST_CASE("the per-key bookkeeping can be reported and reclaimed") {
    // The growth tracked_count() exists to make visible: it grows with how much
    // space has ever been dirtied, OUTSIDE the memory budget.
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    REQUIRE(cpu != nullptr);
    scene::Document doc;
    scene::Node body;
    body.prim = scene::Prim::sphere(0.3f);
    doc.add_sdf_layer("s").sdf->insert(body);

    BrickConfig cfg;
    cfg.voxel_size = 0.03f;
    cfg.dim = 8;
    BrickCache cache(cfg);
    // Dirty far more space than the little sphere occupies — the long-session
    // shape, where a map grows over territory that holds nothing.
    cache.mark_dirty(math::Aabb{cf3(-1.5f, -1.5f, -1.5f), cf3(1.5f, 1.5f, 1.5f)});
    fill_cache(cache, doc, cpu);

    const std::size_t tracked = cache.tracked_count();
    const std::size_t surface = cache.surface_bricks().size();
    CAPTURE(tracked);
    CAPTURE(surface);
    REQUIRE(tracked > surface * 2);  // most of what is tracked holds nothing
    CHECK(cache.bookkeeping_bytes() > 0);

    const std::size_t bytes = cache.memory_usage();
    const std::size_t forgotten = cache.forget_empty();
    CHECK(forgotten > 0);
    CHECK(cache.tracked_count() < tracked);
    CHECK(cache.bookkeeping_bytes() < tracked * sizeof(BrickKey) * 8);
    // ...and it reclaimed a different pool: the payloads are untouched.
    CHECK(cache.memory_usage() == bytes);
    CHECK(cache.surface_bricks().size() == surface);
}

TEST_CASE("forgetting a key never turns solid interior into empty space") {
    // The distinction the first implementation got wrong. "Forget what holds no
    // payload" is too wide: an INSIDE brick holds no lattice but carries real
    // information — this region is solid — and an untracked key reads as
    // OUTSIDE. Forgetting one reports the interior as empty, which is data loss
    // wearing memory reclamation's clothes.
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    REQUIRE(cpu != nullptr);
    scene::Document doc;
    scene::Node body;
    body.prim = scene::Prim::sphere(1.0f);
    doc.add_sdf_layer("s").sdf->insert(body);

    BrickConfig cfg;
    cfg.voxel_size = 0.05f;
    cfg.dim = 8;
    BrickCache cache(cfg);
    cache.mark_dirty(math::Aabb{cf3(-1.4f, -1.4f, -1.4f), cf3(1.4f, 1.4f, 1.4f)});
    fill_cache(cache, doc, cpu);

    // The brick holding the sphere's centre, found from its bounds — there is
    // no public world-to-key helper and this test does not need one.
    BrickKey centre{0, 0, 0};
    bool found = false;
    for (int z = -3; z <= 3 && !found; ++z)
        for (int y = -3; y <= 3 && !found; ++y)
            for (int x = -3; x <= 3 && !found; ++x) {
                const math::Aabb b = cache.brick_bounds(BrickKey{x, y, z});
                if (b.min.x <= 0.0f && 0.0f < b.max.x && b.min.y <= 0.0f && 0.0f < b.max.y &&
                    b.min.z <= 0.0f && 0.0f < b.max.z) {
                    centre = BrickKey{x, y, z};
                    found = true;
                }
            }
    REQUIRE(found);
    const float inside_before = cache.sample(centre, 4, 4, 4);
    REQUIRE(inside_before < 0.0f);  // deep inside the sphere

    REQUIRE(cache.forget_empty() > 0);  // it did reclaim something

    // ...and still does after. The interior survived the sweep.
    CHECK(cache.sample(centre, 4, 4, 4) == inside_before);
}

namespace {

// The definition surface_bounds() promises: the fold the brick raycast used to
// run per ray. Compared field by field and exactly — a union of mins and maxes
// over the same floats is order-independent, so any difference is a
// bookkeeping bug, never rounding.
math::Aabb folded_surface_bounds(const BrickCache& cache) {
    math::Aabb box;
    for (BrickKey key : cache.surface_bricks()) box.expand(cache.brick_bounds(key));
    return box;
}

void check_surface_bounds(const BrickCache& cache, const char* step) {
    CAPTURE(step);
    const math::Aabb want = folded_surface_bounds(cache);
    const math::Aabb got = cache.surface_bounds();
    REQUIRE(got.empty() == want.empty());
    if (want.empty()) return;
    CHECK(got.min.x == want.min.x);
    CHECK(got.min.y == want.min.y);
    CHECK(got.min.z == want.min.z);
    CHECK(got.max.x == want.max.x);
    CHECK(got.max.y == want.max.y);
    CHECK(got.max.z == want.max.z);
}

// A surface brick on a face of the box, and one strictly inside it — the two
// removals that exercise the different branches of the bookkeeping.
BrickKey face_surface_brick(const BrickCache& cache) {
    const math::Aabb box = cache.surface_bounds();
    for (BrickKey key : cache.surface_bricks())
        if (cache.brick_bounds(key).min.x == box.min.x) return key;
    FAIL("no surface brick on the -x face");
    return BrickKey{};
}

BrickKey interior_surface_brick(const BrickCache& cache) {
    const math::Aabb box = cache.surface_bounds();
    for (BrickKey key : cache.surface_bricks()) {
        const math::Aabb b = cache.brick_bounds(key);
        if (b.min.x > box.min.x && b.max.x < box.max.x && b.min.y > box.min.y &&
            b.max.y < box.max.y && b.min.z > box.min.z && b.max.z < box.max.z)
            return key;
    }
    FAIL("no surface brick strictly inside the box");
    return BrickKey{};
}

}  // namespace

TEST_CASE("surface_bounds equals the fold over surface_bricks after every mutation") {
    // The raycast reads surface_bounds() instead of folding surface_bricks()
    // per ray. The two must never disagree, so every path that changes which
    // bricks are Surface — submit in both directions, evict, both trims,
    // forget_empty — is followed by the equality it promises.
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    REQUIRE(cpu != nullptr);
    scene::Document doc;
    scene::Layer& layer = doc.add_sdf_layer("s");
    const scene::NodeId ball = layer.sdf->insert(item(scene::Prim::sphere(0.6f), cf3(0, 0, 0)));

    BrickConfig cfg;
    cfg.voxel_size = 0.03f;
    cfg.dim = 8;
    BrickCache cache(cfg);
    check_surface_bounds(cache, "empty cache");
    CHECK(cache.surface_bounds().empty());

    // A domain well wider than the ball: a mix of Surface, Inside and Outside
    // bricks, with never-evaluated keys at the rim.
    cache.mark_dirty(math::Aabb{cf3(-1.2f, -1.2f, -1.2f), cf3(1.2f, 1.2f, 1.2f)});
    check_surface_bounds(cache, "dirty, nothing submitted");
    fill_cache(cache, doc, cpu);
    check_surface_bounds(cache, "filled");
    REQUIRE_FALSE(cache.surface_bounds().empty());

    // Single evictions: one that cannot move a face, one that must.
    REQUIRE(cache.evict(interior_surface_brick(cache)));
    check_surface_bounds(cache, "evicted an interior brick");
    REQUIRE(cache.evict(face_surface_brick(cache)));
    check_surface_bounds(cache, "evicted a face brick");

    // Both trims: the focused one drops the far bricks — the faces — first.
    REQUIRE(cache.trim_to(cache.memory_usage() / 2, cf3(0, 0, 0)) > 0);
    check_surface_bounds(cache, "trim_to with focus");
    REQUIRE(cache.trim_to(cache.memory_usage() / 2) > 0);
    check_surface_bounds(cache, "trim_to without focus");
    REQUIRE(cache.forget_empty() > 0);
    check_surface_bounds(cache, "forget_empty");

    // Reclassification through submit: shrink the ball and refill, so bricks
    // that were Surface at the old extremes come back Outside or Inside.
    layer.sdf->find_mut(ball)->prim = scene::Prim::sphere(0.3f);
    cache.mark_dirty(math::Aabb{cf3(-1.2f, -1.2f, -1.2f), cf3(1.2f, 1.2f, 1.2f)});
    fill_cache(cache, doc, cpu);
    check_surface_bounds(cache, "shrunk and refilled");
    // ...and grown again, so submit widens the box from a smaller one.
    layer.sdf->find_mut(ball)->prim = scene::Prim::sphere(0.8f);
    cache.mark_dirty(math::Aabb{cf3(-1.2f, -1.2f, -1.2f), cf3(1.2f, 1.2f, 1.2f)});
    fill_cache(cache, doc, cpu);
    check_surface_bounds(cache, "grown and refilled");

    // Evict every surface brick one at a time, checking after each, down to
    // the empty box; then a whole-cache refill brings it back.
    for (BrickKey key : cache.surface_bricks()) {
        REQUIRE(cache.evict(key));
        check_surface_bounds(cache, "evicting one by one");
    }
    CHECK(cache.surface_bounds().empty());
    cache.mark_dirty(math::Aabb::infinite());
    fill_cache(cache, doc, cpu);
    check_surface_bounds(cache, "refilled after emptying");
    CHECK_FALSE(cache.surface_bounds().empty());
}
