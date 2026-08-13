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
