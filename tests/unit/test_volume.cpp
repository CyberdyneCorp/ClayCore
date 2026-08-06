// Sampled fields (sdf-kernels + scene-model specs, add-sampled-fields).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "clay.h"
#include "clay/field/volume.h"
#include "clay/io/clayspace.h"
#include "clay/scene/commands.h"
#include "clay/kernel/exactness.h"
#include "clay/scene/bounds.h"
#include "clay/scene/tape.h"

using namespace clay;
using field::FieldVolume;
using kernel::cf3;

namespace {

auto sphere_field(float r) {
    return [r](kernel::cfloat3 p) { return kernel::clength(p) - r; };
}

FieldVolume sphere_volume(float r = 0.7f, float cell = 0.06f, float band = 0.25f,
                          float half = 1.2f) {
    return FieldVolume::sample(sphere_field(r),
                               math::Aabb(cf3(-half, -half, -half), cf3(half, half, half)), cell,
                               band);
}

scene::Node volume_node(const FieldVolume& v) {
    scene::Node n;
    n.prim = scene::Prim::volume();
    n.volume = std::make_shared<FieldVolume>(v);
    return n;
}

scene::Tape compile_one(const scene::Node& node) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(node);
    return scene::compile_document(doc);
}

}  // namespace

TEST_CASE("volume: a sampled sphere reproduces its source near the surface") {
    const float r = 0.7f;
    FieldVolume v = sphere_volume(r);
    auto exact = sphere_field(r);

    // Within the band the volume is a real distance, so it should track the
    // source to about a cell.
    for (float x = -0.9f; x <= 0.9f; x += 0.07f)
        for (float y = -0.9f; y <= 0.9f; y += 0.11f) {
            kernel::cfloat3 p = cf3(x, y, 0.03f);
            float truth = exact(p);
            if (std::abs(truth) > 0.2f) continue;  // outside the band by design
            CAPTURE(x);
            CAPTURE(y);
            CHECK(v.eval(p) == doctest::Approx(truth).epsilon(0.15));
        }
}

TEST_CASE("volume: storage follows the surface, not the region") {
    // The same sphere in a region eight times the volume: a dense grid would
    // grow with the box, a narrow band with the surface.
    FieldVolume tight = sphere_volume(0.7f, 0.06f, 0.25f, 1.2f);
    FieldVolume roomy = sphere_volume(0.7f, 0.06f, 0.25f, 2.4f);

    CHECK(roomy.brick_count() < tight.brick_count() * 3);  // not 8x
    // ...and far smaller than dense would be.
    std::size_t dense_cells = 1;
    for (int a = 0; a < 3; ++a) dense_cells *= static_cast<std::size_t>(2.4f * 2 / 0.06f);
    CHECK(roomy.sample_count() * 10 < dense_cells);
    INFO("roomy stores " << roomy.sample_count() << " samples against " << dense_cells
                         << " dense");
}

TEST_CASE("volume: the sign is right far from the band") {
    FieldVolume v = sphere_volume();
    // Deep inside, well past the band, there are no samples at all — the brick
    // records which side it is on, which is the thing a band alone could not.
    CHECK(v.eval(cf3(0, 0, 0)) < 0.0f);
    CHECK(v.eval(cf3(1.1f, 1.1f, 1.1f)) > 0.0f);
    CHECK_FALSE(v.has_samples_at(cf3(0, 0, 0)));
}

TEST_CASE("volume: where there are no samples the value is a true lower bound") {
    const float r = 0.7f;
    FieldVolume v = sphere_volume(r, 0.06f, 0.25f);
    auto exact = sphere_field(r);
    // Note the partition is on has_samples_at, NOT on the band: a brick spans
    // eight cells and is kept whole, so a stored brick holds samples well
    // beyond the band and the interpolated branch reaches much further out
    // than the band width suggests.
    int checked = 0;
    for (float x = -3.0f; x <= 3.0f; x += 0.13f)
        for (float y = -3.0f; y <= 3.0f; y += 0.17f) {
            kernel::cfloat3 p = cf3(x, y, 0.21f);
            if (v.has_samples_at(p)) continue;
            float truth = exact(p);
            CAPTURE(x);
            CAPTURE(y);
            // Sphere tracing may never be told a distance larger than the
            // truth, or it steps through the surface. Both sides, near and far.
            if (truth > 0.0f) CHECK(v.eval(p) <= truth + 1e-4f);
            if (truth < 0.0f) CHECK(v.eval(p) >= truth - 1e-4f);
            ++checked;
        }
    REQUIRE(checked > 100);  // the partition must not have emptied the test
}

TEST_CASE("volume: a skipped brick reports less than the band") {
    // A brick is skipped when every SAMPLE in it is beyond the band, but a
    // point between samples can be up to half a cell diagonal nearer the
    // surface. Reporting the band flat would overstep by that much.
    const float cell = 0.06f, band = 0.25f;
    FieldVolume v = sphere_volume(0.7f, cell, band);
    kernel::cfloat3 middle = cf3(0, 0, 0);
    REQUIRE_FALSE(v.has_samples_at(middle));
    CHECK(v.eval(middle) == doctest::Approx(-v.far_value()));
    CHECK(v.far_value() < band);
    CHECK(v.far_value() >= band - 0.87f * cell);

    SUBCASE("a band thinner than the samples is widened rather than obeyed") {
        // Left alone it would drive the far value to zero, which stalls a
        // marcher instead of stepping it.
        FieldVolume thin = sphere_volume(0.7f, 0.1f, 0.02f);
        CHECK(thin.band() >= 0.2f);
        CHECK(thin.far_value() > 0.0f);
    }
}

// Inside the band the value is an INTERPOLATION, and interpolating a convex
// field overshoots: trilinear interpolation lies above the function it samples
// by O(cell^2 x curvature). It is not a lower bound there and cannot be made
// one without biasing the samples, which would visibly move the surface. What
// it is instead is accurate to the sampling, and that is what the caller
// controls.
TEST_CASE("volume: inside the band the error is bounded by the sampling") {
    const float r = 0.7f;
    auto exact = sphere_field(r);
    auto worst_error = [&](float cell) {
        FieldVolume v = sphere_volume(r, cell, 0.25f);
        float worst = 0.0f;
        for (float x = -1.0f; x <= 1.0f; x += 0.037f)
            for (float y = -1.0f; y <= 1.0f; y += 0.041f) {
                kernel::cfloat3 p = cf3(x, y, 0.013f);
                float truth = exact(p);
                if (std::abs(truth) > 0.2f) continue;
                worst = std::max(worst, std::abs(v.eval(p) - truth));
            }
        return worst;
    };

    float coarse = worst_error(0.12f);
    float fine = worst_error(0.03f);
    // The error is small at a usable cell size...
    CHECK(coarse < 0.05f);
    // ...and shrinks with it, which is the property that makes "choose a cell
    // size" a real control rather than a hope.
    CHECK(fine < coarse * 0.5f);
    INFO("worst error: " << coarse << " at cell 0.12, " << fine << " at cell 0.03");
}

TEST_CASE("volume: the seam at a skipped brick jumps, and jumps the safe way") {
    // Where a stored brick meets a skipped one the field is DISCONTINUOUS: one
    // side interpolates a real distance, the other reports a flat bound. This
    // is not fixable without storing the bricks the sparsity exists to avoid,
    // so what matters is that the jump can only ever go from a bound UP to a
    // distance, never from a distance up to something larger than the truth.
    //
    // Marching is safe either way, because each value is individually a lower
    // bound or an accurate distance. Gradients across the seam are not
    // meaningful — and nothing reads them there, since the seam sits a band
    // away from the surface while normals are taken at it.
    const float r = 0.7f, cell = 0.09f, band = 0.3f;
    FieldVolume v = sphere_volume(r, cell, band);
    auto exact = sphere_field(r);

    // A plane, not a line: along the sphere's equator every brick clips the
    // band, so a line through the middle never meets a seam at all.
    int jumps = 0;
    const float h = 1e-3f;
    for (float y = -1.15f; y <= 1.15f; y += 0.031f)
        for (float x = -1.15f; x <= 1.15f; x += 0.011f) {
            kernel::cfloat3 a = cf3(x - h, y, 0.05f);
            kernel::cfloat3 b = cf3(x + h, y, 0.05f);
            if (v.has_samples_at(a) == v.has_samples_at(b)) continue;
            if (std::abs(v.eval(b) - v.eval(a)) > 0.01f) ++jumps;
            CAPTURE(x);
            CAPTURE(y);
            // Both sides are individually safe, which is the property marching
            // needs; continuity is not.
            for (kernel::cfloat3 p : {a, b}) {
                float truth = exact(p);
                if (!v.has_samples_at(p)) {
                    if (truth > 0.0f) CHECK(v.eval(p) <= truth + 1e-4f);
                    if (truth < 0.0f) CHECK(v.eval(p) >= truth - 1e-4f);
                } else {
                    CHECK(std::abs(v.eval(p) - truth) < 2.0f * cell);
                }
            }
        }
    INFO("seam crossings that jumped: " << jumps);
    CHECK(jumps > 0);  // the seam must actually be in the sweep
}

TEST_CASE("volume: a ray finds the real surface, not the edge of the sampling") {
    // Regression. The value outside the sampled box used to be the distance to
    // the BOX, which falls to zero on the box face — and a sphere tracer reads
    // zero as a surface, so every ray hit an invisible shell where the
    // sampling stopped. Nothing caught it, because the tests all probed eval()
    // at points and never marched.
    const float r = 0.7f;
    FieldVolume v = sphere_volume(r, 0.05f, 0.2f, 1.3f);
    scene::Tape tape = compile_one(volume_node(v));

    auto march = [&](kernel::cfloat3 eye, kernel::cfloat3 dir) {
        const float scale = kernel::csafe_step_scale(tape.info);
        float t = 0.0f;
        for (int i = 0; i < 400; ++i) {
            kernel::cfloat3 p = eye + dir * t;
            float d = tape.eval(p).d;
            if (d < 1e-4f) return t;
            t += d * scale;
            if (t > 20.0f) break;
        }
        return -1.0f;
    };

    // Straight at the sphere from well outside the sampled box.
    float hit = march(cf3(0, 0, 4.0f), cf3(0, 0, -1.0f));
    REQUIRE(hit > 0.0f);                                  // it arrives at all
    CHECK(hit == doctest::Approx(4.0f - r).epsilon(0.02));  // at the sphere...
    CHECK(hit > 2.0f);                                     // ...not at the box face

    SUBCASE("a ray that misses the sphere misses the box too") {
        // Offset past the sphere but still through the sampled box, which is
        // where the phantom shell used to be.
        CHECK(march(cf3(1.0f, 0, 4.0f), cf3(0, 0, -1.0f)) < 0.0f);
    }
}

TEST_CASE("volume: empty space opens up as it gets emptier") {
    // Regression. Every sample-free brick used to report the same flat band
    // width, so a marcher crossing the empty majority of the region took steps
    // that never grew and ran out of iterations before arriving.
    FieldVolume v = sphere_volume(0.7f, 0.05f, 0.2f, 3.0f);
    const float brick = 8 * 0.05f;

    // Walking away from the surface along +x, the reported distance must keep
    // opening up rather than sitting at the floor.
    float near_surface = v.eval(cf3(1.0f, 0, 0));
    float further = v.eval(cf3(2.0f, 0, 0));
    float furthest = v.eval(cf3(2.9f, 0, 0));
    CHECK(near_surface < further);
    CHECK(further < furthest);
    CHECK(furthest > brick);  // more than one brick's worth, not the band floor

    SUBCASE("and it is still a bound at every step") {
        for (float x = 0.8f; x < 3.0f; x += 0.03f) {
            kernel::cfloat3 p = cf3(x, 0, 0);
            CAPTURE(x);
            CHECK(v.eval(p) <= (x - 0.7f) + 1e-4f);
        }
    }

    SUBCASE("so crossing the region takes a sane number of steps") {
        const float scale = kernel::csafe_step_scale(kernel::cfi_volume());
        float t = 0.0f;
        int steps = 0;
        for (; steps < 1000 && t < 2.2f; ++steps) {
            float d = v.eval(cf3(-3.5f + t, 0, 0));
            if (d < 1e-4f) break;
            t += d * scale;
        }
        INFO("steps to cross: " << steps);
        CHECK(steps < 120);
    }
}

TEST_CASE("volume: it combines like any other primitive") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node box;
    box.prim = scene::Prim::box(cf3(0.9f, 0.9f, 0.9f));
    l.sdf->insert(std::move(box));
    scene::Node cut = volume_node(sphere_volume(0.5f, 0.05f, 0.2f, 0.9f));
    cut.op = scene::Op::Subtract;
    l.sdf->insert(std::move(cut));

    scene::Tape tape = scene::compile_document(doc);
    CHECK(tape.eval(cf3(0, 0, 0)).d > 0.0f);          // the sphere is gone
    CHECK(tape.eval(cf3(0.8f, 0.8f, 0.8f)).d < 0.0f);  // the box corner remains
}

TEST_CASE("volume: exactness is declared, and so is the interpolant's slope") {
    scene::Tape tape = compile_one(volume_node(sphere_volume()));
    CHECK_FALSE(tape.info.is_exact);

    // Not 1. Trilinear interpolation of a 1-Lipschitz field bounds each
    // partial derivative by 1, so the gradient magnitude by sqrt(3); claiming
    // 1 would let the marcher take a full step where the interpolant is
    // steeper than the field it samples.
    CHECK(tape.info.lipschitz == doctest::Approx(std::sqrt(3.0f)).epsilon(1e-3));
    CHECK(kernel::csafe_step_scale(tape.info) < 1.0f);

    SUBCASE("the interpolant really can outrun its source") {
        // A check on the claim rather than on the constant: the measured slope
        // of the sampled field exceeds 1 somewhere, which is the thing a
        // declaration of 1.0 would have denied.
        //
        // The stencil is required to stay in stored bricks. Two ADJACENT
        // stored bricks are continuous across their shared face — that is what
        // the halo sample buys — but a stored brick against a skipped one is
        // not, and measuring across that seam measures the seam rather than
        // the interpolant. The seam is the subject of the next test.
        FieldVolume v = sphere_volume(0.7f, 0.09f, 0.3f);
        const float h = 1e-3f;
        const float z = 0.017f;
        auto sampled = [&](float x, float y) { return v.has_samples_at(cf3(x, y, z)); };
        float steepest = 0.0f;
        for (float x = -1.0f; x <= 1.0f; x += 0.011f)
            for (float y = -1.0f; y <= 1.0f; y += 0.013f) {
                if (!sampled(x, y) || !sampled(x + h, y) || !sampled(x - h, y) ||
                    !sampled(x, y + h) || !sampled(x, y - h))
                    continue;
                float gx = (v.eval(cf3(x + h, y, z)) - v.eval(cf3(x - h, y, z))) / (2 * h);
                float gy = (v.eval(cf3(x, y + h, z)) - v.eval(cf3(x, y - h, z))) / (2 * h);
                steepest = std::max(steepest, std::sqrt(gx * gx + gy * gy));
            }
        INFO("steepest measured slope " << steepest);
        CHECK(steepest > 1.0f);
        CHECK(steepest <= std::sqrt(3.0f) + 1e-2f);
    }
}

TEST_CASE("volume: bounds are the sampled box") {
    FieldVolume v = sphere_volume(0.7f, 0.06f, 0.25f, 1.2f);
    scene::Node n = volume_node(v);
    math::Aabb b = scene::item_local_bounds(n);
    CHECK(b.min.x <= -1.2f);
    CHECK(b.max.x >= 1.2f);
}

TEST_CASE("volume: an empty volume contributes nothing rather than reading garbage") {
    scene::Node n;
    n.prim = scene::Prim::volume();
    CHECK(compile_one(n).empty());  // no volume at all

    n.volume = std::make_shared<FieldVolume>(
        FieldVolume::sample(sphere_field(1.0f), math::Aabb(), 0.1f, 0.3f));
    CHECK(n.volume->empty());
    CHECK(compile_one(n).empty());
}

TEST_CASE("volume: the blob round trips") {
    FieldVolume v = sphere_volume();
    std::vector<float> flat = v.to_blob();
    auto back = FieldVolume::from_blob(flat);
    REQUIRE(back.has_value());
    CHECK(back->brick_count() == v.brick_count());
    for (float x = -1.1f; x <= 1.1f; x += 0.19f)
        CHECK(back->eval(cf3(x, 0.1f, 0.2f)) == doctest::Approx(v.eval(cf3(x, 0.1f, 0.2f))));

    SUBCASE("a blob too short to hold a header is refused") {
        for (std::size_t cut : {std::size_t(0), std::size_t(5), std::size_t(9)}) {
            std::vector<float> stub(flat.begin(), flat.begin() + static_cast<std::ptrdiff_t>(cut));
            CHECK_FALSE(FieldVolume::from_blob(stub).has_value());
        }
    }

    SUBCASE("a blob whose index runs past its end is refused") {
        std::vector<float> lying = flat;
        lying[5] = 1e6f;  // an impossible brick count
        CHECK_FALSE(FieldVolume::from_blob(lying).has_value());
    }
}

TEST_CASE("volume: serializes and survives a save") {
    FieldVolume v = sphere_volume();
    std::vector<std::uint8_t> bytes = v.serialize();
    auto back = FieldVolume::deserialize(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    CHECK(back->brick_count() == v.brick_count());
    CHECK(back->cell_size() == doctest::Approx(v.cell_size()));
    CHECK(back->band() == doctest::Approx(v.band()));
    for (float x = -1.1f; x <= 1.1f; x += 0.23f)
        CHECK(back->eval(cf3(x, 0.1f, 0)) == doctest::Approx(v.eval(cf3(x, 0.1f, 0))));

    CHECK_FALSE(FieldVolume::deserialize(bytes.data(), 3).has_value());
}

TEST_CASE("volume: a document keeps its volume across a save") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(volume_node(sphere_volume()));

    std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
    std::optional<scene::Document> back = scene::deserialize_document(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    scene::Tape before = scene::compile_document(doc);
    scene::Tape after = scene::compile_document(*back);
    REQUIRE_FALSE(after.empty());
    for (float x = -1.1f; x <= 1.1f; x += 0.17f)
        CHECK(after.eval(cf3(x, 0.2f, 0)).d == doctest::Approx(before.eval(cf3(x, 0.2f, 0)).d));

    SUBCASE("a truncated volume fails the read rather than loading a hollow item") {
        // An item that silently contributed nothing would look like a document
        // that had merely lost a shape, which is the harder bug to notice.
        for (std::size_t drop : {std::size_t(4), bytes.size() / 3}) {
            std::vector<std::uint8_t> cut(bytes.begin(), bytes.end() - static_cast<std::ptrdiff_t>(drop));
            CHECK_FALSE(scene::deserialize_document(cut.data(), cut.size()).has_value());
        }
    }
}

TEST_CASE("volume: an older document still reads") {
    // Minor 3 has no volume field. A document written at that version must
    // still load, without its volume and without desynchronising the record
    // that follows it.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n = volume_node(sphere_volume());
    n.rounding = 0.03f;  // written AFTER the volume: proves the stream stays aligned
    n.transition.r0 = 0.25f;
    l.sdf->insert(std::move(n));

    std::vector<std::uint8_t> older = scene::serialize_document(doc, 3);
    std::optional<scene::Document> back = scene::deserialize_document(older.data(), older.size(), 3);
    REQUIRE(back.has_value());
    const scene::Node* got = nullptr;
    for (const auto& [id, c] : back->layers[0].sdf->nodes())
        if (c.prim.type == scene::PrimType::Volume) got = &c;
    REQUIRE(got != nullptr);
    CHECK(got->volume == nullptr);
    CHECK(got->rounding == doctest::Approx(0.03f));
    CHECK(got->transition.r0 == doctest::Approx(0.25f));
}

TEST_CASE("volume: the tape agrees with the volume it was built from") {
    FieldVolume v = sphere_volume();
    scene::Tape tape = compile_one(volume_node(v));
    // The kernel walks the blob's own index and halo; a disagreement here
    // would mean the two implementations of the same lookup had drifted.
    for (float x = -1.4f; x <= 1.4f; x += 0.11f)
        for (float y = -1.4f; y <= 1.4f; y += 0.17f) {
            kernel::cfloat3 p = cf3(x, y, 0.09f);
            CAPTURE(x);
            CAPTURE(y);
            CHECK(tape.eval(p).d == doctest::Approx(v.eval(p)).epsilon(1e-4));
        }
}
