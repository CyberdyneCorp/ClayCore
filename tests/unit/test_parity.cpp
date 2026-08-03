#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "clay/eval/backend.h"
#include "kernel_utils.h"
#include "scene_utils.h"

// Backend parity suite (evaluation-backends spec): every registered backend
// vs the CPU scalar reference — per-kernel single-item scenes, per-profile
// blend pairs, and the composed gnarly scene. CPU batch path must match to
// 1e-6 relative; GPU backends to 1e-4 (per-kernel overrides in the table).

using namespace clay;
using namespace clay::kernel;
using clay_test::gnarly_document;
using clay_test::item;

namespace {

struct ParityScene {
    std::string name;
    scene::Document doc;
    float extent;
};

std::vector<ParityScene> parity_scenes() {
    using namespace scene;
    std::vector<ParityScene> scenes;
    auto single = [&](const char* name, Prim prim) {
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(item(prim, cf3(0.1f, -0.05f, 0.2f)));
        scenes.push_back({name, std::move(doc), 3.0f});
    };
    single("sphere", Prim::sphere(1.0f));
    single("box", Prim::box(cf3(1, 0.7f, 0.4f)));
    single("round_box", Prim::round_box(cf3(0.8f, 0.6f, 0.5f), 0.1f));
    single("box_frame", Prim::box_frame(cf3(1, 0.8f, 0.6f), 0.1f));
    single("torus", Prim::torus(1.2f, 0.3f));
    single("capsule", Prim::capsule(cf3(-0.5f, 0, 0), cf3(0.5f, 0.3f, 0), 0.4f));
    single("capped_cylinder", Prim::capped_cylinder(0.7f, 0.9f));
    single("rounded_cylinder", Prim::rounded_cylinder(0.7f, 0.2f, 0.9f));
    single("capped_cone", Prim::capped_cone(0.9f, 0.8f, 0.3f));
    single("round_cone", Prim::round_cone(0.5f, 0.2f, 1.0f));
    single("ellipsoid", Prim::ellipsoid(cf3(1.0f, 0.5f, 0.7f)));
    single("octahedron", Prim::octahedron(0.9f));
    single("hex_prism", Prim::hex_prism(0.7f, 0.4f));
    single("pyramid", Prim::pyramid(0.9f));
    {
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        scene::Node stroke;
        stroke.prim = scene::Prim::stroke();
        stroke.stroke = {{cf3(-0.8f, 0, 0), 0.3f},
                         {cf3(0, 0.5f, 0.2f), 0.22f},
                         {cf3(0.8f, 0.1f, -0.2f), 0.28f}};
        stroke.stroke_blend_k = 0.05f;
        l.sdf->insert(stroke);
        scenes.push_back({"stroke", std::move(doc), 3.0f});
    }
    // blend-profile pairs (union + subtract per profile)
    auto pair = [&](const char* name, scene::BlendProfile profile) {
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(item(scene::Prim::sphere(0.8f), cf3(-0.4f, 0, 0)));
        l.sdf->insert(item(scene::Prim::box(cf3(0.6f, 0.5f, 0.7f)), cf3(0.5f, 0.2f, 0),
                           scene::Op::Add, scene::Blend{profile, 0.15f}));
        l.sdf->insert(item(scene::Prim::capped_cylinder(0.3f, 1.2f), cf3(0, 0.3f, 0),
                           scene::Op::Subtract, scene::Blend{profile, 0.08f}));
        scenes.push_back({name, std::move(doc), 3.0f});
    };
    pair("blend_quadratic", scene::BlendProfile::Quadratic);
    pair("blend_cubic", scene::BlendProfile::Cubic);
    pair("blend_circular", scene::BlendProfile::Circular);
    pair("blend_chamfer", scene::BlendProfile::Chamfer);

    scenes.push_back({"gnarly", gnarly_document(), 4.5f});
    return scenes;
}

// per-kernel relative tolerance for GPU backends; CPU batch gets 1e-6
float gpu_tolerance(const std::string&) { return 1e-4f; }

float rel_err(float a, float b) {
    float scale = kernel::cmax(kernel::cmax(cabs(a), cabs(b)), 1.0f);
    return cabs(a - b) / scale;
}

}  // namespace

TEST_CASE("registry: CPU backend always present") {
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    REQUIRE(cpu != nullptr);
    CHECK(std::string(cpu->name()) == "cpu");
}

TEST_CASE("parity: every registered backend matches the scalar reference") {
    std::vector<ParityScene> scenes = parity_scenes();
    for (eval::Backend* backend : eval::Registry::instance().all()) {
        bool is_cpu = std::string(backend->name()) == "cpu";
        for (ParityScene& ps : scenes) {
            CAPTURE(backend->name());
            CAPTURE(ps.name);
            scene::Tape tape = scene::compile_document(ps.doc);
            REQUIRE(!tape.empty());

            const std::size_t n = 4096;
            clay_test::Lcg rng(511);
            std::vector<float> pts(n * 3);
            for (float& v : pts) v = rng.range(-ps.extent, ps.extent);

            std::vector<float> ref_d(n), got_d(n);
            std::vector<float> ref_c(n * 3), got_c(n * 3);
            eval::PointQuery q{pts.data(), n, 1e-4f};
            eval::PointResults ref_out{ref_d.data(), nullptr, ref_c.data()};
            eval::eval_points_reference(tape, q, ref_out);

            eval::PointResults got_out{got_d.data(), nullptr, got_c.data()};
            REQUIRE(backend->eval_points(tape, q, got_out) == eval::Status::Ok);

            float tol = is_cpu ? 1e-6f : gpu_tolerance(ps.name);
            float worst = 0.0f;
            for (std::size_t i = 0; i < n; ++i) worst = cmax(worst, rel_err(ref_d[i], got_d[i]));
            CAPTURE(worst);
            CHECK(worst <= tol);
            float worst_c = 0.0f;
            for (std::size_t i = 0; i < n * 3; ++i)
                worst_c = cmax(worst_c, rel_err(ref_c[i], got_c[i]));
            CHECK(worst_c <= cmax(tol, 1e-4f));
        }
    }
}

TEST_CASE("parity: grid evaluation equals point evaluation") {
    scene::Tape tape = scene::compile_document(*[] {
        static scene::Document doc = gnarly_document();
        return &doc;
    }());
    eval::GridQuery grid;
    grid.origin = cf3(-1.5f, -1.5f, -1.5f);
    grid.spacing = 0.25f;
    grid.nx = grid.ny = grid.nz = 13;
    for (eval::Backend* backend : eval::Registry::instance().all()) {
        CAPTURE(backend->name());
        std::size_t total = static_cast<std::size_t>(grid.nx) * grid.ny * grid.nz;
        std::vector<float> values(total);
        REQUIRE(backend->eval_grid(tape, grid, values.data()) == eval::Status::Ok);
        bool is_cpu = std::string(backend->name()) == "cpu";
        float tol = is_cpu ? 1e-6f : 1e-4f;
        clay_test::Lcg rng(512);
        for (int check = 0; check < 300; ++check) {
            int x = static_cast<int>(rng.range(0, 12.99f));
            int y = static_cast<int>(rng.range(0, 12.99f));
            int z = static_cast<int>(rng.range(0, 12.99f));
            cfloat3 p = grid.origin + cf3((float)x, (float)y, (float)z) * grid.spacing;
            float expected = tape.eval(p).d;
            std::size_t idx = static_cast<std::size_t>(z) * grid.nx * grid.ny +
                              static_cast<std::size_t>(y) * grid.nx + x;
            CHECK(rel_err(values[idx], expected) <= tol);
        }
    }
}

TEST_CASE("parity: raycast hits agree with reference sphere tracing") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::sphere(1.0f), cf3(0, 0, 0)));
    l.sdf->insert(item(scene::Prim::box(cf3(0.4f, 0.4f, 0.4f)), cf3(1.2f, 0, 0), scene::Op::Add,
                       scene::Blend{scene::BlendProfile::Quadratic, 0.1f}));
    scene::Tape tape = scene::compile_document(doc);

    const std::size_t n = 128;
    clay_test::Lcg rng(513);
    std::vector<float> rays(n * 6);
    for (std::size_t i = 0; i < n; ++i) {
        cfloat3 ro = cf3(rng.range(-1, 2), rng.range(-1, 1), -4.0f);
        cfloat3 rd = cnormalize(cf3(rng.range(-0.2f, 0.2f), rng.range(-0.2f, 0.2f), 1.0f));
        rays[i * 6] = ro.x;
        rays[i * 6 + 1] = ro.y;
        rays[i * 6 + 2] = ro.z;
        rays[i * 6 + 3] = rd.x;
        rays[i * 6 + 4] = rd.y;
        rays[i * 6 + 5] = rd.z;
    }
    eval::RayQuery q{rays.data(), n, 0.0f, 20.0f, 1e-4f, 256};

    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    REQUIRE(cpu);
    std::vector<eval::RayHit> ref(n);
    REQUIRE(cpu->raycast(tape, q, ref.data()) == eval::Status::Ok);
    int hits = 0;
    for (const eval::RayHit& h : ref)
        if (h.hit) ++hits;
    CHECK(hits > 32);  // scenario sanity

    for (eval::Backend* backend : eval::Registry::instance().all()) {
        if (backend == cpu) continue;
        CAPTURE(backend->name());
        std::vector<eval::RayHit> got(n);
        eval::Status s = backend->raycast(tape, q, got.data());
        if (s == eval::Status::Unsupported) continue;
        REQUIRE(s == eval::Status::Ok);
        for (std::size_t i = 0; i < n; ++i) {
            CAPTURE(i);
            CHECK(got[i].hit == ref[i].hit);
            if (got[i].hit && ref[i].hit)
                CHECK(cabs(got[i].t - ref[i].t) < 5e-3f);
        }
    }
}
