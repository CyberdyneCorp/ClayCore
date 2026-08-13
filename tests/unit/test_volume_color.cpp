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
