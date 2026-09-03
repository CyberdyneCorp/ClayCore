#include <doctest/doctest.h>

#include <vector>

#include "clay/scene/bounds.h"
#include "clay/scene/document.h"
#include "kernel_utils.h"
#include "scene_utils.h"

// ONE LAYER WALK PER QUERY, however many intersects the layer holds (#451).
//
// An intersect's influence bound IS the layer's extent, and taking that extent
// walks every visible node computing its geometry bound -- which for a stroke
// or a sweep re-tessellates its curve, for a mirrored item bounds every copy,
// and for a deformer chain runs its slope probes. It is not a field read.
//
// Before #319 an intersect answered `Everything` in constant time, so nothing
// cared how many of them a layer held. After it, every caller that meets an
// intersect meets it in a LOOP -- `layer_influence_bound` once per root,
// `pick`'s attribution once per item, `clay_brick_cache_mark_dirty_nodes` once
// per selected node -- and each of those went from O(items) to
// O(intersects * items). Measured on a layer of 1,000 strokes holding 20
// intersects: a layer bound 0.082 ms on v0.73.0 against 1.61 ms on v0.78.0,
// and an attributed raycast 1.51 ms against 3.09 ms.
//
// The count is the test, not a timing. `LayerExtent` returns exactly what
// recomputing the extent returns, so no bound can tell whether it fired; a
// caller that stopped sharing one would read as correct and pay the walk per
// intersect again.

using namespace clay;
using kernel::cf3;

namespace {

// `roots` items in one layer, of which every `every`-th is an INTERSECT. The
// spheres are placed apart so the extent is a real union rather than one box
// repeated -- a memo that returned a stale box would pass a one-item fixture.
scene::Document layer_of(int roots, int every) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    for (int i = 0; i < roots; ++i) {
        const scene::Op op = (every > 0 && i % every == every - 1) ? scene::Op::Intersect
                                                                  : scene::Op::Add;
        const float x = 0.4f * static_cast<float>(i);
        l.sdf->insert(clay_test::item(scene::Prim::sphere(0.3f), cf3(x, 0, 0), op));
    }
    return doc;
}

}  // namespace

TEST_CASE("a layer bound walks the layer once, not once per intersect") {
    // Twenty intersects among a hundred roots: the shape #451 measured.
    scene::Document doc = layer_of(/*roots=*/100, /*every=*/5);
    const scene::Layer& layer = doc.layers.front();

    scene::LayerExtent extent;
    const math::Aabb bound = scene::layer_influence_bound(layer, &extent);

    CHECK(extent.walks() == 1);
    CHECK_FALSE(bound.empty());
    CHECK_FALSE(bound.is_infinite());
}

TEST_CASE("the memo answers what recomputing answers") {
    // The whole risk of a memo: that it is fast and wrong. Every node's bound
    // is taken both ways and required to agree exactly -- not band-clamped,
    // because these are the same expression evaluated twice and anything but
    // equality is a bug rather than drift.
    scene::Document doc = layer_of(/*roots=*/40, /*every=*/3);
    const scene::Layer& layer = doc.layers.front();
    scene::LayerExtent extent;

    for (scene::NodeId id : layer.sdf->roots) {
        const math::Aabb fresh = scene::node_influence_bound(*layer.sdf, id, layer);
        const math::Aabb memoed = scene::node_influence_bound(*layer.sdf, id, layer, &extent);
        CHECK(fresh.is_infinite() == memoed.is_infinite());
        if (fresh.is_infinite()) continue;
        CHECK(fresh.min.x == doctest::Approx(memoed.min.x));
        CHECK(fresh.min.y == doctest::Approx(memoed.min.y));
        CHECK(fresh.min.z == doctest::Approx(memoed.min.z));
        CHECK(fresh.max.x == doctest::Approx(memoed.max.x));
        CHECK(fresh.max.y == doctest::Approx(memoed.max.y));
        CHECK(fresh.max.z == doctest::Approx(memoed.max.z));
    }
    // One walk for all forty, and the intersects among them agreed with it.
    CHECK(extent.walks() == 1);
}

TEST_CASE("the memo recomputes when the layer changes") {
    // THE SOUNDNESS HAZARD, and the reason the memo is keyed on the pair rather
    // than on the content alone. An instanced layer SHARES its content by
    // shared_ptr and places it under its own transform, so one SdfContent has
    // two extents. `node_influence_bound_in_document` visits every sharer with
    // the same memo; carrying the first layer's box into the second would
    // report a box the edit does not cover, which is a stale brick rather than
    // a slow one (#325 is the same failure from the other direction).
    scene::Document doc = layer_of(/*roots=*/8, /*every=*/4);
    const scene::LayerId src = doc.layers.front().id;
    scene::Layer* copy = doc.instance_layer(src, "instance");
    REQUIRE(copy != nullptr);
    copy->xform.position = cf3(50.0f, 0, 0);

    const scene::SdfContent& content = *doc.layers.front().sdf;
    REQUIRE(doc.layers.back().sdf.get() == &content);

    scene::LayerExtent extent;
    const math::Aabb here = extent.of(content, doc.layers.front());
    const math::Aabb there = extent.of(content, doc.layers.back());
    CHECK(extent.walks() == 2);
    // Fifty units apart, so a memo that answered from the wrong layer would be
    // caught by the box rather than by the count alone.
    CHECK(there.min.x > here.max.x);

    // And the document-wide bound, which is what a host is handed, still covers
    // BOTH placements.
    scene::NodeId intersect = 0;
    for (scene::NodeId id : content.roots)
        if (content.find(id)->op == scene::Op::Intersect) intersect = id;
    REQUIRE(intersect != 0);
    scene::LayerExtent shared;
    const math::Aabb both =
        scene::node_influence_bound_in_document(doc, content, intersect, &shared);
    CHECK(shared.walks() == 2);
    CHECK(both.min.x <= here.min.x);
    CHECK(both.max.x >= there.max.x);
}

TEST_CASE("a layer with no intersect never walks") {
    // The other half of the contract: threading a memo through costs a layer
    // that has no intersect nothing at all, because nothing asks it for the
    // extent. This is what makes the parameter safe to add everywhere.
    scene::Document doc = layer_of(/*roots=*/50, /*every=*/0);
    const scene::Layer& layer = doc.layers.front();

    scene::LayerExtent extent;
    const math::Aabb bound = scene::layer_influence_bound(layer, &extent);

    CHECK(extent.walks() == 0);
    CHECK_FALSE(bound.empty());
}
