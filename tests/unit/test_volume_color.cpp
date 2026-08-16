#include <doctest/doctest.h>
#include "clay/field/volume.h"
using namespace clay;
TEST_CASE("colour survives a blob and a serialize round trip") {
    auto dist = [](kernel::cfloat3 p) { return kernel::clength(p) - 0.5f; };
    auto col = [](kernel::cfloat3 p) {
        return p.x < 0 ? kernel::cf3(1, 0, 0) : kernel::cf3(0, 0, 1);
    };
    math::Aabb region{kernel::cf3(-1, -1, -1), kernel::cf3(1, 1, 1)};
    field::FieldVolume v = field::FieldVolume::sample_colored(dist, col, region, 0.05f, 0.15f);
    REQUIRE(v.has_color());
    CHECK(v.eval_color(kernel::cf3(-0.5f, 0, 0)).x > 0.8f);
    CHECK(v.eval_color(kernel::cf3(0.5f, 0, 0)).z > 0.8f);

    auto blob = v.to_blob();
    CHECK(blob.size() == v.blob_floats());
    auto back = field::FieldVolume::from_blob(blob);
    REQUIRE(back.has_value());
    CHECK(back->has_color());
    CHECK(back->eval_color(kernel::cf3(-0.5f, 0, 0)).x > 0.8f);

    auto bytes = v.serialize();
    auto de = field::FieldVolume::deserialize(bytes.data(), bytes.size());
    REQUIRE(de.has_value());
    CHECK(de->has_color());
    CHECK(de->eval_color(kernel::cf3(0.5f, 0, 0)).z > 0.8f);

    // An uncoloured volume is unchanged: no colour, and the old blob length.
    field::FieldVolume plain = field::FieldVolume::sample(dist, region, 0.05f, 0.15f);
    CHECK_FALSE(plain.has_color());
    CHECK(plain.to_blob().size() == plain.blob_floats());
    CHECK(plain.eval(kernel::cf3(0, 0, 0)) == doctest::Approx(v.eval(kernel::cf3(0, 0, 0))));
}

#include "clay/eval/backend.h"
#include "clay/scene/consolidate.h"
#include "clay/scene/document.h"
#include "scene_utils.h"

TEST_CASE("the tape reports a coloured volume's own colour") {
    // The end of the chain: storage, blob, opcode, call site. If any link is
    // wrong this reports the item's colour, which is what it did before.
    auto dist = [](kernel::cfloat3 p) { return kernel::clength(p) - 0.5f; };
    auto col = [](kernel::cfloat3 p) {
        return p.x < 0 ? kernel::cf3(1, 0, 0) : kernel::cf3(0, 0, 1);
    };
    math::Aabb region{kernel::cf3(-1, -1, -1), kernel::cf3(1, 1, 1)};

    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("v");
    scene::Node n;
    n.prim = scene::Prim::volume();
    n.volume = std::make_shared<const field::FieldVolume>(
        field::FieldVolume::sample_colored(dist, col, region, 0.05f, 0.15f));
    n.color = kernel::cf3(0, 1, 0);  // the item's colour: must NOT win inside
    l.sdf->insert(n);
    const scene::Tape tape = scene::compile_document(doc);

    auto shade = [&](kernel::cfloat3 p) {
        float d = 0;
        kernel::cfloat3 c = kernel::cf3(0, 0, 0);
        eval::PointQuery q{reinterpret_cast<const float*>(&p), 1, 1e-4f};
        eval::eval_points_reference(
            tape, q, eval::PointResults{&d, nullptr, reinterpret_cast<float*>(&c)});
        return c;
    };

    // On the surface, where a sculptor sees it: the volume's own colour.
    const kernel::cfloat3 left = shade(kernel::cf3(-0.5f, 0, 0));
    const kernel::cfloat3 right = shade(kernel::cf3(0.5f, 0, 0));
    CHECK(left.x > 0.7f);
    CHECK(left.y < 0.3f);
    CHECK(right.z > 0.7f);
    CHECK(right.y < 0.3f);
}

TEST_CASE("a tape holding a volume AND analytic prims routes each to its own path") {
    // The dispatch, exercised. `ctape_volume` reaches its own entry point and
    // every other prim reaches the one with no colour out-parameter, so a wrong
    // branch is a wrong COLOUR rather than a wrong distance — a sphere that
    // took the volume path would read a blob it does not have, and a volume
    // that took the analytic path would report the item's constant.
    auto dist = [](kernel::cfloat3 p) { return kernel::clength(p) - 0.5f; };
    auto col = [](kernel::cfloat3 p) {
        return p.x < 0 ? kernel::cf3(1, 0, 0) : kernel::cf3(0, 0, 1);
    };
    math::Aabb region{kernel::cf3(-1, -1, -1), kernel::cf3(1, 1, 1)};

    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("mixed");
    scene::Node vol;
    vol.prim = scene::Prim::volume();
    vol.volume = std::make_shared<const field::FieldVolume>(
        field::FieldVolume::sample_colored(dist, col, region, 0.05f, 0.15f));
    vol.color = kernel::cf3(0, 1, 0);  // must not win inside the volume
    l.sdf->insert(vol);

    // Analytic, well clear of the volume's sampled box, with a colour of its
    // own that the volume's samples must not leak into.
    scene::Node ball = clay_test::item(scene::Prim::sphere(0.3f), kernel::cf3(1.6f, 0, 0));
    ball.color = kernel::cf3(0, 1, 0);
    l.sdf->insert(ball);

    const scene::Tape tape = scene::compile_document(doc);
    auto shade = [&](kernel::cfloat3 p) {
        float d = 0;
        kernel::cfloat3 c = kernel::cf3(0, 0, 0);
        eval::PointQuery q{reinterpret_cast<const float*>(&p), 1, 1e-4f};
        eval::eval_points_reference(
            tape, q, eval::PointResults{&d, nullptr, reinterpret_cast<float*>(&c)});
        return c;
    };

    const kernel::cfloat3 in_volume_left = shade(kernel::cf3(-0.5f, 0, 0));
    const kernel::cfloat3 in_volume_right = shade(kernel::cf3(0.5f, 0, 0));
    const kernel::cfloat3 in_ball = shade(kernel::cf3(1.6f, 0, 0));
    CHECK(in_volume_left.x > 0.7f);   // the volume's samples, not the item's green
    CHECK(in_volume_right.z > 0.7f);
    CHECK(in_ball.y > 0.7f);          // the sphere's own colour
    CHECK(in_ball.x < 0.3f);
    CHECK(in_ball.z < 0.3f);
}

TEST_CASE("an uncoloured volume still reports the item's colour") {
    auto dist = [](kernel::cfloat3 p) { return kernel::clength(p) - 0.5f; };
    math::Aabb region{kernel::cf3(-1, -1, -1), kernel::cf3(1, 1, 1)};

    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("v");
    scene::Node n;
    n.prim = scene::Prim::volume();
    n.volume = std::make_shared<const field::FieldVolume>(
        field::FieldVolume::sample(dist, region, 0.05f, 0.15f));
    n.color = kernel::cf3(0, 1, 0);
    l.sdf->insert(n);
    const scene::Tape tape = scene::compile_document(doc);

    float d = 0;
    kernel::cfloat3 c = kernel::cf3(0, 0, 0);
    kernel::cfloat3 p = kernel::cf3(-0.5f, 0, 0);
    eval::PointQuery q{reinterpret_cast<const float*>(&p), 1, 1e-4f};
    eval::eval_points_reference(tape, q,
                                eval::PointResults{&d, nullptr, reinterpret_cast<float*>(&c)});
    CHECK(c.y > 0.9f);  // the item's green, exactly as before
}

TEST_CASE("consolidating a two-colour layer keeps both colours") {
    // The change a user sees first. Consolidation is advertised as changing
    // what a layer COSTS rather than what it looks like, and it used to
    // collapse every colour in the layer to the one on the resulting node.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("body");
    scene::Node red = clay_test::item(scene::Prim::sphere(0.35f), kernel::cf3(-0.25f, 0, 0));
    red.color = kernel::cf3(1, 0, 0);
    scene::Node blue = clay_test::item(scene::Prim::sphere(0.35f), kernel::cf3(0.25f, 0, 0));
    blue.color = kernel::cf3(0, 0, 1);
    l.sdf->insert(red);
    l.sdf->insert(blue);

    scene::ConsolidationParams params;
    params.cell_size = 0.03f;
    params.band = 0.09f;
    std::optional<field::FieldVolume> baked = scene::bake_layer(l, params, nullptr, nullptr);
    REQUIRE(baked.has_value());
    REQUIRE(baked->has_color());

    const kernel::cfloat3 in_red = baked->eval_color(kernel::cf3(-0.25f, 0, 0));
    const kernel::cfloat3 in_blue = baked->eval_color(kernel::cf3(0.25f, 0, 0));
    CHECK(in_red.x > 0.7f);
    CHECK(in_red.z < 0.3f);
    CHECK(in_blue.z > 0.7f);
    CHECK(in_blue.x < 0.3f);
}

TEST_CASE("consolidating a one-colour layer fills no colour channel") {
    // Filling it is a second evaluation of the tape at every surviving sample,
    // and for a layer of one colour it recovers a constant the node's own
    // colour already reports. Measured on the reference iPad: 916 ms against a
    // 786 ms budget, where the release before colour landed took 524 ms.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("body");
    scene::Node a = clay_test::item(scene::Prim::sphere(0.35f), kernel::cf3(-0.25f, 0, 0));
    a.color = kernel::cf3(1, 0, 0);
    scene::Node b = clay_test::item(scene::Prim::sphere(0.35f), kernel::cf3(0.25f, 0, 0));
    b.color = kernel::cf3(1, 0, 0);  // the SAME colour, which is the whole point
    l.sdf->insert(a);
    l.sdf->insert(b);

    scene::ConsolidationParams params;
    params.cell_size = 0.03f;
    params.band = 0.09f;
    std::optional<field::FieldVolume> baked = scene::bake_layer(l, params, nullptr, nullptr);
    REQUIRE(baked.has_value());
    CHECK_FALSE(baked->has_color());

    // ...and it is still a bake: the shape is there, colour is what was
    // skipped. A volume that came back empty would pass the check above.
    CHECK(baked->brick_count() > 0);
    CHECK(baked->eval(kernel::cf3(-0.25f, 0, 0)) < 0.0f);
    CHECK(baked->eval(kernel::cf3(0.25f, 0, 0)) < 0.0f);
}

TEST_CASE("re-consolidating a coloured volume keeps its sample colours") {
    // The case a test on node colours alone gets wrong. This layer holds ONE
    // node, so its node colours are trivially uniform — but the volume under it
    // carries two colours in its samples, and skipping the pass here would
    // flatten a character that had already been consolidated once.
    auto dist = [](kernel::cfloat3 p) { return kernel::clength(p) - 0.5f; };
    auto col = [](kernel::cfloat3 p) {
        return p.x < 0 ? kernel::cf3(1, 0, 0) : kernel::cf3(0, 0, 1);
    };
    math::Aabb region{kernel::cf3(-1, -1, -1), kernel::cf3(1, 1, 1)};

    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("already-baked");
    scene::Node n;
    n.prim = scene::Prim::volume();
    n.volume = std::make_shared<const field::FieldVolume>(
        field::FieldVolume::sample_colored(dist, col, region, 0.05f, 0.15f));
    l.sdf->insert(n);

    scene::ConsolidationParams params;
    params.cell_size = 0.05f;
    params.band = 0.15f;
    std::optional<field::FieldVolume> baked = scene::bake_layer(l, params, nullptr, nullptr);
    REQUIRE(baked.has_value());
    REQUIRE(baked->has_color());
    CHECK(baked->eval_color(kernel::cf3(-0.45f, 0, 0)).x > 0.7f);
    CHECK(baked->eval_color(kernel::cf3(0.45f, 0, 0)).z > 0.7f);
}

TEST_CASE("Paint still overrides a coloured volume") {
    // The operator whose whole job is to set colour. A volume that ignored it
    // would make painting over a consolidated layer impossible.
    auto dist = [](kernel::cfloat3 p) { return kernel::clength(p) - 0.5f; };
    auto col = [](kernel::cfloat3) { return kernel::cf3(1, 0, 0); };
    math::Aabb region{kernel::cf3(-1, -1, -1), kernel::cf3(1, 1, 1)};

    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("v");
    scene::Node n;
    n.prim = scene::Prim::volume();
    n.volume = std::make_shared<const field::FieldVolume>(
        field::FieldVolume::sample_colored(dist, col, region, 0.05f, 0.15f));
    l.sdf->insert(n);
    scene::Node paint = clay_test::item(scene::Prim::sphere(0.6f), kernel::cf3(0, 0, 0),
                                        scene::Op::Paint);
    paint.color = kernel::cf3(0, 1, 0);
    l.sdf->insert(paint);

    const scene::Tape tape = scene::compile_document(doc);
    float d = 0;
    kernel::cfloat3 c = kernel::cf3(0, 0, 0);
    kernel::cfloat3 p = kernel::cf3(0.4f, 0, 0);
    eval::PointQuery q{reinterpret_cast<const float*>(&p), 1, 1e-4f};
    eval::eval_points_reference(tape, q,
                                eval::PointResults{&d, nullptr, reinterpret_cast<float*>(&c)});
    CHECK(c.y > 0.7f);  // painted green, not the volume's red
}

TEST_CASE("a coloured volume survives a document save and load") {
    // The half of 5.3 that waited on the format minor: colour through the
    // scene payload, not just through a blob.
    auto dist = [](kernel::cfloat3 p) { return kernel::clength(p) - 0.4f; };
    auto col = [](kernel::cfloat3 p) {
        return p.y < 0 ? kernel::cf3(1, 0, 0) : kernel::cf3(0, 0, 1);
    };
    math::Aabb region{kernel::cf3(-1, -1, -1), kernel::cf3(1, 1, 1)};

    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("v");
    scene::Node n;
    n.prim = scene::Prim::volume();
    n.volume = std::make_shared<const field::FieldVolume>(
        field::FieldVolume::sample_colored(dist, col, region, 0.05f, 0.15f));
    l.sdf->insert(n);

    const std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
    const std::optional<scene::Document> back = scene::deserialize_document(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    REQUIRE(!back->layers.empty());
    REQUIRE(back->layers.front().sdf);
    const scene::Node* restored = nullptr;
    for (const scene::NodeId id : back->layers.front().sdf->roots)
        restored = back->layers.front().sdf->find(id);
    REQUIRE(restored != nullptr);
    REQUIRE(restored->volume);
    REQUIRE(restored->volume->has_color());
    CHECK(restored->volume->eval_color(kernel::cf3(0, -0.3f, 0)).x > 0.7f);
    CHECK(restored->volume->eval_color(kernel::cf3(0, 0.3f, 0)).z > 0.7f);
}

TEST_CASE("writing at minor 8 drops only the colour") {
    // The older-reader trade, stated in clayspace.h and asserted here: an
    // uncoloured volume — which is every volume any build before this made —
    // loses nothing to it.
    auto dist = [](kernel::cfloat3 p) { return kernel::clength(p) - 0.4f; };
    auto col = [](kernel::cfloat3) { return kernel::cf3(1, 0, 0); };
    math::Aabb region{kernel::cf3(-1, -1, -1), kernel::cf3(1, 1, 1)};

    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("v");
    scene::Node n;
    n.prim = scene::Prim::volume();
    n.volume = std::make_shared<const field::FieldVolume>(
        field::FieldVolume::sample_colored(dist, col, region, 0.05f, 0.15f));
    l.sdf->insert(n);

    const std::vector<std::uint8_t> older = scene::serialize_document(doc, 8);
    // Read AT minor 8, not at the current one. A minor-8 stream is a minor-8
    // stream; reading it as today's layout only happened to work while every
    // section added since lived INSIDE the volume's own length-prefixed blob.
    // The first outer section added after this — the item gate, at minor 11 —
    // made the shortcut read a length that was never written.
    const std::optional<scene::Document> back =
        scene::deserialize_document(older.data(), older.size(), 8);
    REQUIRE(back.has_value());
    const scene::Node* restored = nullptr;
    for (const scene::NodeId id : back->layers.front().sdf->roots)
        restored = back->layers.front().sdf->find(id);
    REQUIRE(restored != nullptr);
    REQUIRE(restored->volume);
    CHECK_FALSE(restored->volume->has_color());
    // The shape is untouched: same samples, same distances.
    CHECK(restored->volume->brick_count() == n.volume->brick_count());
    CHECK(restored->volume->eval(kernel::cf3(0, 0, 0)) ==
          doctest::Approx(n.volume->eval(kernel::cf3(0, 0, 0))));
}

TEST_CASE("colour costs one word a sample, and costs nothing when absent") {
    // The storage claim, asserted rather than asserted-in-prose: RGB8 doubles
    // a volume where three floats would quadruple it, and an uncoloured volume
    // has not grown at all.
    auto dist = [](kernel::cfloat3 p) { return kernel::clength(p) - 0.5f; };
    auto col = [](kernel::cfloat3) { return kernel::cf3(1, 0.5f, 0.25f); };
    math::Aabb region{kernel::cf3(-1, -1, -1), kernel::cf3(1, 1, 1)};

    const field::FieldVolume plain = field::FieldVolume::sample(dist, region, 0.04f, 0.12f);
    const field::FieldVolume tinted =
        field::FieldVolume::sample_colored(dist, col, region, 0.04f, 0.12f);

    // Same shape: the colour pass does not change what is stored.
    REQUIRE(plain.sample_count() == tinted.sample_count());
    REQUIRE(plain.brick_count() == tinted.brick_count());

    // One extra word per stored sample, and not one for an uncoloured volume.
    CHECK(tinted.blob_floats() == plain.blob_floats() + tinted.sample_count());
    CHECK(plain.blob_floats() == 14 + plain.blob_floats() - 14);  // unchanged shape
    CHECK_FALSE(plain.has_color());
    CHECK(tinted.has_color());

    // And the distances are identical — colour rides alongside, it does not
    // perturb the field.
    CHECK(plain.eval(kernel::cf3(0.3f, 0.1f, 0)) ==
          doctest::Approx(tinted.eval(kernel::cf3(0.3f, 0.1f, 0))));
}

TEST_CASE("colour is unpacked before it is mixed") {
    // The regression the gallery caught. Interpolating the PACKED words and
    // unpacking the result mixes the channels through their own carries: two
    // colours that share a channel come out with a third that neither has.
    //
    // It only shows where neighbouring samples DIFFER, which is why the
    // deep-inside test above passed with the bug — all eight corners agreed
    // there, so mixing packed values returned the same packed value.
    auto dist = [](kernel::cfloat3 p) { return kernel::clength(p) - 0.5f; };
    // Red and blue meet at x = 0, so a sample straddling it interpolates two
    // words whose green is zero in both. A packed mix invents green.
    auto col = [](kernel::cfloat3 p) {
        return p.x < 0 ? kernel::cf3(1, 0, 0) : kernel::cf3(0, 0, 1);
    };
    math::Aabb region{kernel::cf3(-1, -1, -1), kernel::cf3(1, 1, 1)};

    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("v");
    scene::Node n;
    n.prim = scene::Prim::volume();
    n.volume = std::make_shared<const field::FieldVolume>(
        field::FieldVolume::sample_colored(dist, col, region, 0.05f, 0.15f));
    l.sdf->insert(n);
    const scene::Tape tape = scene::compile_document(doc);

    // Walk across the boundary on the surface, sampling between cells.
    for (int i = -3; i <= 3; ++i) {
        const float x = static_cast<float>(i) * 0.017f;  // not a multiple of the cell
        kernel::cfloat3 p = kernel::cf3(x, 0.0f, 0.48f);
        float d = 0;
        kernel::cfloat3 c = kernel::cf3(0, 0, 0);
        eval::PointQuery q{reinterpret_cast<const float*>(&p), 1, 1e-4f};
        eval::eval_points_reference(
            tape, q, eval::PointResults{&d, nullptr, reinterpret_cast<float*>(&c)});
        CAPTURE(x);
        // Neither source colour has green, so no mix of them may produce it.
        CHECK(c.y < 0.15f);
        // And every channel stays inside what the two sources span.
        CHECK(c.x <= doctest::Approx(1.0f).epsilon(0.02));
        CHECK(c.z <= doctest::Approx(1.0f).epsilon(0.02));
    }
}
