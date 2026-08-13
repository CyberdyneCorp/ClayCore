#include <doctest/doctest.h>

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "clay/eval/backend.h"
#include "clay/scene/bounds.h"
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
    single("capped_torus", Prim::capped_torus(1.0f, 0.9f, 0.25f));
    single("link", Prim::link(0.4f, 0.7f, 0.25f));
    single("cylinder_infinite", Prim::cylinder_infinite(0.1f, -0.2f, 0.6f));
    single("cone_exact", Prim::cone(0.5f, 1.1f));
    single("plane", Prim::plane(cf3(0.3f, 1.0f, -0.2f), 0.2f));
    single("cut_sphere", Prim::cut_sphere(1.0f, 0.3f));
    single("cut_hollow_sphere", Prim::cut_hollow_sphere(1.0f, 0.3f, 0.08f));
    single("solid_angle", Prim::solid_angle(0.8f, 1.0f));
    single("tetrahedron", Prim::tetrahedron(0.9f));
    single("dodecahedron", Prim::dodecahedron(0.8f));
    single("icosahedron", Prim::icosahedron(0.8f));
    single("tri_prism", Prim::tri_prism(0.8f, 0.5f));
    single("octahedron_cheap", Prim::octahedron_cheap(0.9f));
    single("lnorm_sphere", Prim::lnorm_sphere(0.9f, 4.0f));

    // lifts carry a Profile on the node, so they need the long form
    auto lift = [&](const char* name, Prim prim, Profile profile,
                    std::vector<cfloat2> points = {}) {
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        Node n = item(prim, cf3(0.1f, -0.05f, 0.2f));
        n.profile = profile;
        n.profile_points = std::move(points);
        l.sdf->insert(n);
        scenes.push_back({name, std::move(doc), 3.0f});
    };
    {   // grab: a region of a sphere pulled sideways
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        Node n = item(Prim::sphere(1.0f), cf3(0, 0, 0));
        n.deformers.push_back(
            scene::Deformer::grab(cf3(1.0f, 0, 0), 0.8f, cf3(0.4f, 0.2f, 0), 3));
        l.sdf->insert(n);
        scenes.push_back({"grab", std::move(doc), 3.0f});
    }

    {   // pose_line: a rotation ramped along a segment
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        Node n = item(Prim::capsule(cf3(0, -1, 0), cf3(0, 1, 0), 0.25f), cf3(0, 0, 0));
        n.deformers.push_back(scene::Deformer::pose_line(
            cf3(0, -1, 0), cf3(0, 1, 0), cf3(0, 0, 1), 0.8f, 3));
        l.sdf->insert(n);
        scenes.push_back({"pose_line", std::move(doc), 3.0f});
    }

    {   // pose: a region rotated about its centre
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        Node n = item(Prim::capped_cylinder(0.3f, 1.0f), cf3(0, 0, 0));
        n.deformers.push_back(
            scene::Deformer::pose(cf3(0, 0.8f, 0), 1.0f, cf3(0, 0, 1), 0.7f, 2));
        l.sdf->insert(n);
        scenes.push_back({"pose", std::move(doc), 3.0f});
    }

    {   // noise: the case the integer hash exists for. A float hash would put
        // each backend's own `sin` inside a chaotic amplifier, and this corpus
        // holds every backend to 1e-4 — it would fail here first.
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        Node n = item(Prim::sphere(0.8f), cf3(0, 0, 0));
        n.deformers.push_back(scene::Deformer::noise(0.09f, 5.0f, 4, 0.5f, 17u));
        l.sdf->insert(n);
        scenes.push_back({"noise_fractal", std::move(doc), 3.0f});
    }

    {   // relief: an item used as a REGION, displacing the field accumulated
        // before it — the op that reads the accumulator rather than adding to it
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(item(Prim::sphere(0.8f), cf3(0, 0, 0)));
        Node region = item(Prim::sphere(0.4f), cf3(0.2f, 0.7f, 0));
        region.op = scene::Op::Relief;
        region.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.13f};
        region.rounding = 0.22f;
        l.sdf->insert(region);
        scenes.push_back({"relief", std::move(doc), 3.0f});
    }

    {   // magnify: a radial scale about a point, with finite support
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        Node n = item(Prim::round_box(cf3(0.5f, 0.4f, 0.5f), 0.12f), cf3(0, 0, 0));
        n.deformers.push_back(scene::Deformer::magnify(cf3(0.3f, 0.2f, 0), 0.7f, 0.45f, 2));
        l.sdf->insert(n);
        scenes.push_back({"magnify", std::move(doc), 3.0f});
    }

    {   // elongate_axis: an asymmetric primitive stretched per axis
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        Node n = item(Prim::capped_cone(0.6f, 0.5f, 0.1f), cf3(0, 0, 0));
        n.deformers.push_back(scene::Deformer::elongate_axis(cf3(0.7f, 0.0f, 0.3f)));
        l.sdf->insert(n);
        scenes.push_back({"elongate_axis", std::move(doc), 3.0f});
    }

    {   // bend_linear: a slab tilted by a ramped displacement
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        Node n = item(Prim::box(cf3(0.3f, 1.0f, 0.3f)), cf3(0, 0, 0));
        n.deformers.push_back(scene::Deformer::bend_linear(
            cf3(0, -1, 0), cf3(0, 1, 0), cf3(0.8f, 0, 0.2f), 3));
        l.sdf->insert(n);
        scenes.push_back({"bend_linear", std::move(doc), 3.0f});
    }

    {   // bend_radial: a disc whose rim is lifted
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        Node n = item(Prim::capped_cylinder(1.2f, 0.15f), cf3(0, 0, 0));
        n.deformers.push_back(scene::Deformer::bend_radial(0.2f, 1.2f, 0.6f, 5));
        l.sdf->insert(n);
        scenes.push_back({"bend_radial", std::move(doc), 3.0f});
    }

    {   // elongate: a sphere stretched into a capsule
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        Node n = item(Prim::sphere(0.5f), cf3(0, 0, 0));
        n.deformers.push_back(scene::Deformer::elongate(cf3(0.8f, 0.0f, 0.3f)));
        l.sdf->insert(n);
        scenes.push_back({"elongate", std::move(doc), 3.0f});
    }

    {   // wrap_around: a flat slab bent around the Z axis
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        Node n = item(Prim::box(cf3(3.14f, 0.2f, 0.5f)), cf3(0, 0, 0));
        n.deformers.push_back(scene::Deformer::wrap_around(-3.14159f, 3.14159f));
        l.sdf->insert(n);
        scenes.push_back({"wrap_around", std::move(doc), 3.0f});
    }

    lift("extrude_hexagon", Prim::extrude(0.5f), Profile::hexagon(0.8f));
    {   // loft: a circle to a polygon, so both the parametric and the
        // out-of-line profile paths cross every backend
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        Node n = item(Prim::loft(0.9f, kernel::ease_smoothstep), cf3(0, 0, 0));
        n.profiles = {Profile::circle(0.8f), Profile::polygon()};
        n.profile_polygons = {{},
                              {cf2(-0.5f, -0.5f), cf2(0.5f, -0.5f), cf2(0.5f, 0.5f),
                               cf2(-0.5f, 0.5f)}};
        l.sdf->insert(n);
        scenes.push_back({"loft_circle_to_polygon", std::move(doc), 3.0f});
    }
    {   // swept: a tapering circle along a bent spline guide, so the
        // closest-point search, the transported frames and the arc-length
        // bracketing all cross every backend
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        Node n = item(Prim::swept(0), cf3(0, 0, 0));
        for (kernel::cfloat3 p : {cf3(-1.2f, -0.4f, 0), cf3(-0.4f, 0.5f, 0.2f),
                                  cf3(0.5f, 0.4f, -0.2f), cf3(1.2f, -0.3f, 0)}) {
            scene::StrokePoint sp;
            sp.pos = p;
            sp.type = scene::StrokePointType::Spline;
            n.stroke.push_back(sp);
        }
        n.curve_tolerance = 0.03f;
        n.profiles = {Profile::circle(0.35f), Profile::circle(0.12f)};
        n.profile_polygons = {{}, {}};
        l.sdf->insert(n);
        scenes.push_back({"swept_circle_along_guide", std::move(doc), 3.0f});
    }
    {   // a sampled volume: the blob carries a brick index and interpolated
        // samples, so every backend has to walk the same sparse structure
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        Node n = item(Prim::volume(), cf3(0, 0, 0));
        auto sphere = [](kernel::cfloat3 p) { return kernel::clength(p) - 0.7f; };
        n.volume = std::make_shared<field::FieldVolume>(field::FieldVolume::sample(
            sphere, math::Aabb{cf3(-1.2f, -1.2f, -1.2f), cf3(1.2f, 1.2f, 1.2f)}, 0.12f, 0.3f));
        l.sdf->insert(n);
        scenes.push_back({"sampled_volume_sphere", std::move(doc), 3.0f});
    }
    {   // The same structure carrying COLOUR per sample. The comparison this
        // scene exists for is the colour one: the distance path is already
        // covered above, and a backend that read the colour section at the
        // wrong offset — or not at all — would still agree about distance and
        // report the item's colour everywhere.
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        Node n = item(Prim::volume(), cf3(0, 0, 0));
        n.color = cf3(0, 1, 0);  // must NOT be what the samples report
        auto sphere = [](kernel::cfloat3 p) { return kernel::clength(p) - 0.7f; };
        auto colour = [](kernel::cfloat3 p) {
            return p.x < 0 ? cf3(0.9f, 0.1f, 0.1f) : cf3(0.1f, 0.1f, 0.9f);
        };
        n.volume = std::make_shared<field::FieldVolume>(field::FieldVolume::sample_colored(
            sphere, colour, math::Aabb{cf3(-1.2f, -1.2f, -1.2f), cf3(1.2f, 1.2f, 1.2f)}, 0.12f,
            0.3f));
        l.sdf->insert(n);
        scenes.push_back({"sampled_volume_colored", std::move(doc), 3.0f});
    }
    lift("revolve_polygon", Prim::revolve(1.2f), Profile::polygon(),
         {cf2(-0.3f, -0.3f), cf2(0.3f, -0.3f), cf2(0.3f, 0.3f), cf2(-0.3f, 0.3f)});
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
    {   // an armature: a BRANCHING tree, so every backend walks parent links
        // and folds three links at one node in the same order
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        scene::Node arm;
        arm.prim = scene::Prim::armature();
        arm.stroke = {{cf3(0, -0.35f, 0), 0.30f},
                      {cf3(0, 0.35f, 0), 0.24f},
                      {cf3(-0.6f, 0.15f, 0.1f), 0.16f},
                      {cf3(0.6f, 0.15f, -0.1f), 0.16f}};
        arm.armature_parents = {0, 0, 0, 0};
        arm.stroke_blend_k = 0.06f;
        l.sdf->insert(arm);
        scenes.push_back({"armature_branch", std::move(doc), 3.0f});
    }
    {   // a signed armature: a negative interior node and a referenced
        // negative root, so every backend runs the two-pass fold — positive
        // links, then subtracted ones — in the same order
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        scene::Node arm;
        arm.prim = scene::Prim::armature();
        arm.stroke = {{cf3(0, -0.35f, 0), 0.30f},
                      {cf3(0, 0.35f, 0), 0.24f},
                      {cf3(-0.6f, 0.15f, 0.1f), 0.16f},
                      {cf3(0.6f, 0.15f, -0.1f), 0.16f}};
        arm.armature_parents = {0, 0, 1, 1};
        arm.armature_signs = {-1, 1, -1, 1};
        arm.stroke_blend_k = 0.06f;
        l.sdf->insert(arm);
        scenes.push_back({"armature_negative", std::move(doc), 3.0f});
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

    // -- the rest of the combine vocabulary -----------------------------------
    //
    // Added when the op/deformer coverage guard below was written and found
    // that 12 of the 16 combine ops and 4 of the 14 deformers reached NO
    // parity scene. They were evaluated on Metal — the iPad app's production
    // path — with nothing checking that Metal agreed with the CPU about them.
    // The four that were covered (add, subtract, paint, relief) are the ones
    // whose own changes happened to add a scene.
    auto combine = [&](const char* name, scene::Op op, float k, float rounding = 0.0f) {
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(item(scene::Prim::sphere(0.9f), cf3(-0.15f, 0, 0)));
        scene::Node n = item(scene::Prim::box(cf3(0.55f, 0.75f, 0.5f)), cf3(0.35f, 0.1f, 0.1f));
        n.op = op;
        n.blend = scene::Blend{scene::BlendProfile::Quadratic, k};
        n.rounding = rounding;
        l.sdf->insert(n);
        scenes.push_back({name, std::move(doc), 3.0f});
    };
    combine("op_intersect", scene::Op::Intersect, 0.12f);
    // groove and tongue additionally consume the node's rounding as the
    // channel half-width, so a zero there would measure a degenerate channel
    combine("op_groove", scene::Op::Groove, 0.18f, 0.09f);
    combine("op_tongue", scene::Op::Tongue, 0.18f, 0.09f);
    combine("op_pipe", scene::Op::Pipe, 0.14f);
    combine("op_engrave", scene::Op::Engrave, 0.14f);
    combine("op_emboss", scene::Op::Emboss, 0.14f);
    combine("op_inset", scene::Op::Inset, 0.12f);
    combine("op_shell", scene::Op::Shell, 0.10f);
    combine("op_replace", scene::Op::Replace, 0.10f);
    combine("op_incise", scene::Op::Incise, 0.13f, 0.20f);

    {   // the transitions are NON-LOCAL: their weight reaches arbitrarily far,
        // which is why they are never culled. A parity scene has to sample
        // well outside both shapes for that to mean anything.
        auto transition = [&](const char* name, scene::Op op) {
            scene::Document doc;
            scene::Layer& l = doc.add_sdf_layer("l");
            l.sdf->insert(item(scene::Prim::sphere(0.8f), cf3(0, -0.5f, 0)));
            scene::Node n = item(scene::Prim::box(cf3(0.6f, 0.6f, 0.6f)), cf3(0, 0.5f, 0));
            n.op = op;
            n.transition.a = cf3(0, -1.0f, 0);
            n.transition.b = cf3(0, 1.0f, 0);
            n.transition.r0 = 0.2f;
            n.transition.r1 = 1.4f;
            n.transition.ease = 3;
            l.sdf->insert(n);
            scenes.push_back({name, std::move(doc), 3.5f});
        };
        transition("op_transition_linear", scene::Op::TransitionLinear);
        transition("op_transition_radial", scene::Op::TransitionRadial);
    }

    // -- the four original deformers ------------------------------------------
    auto warped = [&](const char* name, scene::Prim prim, scene::Deformer d) {
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        scene::Node n = item(prim, cf3(0, 0, 0));
        n.deformers.push_back(d);
        l.sdf->insert(n);
        scenes.push_back({name, std::move(doc), 3.0f});
    };
    warped("twist", scene::Prim::box(cf3(0.5f, 1.0f, 0.5f)),
           scene::Deformer::twist(1.1f));
    warped("bend", scene::Prim::box(cf3(1.0f, 0.35f, 0.35f)),
           scene::Deformer::bend(0.9f));
    warped("taper", scene::Prim::capped_cylinder(0.5f, 1.0f),
           scene::Deformer::taper(-1.0f, 1.0f, 1.3f, 0.4f, 3));
    // displace is by-callable in the engine but tape-expressible here: the
    // sine is the classic backend-disagreement case, which is the point
    warped("displace", scene::Prim::sphere(0.9f),
           scene::Deformer::displace(0.08f, 4.0f));

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

// Guard: a primitive that never reaches the parity corpus is never evaluated
// on Metal/CUDA/OpenCL, so single-source agreement would be unverified for it.
TEST_CASE("parity: the corpus exercises every primitive type") {
    std::set<int> covered;
    for (ParityScene& ps : parity_scenes())
        for (scene::Layer& l : ps.doc.layers)
            if (l.sdf)
                for (const auto& kv : l.sdf->nodes())
                    if (!kv.second.is_group) covered.insert(static_cast<int>(kv.second.prim.type));

    for (int op = 0; op < kernel::ctape_prim_count; ++op) {
        CAPTURE(op);
        CHECK(covered.count(op) == 1);
    }
}

// Guard: the same argument as the primitive guard above, for the rest of the
// vocabulary. An op, deformer or blend profile that no parity scene exercises
// is never evaluated on Metal/CUDA/OpenCL by this suite, so single-source
// agreement is unverified for it — and Metal is the iPad app's production
// path, where an unverified opcode is one a sculptor can reach.
TEST_CASE("parity: the corpus exercises every op, deformer and blend profile") {
    // These pin what "every" currently means. Adding an opcode fails here
    // rather than silently widening the set the corpus is measured against,
    // which is the whole point: a new op with no parity scene is unverified.
    static_assert(kernel::ccombine_incise == 15, "a combine op was added; widen this test");
    static_assert(kernel::cdeform_noise == 13, "a deformer was added; widen this test");
    static_assert(kernel::cblend_chamfer == 4, "a blend profile was added; widen this test");

    std::set<int> ops, deformers, profiles;
    for (ParityScene& ps : parity_scenes())
        for (scene::Layer& l : ps.doc.layers) {
            if (!l.sdf) continue;
            for (const auto& kv : l.sdf->nodes()) {
                const scene::Node& n = kv.second;
                if (n.op != scene::Op::None) ops.insert(static_cast<int>(n.op));
                profiles.insert(static_cast<int>(n.blend.profile));
                for (const scene::Deformer& d : n.deformers)
                    deformers.insert(static_cast<int>(d.type));
            }
        }

    for (int op = 0; op <= kernel::ccombine_incise; ++op) {
        CAPTURE(op);
        CHECK(ops.count(op) == 1);
    }
    for (int d = 0; d <= kernel::cdeform_noise; ++d) {
        CAPTURE(d);
        CHECK(deformers.count(d) == 1);
    }
    for (int p = 0; p <= kernel::cblend_chamfer; ++p) {
        CAPTURE(p);
        CHECK(profiles.count(p) == 1);
    }
}

// A skipped gate and a passing gate look identical in a log, which is how
// "Metal parity runs when the runner exposes a Metal device" became a claim
// nobody could check. This prints what was ACTUALLY compared, in a form CI
// greps, so an absent Metal backend is visible rather than inferred from the
// suite going green.
TEST_CASE("parity: report which backends were actually compared") {
    std::string names;
    for (eval::Backend* backend : eval::Registry::instance().all()) {
        if (!names.empty()) names += ",";
        names += backend->name();
    }
    std::printf("PARITY_BACKENDS_CHECKED: %s\n", names.c_str());
    CHECK_FALSE(names.empty());
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

TEST_CASE("parity: a grid batch answers what per-grid evaluation does") {
    // The brick-refill shape (issue #64): many small lattices, each with its
    // own culled tape, handed to the backend as ONE batch. A backend that
    // turns the batch into a single device submission — the Metal override —
    // must produce the values the per-grid path produces, including for a
    // block whose culled tape is EMPTY (nothing reaches it), whose tape slice
    // in the concatenated upload has zero length.
    scene::Document doc = gnarly_document();
    const int dim = 8;
    const float spacing = 0.15f;
    const float block = spacing * static_cast<float>(dim);

    std::vector<scene::Tape> tapes;
    std::vector<const scene::Tape*> tape_ptrs;
    std::vector<cfloat3> origins;
    for (int bz = -2; bz < 2; ++bz)
        for (int by = -2; by < 2; ++by)
            for (int bx = -2; bx < 2; ++bx) {
                cfloat3 origin = cf3((float)bx, (float)by, (float)bz) * block;
                scene::CullRegion cull;
                cull.region = math::Aabb{origin, origin + cf3(block, block, block)}.dilated(
                    3.0f * spacing);
                tapes.push_back(scene::compile_document(doc, &cull));
                origins.push_back(origin);
            }
    {   // far from everything: the empty-tape block
        cfloat3 origin = cf3(50.0f, 50.0f, 50.0f);
        scene::CullRegion cull;
        cull.region = math::Aabb{origin, origin + cf3(block, block, block)};
        tapes.push_back(scene::compile_document(doc, &cull));
        REQUIRE(tapes.back().instrs.empty());
        origins.push_back(origin);
    }
    for (const scene::Tape& t : tapes) tape_ptrs.push_back(&t);

    eval::GridBatchQuery batch;
    batch.tapes = tape_ptrs.data();
    batch.origins = origins.data();
    batch.spacing = spacing;
    batch.nx = batch.ny = batch.nz = dim;
    batch.count = tapes.size();
    const std::size_t per = static_cast<std::size_t>(dim) * dim * dim;

    for (eval::Backend* backend : eval::Registry::instance().all()) {
        CAPTURE(backend->name());
        const float kPoison = -12345.678f;
        std::vector<float> got(batch.count * per, kPoison);
        REQUIRE(backend->eval_grid_batch(batch, got.data()) == eval::Status::Ok);
        const bool is_cpu = std::string(backend->name()) == "cpu";
        const float tol = is_cpu ? 1e-6f : 1e-4f;
        float worst = 0.0f;
        for (std::size_t g = 0; g < batch.count; ++g)
            for (std::size_t s = 0; s < per; ++s) {
                const std::size_t x = s % dim, y = (s / dim) % dim, z = s / (dim * dim);
                cfloat3 p = origins[g] +
                            cf3((float)x, (float)y, (float)z) * spacing;
                REQUIRE(got[g * per + s] != kPoison);
                worst = kernel::cmax(worst,
                                     rel_err(got[g * per + s], tapes[g].eval(p).d));
            }
        CHECK(worst <= tol);
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

TEST_CASE("batch dispatch covers every element exactly once, at every size") {
    // The thread pool over-decomposes into several chunks per worker so its
    // atomic claim counter can rebalance across unequal cores. That changes the
    // chunk arithmetic, whose failure mode is a gap (an element nobody
    // computed) or an overlap (one computed twice) — neither of which shows up
    // as a crash, only as a wrong value at a size nobody happened to test.
    //
    // The sizes below straddle every boundary that arithmetic has: the
    // min_chunk floors the CPU backend passes (16 for rays, 256 for points),
    // one either side of them, exact multiples of the chunk count, and primes
    // that divide evenly into nothing.
    scene::Document doc = gnarly_document();
    scene::Tape tape = scene::compile_document(doc);
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    REQUIRE(cpu != nullptr);

    const std::size_t sizes[] = {1,   2,   15,   16,   17,   31,   255,  256,   257,
                                 511, 512, 1000, 1023, 1024, 1025, 4096, 10007};
    for (std::size_t n : sizes) {
        CAPTURE(n);
        std::vector<float> points(n * 3);
        for (std::size_t i = 0; i < n; ++i) {
            // a deterministic spread over the gnarly scene's extent
            float t = static_cast<float>(i);
            points[i * 3 + 0] = -1.4f + 0.0037f * t;
            points[i * 3 + 1] = -1.1f + 0.0053f * t;
            points[i * 3 + 2] = -0.9f + 0.0071f * t;
        }
        // Poison the output so an element nobody wrote is detectable rather
        // than accidentally correct.
        const float kPoison = -12345.678f;
        std::vector<float> got(n, kPoison);
        eval::PointQuery q;
        q.points_xyz = points.data();
        q.count = n;
        REQUIRE(cpu->eval_points(tape, q, eval::PointResults{got.data(), nullptr, nullptr}) ==
                eval::Status::Ok);
        for (std::size_t i = 0; i < n; ++i) {
            const float expected =
                tape.eval(cf3(points[i * 3], points[i * 3 + 1], points[i * 3 + 2])).d;
            REQUIRE(got[i] != kPoison);  // gap: never written
            REQUIRE(got[i] == expected); // overlap or misrouted range
        }
    }
}

TEST_CASE("culling never drops an item whose influence is not local") {
    // compile_document now derives cullability from item_influence_is_local
    // rather than from a second call to item_influence_bound. The two must
    // agree: an item the predicate calls non-local has an INFINITE influence
    // bound and may never be culled, however far the cull region is from its
    // geometry. Getting this wrong drops the item from per-brick tapes only,
    // so the whole-document tape would still look right.
    const scene::Layer bare;

    SUBCASE("the predicate and the bound agree on every non-local kind") {
        scene::Node intersect = clay_test::item(scene::Prim::sphere(0.2f), cf3(0, 0, 0));
        intersect.op = scene::Op::Intersect;
        scene::Node repeated = clay_test::item(scene::Prim::sphere(0.2f), cf3(0, 0, 0));
        repeated.repeat = scene::Repeat::grid_infinite(cf3(0.5f, 0.5f, 0.5f));
        scene::Node unbounded =
            clay_test::item(scene::Prim::plane(cf3(0, 1, 0), 0.0f), cf3(0, 0, 0));

        for (const scene::Node* n : {&intersect, &repeated, &unbounded}) {
            CHECK_FALSE(scene::item_influence_is_local(*n));
            CHECK(scene::item_influence_bound(*n, bare).is_infinite());
        }
        // ...and an ordinary item is local, with a finite bound
        scene::Node plain = clay_test::item(scene::Prim::sphere(0.2f), cf3(0, 0, 0));
        CHECK(scene::item_influence_is_local(plain));
        CHECK_FALSE(scene::item_influence_bound(plain, bare).is_infinite());
    }

    // The compile-level check uses only the Add-op kinds. An Intersect is
    // dropped by the empty-chain guard above the cull test when everything
    // before it culls away — correctly, and for a reason that has nothing to
    // do with influence bounds — so it cannot distinguish the two behaviours.
    scene::CullRegion far;
    far.region = math::Aabb{cf3(40.0f, 40.0f, 40.0f), cf3(41.0f, 41.0f, 41.0f)};

    SUBCASE("an infinite grid repeat survives a distant cull region") {
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(clay_test::item(scene::Prim::box(cf3(0.3f, 0.3f, 0.3f)), cf3(0, 0, 0)));
        scene::Node repeated = clay_test::item(scene::Prim::sphere(0.2f), cf3(0, 0, 0));
        repeated.repeat = scene::Repeat::grid_infinite(cf3(0.5f, 0.5f, 0.5f));
        l.sdf->insert(repeated);

        scene::Tape culled = scene::compile_document(doc, &far);
        scene::Tape whole = scene::compile_document(doc);
        CHECK_FALSE(culled.empty());                        // the repeat stayed
        CHECK(culled.instrs.size() < whole.instrs.size());  // the box went
    }

    SUBCASE("an unbounded primitive survives a distant cull region") {
        scene::Document doc;
        scene::Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(clay_test::item(scene::Prim::box(cf3(0.3f, 0.3f, 0.3f)), cf3(0, 0, 0)));
        l.sdf->insert(clay_test::item(scene::Prim::plane(cf3(0, 1, 0), 0.0f), cf3(0, 0, 0)));

        scene::Tape culled = scene::compile_document(doc, &far);
        scene::Tape whole = scene::compile_document(doc);
        CHECK_FALSE(culled.empty());                        // the plane stayed
        CHECK(culled.instrs.size() < whole.instrs.size());  // the box went
    }

    SUBCASE("a purely local document culls away to nothing") {
        scene::Document local;
        scene::Layer& ll = local.add_sdf_layer("l");
        ll.sdf->insert(clay_test::item(scene::Prim::sphere(0.2f), cf3(0, 0, 0)));
        CHECK(scene::compile_document(local, &far).empty());
    }

    SUBCASE("culling changes nothing a whole-document compile would see") {
        // The cull region covering everything must give the same tape as no
        // cull region at all — the guard is a filter, not a transform.
        scene::Document doc = gnarly_document();
        scene::CullRegion all;
        all.region = math::Aabb{cf3(-1000, -1000, -1000), cf3(1000, 1000, 1000)};
        scene::Tape a = scene::compile_document(doc, &all);
        scene::Tape b = scene::compile_document(doc);
        REQUIRE(a.instrs.size() == b.instrs.size());
        CHECK(a.params == b.params);
        CHECK(a.blob == b.blob);
        for (std::size_t i = 0; i < a.instrs.size(); ++i) {
            CHECK(a.instrs[i].op == b.instrs[i].op);
            CHECK(a.instrs[i].param_offset == b.instrs[i].param_offset);
        }
    }
}
