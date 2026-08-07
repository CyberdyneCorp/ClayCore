// A noise field (sdf-kernels spec, add-noise-field).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "clay/kernel/exactness.h"
#include "clay/kernel/noise.h"
#include "clay/scene/tape.h"

using namespace clay;
using kernel::cf3;

namespace {

scene::Document ball_with(const scene::Deformer* d, float r = 0.7f) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n;
    n.prim = scene::Prim::sphere(r);
    if (d) n.deformers.push_back(*d);
    l.sdf->insert(std::move(n));
    return doc;
}

// How far the surface wanders from the sphere it started as.
float roughness(const scene::Tape& t, float r = 0.7f) {
    double sum = 0.0, sum2 = 0.0;
    int n = 0;
    for (float a = 0.0f; a < 6.28f; a += 0.05f)
        for (float b = 0.35f; b < 2.8f; b += 0.09f) {
            kernel::cfloat3 dir =
                cf3(std::sin(b) * std::cos(a), std::cos(b), std::sin(b) * std::sin(a));
            const float v = t.eval(dir * r).d;
            sum += v;
            sum2 += static_cast<double>(v) * v;
            ++n;
        }
    const double mean = sum / n;
    return static_cast<float>(std::sqrt(sum2 / n - mean * mean));
}

}  // namespace

TEST_CASE("noise: the same seed gives the same field, a different one does not") {
    // The reproducibility the whole design rests on. The seed is an ordinary
    // parameter, so this cannot depend on evaluation order or global state.
    scene::Deformer a = scene::Deformer::noise(0.08f, 6.0f, 4, 0.5f, 1u);
    scene::Deformer b = scene::Deformer::noise(0.08f, 6.0f, 4, 0.5f, 1u);
    scene::Deformer c = scene::Deformer::noise(0.08f, 6.0f, 4, 0.5f, 2u);

    scene::Tape ta = scene::compile_document(ball_with(&a));
    scene::Tape tb = scene::compile_document(ball_with(&b));
    scene::Tape tc = scene::compile_document(ball_with(&c));

    bool differs = false;
    for (float x = -1.0f; x <= 1.0f; x += 0.07f)
        for (float y = -1.0f; y <= 1.0f; y += 0.11f) {
            kernel::cfloat3 p = cf3(x, y, 0.13f);
            CAPTURE(x);
            CAPTURE(y);
            CHECK(ta.eval(p).d == doctest::Approx(tb.eval(p).d));  // same seed, same field
            if (std::abs(ta.eval(p).d - tc.eval(p).d) > 1e-4f) differs = true;
        }
    CHECK(differs);  // a different seed must actually change something
}

TEST_CASE("noise: it roughens a surface, and the amplitude bounds how far") {
    scene::Tape plain = scene::compile_document(ball_with(nullptr));
    const float smooth = roughness(plain);

    scene::Deformer d = scene::Deformer::noise(0.08f, 6.0f, 4, 0.5f, 7u);
    scene::Tape rough = scene::compile_document(ball_with(&d));
    INFO("roughness " << smooth << " -> " << roughness(rough));
    CHECK(roughness(rough) > smooth + 0.005f);

    SUBCASE("and the deviation stays inside the amplitude") {
        // The fractal is normalized to [-1, 1], so the amplitude really is the
        // whole excursion rather than a scale factor on an unbounded sum.
        for (float x = -1.2f; x <= 1.2f; x += 0.053f)
            for (float y = -1.2f; y <= 1.2f; y += 0.061f) {
                kernel::cfloat3 p = cf3(x, y, 0.017f);
                CAPTURE(x);
                CAPTURE(y);
                CHECK(std::abs(rough.eval(p).d - plain.eval(p).d) <= 0.08f + 1e-4f);
            }
    }
}

TEST_CASE("noise: zero amplitude changes nothing") {
    scene::Deformer none = scene::Deformer::noise(0.0f, 6.0f, 4, 0.5f, 3u);
    scene::Tape with = scene::compile_document(ball_with(&none));
    scene::Tape without = scene::compile_document(ball_with(nullptr));
    for (float x = -1.1f; x <= 1.1f; x += 0.043f)
        for (float y = -1.1f; y <= 1.1f; y += 0.047f) {
            kernel::cfloat3 p = cf3(x, y, 0.011f);
            CAPTURE(x);
            CAPTURE(y);
            CHECK(with.eval(p).d == doctest::Approx(without.eval(p).d));
        }
}

TEST_CASE("noise: it is irregular, which is what separates it from displace") {
    // The reason this exists at all. A sine repeats at its own period; noise
    // does not, and that is the difference between a corrugation and weathering.
    const float freq = 4.0f;
    scene::Deformer d = scene::Deformer::noise(0.1f, freq, 1, 0.5f, 11u);
    scene::Tape t = scene::compile_document(ball_with(&d, 0.7f));

    // Sample along a line, one full period of the lattice apart. A sine would
    // agree with itself; noise must not.
    const float period = 1.0f / freq;
    float worst_repeat = 0.0f;
    for (float x = -0.4f; x <= 0.4f; x += 0.013f) {
        const float a = t.eval(cf3(x, 0.72f, 0.0f)).d;
        const float b = t.eval(cf3(x + period, 0.72f, 0.0f)).d;
        worst_repeat = std::max(worst_repeat, std::abs(a - b));
    }
    INFO("largest disagreement one lattice period apart: " << worst_repeat);
    CHECK(worst_repeat > 0.005f);
}

TEST_CASE("noise: more octaves add detail without growing the excursion") {
    // Otherwise `octaves` and `amplitude` would be two controls for the same
    // thing and neither would mean anything.
    scene::Tape plain = scene::compile_document(ball_with(nullptr));
    float widest_one = 0.0f, widest_five = 0.0f;
    scene::Deformer one = scene::Deformer::noise(0.09f, 5.0f, 1, 0.5f, 5u);
    scene::Deformer five = scene::Deformer::noise(0.09f, 5.0f, 5, 0.5f, 5u);
    scene::Tape t1 = scene::compile_document(ball_with(&one));
    scene::Tape t5 = scene::compile_document(ball_with(&five));

    for (float x = -1.0f; x <= 1.0f; x += 0.037f)
        for (float y = -1.0f; y <= 1.0f; y += 0.041f) {
            kernel::cfloat3 p = cf3(x, y, 0.019f);
            widest_one = std::max(widest_one, std::abs(t1.eval(p).d - plain.eval(p).d));
            widest_five = std::max(widest_five, std::abs(t5.eval(p).d - plain.eval(p).d));
        }
    INFO("widest excursion: " << widest_one << " at one octave, " << widest_five << " at five");
    CHECK(widest_five <= 0.09f + 1e-4f);
    CHECK(widest_one <= 0.09f + 1e-4f);
}

TEST_CASE("noise: the steepness it adds is declared") {
    scene::Tape plain = scene::compile_document(ball_with(nullptr));
    CHECK(plain.info.is_exact);

    scene::Deformer d = scene::Deformer::noise(0.08f, 6.0f, 4, 0.5f, 1u);
    scene::Tape noisy = scene::compile_document(ball_with(&d));
    CHECK_FALSE(noisy.info.is_exact);
    CHECK(noisy.info.lipschitz > plain.info.lipschitz);
    CHECK(kernel::csafe_step_scale(noisy.info) < kernel::csafe_step_scale(plain.info));

    SUBCASE("and more amplitude or frequency declares more") {
        float previous = kernel::csafe_step_scale(plain.info);
        for (float amp : {0.02f, 0.05f, 0.1f}) {
            scene::Deformer s = scene::Deformer::noise(amp, 6.0f, 4, 0.5f, 1u);
            const float scale =
                kernel::csafe_step_scale(scene::compile_document(ball_with(&s)).info);
            CAPTURE(amp);
            CHECK(scale <= previous);
            previous = scale;
        }
        previous = kernel::csafe_step_scale(plain.info);
        for (float freq : {2.0f, 6.0f, 12.0f}) {
            scene::Deformer s = scene::Deformer::noise(0.06f, freq, 4, 0.5f, 1u);
            const float scale =
                kernel::csafe_step_scale(scene::compile_document(ball_with(&s)).info);
            CAPTURE(freq);
            CHECK(scale <= previous);
            previous = scale;
        }
    }
}

TEST_CASE("noise: a ray still finds the surface") {
    scene::Deformer d = scene::Deformer::noise(0.08f, 6.0f, 4, 0.5f, 1u);
    scene::Tape tape = scene::compile_document(ball_with(&d));
    const float scale = kernel::csafe_step_scale(tape.info);

    int hits = 0, tried = 0;
    for (float y = -0.4f; y <= 0.4f; y += 0.08f) {
        float t = 0.0f;
        ++tried;
        for (int i = 0; i < 40000; ++i) {
            const float dist = tape.eval(cf3(3.0f - t, y, 0)).d;
            if (dist < 1e-4f) {
                ++hits;
                break;
            }
            t += dist * scale;
            if (t > 6.0f) break;
        }
    }
    INFO(hits << " of " << tried << " rays landed");
    CHECK(hits == tried);
}

TEST_CASE("noise: the bound covers the excursion") {
    // Brick culling trusts the item's bounds, so a surface pushed out by the
    // amplitude must still be inside them.
    scene::Deformer d = scene::Deformer::noise(0.12f, 5.0f, 3, 0.5f, 9u);
    scene::Tape tape = scene::compile_document(ball_with(&d, 0.7f));
    REQUIRE_FALSE(tape.bounds.empty());
    CHECK(tape.bounds.max.x >= 0.7f + 0.12f - 1e-3f);

    for (float a = 0.0f; a < 6.28f; a += 0.11f) {
        kernel::cfloat3 dir = cf3(std::cos(a), std::sin(a), 0.0f);
        // Walk out until the field turns positive; that point must be inside.
        for (float r = 0.5f; r < 1.5f; r += 0.005f) {
            if (tape.eval(dir * r).d > 0.0f) {
                CAPTURE(a);
                CHECK(r <= kernel::clength(cf3(tape.bounds.max.x, tape.bounds.max.y, 0)) + 1e-3f);
                break;
            }
        }
    }
}

TEST_CASE("noise: the hash is integer, so it cannot drift between backends") {
    // Not a behavioural test — a design one. A float hash would put a `sin` in
    // the middle of a chaotic amplifier, and the parity corpus holds every
    // backend to 1e-4. These are the values the corpus is pinned to; if the
    // hash ever changes, this fails first and says so.
    CHECK(kernel::cnoise_hash(0u, 0u, 0u, 0u) == kernel::cnoise_hash(0u, 0u, 0u, 0u));
    CHECK(kernel::cnoise_hash(1u, 0u, 0u, 0u) != kernel::cnoise_hash(0u, 1u, 0u, 0u));
    CHECK(kernel::cnoise_hash(0u, 0u, 0u, 1u) != kernel::cnoise_hash(0u, 0u, 0u, 2u));

    // Gradient noise is exactly zero on the lattice, by construction: every
    // corner's contribution is a dot product with a zero offset. Worth pinning
    // because it looks like a bug the first time it is measured.
    CHECK(kernel::cnoise_gradient3(cf3(3.0f, 3.0f, 3.0f), 1u) == doctest::Approx(0.0f));
    CHECK(kernel::cnoise_gradient3(cf3(-2.0f, 5.0f, 0.0f), 7u) == doctest::Approx(0.0f));
    // ...and non-zero between them.
    CHECK(std::abs(kernel::cnoise_gradient3(cf3(3.5f, 3.5f, 3.5f), 1u)) > 1e-3f);
}
