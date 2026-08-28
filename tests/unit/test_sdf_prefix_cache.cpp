// The editable SDF prefix field cache (sdf-prefix-cache spec).
//
// The claim is a strong one and everything here exists to hold it: caching an
// old prefix as a volume changes what evaluation COSTS and not what it
// produces. So the first and last test in this file are both parity against a
// full walk, and the cache is only ever allowed to be faster.
//
// The measurement the design rests on, reproduced by "a volume seeds only where
// it stores samples" below: seeding a suffix from a prefix volume is exact to
// ~3e-7 where the volume stores the sample and wrong by ~14 CELLS where it does
// not, because `eval` answers with a conservative far bound out there rather
// than the distance the history had.

#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <vector>

#include "clay/eval/backend.h"
#include "clay/field/volume.h"
#include "clay/scene/consolidate.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"
#include "clay/session/sdf_prefix_cache.h"
#include "clay/session/sdf_sculpt.h"

using namespace clay;
using kernel::cf3;
using kernel::cfloat3;
using session::SdfPrefixCache;
using session::SdfPrefixPolicy;
using session::SdfSourceField;

namespace {

const float kCell = 0.03f;

SdfPrefixPolicy policy_keeping(std::size_t live, std::size_t min_history = 4) {
    SdfPrefixPolicy p;
    p.cell_size = kCell;
    p.min_history_roots = min_history;
    p.keep_live_suffix_roots = live;
    p.max_bytes = 256u * 1024u * 1024u;
    return p;
}

// A worked sphere: a base plus `dabs` stamps walked over its surface by the
// golden angle, which is the shape #306's long-history fixtures use.
scene::Document worked(int dabs, float blend_k = 0.04f, scene::Op op = scene::Op::Add) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("body");
    scene::Node base;
    base.prim = scene::Prim::sphere(1.0f);
    l.sdf->insert(base);
    for (int i = 1; i < dabs; ++i) {
        const float t = static_cast<float>(i) * 2.399963f;
        const float z = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / static_cast<float>(dabs);
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        scene::Node d;
        d.prim = scene::Prim::sphere(0.09f);
        d.xform.position = cf3(r * std::cos(t), r * std::sin(t), z);
        d.op = op;
        if (blend_k > 0.0f) d.blend = scene::Blend{scene::BlendProfile::Quadratic, blend_k};
        l.sdf->insert(d);
    }
    return doc;
}

// The layer's field as `bake_layer` sees it: the oracle every accelerated
// answer is measured against.
scene::Tape oracle_tape(const scene::Document& doc) {
    scene::Layer view = doc.layers.front();
    view.visible = true;
    view.xform = math::Transform{};
    return scene::compile_layer(view);
}

std::vector<float> random_points(std::size_t n, float extent, std::uint32_t seed = 12345u) {
    std::vector<float> pts(n * 3);
    std::uint32_t s = seed;
    auto next = [&s]() {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(s >> 8) / static_cast<float>(1 << 24);
    };
    for (std::size_t i = 0; i < n * 3; ++i) pts[i] = next() * 2.0f * extent - extent;
    return pts;
}

// The worst difference between a source field and the oracle over `pts`.
double worst_error(const SdfSourceField& src, const scene::Tape& want,
                   const std::vector<float>& pts) {
    const std::size_t n = pts.size() / 3;
    std::vector<float> got(n), expect(n);
    src.fill_points(pts.data(), n, got.data());
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    REQUIRE(cpu != nullptr);
    cpu->eval_points(want, eval::PointQuery{pts.data(), n, 1e-4f},
                     eval::PointResults{expect.data(), nullptr, nullptr});
    double worst = 0.0;
    for (std::size_t i = 0; i < n; ++i)
        worst = std::max(worst, std::abs(static_cast<double>(got[i]) -
                                         static_cast<double>(expect[i])));
    return worst;
}

}  // namespace

// -- 9.1 the parity matrix ----------------------------------------------------

TEST_CASE("prefix cache: an accelerated field equals the full walk") {
    // Every op/blend combination the spec's matrix names, at several boundaries.
    // A cache that is faster and different is not a cache, so this is the gate
    // the whole feature has to clear before any of the rest matters.
    struct Case {
        const char* name;
        float blend_k;
        scene::Op op;
    };
    const Case cases[] = {
        {"hard add", 0.0f, scene::Op::Add},
        {"smooth quadratic", 0.04f, scene::Op::Add},
        {"wide smooth", 0.12f, scene::Op::Add},
        {"subtract", 0.0f, scene::Op::Subtract},
        {"smooth subtract", 0.05f, scene::Op::Subtract},
    };
    for (const Case& c : cases) {
        for (std::size_t live : {std::size_t{1}, std::size_t{8}, std::size_t{40}}) {
            scene::Document doc = worked(80, c.blend_k, c.op);
            const scene::Tape want = oracle_tape(doc);
            SdfPrefixCache cache;
            // open() never builds -- see its doc comment. A host schedules the
            // build; here the test is the host.
            REQUIRE(cache.build(doc, doc.layers.front().id, policy_keeping(live)) != nullptr);
            auto src = SdfSourceField::open(doc, doc.layers.front().id, &cache,
                                            policy_keeping(live));
            REQUIRE(src);
            CAPTURE(c.name);
            CAPTURE(live);
            CHECK(src->accelerated());
            CHECK(src->prefix_roots() == 80 - live);
            CHECK(src->suffix_roots() == live);
            // Exact where the cached volume covers the point, and exact where
            // it does not because the prefix tape is evaluated instead. Either
            // way the answer is the walk's.
            CHECK(worst_error(*src, want, random_points(4000, 1.4f)) < 1e-5);
        }
    }
}

TEST_CASE("prefix cache: a volume seeds only where it stores samples") {
    // The measurement the far-bound rule comes from, as a test. Points far
    // outside the band are exactly where a cached volume has nothing stored,
    // and folding a suffix onto its far bound there would be wrong by cells.
    // The fallback counter is how we know the rule actually fired rather than
    // the fixture simply never leaving the band.
    scene::Document doc = worked(80);
    const scene::Tape want = oracle_tape(doc);
    SdfPrefixCache cache;
    REQUIRE(cache.build(doc, doc.layers.front().id, policy_keeping(8)) != nullptr);
    auto src = SdfSourceField::open(doc, doc.layers.front().id, &cache, policy_keeping(8));
    REQUIRE(src);
    REQUIRE(src->accelerated());

    // ON the surface shell, where the volume stores every sample -- and asked
    // a window at a time, which is how the block fill asks. Coverage is a
    // WINDOW's property, so a scattered handful spanning the whole model would
    // fail it for the one point that strayed and prove nothing.
    std::vector<float> shell;
    for (int i = 0; i < 600; ++i) {
        const float t = static_cast<float>(i) * 2.399963f;
        const float z = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / 600.0f;
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        shell.push_back(r * std::cos(t));
        shell.push_back(r * std::sin(t));
        shell.push_back(z);
    }
    std::vector<float> got(shell.size() / 3);
    for (std::size_t i = 0; i < shell.size() / 3; ++i)
        src->fill_points(shell.data() + i * 3, 1, got.data() + i);
    const std::uint64_t seeded_near = cache.stats().seeded_windows;
    CHECK(seeded_near > 0);
    // Between lattice points the seed is INTERPOLATED, so this is the sampling
    // tolerance of a volume at this cell size and not the exactness the lattice
    // itself gets -- see "exact on the lattice it was built for" below, which
    // is the contract a consumer actually relies on.
    MESSAGE("off-lattice error: " << worst_error(*src, want, shell));
    CHECK(worst_error(*src, want, shell) < 0.5 * kCell);

    // Far outside it: the volume stores nothing, and the answer must still be
    // the walk's.
    std::vector<float> far = random_points(2000, 1.0f, 99u);
    for (float& v : far) v = v < 0 ? v - 3.0f : v + 3.0f;
    CHECK(worst_error(*src, want, far) < 1e-5);
    CHECK(cache.stats().fallback_windows > 0);  // the rule fired
}

TEST_CASE("prefix cache: exact on the lattice it was built for") {
    // THE CONTRACT. `SdfSourceField` is a SAMPLING source: it answers what a
    // volume at `cell_size` answers, and on that volume's own lattice a seed
    // taken from the cached prefix is the stored number rather than an
    // interpolation of it. So a bake through the accelerated source and a bake
    // through the full walk must agree to float rounding -- which is what makes
    // the cache usable for Smooth, whose working field is that very lattice.
    scene::Document doc = worked(80);
    const scene::LayerId id = doc.layers.front().id;
    const scene::Tape want = oracle_tape(doc);

    scene::ConsolidationParams params;
    params.cell_size = kCell;
    params.band = kCell * 3.0f;
    const kernel::cfloat3 pad = cf3(params.band, params.band, params.band);
    params.region = math::Aabb{want.bounds.min - pad, want.bounds.max + pad};

    SdfPrefixCache cache;
    REQUIRE(cache.build(doc, id, policy_keeping(8)) != nullptr);
    auto src = SdfSourceField::open(doc, id, &cache, policy_keeping(8));
    REQUIRE(src);
    REQUIRE(src->accelerated());

    const field::FieldVolume through_cache =
        field::FieldVolume::sample_blocks(src->block_fill(), params.region, params.cell_size,
                                          params.band);
    std::optional<field::FieldVolume> through_walk =
        scene::bake_tape(want, params, false, nullptr, {}, nullptr);
    REQUIRE(through_walk);

    // Compare the SAMPLES, before any redistance moves them.
    const field::FieldVolume raw_walk = field::FieldVolume::sample_blocks(
        [&want](const field::FieldVolume::BrickGrid& grid, std::size_t first, std::size_t count,
                float* out) {
            const std::size_t n = count * field::kBrickSamples;
            std::vector<float> pts(n * 3);
            for (std::size_t s = 0; s < count; ++s)
                for (int i = 0; i < field::kBrickSamples; ++i) {
                    const kernel::cfloat3 p = grid.sample_position(first + s, i);
                    const std::size_t at = (s * field::kBrickSamples + std::size_t(i)) * 3;
                    pts[at] = p.x;
                    pts[at + 1] = p.y;
                    pts[at + 2] = p.z;
                }
            eval::Backend* cpu = eval::Registry::instance().find("cpu");
            cpu->eval_points(want, eval::PointQuery{pts.data(), n, 1e-4f},
                             eval::PointResults{out, nullptr, nullptr});
        },
        params.region, params.cell_size, params.band);

    double worst = 0.0;
    std::size_t compared = 0;
    for (int gz = 0; gz < raw_walk.sample_extent(2); ++gz)
        for (int gy = 0; gy < raw_walk.sample_extent(1); ++gy)
            for (int gx = 0; gx < raw_walk.sample_extent(0); ++gx) {
                const std::optional<float> a = raw_walk.sample_at(gx, gy, gz);
                const std::optional<float> b = through_cache.sample_at(gx, gy, gz);
                if (!a || !b) continue;
                worst = std::max(worst, std::abs(double(*a) - double(*b)));
                ++compared;
            }
    CHECK(compared > 10000);
    MESSAGE("on-lattice worst error: " << worst);
    CHECK(worst < 1e-5);  // float rounding, not sampling
    CHECK(cache.stats().seeded_windows > 0);
}

TEST_CASE("prefix cache: opening a source never builds one") {
    // The property lazy Smooth's pointer-down cost rests on. Opening is
    // metadata plus a compile; a bake here would be exactly the whole-layer
    // cost the lazy path exists to remove.
    scene::Document doc = worked(60);
    SdfPrefixCache cache;
    auto src = SdfSourceField::open(doc, doc.layers.front().id, &cache, policy_keeping(8));
    REQUIRE(src);
    CHECK(cache.stats().builds == 0);
    CHECK(cache.stats().entries == 0);
    CHECK_FALSE(src->accelerated());  // ...and it is correct anyway
}

TEST_CASE("prefix cache: with no cache at all the source is still correct") {
    // The flush-a-CPU-cache property, directly: a null cache is the full walk.
    scene::Document doc = worked(60);
    const scene::Tape want = oracle_tape(doc);
    auto src = SdfSourceField::open(doc, doc.layers.front().id, nullptr);
    REQUIRE(src);
    CHECK_FALSE(src->accelerated());
    CHECK(worst_error(*src, want, random_points(3000, 1.4f)) < 1e-6);
}

TEST_CASE("prefix cache: a disabled policy caches nothing") {
    scene::Document doc = worked(60);
    SdfPrefixCache cache;
    SdfPrefixPolicy off = policy_keeping(8);
    off.max_bytes = 0;  // zero is OFF, not unbounded
    auto src = SdfSourceField::open(doc, doc.layers.front().id, &cache, off);
    REQUIRE(src);
    CHECK_FALSE(src->accelerated());
    CHECK(cache.stats().builds == 0);

    SdfPrefixPolicy no_cell = policy_keeping(8);
    no_cell.cell_size = 0.0f;
    CHECK(session::prefix_boundary_for(doc.layers.front(), no_cell) == 0);

    // ...and a history too short to be worth caching.
    SdfPrefixPolicy tall = policy_keeping(8, /*min_history=*/1000);
    CHECK(session::prefix_boundary_for(doc.layers.front(), tall) == 0);
}

// -- 9.3 / 9.4 / 9.5 the invalidation rules -----------------------------------

TEST_CASE("prefix cache: appending after the boundary keeps the prefix") {
    // The best case and the reason a whole-layer digest would be the wrong key:
    // the represented prefix did not change, so nothing should be rebuilt.
    scene::Document doc = worked(60);
    const scene::LayerId id = doc.layers.front().id;
    SdfPrefixCache cache;
    const SdfPrefixPolicy policy = policy_keeping(8);
    const std::size_t boundary = session::prefix_boundary_for(doc.layers.front(), policy);
    REQUIRE(boundary > 0);

    REQUIRE(cache.build(doc, id, policy) != nullptr);
    CHECK(cache.stats().builds == 1);
    const std::uint64_t before = session::layer_prefix_fingerprint(doc.layers.front(), boundary);

    scene::Node extra;
    extra.prim = scene::Prim::sphere(0.1f);
    extra.xform.position = cf3(0, 1.2f, 0);
    doc.layers.front().sdf->insert(extra);

    // The PREFIX digest is unchanged though the whole-layer one moved.
    CHECK(session::layer_prefix_fingerprint(doc.layers.front(), boundary) == before);
    CHECK(cache.find(doc.layers.front(), boundary, policy) != nullptr);
    CHECK(cache.stats().builds == 1);  // nothing was rebuilt

    // ...and the accelerated answer still equals the walk over the NEW layer.
    const scene::Tape want = oracle_tape(doc);
    auto src = SdfSourceField::open(doc, id, &cache, policy);
    REQUIRE(src);
    CHECK(worst_error(*src, want, random_points(3000, 1.4f)) < 1e-5);
}

TEST_CASE("prefix cache: editing the suffix keeps the prefix, editing the prefix does not") {
    scene::Document doc = worked(60);
    const scene::LayerId id = doc.layers.front().id;
    SdfPrefixCache cache;
    const SdfPrefixPolicy policy = policy_keeping(8);
    const std::size_t boundary = session::prefix_boundary_for(doc.layers.front(), policy);
    REQUIRE(cache.build(doc, id, policy) != nullptr);

    scene::SdfContent& content = *doc.layers.front().sdf;
    const std::vector<scene::NodeId> roots = content.roots;

    SUBCASE("a suffix edit is not the prefix's business") {
        content.find_mut(roots[boundary + 2])->prim = scene::Prim::sphere(0.2f);
        CHECK(cache.find(doc.layers.front(), boundary, policy) != nullptr);
    }
    SUBCASE("every kind of prefix edit invalidates") {
        // One property per subcase would be one revert per property; these are
        // the fields the digest exists to cover, and a miss on any of them is a
        // cache serving geometry the artist has already changed.
        const std::size_t inside = boundary / 2;
        SUBCASE("primitive") { content.find_mut(roots[inside])->prim = scene::Prim::sphere(0.3f); }
        SUBCASE("transform") { content.find_mut(roots[inside])->xform.position = cf3(9, 9, 9); }
        SUBCASE("op") { content.find_mut(roots[inside])->op = scene::Op::Subtract; }
        SUBCASE("blend") { content.find_mut(roots[inside])->blend.k = 0.5f; }
        SUBCASE("visibility") { content.find_mut(roots[inside])->visible = false; }
        SUBCASE("colour") { content.find_mut(roots[inside])->color = cf3(1, 0, 0); }
        SUBCASE("scale axes") { content.find_mut(roots[inside])->scale_axes = cf3(2, 1, 1); }
        SUBCASE("a deformer") {
            content.find_mut(roots[inside])
                ->deformers.push_back(scene::Deformer::grab(cf3(0, 0, 0), 0.5f, cf3(0, 0.1f, 0)));
        }
        SUBCASE("the layer's own transform") { doc.layers.front().xform.scale = 2.0f; }
        SUBCASE("the layer's mirror") { doc.layers.front().mirror_axes = scene::kMirrorX; }
        CHECK(cache.find(doc.layers.front(), boundary, policy) == nullptr);
        CHECK(cache.stats().invalidations >= 1);
    }
}

TEST_CASE("prefix cache: a stale entry is never served, whatever moved") {
    // The safety net, stated as the property rather than as a list: after ANY
    // edit, an accelerated read equals the walk. Either the cache noticed and
    // rebuilt, or it noticed and fell back; it may not answer from the old one.
    scene::Document doc = worked(60);
    const scene::LayerId id = doc.layers.front().id;
    SdfPrefixCache cache;
    const SdfPrefixPolicy policy = policy_keeping(8);
    REQUIRE(cache.build(doc, id, policy) != nullptr);

    scene::SdfContent& content = *doc.layers.front().sdf;
    content.find_mut(content.roots[3])->prim = scene::Prim::sphere(0.45f);

    const scene::Tape want = oracle_tape(doc);
    auto src = SdfSourceField::open(doc, id, &cache, policy);
    REQUIRE(src);
    CHECK(worst_error(*src, want, random_points(3000, 1.4f)) < 1e-5);
}

TEST_CASE("prefix cache: a sibling instance editing shared content invalidates") {
    // document.h shares SdfContent between instanced layers, so an edit through
    // a sibling is an edit here and the shared POINTER would not have moved.
    // The digest reads the content, which is what makes this work.
    scene::Document doc = worked(60);
    const scene::LayerId id = doc.layers.front().id;
    SdfPrefixCache cache;
    const SdfPrefixPolicy policy = policy_keeping(8);
    const std::size_t boundary = session::prefix_boundary_for(doc.layers.front(), policy);
    REQUIRE(cache.build(doc, id, policy) != nullptr);

    scene::Layer* instance = doc.instance_layer(id, "copy");
    REQUIRE(instance != nullptr);
    REQUIRE(instance->sdf.get() == doc.layers.front().sdf.get());

    instance->sdf->find_mut(instance->sdf->roots[2])->prim = scene::Prim::sphere(0.4f);
    CHECK(cache.find(doc.layers.front(), boundary, policy) == nullptr);
}

// -- 9.9 the memory budget ----------------------------------------------------

TEST_CASE("prefix cache: the budget evicts, and zero means off") {
    scene::Document doc = worked(60);
    const scene::LayerId id = doc.layers.front().id;
    SdfPrefixCache cache;

    // Two resolutions are two entries: the same prefix at two cell sizes is two
    // caches and neither is wrong.
    SdfPrefixPolicy fine = policy_keeping(8);
    SdfPrefixPolicy coarse = policy_keeping(8);
    coarse.cell_size = kCell * 2.0f;
    REQUIRE(cache.build(doc, id, fine) != nullptr);
    REQUIRE(cache.build(doc, id, coarse) != nullptr);
    CHECK(cache.stats().entries == 2);
    CHECK(cache.stats().bytes > 0);

    cache.set_max_bytes(1);  // evict everything it can
    CHECK(cache.stats().entries == 0);
    CHECK(cache.stats().evictions >= 2);
    CHECK(cache.stats().bytes == 0);

    cache.clear();
    CHECK(cache.stats().entries == 0);
}

TEST_CASE("prefix cache: invalidate_layer drops only that layer") {
    scene::Document doc = worked(60);
    const scene::LayerId id = doc.layers.front().id;
    SdfPrefixCache cache;
    const SdfPrefixPolicy policy = policy_keeping(8);
    REQUIRE(cache.build(doc, id, policy) != nullptr);
    CHECK(cache.stats().entries == 1);
    cache.invalidate_layer(id + 999);
    CHECK(cache.stats().entries == 1);
    cache.invalidate_layer(id);
    CHECK(cache.stats().entries == 0);
}

// -- the fingerprint pair -----------------------------------------------------

TEST_CASE("prefix cache: a whole-layer fingerprint is the prefix of every root") {
    // The invariant that keeps the two digests from drifting: one walks the
    // layer head then N roots, the other the layer head then all of them.
    scene::Document doc = worked(20);
    const scene::Layer& layer = doc.layers.front();
    CHECK(session::layer_prefix_fingerprint(layer, layer.sdf->roots.size()) ==
          session::layer_fingerprint(layer));

    // ...and a boundary is part of what is identified, so two prefixes that
    // agree on their shared roots are still different digests.
    CHECK(session::layer_prefix_fingerprint(layer, 5) !=
          session::layer_prefix_fingerprint(layer, 6));
}

TEST_CASE("prefix cache: a cancelled build caches nothing") {
    scene::Document doc = worked(60);
    SdfPrefixCache cache;
    parallel::CancelToken token;
    token.cancel();
    CHECK(cache.build(doc, doc.layers.front().id, policy_keeping(8), {}, &token) == nullptr);
    CHECK(cache.stats().entries == 0);
}
