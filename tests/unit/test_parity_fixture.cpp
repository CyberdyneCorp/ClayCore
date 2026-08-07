#include <doctest/doctest.h>

#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "clay/eval/backend.h"
#include "clay/io/parity_fixture.h"
#include "clay/kernel/ops.h"
#include "clay/kernel/prim3d.h"

// Host parity fixture (build-packaging spec). The fixture is what a host GPU
// preview runs to prove it evaluates the same field ClayCore bakes, so the
// suite has to gate two things: that its recorded expectations are what
// ClayCore actually produces (through the tape AND through every registered
// backend), and that it is discriminating — a preview that mis-copies a blend
// must fail it rather than pass by luck.

using namespace clay;
using kernel::cfloat3;

namespace {

bool within(float got, float expected, const io::FixtureTolerance& tol) {
    return std::fabs(got - expected) <=
           tol.distance_abs + tol.distance_rel * std::fabs(expected);
}

// The drifted smin, verbatim in shape: mix form, support k. This is what
// ClaySpace's Metal preview computed while the engine used support 4k, and
// what the fixture has to reject.
float smin_support_k(float a, float b, float k) {
    if (k <= 0.0f) return kernel::cmin(a, b);
    float h = kernel::cclamp(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
    return kernel::cmix(b, a, h) - k * h * (1.0f - h);
}

}  // namespace

TEST_CASE("parity fixture: cases cover the drift-prone vocabulary") {
    std::vector<io::FixtureCase> cases = io::kernel_parity_cases();
    REQUIRE(cases.size() >= 25);

    std::set<std::string> names;
    for (const io::FixtureCase& c : cases) {
        CAPTURE(c.name);
        CHECK(names.insert(c.name).second);  // names are the fixture's keys
        CHECK_FALSE(c.tape.empty());
        CHECK(c.points.size() == c.distances.size());
        CHECK(c.points.size() == c.colors.size());
        CHECK(c.points.size() >= 24);
        for (float d : c.distances) CHECK(std::isfinite(d));
    }

    // the cases the issue named: every profile against every smooth boolean,
    // every extended mode, the blob carriers, and a composed document
    for (const char* op : {"union", "subtract", "intersect"})
        for (const char* profile : {"hard", "quadratic", "cubic", "circular", "chamfer"})
            CHECK(names.count(std::string("blend_") + op + "_" + profile) == 1);
    for (const char* mode : {"paint", "groove", "tongue", "pipe", "engrave", "emboss", "inset",
                             "shell", "replace"})
        CHECK(names.count(std::string("extended_") + mode) == 1);
    CHECK(names.count("deformer_chain") == 1);
    CHECK(names.count("deformer_region") == 1);
    CHECK(names.count("repetition_finite_grid") == 1);
    CHECK(names.count("lift_polygon_revolve") == 1);
    CHECK(names.count("lift_polygon_extrude") == 1);
    CHECK(names.count("lift_loft_profiles") == 1);
    CHECK(names.count("lift_swept_guide") == 1);
    CHECK(names.count("stroke_chain") == 1);
    CHECK(names.count("curve_spline_chain") == 1);
    CHECK(names.count("composed_document") == 1);
}

// A host that raymarches an exported tape has to step by the tape's own
// safe_step_scale, not by 1. That only bites on a bound field, so the fixture
// must actually contain one — otherwise every case would pass for a host that
// hardcodes 1 and the export field would be decorative.
TEST_CASE("parity fixture: a bound case pins safe_step_scale below 1") {
    std::vector<io::FixtureCase> cases = io::kernel_parity_cases();
    std::size_t bound = 0;
    for (const io::FixtureCase& c : cases) {
        CAPTURE(c.name);
        CHECK(c.tape.safe_step_scale() > 0.0f);
        CHECK(c.tape.safe_step_scale() <= 1.0f);
        if (!c.tape.info.is_exact && c.tape.safe_step_scale() < 1.0f) ++bound;
    }
    CHECK(bound > 0);

    // and it reaches the file, where a consumer can read it
    std::string json = io::kernel_parity_fixture_json(cases);
    CHECK(json.find("\"safe_step_scale\":") != std::string::npos);
    CHECK(json.find("\"is_exact\":false") != std::string::npos);
}

TEST_CASE("parity fixture: expectations are what the tape evaluates") {
    io::FixtureTolerance tol;
    for (const io::FixtureCase& c : io::kernel_parity_cases()) {
        CAPTURE(c.name);
        for (std::size_t i = 0; i < c.points.size(); ++i) {
            kernel::CTapeValue v = c.tape.eval(c.points[i]);
            CAPTURE(i);
            CHECK(within(v.d, c.distances[i], tol));
            CHECK(std::fabs(v.color.x - c.colors[i].x) <= tol.color_abs);
            CHECK(std::fabs(v.color.y - c.colors[i].y) <= tol.color_abs);
            CHECK(std::fabs(v.color.z - c.colors[i].z) <= tol.color_abs);
        }
    }
}

TEST_CASE("parity fixture: expectations hold on every registered backend") {
    std::vector<io::FixtureCase> cases = io::kernel_parity_cases();
    io::FixtureTolerance tol;
    for (eval::Backend* backend : eval::Registry::instance().all()) {
        CAPTURE(backend->name());
        for (const io::FixtureCase& c : cases) {
            CAPTURE(c.name);
            std::vector<float> flat;
            flat.reserve(c.points.size() * 3);
            for (cfloat3 p : c.points) {
                flat.push_back(p.x);
                flat.push_back(p.y);
                flat.push_back(p.z);
            }
            std::vector<float> got(c.points.size(), 0.0f);
            eval::PointQuery q;
            q.points_xyz = flat.data();
            q.count = c.points.size();
            eval::PointResults out;
            out.distances = got.data();
            REQUIRE(backend->eval_points(c.tape, q, out) == eval::Status::Ok);
            for (std::size_t i = 0; i < got.size(); ++i) {
                CAPTURE(i);
                CHECK(within(got[i], c.distances[i], tol));
            }
        }
    }
}

TEST_CASE("parity fixture: export is deterministic and parseable as JSON") {
    std::string a = io::kernel_parity_fixture_json(io::kernel_parity_cases());
    std::string b = io::kernel_parity_fixture_json(io::kernel_parity_cases());
    CHECK(a == b);  // no clock, no address, no locale in the output

    // structural sanity: balanced brackets, one "name" per case, and no
    // locale-borne decimal comma masquerading as a separator
    int braces = 0, brackets = 0;
    bool in_string = false;
    std::size_t names = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ch = a[i];
        if (ch == '"' && (i == 0 || a[i - 1] != '\\')) in_string = !in_string;
        if (in_string) continue;
        braces += (ch == '{') - (ch == '}');
        brackets += (ch == '[') - (ch == ']');
        CHECK(braces >= 0);
        CHECK(brackets >= 0);
    }
    CHECK(braces == 0);
    CHECK(brackets == 0);
    for (std::size_t at = a.find("\"name\":"); at != std::string::npos;
         at = a.find("\"name\":", at + 1))
        ++names;
    CHECK(names == io::kernel_parity_cases().size());
    CHECK(a.find("\"schema\": 1") != std::string::npos);
}

// Regression for the drift this fixture exists to catch (issue #3): the app's
// Metal preview used a mix-form quadratic smin of support k where
// csmin_quadratic uses support 4k. Every blend was four times narrower on
// screen than in the field ClayCore baked. A fixture that such a preview could
// still pass would be worthless, so assert it cannot.
TEST_CASE("parity fixture: a support-k quadratic smin is rejected") {
    // The tape's own orientation (kernel/tape.h ctape_combine_values):
    // subtract is -smin(-a, b), intersect is -smin(-a, -b).
    using Smin = float (*)(float, float, float);
    auto compose = [](const std::string& name, float a, float b, Smin smin) {
        if (name == "blend_union_quadratic") return smin(a, b, 0.25f);
        if (name == "blend_subtract_quadratic") return -smin(-a, b, 0.25f);
        return -smin(-a, -b, 0.25f);  // blend_intersect_quadratic
    };
    const Smin correct = &kernel::csmin_quadratic;
    const Smin drifted = &smin_support_k;

    io::FixtureTolerance tol;
    std::size_t checked = 0;
    for (const io::FixtureCase& c : io::kernel_parity_cases()) {
        if (c.name.find("quadratic") == std::string::npos) continue;
        ++checked;
        CAPTURE(c.name);
        std::size_t disagreements = 0;
        for (std::size_t i = 0; i < c.points.size(); ++i) {
            cfloat3 p = c.points[i];
            float a = kernel::sd_sphere(p - kernel::cf3(-0.55f, 0, 0), 0.8f);
            float b = kernel::sd_sphere(p - kernel::cf3(0.55f, 0, 0), 0.8f);
            CAPTURE(i);
            // the operand derivation is right: support 4k reproduces the fixture
            CHECK(within(compose(c.name, a, b, correct), c.distances[i], tol));
            if (!within(compose(c.name, a, b, drifted), c.distances[i], tol)) ++disagreements;
        }
        CHECK(disagreements > 0);
    }
    CHECK(checked == 3);  // union, subtract, intersect
}

TEST_CASE("fixture: every combine op the kernel implements has a case") {
    // Scanned from the COMPILED TAPES, not from case names: a name is a label a
    // human chose, and the question is which math a consumer actually gets to
    // check. An op that ships without a case leaves the fixture reading as
    // validation while asserting nothing about it.
    //
    // The layout is emit_prim's: 12 transform floats, scale, rounding, 3 colour,
    // CLAY_TAPE_PRIM_PARAMS prim params, 7 repeat floats, then the deformer
    // count. If that ever changes this fails loudly, which is correct.
    std::set<int> ops;
    for (const io::FixtureCase& c : io::kernel_parity_cases())
        for (const kernel::CTapeInstr& in : c.tape.instrs)
            if (in.op == kernel::ctape_combine)
                ops.insert(static_cast<int>(c.tape.params[in.param_offset]));

    const struct {
        int mode;
        const char* name;
    } kOps[] = {
        {kernel::ccombine_add, "add"},
        {kernel::ccombine_subtract, "subtract"},
        {kernel::ccombine_intersect, "intersect"},
        {kernel::ccombine_paint, "paint"},
        {kernel::ccombine_groove, "groove"},
        {kernel::ccombine_tongue, "tongue"},
        {kernel::ccombine_pipe, "pipe"},
        {kernel::ccombine_engrave, "engrave"},
        {kernel::ccombine_emboss, "emboss"},
        {kernel::ccombine_inset, "inset"},
        {kernel::ccombine_shell, "shell"},
        {kernel::ccombine_replace, "replace"},
        {kernel::ccombine_transition_linear, "transition_linear"},
        {kernel::ccombine_transition_radial, "transition_radial"},
        {kernel::ccombine_relief, "relief"},
        {kernel::ccombine_incise, "incise"},
    };
    // Every mode from add to the last one is listed, so a new op added without a
    // row here fails this rather than slipping past unnoticed.
    REQUIRE(static_cast<int>(sizeof kOps / sizeof kOps[0]) == kernel::ccombine_incise + 1);

    for (const auto& op : kOps) {
        INFO("combine op: " << op.name);
        CHECK(ops.count(op.mode) == 1);
    }
}

TEST_CASE("fixture: a case reaches the geometry it exists to exercise") {
    // A case whose probes all sit in empty space records agreement about
    // nothing. Every case must put at least one probe near a surface.
    for (const io::FixtureCase& c : io::kernel_parity_cases()) {
        INFO("case: " << c.name);
        REQUIRE_FALSE(c.distances.empty());
        float nearest = 1e9f;
        for (float d : c.distances) nearest = std::min(nearest, std::abs(d));
        CHECK(nearest < 0.25f);
    }
}

