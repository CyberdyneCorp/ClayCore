// Sampled fields (sdf-kernels + scene-model specs, add-sampled-fields).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "clay.h"
#include "clay/field/volume.h"
#include "kernel_utils.h"
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
                               math::Aabb{cf3(-half, -half, -half), cf3(half, half, half)}, cell,
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
        const float scale = kernel::csafe_step_scale(kernel::cfi_volume(1.0f));
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

    SUBCASE("a blob whose index ENTRY runs past the samples is refused") {
        // The section offsets were checked but the per-brick entries were not,
        // so one hostile entry became an arbitrary-offset read of 729 floats
        // in eval_inside and in the tape. Reachable by loading a .clayspace.
        const std::size_t index_off = static_cast<std::size_t>(flat[8]);
        const std::size_t index_size = static_cast<std::size_t>(flat[5]) *
                                       static_cast<std::size_t>(flat[6]) *
                                       static_cast<std::size_t>(flat[7]);
        for (float bad : {5e8f, -7.0f}) {
            std::vector<float> lying = flat;
            bool poisoned = false;
            for (std::size_t i = 0; i < index_size; ++i)
                if (lying[index_off + i] >= 0.0f) {
                    lying[index_off + i] = bad;
                    poisoned = true;
                }
            REQUIRE(poisoned);
            CHECK_FALSE(FieldVolume::from_blob(lying).has_value());
        }
    }

    SUBCASE("an empty-brick entry is still accepted") {
        // kBrickEmpty is the one negative value that is legal, so the new
        // bounds check must not reject an ordinary sparse volume.
        CHECK(FieldVolume::from_blob(flat).has_value());
    }

    SUBCASE("an entry off by less than a brick is snapped, not refused") {
        // Index entries are stored as float. Past 2^24 a float cannot hold
        // consecutive integers, so a large volume reads its own offsets back
        // rounded — and a bounds check written as an exact comparison then
        // refuses the LAST brick of any volume big enough to matter, which
        // means a document this library wrote will not reopen.
        //
        // Every stored brick starts on a kBrickSamples boundary, so the reader
        // snaps to that boundary. Perturbing an entry by a fraction of a brick
        // stands in for the float's own rounding at scale.
        const std::size_t index_off = static_cast<std::size_t>(flat[8]);
        const std::size_t index_size = static_cast<std::size_t>(flat[5]) *
                                       static_cast<std::size_t>(flat[6]) *
                                       static_cast<std::size_t>(flat[7]);
        for (float nudge : {1.0f, -1.0f, 4.0f}) {
            std::vector<float> jittered = flat;
            for (std::size_t i = 0; i < index_size; ++i)
                if (jittered[index_off + i] > 0.0f) jittered[index_off + i] += nudge;
            auto v2 = FieldVolume::from_blob(jittered);
            REQUIRE(v2.has_value());
            // and it reads the same field, i.e. it snapped to the true offset
            for (float x = -1.1f; x <= 1.1f; x += 0.31f)
                CHECK(v2->eval(cf3(x, 0.1f, 0.2f)) ==
                      doctest::Approx(v.eval(cf3(x, 0.1f, 0.2f))));
        }
    }

    SUBCASE("the last brick, which ends exactly at the end of the data, is accepted") {
        // The boundary case the exact comparison got wrong.
        auto v2 = FieldVolume::from_blob(flat);
        REQUIRE(v2.has_value());
        CHECK(v2->brick_count() == v.brick_count());
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
    // Reduced to a bool before the assertion: comparing the shared_ptr inside
    // the macro makes doctest try to stringify it, and MSVC's <memory> declares
    // an operator<< for shared_ptr that then fails to deduce a template
    // argument. A bool is stringifiable everywhere.
    const bool carries_a_volume = static_cast<bool>(got->volume);
    CHECK_FALSE(carries_a_volume);
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

TEST_CASE("volume: a volume reads its own blob, not the tape's") {
    // Issue #35. The symptom reported was "clay_item_volume_from_document
    // silently drops CLAY_PRIM_STROKE items": sampling a document containing a
    // stroke and swapping the region back produced the bare ball, and three
    // different ops agreed to four decimals because the item carrying them had
    // contributed nothing.
    //
    // Sampling was never the problem — a stroke round trips as well as a sphere
    // does. What was wrong is on the READ side: FieldVolume::to_blob writes its
    // index/far/data offsets relative to its OWN twelve-float header, and the
    // interpreter used them as absolute indices into the tape blob. Those two
    // agree exactly when the volume sits at blob offset 0, which is true when
    // nothing else out-of-line was emitted before it. A sphere carries no blob
    // payload, so a volume beside spheres worked and hid this for as long as it
    // did; a stroke, loft, swept or armature writes its points there first, and
    // the volume then read whatever they had left.
    //
    // So the test is not about strokes. It is that a volume means the same
    // thing wherever it lands in the blob.
    scene::Document src;
    src.add_sdf_layer("s").sdf->insert([] {
        scene::Node n;
        n.prim = scene::Prim::sphere(0.30f);
        return n;
    }());
    scene::Tape src_tape = scene::compile_document(src);
    field::FieldVolume vol = field::FieldVolume::sample(
        [&](kernel::cfloat3 p) { return src_tape.eval(p).d; },
        math::Aabb{cf3(-0.5f, -0.5f, -0.5f), cf3(0.5f, 0.5f, 0.5f)}, 0.02f, 0.12f);
    REQUIRE(!vol.empty());
    auto shared = std::make_shared<const field::FieldVolume>(vol);

    auto volume_node = [&] {
        scene::Node n;
        n.prim = scene::Prim::volume();
        n.volume = shared;
        return n;
    };
    // A stroke well away from the probes: it must not change the answer, only
    // the volume's position in the blob.
    auto stroke_node = [] {
        scene::Node n;
        n.prim = scene::Prim::stroke();
        n.stroke = {{cf3(4.0f, 4.0f, 4.0f), 0.05f}, {cf3(4.2f, 4.0f, 4.0f), 0.05f}};
        return n;
    };

    scene::Document alone;
    alone.add_sdf_layer("l").sdf->insert(volume_node());
    scene::Document behind;
    scene::Layer& bl = behind.add_sdf_layer("l");
    bl.sdf->insert(stroke_node());   // claims the start of the blob
    bl.sdf->insert(volume_node());

    scene::Tape ta = scene::compile_document(alone);
    scene::Tape tb = scene::compile_document(behind);
    clay_test::Lcg rng(23);
    float worst = 0.0f;
    for (int i = 0; i < 3000; ++i) {
        kernel::cfloat3 p = rng.vec3(-0.45f, 0.45f);
        worst = kernel::cmax(worst, std::fabs(ta.eval(p).d - tb.eval(p).d));
    }
    CHECK(worst < 1e-6f);
}

TEST_CASE("a shrink_band that cannot narrow the band changes nothing") {
    // What build_far_bounds() derives depends on the stored-brick set, the grid
    // and the band. An operator that rewrites samples in place changes none of
    // the first two, so once the band has reached its floor a further shrink
    // recomputes the array that is already there — chamfer over every brick
    // slot included.
    //
    // This is the steady state of a stroke, not an edge case: a bake starts at
    // three cells and the floor is two, so the first dab spends the narrowing
    // and every dab after it asks for one that cannot happen.
    auto sphere = [](kernel::cfloat3 p) { return kernel::clength(p) - 0.7f; };
    const math::Aabb region{cf3(-1, -1, -1), cf3(1, 1, 1)};
    const float cell = 0.02f;
    FieldVolume v = FieldVolume::sample(sphere, region, cell, cell * 3.0f);
    REQUIRE(v.brick_count() > 0);
    REQUIRE(v.band() == doctest::Approx(cell * 3.0f));

    SUBCASE("a shrink that DOES narrow re-derives the bounds") {
        const std::vector<std::uint8_t> before = v.serialize();
        v.shrink_band(cell);
        CHECK(v.band() < cell * 3.0f);
        CHECK(v.serialize() != before);
    }

    SUBCASE("a shrink at the floor is a no-op, byte for byte") {
        v.shrink_band(cell * 4.0f);  // straight to the floor
        REQUIRE(v.band() == doctest::Approx(cell * 2.0f));
        const std::vector<std::uint8_t> settled = v.serialize();
        v.shrink_band(cell);
        CHECK(v.band() == doctest::Approx(cell * 2.0f));
        CHECK(v.serialize() == settled);
        v.shrink_band(cell * 10.0f);
        CHECK(v.serialize() == settled);
    }

    SUBCASE("and the field it reports is unchanged, samples and bounds alike") {
        // serialize() already covers this, but the point of the far bounds is
        // what eval() says in the empty majority of the region — so ask it
        // there rather than trusting the bytes.
        v.shrink_band(cell * 4.0f);
        clay_test::Lcg rng(4411);
        std::vector<float> before;
        std::vector<kernel::cfloat3> points;
        for (int i = 0; i < 500; ++i) {
            points.push_back(rng.vec3(-1.1f, 1.1f));
            before.push_back(v.eval(points.back()));
        }
        v.shrink_band(cell);
        for (std::size_t i = 0; i < points.size(); ++i) REQUIRE(v.eval(points[i]) == before[i]);
    }
}

TEST_CASE("rewrite_region equals rewrite where fn is identity outside the region") {
    // The property the region-limited brush rests on. `rewrite_region` writes
    // only the bricks that meet the region, and that is the same answer as
    // writing all of them exactly when `fn` would have left the rest alone.
    //
    // The second half is the one that bites: a sample on a brick face lives in
    // every brick sharing it, and this writes only the copies held by selected
    // bricks. serialize() compares the raw arrays, so two copies of a shared
    // sample drifting apart shows up here rather than as a step in the field
    // someone finds later.
    auto bumpy = [](kernel::cfloat3 p) {
        return kernel::clength(p) -
               (0.7f + 0.05f * std::sin(11.0f * p.x) * std::sin(11.0f * p.y) *
                           std::sin(11.0f * p.z));
    };
    const math::Aabb whole{cf3(-1, -1, -1), cf3(1, 1, 1)};

    for (float cell : {0.05f, 0.02f}) {
        CAPTURE(cell);
        const FieldVolume base = FieldVolume::sample(bumpy, whole, cell, cell * 3.0f);
        REQUIRE(base.brick_count() > 1);

        // Deliberately NOT brick-aligned. A box on the lattice would never put
        // a written sample and an unwritten one in the same brick face.
        const math::Aabb boxes[] = {
            {cf3(0.55f, -0.13f, -0.13f), cf3(0.86f, 0.13f, 0.13f)},
            {cf3(-0.037f, -0.037f, 0.61f), cf3(0.037f, 0.037f, 0.79f)},
            {cf3(-2, -2, -2), cf3(2, 2, 2)},  // covers every brick
            {cf3(5, 5, 5), cf3(6, 6, 6)},     // meets none of them
        };
        int which = 0;
        for (const math::Aabb& box : boxes) {
            CAPTURE(++which);
            // Structure inside, identity outside — a constant would cancel with
            // itself and hide a sample written twice or not at all.
            auto fn = [&box, &base](int gx, int gy, int gz, float old) {
                const kernel::cfloat3 p = base.cell_position(gx, gy, gz);
                if (p.x < box.min.x || p.y < box.min.y || p.z < box.min.z) return old;
                if (p.x > box.max.x || p.y > box.max.y || p.z > box.max.z) return old;
                return old + 0.013f * static_cast<float>((gx * 7 + gy * 13 + gz * 17) % 11);
            };
            FieldVolume full = base, part = base;
            full.rewrite(fn);
            part.rewrite_region(box, fn);
            CHECK(full.serialize() == part.serialize());
        }
    }

    SUBCASE("and the comparison can fail") {
        // Without this the case above proves nothing: the selection rounds
        // outward by a whole brick, so a region merely a little too small is
        // absorbed and still agrees. Breaking the precondition outright is what
        // shows the comparison has teeth.
        const FieldVolume base = FieldVolume::sample(bumpy, whole, 0.02f, 0.06f);
        auto everywhere = [](int, int, int, float old) { return old + 0.05f; };
        FieldVolume full = base, part = base;
        full.rewrite(everywhere);
        part.rewrite_region(math::Aabb{cf3(0.5f, -0.2f, -0.2f), cf3(0.52f, 0.2f, 0.2f)},
                            everywhere);
        CHECK(full.serialize() != part.serialize());
    }
}

TEST_CASE("sample_parallel is byte-identical to sample") {
    // The whole contract of the parallel sampler: same samples, same sparsity,
    // same far bounds, same measured Lipschitz. Not "within a tolerance" —
    // each sample depends only on its own position, so no ordering can change
    // any of them, and the comparison is exact.
    //
    // The function is deliberately NOT a smooth analytic one: a field with
    // structure at the sample scale is what would expose a mis-partitioned
    // window, where a smooth one would look plausible either way.
    auto f = [](kernel::cfloat3 p) {
        const float sphere = kernel::clength(p) - 0.55f;
        const float ripple = 0.04f * std::sin(p.x * 37.0f) * std::sin(p.y * 31.0f) *
                             std::sin(p.z * 41.0f);
        return sphere + ripple;
    };
    const math::Aabb region{cf3(-0.8f, -0.8f, -0.8f), cf3(0.8f, 0.8f, 0.8f)};

    for (float cell : {0.05f, 0.02f, 0.011f}) {
        CAPTURE(cell);
        const FieldVolume a = FieldVolume::sample(f, region, cell, cell * 3.0f);
        const FieldVolume b =
            FieldVolume::sample_parallel(f, region, cell, cell * 3.0f);

        CHECK(a.brick_count() == b.brick_count());
        CHECK(a.cell_size() == b.cell_size());
        CHECK(a.band() == b.band());
        CHECK(a.sample_lipschitz() == b.sample_lipschitz());
        CHECK(a.bounds().min.x == b.bounds().min.x);
        CHECK(a.bounds().max.z == b.bounds().max.z);

        // And the field itself, at points the samples do not sit on, so the
        // interpolation and the far bounds are compared too.
        clay_test::Lcg rng(2200);
        for (int i = 0; i < 800; ++i) {
            const kernel::cfloat3 p = rng.vec3(-1.1f, 1.1f);
            REQUIRE(b.eval(p) == a.eval(p));
        }
    }
}

TEST_CASE("measure_sample_lipschitz matches the dense traversal it replaced") {
    // The measurement walks the STORED bricks; it used to walk the whole
    // bounding lattice and ask sample_at() for every point of it. The two see
    // the same pairs — a forward pair lies wholly inside one brick — and this
    // holds the claim against the traversal it replaced rather than against a
    // number someone wrote down.
    auto dense = [](const FieldVolume& v) {
        if (v.empty()) return 1.0f;
        const int nx = v.sample_extent(0), ny = v.sample_extent(1), nz = v.sample_extent(2);
        const std::size_t total = static_cast<std::size_t>(nx) * ny * nz;
        constexpr int kForward[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        float steepest = 0.0f;
        for (std::size_t i = 0; i < total; ++i) {
            const int gx = static_cast<int>(i) % nx;
            const int gy = (static_cast<int>(i) / nx) % ny;
            const int gz = static_cast<int>(i) / (nx * ny);
            const std::optional<float> here = v.sample_at(gx, gy, gz);
            if (!here) continue;
            for (const auto& d : kForward) {
                const std::optional<float> next = v.sample_at(gx + d[0], gy + d[1], gz + d[2]);
                if (next) steepest = std::max(steepest, std::abs(*next - *here));
            }
        }
        return std::max(1.0f, steepest / v.cell_size());
    };

    const math::Aabb region{cf3(-1, -1, -1), cf3(1, 1, 1)};

    // A field with structure at the SAMPLE scale, for the same reason
    // sample_parallel's parity test uses one: a smooth analytic field looks
    // plausible whichever pairs a traversal happened to miss.
    auto ripple = [](kernel::cfloat3 p) {
        return kernel::clength(p) - 0.7f +
               0.03f * std::sin(37.0f * p.x) * std::sin(31.0f * p.y) * std::sin(41.0f * p.z);
    };
    for (float cell : {0.05f, 0.02f, 0.011f}) {
        CAPTURE(cell);
        const FieldVolume v = FieldVolume::sample(ripple, region, cell, cell * 3.0f);
        REQUIRE(v.brick_count() > 0);
        // Exact, not approximate: both traversals take a max over the same set
        // of differences between the same stored floats.
        CHECK(v.measure_sample_lipschitz() == dense(v));
    }

    // A STEEP field is the case the measurement exists for — a volume that
    // measures 1 tells nothing apart.
    for (float scale : {3.0f, 14.0f}) {
        CAPTURE(scale);
        auto steep = [scale](kernel::cfloat3 p) {
            return (kernel::clength(p) - 0.7f) * scale;
        };
        const FieldVolume v = FieldVolume::sample(steep, region, 0.02f, 0.06f * scale);
        REQUIRE(v.measure_sample_lipschitz() > 2.0f);
        CHECK(v.measure_sample_lipschitz() == dense(v));
    }
}

TEST_CASE("measure_sample_lipschitz compares a brick's halo sample") {
    // The one pair a stored-brick walk can drop. A brick holds kBrickDim + 1
    // samples per axis, and the last of them is the halo — shared with the
    // next brick, which holds it as its FIRST. So the pair (7, 8) lives only
    // in brick 0, at locals 7 and 8, and a sweep that stopped at local
    // kBrickDim - 1 would never compare it.
    //
    // The planted field makes that pair the steepest one in the volume, so
    // missing it does not shade the answer, it changes it: 2.5 becomes the 1.5
    // planted one sample further along, which lives at locals 0 and 1 of the
    // next brick and would survive.
    const math::Aabb region{cf3(0, 0, 0), cf3(0.8f, 0.4f, 0.4f)};
    const float cell = 0.05f;
    // A field that is zero everywhere is within any band, so every brick is
    // stored and the planted pattern is the whole of what is measured.
    FieldVolume v =
        FieldVolume::sample([](kernel::cfloat3) { return 0.0f; }, region, cell, cell * 3.0f);
    REQUIRE(v.sample_extent(0) > field::kBrickDim + 1);  // more than one brick across

    v.rewrite([cell](int gx, int, int, float) {
        const float ramp = static_cast<float>(gx) * cell * 0.5f;  // 0.5 per cell
        return gx == field::kBrickDim ? ramp + 2.0f * cell : ramp;
    });

    // |v(8) - v(7)| = 0.5 + 2.0 cells; |v(9) - v(8)| = 2.0 - 0.5.
    CHECK(v.measure_sample_lipschitz() == doctest::Approx(2.5f));
}

TEST_CASE("sample_parallel gives the same answer every time") {
    // A race would show up as an answer that VARIES rather than one that is
    // wrong, so this runs the same sample repeatedly and requires identity.
    auto f = [](kernel::cfloat3 p) { return kernel::clength(p) - 0.5f + 0.03f * std::sin(p.x * 53.0f); };
    const math::Aabb region{cf3(-0.7f, -0.7f, -0.7f), cf3(0.7f, 0.7f, 0.7f)};

    const FieldVolume first =
        FieldVolume::sample_parallel(f, region, 0.012f, 0.036f);
    clay_test::Lcg rng(2201);
    std::vector<kernel::cfloat3> probes;
    for (int i = 0; i < 400; ++i) probes.push_back(rng.vec3(-1.0f, 1.0f));

    for (int run = 0; run < 6; ++run) {
        const FieldVolume again =
            FieldVolume::sample_parallel(f, region, 0.012f, 0.036f);
        CAPTURE(run);
        REQUIRE(again.brick_count() == first.brick_count());
        for (const kernel::cfloat3& p : probes) REQUIRE(again.eval(p) == first.eval(p));
    }
}
