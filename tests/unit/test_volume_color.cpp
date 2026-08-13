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
    const std::optional<scene::Document> back = scene::deserialize_document(older.data(), older.size());
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
