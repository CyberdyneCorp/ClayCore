// Host parity fixture export — see include/clay/io/parity_fixture.h.

#include "clay/io/parity_fixture.h"

#include "clay/field/volume.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "clay/scene/document.h"
#include "clay/version.h"

namespace clay {
namespace io {
namespace {

using kernel::cf3;
using kernel::cfloat3;
using scene::Blend;
using scene::BlendProfile;
using scene::Document;
using scene::Layer;
using scene::Node;
using scene::Op;
using scene::Prim;

constexpr int kFixtureSchema = 1;

// -- deterministic sampling --------------------------------------------------

// The same LCG the kernel property tests use. Fixed seed, no clock, no
// platform-dependent RNG: two exports of one build must be byte-identical.
struct Lcg {
    std::uint64_t state;
    float next01() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<float>((state >> 40) & 0xFFFFFF) / 16777216.0f;
    }
    float range(float lo, float hi) { return lo + (hi - lo) * next01(); }
};

// -- scene builders ----------------------------------------------------------

Node item(Prim prim, cfloat3 pos, cfloat3 color, Op op = Op::Add, Blend blend = {}) {
    Node n;
    n.prim = prim;
    n.xform.position = pos;
    n.color = color;
    n.op = op;
    n.blend = blend;
    return n;
}

const cfloat3 kColorA = cf3(0.85f, 0.27f, 0.20f);
const cfloat3 kColorB = cf3(0.16f, 0.47f, 0.88f);

// Two equal spheres straddling the origin: their surfaces cross the plane
// x = 0 on a circle of radius ~0.58, which is what the probe ring brackets.
// Every combine case is built on this pair so a blend's support width is the
// only thing under test.
Document seam_pair(Op op, BlendProfile profile, float k) {
    Document doc;
    Layer& l = doc.add_sdf_layer("seam");
    l.sdf->insert(item(Prim::sphere(0.8f), cf3(-0.55f, 0, 0), kColorA));
    l.sdf->insert(item(Prim::sphere(0.8f), cf3(0.55f, 0, 0), kColorB, op, Blend{profile, k}));
    return doc;
}

// A box carved by a sphere with one of the extended modes. Rounding is what
// groove/tongue read as their channel half-width, so it is set for all of
// them and simply ignored elsewhere.
Document extended_pair(Op op, float k) {
    Document doc;
    Layer& l = doc.add_sdf_layer("extended");
    l.sdf->insert(item(Prim::box(cf3(0.9f, 0.6f, 0.7f)), cf3(0, 0, 0), kColorA));
    Node cut = item(Prim::sphere(0.55f), cf3(0.45f, 0.35f, 0.30f), kColorB, op,
                    Blend{BlendProfile::Quadratic, k});
    cut.rounding = 0.06f;
    l.sdf->insert(cut);
    return doc;
}

Document transition_pair(Op op) {
    Document doc;
    Layer& l = doc.add_sdf_layer("transition");
    l.sdf->insert(item(Prim::box(cf3(0.7f, 0.7f, 0.7f)), cf3(0, 0, 0), kColorA));
    Node morph = item(Prim::sphere(0.85f), cf3(0, 0, 0), kColorB, op);
    morph.transition.a = cf3(0, -1.0f, 0);
    morph.transition.b = cf3(0, 1.0f, 0);
    morph.transition.r0 = 0.2f;
    morph.transition.r1 = 1.3f;
    morph.transition.ease = 3;
    l.sdf->insert(morph);
    return doc;
}

Document deformer_chain() {
    Document doc;
    Layer& l = doc.add_sdf_layer("deformed");
    Node n = item(Prim::box(cf3(0.45f, 1.0f, 0.35f)), cf3(0, 0, 0), kColorA);
    n.deformers.push_back(scene::Deformer::twist(1.3f));
    n.deformers.push_back(scene::Deformer::taper(-1.0f, 1.0f, 1.0f, 0.4f, 3));
    l.sdf->insert(n);
    return doc;
}

// The ranged pair, with an eased ramp so the case exercises the ease slot as
// well as the range — a linear ramp would only prove the equivalence the unit
// test already asserts, and would let a backend that ignored the ease pass.
Document ranged_twist_bend() {
    Document doc;
    Layer& l = doc.add_sdf_layer("ranged");
    Node a = item(Prim::box(cf3(0.35f, 0.9f, 0.3f)), cf3(-0.7f, 0, 0), kColorA);
    a.deformers.push_back(scene::Deformer::twist_range(2.1f, -0.5f, 0.5f, 3));
    l.sdf->insert(a);
    Node b = item(Prim::box(cf3(0.9f, 0.3f, 0.3f)), cf3(0.9f, 0, 0), kColorB);
    b.deformers.push_back(scene::Deformer::bend_range(1.4f, -0.4f, 0.4f, 3));
    l.sdf->insert(b);
    return doc;
}

Document region_deformer() {
    Document doc;
    Layer& l = doc.add_sdf_layer("region");
    Node n = item(Prim::sphere(0.9f), cf3(0, 0, 0), kColorB);
    n.deformers.push_back(scene::Deformer::grab(cf3(0.9f, 0, 0), 0.85f, cf3(0.35f, 0.25f, 0), 3));
    l.sdf->insert(n);
    return doc;
}

Document repetition() {
    Document doc;
    Layer& l = doc.add_sdf_layer("array");
    Node n = item(Prim::round_box(cf3(0.18f, 0.18f, 0.18f), 0.05f), cf3(0, 0, 0), kColorA);
    n.repeat = scene::Repeat::grid_finite(0.7f, cf3(1, 1, 0));
    l.sdf->insert(n);
    return doc;
}

// Lift of a polygon profile: the vertices live in the tape's out-of-line
// blob, so this case is also what proves a host reads the blob correctly.
Document polygon_revolve() {
    Document doc;
    Layer& l = doc.add_sdf_layer("lift");
    Node n = item(Prim::revolve(0.55f), cf3(0, 0, 0), kColorB);
    n.profile = scene::Profile::polygon();
    n.profile_points = {kernel::cf2(-0.30f, -0.45f), kernel::cf2(0.30f, -0.45f),
                        kernel::cf2(0.12f, 0.0f),    kernel::cf2(0.34f, 0.50f),
                        kernel::cf2(-0.34f, 0.50f),  kernel::cf2(-0.12f, 0.0f)};
    l.sdf->insert(n);
    return doc;
}

// Extruded polygon: the other lift opcode, and the shape the cut tool resolves
// a drawn outline into (cut-tool spec), so a host previewing a trim is
// previewing this.
Document polygon_extrude() {
    Document doc;
    Layer& l = doc.add_sdf_layer("lift");
    Node n = item(Prim::extrude(0.4f), cf3(0, 0, 0), kColorA);
    n.profile = scene::Profile::polygon();
    n.profile_points = {kernel::cf2(-0.6f, -0.35f), kernel::cf2(0.6f, -0.35f),
                        kernel::cf2(0.25f, 0.1f),   kernel::cf2(0.6f, 0.55f),
                        kernel::cf2(-0.6f, 0.55f)};
    n.rounding = 0.05f;
    l.sdf->insert(n);
    return doc;
}

// Loft: N profiles interpolated along Z. The field is a lerp of two distance
// fields, so it is a BOUND — the tape's safe_step_scale drops well below 1 and
// a host that assumes 1 raymarches through the surface. Three profiles, so the
// bracketing path is exercised rather than the two-profile shortcut.
Document loft_profiles() {
    Document doc;
    Layer& l = doc.add_sdf_layer("loft");
    Node n = item(Prim::loft(0.7f, 3), cf3(0, 0, 0), kColorA);
    n.profiles = {scene::Profile::circle(0.75f), scene::Profile::box(0.25f, 0.25f),
                  scene::Profile::circle(0.6f)};
    n.profile_polygons.resize(n.profiles.size());
    l.sdf->insert(n);
    return doc;
}

// Swept: the same profile list carried along a guide, with parallel-transported
// frames baked into the blob when the item compiles. Its Lipschitz factor
// carries both a curvature and an interpolation term, so like the loft above it
// is a case about safe_step_scale as much as about distance.
Document swept_guide() {
    Document doc;
    Layer& l = doc.add_sdf_layer("swept");
    Node n = item(Prim::swept(3), cf3(0, 0, 0), kColorB);
    const cfloat3 guide[4] = {cf3(-0.9f, -0.5f, 0.0f), cf3(-0.3f, 0.25f, 0.2f),
                              cf3(0.4f, 0.45f, -0.15f), cf3(0.95f, -0.2f, 0.1f)};
    for (int i = 0; i < 4; ++i) {
        scene::StrokePoint sp;
        sp.pos = guide[i];
        sp.type = scene::StrokePointType::Spline;
        n.stroke.push_back(sp);
    }
    n.curve_tolerance = 0.02f;
    n.profiles = {scene::Profile::circle(0.28f), scene::Profile::circle(0.12f)};
    n.profile_polygons.resize(n.profiles.size());
    l.sdf->insert(n);
    return doc;
}

// A sampled narrow-band volume. Unlike every other case here the tape carries
// no formula at all — the blob holds a brick index, per-brick bounds and the
// samples, and the opcode is a lookup. It is the case a hand-written preview is
// most likely to get subtly wrong, because there are three things to agree on
// rather than one: the trilinear tap inside a stored brick, the signed lower
// bound a sample-free brick reports instead, and how the outside-the-box
// distance folds together with the field at the projected point.
Document sampled_volume() {
    Document doc;
    Layer& l = doc.add_sdf_layer("volume");
    Node n;
    n.prim = Prim::volume();
    n.color = kColorB;
    // A torus rather than a sphere: it puts sample-free bricks INSIDE the
    // sampled box as well as outside, so the bound path is exercised on both
    // sides of the surface.
    n.volume = std::make_shared<field::FieldVolume>(field::FieldVolume::sample(
        [](kernel::cfloat3 p) {
            float q = std::sqrt(p.x * p.x + p.z * p.z) - 0.55f;
            return std::sqrt(q * q + p.y * p.y) - 0.22f;
        },
        math::Aabb{cf3(-1.0f, -0.45f, -1.0f), cf3(1.0f, 0.45f, 1.0f)}, 0.06f, 0.2f));
    l.sdf->insert(n);
    return doc;
}

Document stroke_chain() {
    Document doc;
    Layer& l = doc.add_sdf_layer("stroke");
    Node n;
    n.prim = Prim::stroke();
    n.color = kColorA;
    n.stroke = {{cf3(-0.9f, -0.3f, 0.1f), 0.28f},
                {cf3(-0.2f, 0.35f, -0.15f), 0.20f},
                {cf3(0.6f, 0.05f, 0.25f), 0.32f}};
    n.stroke_blend_k = 0.06f;
    l.sdf->insert(n);
    return doc;
}

// A control-point curve. Typed points are tessellated into the segment chain
// the stroke opcode already reads (scene-model: curves are not a new
// primitive), so what reaches a host is an ordinary stroke item with a much
// longer blob. Closed, with a document tolerance — both change the point count,
// which is exactly what a host reading the blob by offset gets wrong.
Document curve_chain() {
    Document doc;
    Layer& l = doc.add_sdf_layer("curve");
    Node n;
    n.prim = Prim::stroke();
    n.color = kColorB;
    const cfloat3 control[5] = {cf3(-0.8f, -0.4f, 0.0f), cf3(-0.1f, 0.6f, 0.25f),
                                cf3(0.75f, 0.15f, -0.2f), cf3(0.35f, -0.7f, 0.3f),
                                cf3(-0.45f, -0.55f, -0.25f)};
    const float radius[5] = {0.22f, 0.17f, 0.26f, 0.15f, 0.20f};
    for (int i = 0; i < 5; ++i) {
        scene::StrokePoint sp;
        sp.pos = control[i];
        sp.radius = radius[i];
        sp.type = scene::StrokePointType::Spline;
        n.stroke.push_back(sp);
    }
    n.stroke_closed = true;
    n.curve_tolerance = 0.01f;
    n.stroke_blend_k = 0.04f;
    l.sdf->insert(n);
    return doc;
}

// Everything at once: two layers, a mirror with a blended seam, a nested
// group with its own op, a paint pass. A host that agrees on every case above
// and disagrees here has a traversal or transform bug rather than a math one.
Document composed() {
    Document doc;
    Layer& body = doc.add_sdf_layer("body");
    body.xform.position = cf3(0.1f, -0.05f, 0.0f);
    body.mirror_axes = scene::kMirrorX;
    body.mirror_k = 0.08f;
    scene::SdfContent& c = *body.sdf;
    c.insert(item(Prim::sphere(0.85f), cf3(0, 0, 0), kColorA));
    c.insert(item(Prim::capped_cylinder(0.25f, 0.7f), cf3(-0.3f, 0.4f, 0), kColorB, Op::Subtract,
                  Blend{BlendProfile::Cubic, 0.07f}));
    Node ear = item(Prim::round_cone(0.22f, 0.09f, 0.4f), cf3(0.7f, 0.5f, 0), kColorB, Op::Add,
                    Blend{BlendProfile::Quadratic, 0.06f});
    ear.mirror = true;
    c.insert(ear);

    Node group;
    group.is_group = true;
    group.op = Op::Add;
    group.blend = Blend{BlendProfile::Quadratic, 0.1f};
    scene::NodeId gid = c.insert(group);
    c.insert(item(Prim::octahedron(0.4f), cf3(0, 0, -0.7f), kColorA), gid);
    c.insert(item(Prim::torus(0.5f, 0.12f), cf3(0, 0.2f, -0.7f), kColorB, Op::Add,
                  Blend{BlendProfile::Circular, 0.05f}),
             gid);

    c.insert(item(Prim::sphere(0.4f), cf3(0.3f, 0.4f, 0.4f), kColorB, Op::Paint,
                  Blend{BlendProfile::Quadratic, 0.12f}));

    Layer& base = doc.add_sdf_layer("base");
    base.xform.position = cf3(0, -1.2f, 0);
    base.sdf->insert(item(Prim::box(cf3(1.2f, 0.15f, 1.2f)), cf3(0, 0, 0), kColorA));
    return doc;
}

// -- JSON ---------------------------------------------------------------------

void append_float(std::string* out, float v) {
    if (!std::isfinite(v)) {  // never produced by the cases; keeps the JSON valid if it were
        *out += "null";
        return;
    }
    // The shortest form that round-trips: the smallest precision whose text
    // parses back to the same bits. Nine significant digits always suffice for
    // a float, so the loop terminates.
    //
    // NOT std::to_chars, whose floating-point overload arrived in macOS 13.3
    // and iOS 16.3 — later than the xcframework's own minimum, which is a
    // promise about what the library runs on rather than a build detail to be
    // raised when a source file finds it inconvenient.
    char buf[40];
    for (int precision = 1; precision <= 9; ++precision) {
        std::snprintf(buf, sizeof buf, "%.*g", precision, static_cast<double>(v));
        if (std::strtof(buf, nullptr) == v) break;
    }
    // %g writes the host locale's decimal separator, which under a comma
    // locale would emit JSON no parser accepts. strtof read it back the same
    // way, so the round-trip test above still held; only the text needs fixing.
    for (char* c = buf; *c; ++c)
        if (*c == ',') *c = '.';
    *out += buf;
}

void append_floats(std::string* out, const float* values, std::size_t count) {
    *out += '[';
    for (std::size_t i = 0; i < count; ++i) {
        if (i) *out += ',';
        append_float(out, values[i]);
    }
    *out += ']';
}

void append_vec3_array(std::string* out, const std::vector<cfloat3>& v) {
    *out += '[';
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) *out += ',';
        const float xyz[3] = {v[i].x, v[i].y, v[i].z};
        append_floats(out, xyz, 3);
    }
    *out += ']';
}

void append_string(std::string* out, const std::string& s) {
    *out += '"';
    for (char ch : s) {
        if (ch == '"' || ch == '\\') *out += '\\';
        *out += ch;
    }
    *out += '"';
}

void append_tape(std::string* out, const scene::Tape& tape) {
    *out += "{\"instrs\":[";
    for (std::size_t i = 0; i < tape.instrs.size(); ++i) {
        if (i) *out += ',';
        *out += '[';
        *out += std::to_string(tape.instrs[i].op);
        *out += ',';
        *out += std::to_string(tape.instrs[i].param_offset);
        *out += ']';
    }
    *out += "],\"params\":";
    append_floats(out, tape.params.data(), tape.params.size());
    *out += ",\"blob\":";
    append_floats(out, tape.blob.data(), tape.blob.size());
    *out += ",\"is_exact\":";
    *out += tape.info.is_exact ? "true" : "false";
    *out += ",\"safe_step_scale\":";
    append_float(out, tape.safe_step_scale());
    *out += '}';
}

void append_case(std::string* out, const FixtureCase& c) {
    *out += "{\"name\":";
    append_string(out, c.name);
    *out += ",\"note\":";
    append_string(out, c.note);
    *out += ",\"tape\":";
    append_tape(out, c.tape);
    *out += ",\"points\":";
    append_vec3_array(out, c.points);
    *out += ",\"distance\":";
    append_floats(out, c.distances.data(), c.distances.size());
    *out += ",\"color\":";
    append_vec3_array(out, c.colors);
    *out += '}';
}

// -- case assembly -------------------------------------------------------------

void add_case(std::vector<FixtureCase>* cases, std::string name, std::string note,
              const Document& doc) {
    FixtureCase c;
    c.name = std::move(name);
    c.note = std::move(note);
    c.tape = scene::compile_document(doc);
    c.points = kernel_parity_probe_points();
    c.distances.reserve(c.points.size());
    c.colors.reserve(c.points.size());
    for (cfloat3 p : c.points) {
        kernel::CTapeValue v = c.tape.eval(p);
        c.distances.push_back(v.d);
        c.colors.push_back(v.color);
    }
    cases->push_back(std::move(c));
}

const char* profile_name(BlendProfile p) {
    switch (p) {
        case BlendProfile::Hard: return "hard";
        case BlendProfile::Quadratic: return "quadratic";
        case BlendProfile::Cubic: return "cubic";
        case BlendProfile::Circular: return "circular";
        case BlendProfile::Chamfer: return "chamfer";
    }
    return "unknown";
}

void add_blend_cases(std::vector<FixtureCase>* cases) {
    const struct {
        const char* name;
        Op op;
    } ops[] = {{"union", Op::Add}, {"subtract", Op::Subtract}, {"intersect", Op::Intersect}};
    const BlendProfile profiles[] = {BlendProfile::Hard, BlendProfile::Quadratic,
                                     BlendProfile::Cubic, BlendProfile::Circular,
                                     BlendProfile::Chamfer};
    for (const auto& op : ops) {
        for (BlendProfile profile : profiles) {
            std::string name = std::string("blend_") + op.name + "_" + profile_name(profile);
            add_case(cases, name,
                     "two spheres across the seam plane; k = 0.25, so the support width "
                     "is the profile's own multiple of k",
                     seam_pair(op.op, profile, 0.25f));
        }
    }
}

// Relief and incise: the item is a REGION, not geometry. A host that treats the
// second operand as a shape to union or carve disagrees on the first probe.
// The region sits on the +Y pole, where the fixture's ring probes already are.
Document relief_pair(Op op) {
    Document doc;
    Layer& l = doc.add_sdf_layer("relief");
    l.sdf->insert(item(Prim::sphere(0.9f), cf3(0, 0, 0), kColorA));
    // Sized so it reaches 17 of the 32 probes and leaves 15 alone: the untouched
    // half is what exercises the FINITE SUPPORT, which a region covering every
    // probe could not catch.
    Node region = item(Prim::sphere(0.75f), cf3(0, 0.9f, 0), kColorB, op,
                       Blend{BlendProfile::Quadratic, 0.18f});
    region.rounding = 0.35f;  // the falloff width, and it rounds the region too
    l.sdf->insert(region);
    return doc;
}

// The integer-hashed gradient noise. This case earns its place by catching the
// failure the design exists to avoid: a host that reaches for the familiar
// fract(sin(dot(p, k)) * 43758.5453) hash instead of compiling ours diverges by
// O(1), not by a tolerance, because a chaotic amplifier turns the backends'
// last-place disagreement in sin() into a different number entirely.
Document noise_deformer() {
    Document doc;
    Layer& l = doc.add_sdf_layer("noise");
    Node n = item(Prim::sphere(0.9f), cf3(0, 0, 0), kColorB);
    n.deformers.push_back(scene::Deformer::noise(0.12f, 3.0f, 4, 0.5f, 7u));
    l.sdf->insert(n);
    return doc;
}

// Magnify and pinch are one deformation with a signed strength, so both signs:
// a backend can reproduce the swell and invert the gather.
Document magnify_deformer(float strength) {
    Document doc;
    Layer& l = doc.add_sdf_layer("magnify");
    Node n = item(Prim::sphere(0.9f), cf3(0, 0, 0), kColorA);
    // Off the origin on purpose: centred on the sphere's own middle the scale is
    // symmetric and the falloff barely shows. Here it reaches 14 probes and
    // leaves 18, so the support is exercised as well as the deformation.
    n.deformers.push_back(scene::Deformer::magnify(cf3(0, 0.45f, 0), 1.2f, strength, 3));
    l.sdf->insert(n);
    return doc;
}

void add_extended_cases(std::vector<FixtureCase>* cases) {
    const struct {
        const char* name;
        Op op;
    } modes[] = {{"paint", Op::Paint},     {"groove", Op::Groove}, {"tongue", Op::Tongue},
                 {"pipe", Op::Pipe},       {"engrave", Op::Engrave}, {"emboss", Op::Emboss},
                 {"inset", Op::Inset},     {"shell", Op::Shell},   {"replace", Op::Replace}};
    for (const auto& mode : modes) {
        add_case(cases, std::string("extended_") + mode.name,
                 "box combined with a sphere through an extended mode; k = 0.12, "
                 "rounding 0.06 (the groove/tongue channel half-width)",
                 extended_pair(mode.op, 0.12f));
    }
}

}  // namespace

std::vector<cfloat3> kernel_parity_probe_points() {
    std::vector<cfloat3> points;
    // 12 on the seam plane, bracketing the two-sphere intersection circle
    // (radius ~0.58): this is where a wrong blend support shows up first.
    const float radii[2] = {0.45f, 0.85f};
    for (float r : radii) {
        for (int i = 0; i < 6; ++i) {
            float a = 6.2831853f * static_cast<float>(i) / 6.0f;
            points.push_back(cf3(0.0f, r * kernel::ccos(a), r * kernel::csin(a)));
        }
    }
    // 20 spread through the interaction volume, deterministically.
    Lcg rng{0x9E3779B97F4A7C15ull};
    for (int i = 0; i < 20; ++i)
        points.push_back(cf3(rng.range(-1.7f, 1.7f), rng.range(-1.7f, 1.7f),
                             rng.range(-1.7f, 1.7f)));
    return points;
}

std::vector<FixtureCase> kernel_parity_cases() {
    std::vector<FixtureCase> cases;
    add_blend_cases(&cases);
    add_extended_cases(&cases);
    add_case(&cases, "transition_linear", "spatial morph eased along a segment",
             transition_pair(Op::TransitionLinear));
    add_case(&cases, "transition_radial", "spatial morph eased across a radial band",
             transition_pair(Op::TransitionRadial));
    add_case(&cases, "deformer_chain", "twist then taper, applied in authoring order",
             deformer_chain());
    add_case(&cases, "ranged_twist_bend",
             "twist and bend ramped across a span and held beyond it, eased",
             ranged_twist_bend());
    add_case(&cases, "deformer_region", "grab: a finitely supported pull on part of a sphere",
             region_deformer());
    add_case(&cases, "repetition_finite_grid", "clamped-cell array with neighbour evaluation",
             repetition());
    add_case(&cases, "lift_polygon_revolve",
             "revolved polygon profile; the vertices live in the tape blob", polygon_revolve());
    add_case(&cases, "lift_polygon_extrude",
             "extruded rounded polygon — what the cut tool resolves a drawn outline into",
             polygon_extrude());
    add_case(&cases, "lift_loft_profiles",
             "three profiles interpolated along Z; a bound, so safe_step_scale is not 1",
             loft_profiles());
    add_case(&cases, "lift_swept_guide",
             "profiles carried along a guide on parallel-transported frames, in the blob",
             swept_guide());
    add_case(&cases, "sampled_volume",
             "a narrow-band volume: no formula, a brick index and samples in the blob",
             sampled_volume());
    add_case(&cases, "stroke_chain", "sphere-swept polyline; the points live in the tape blob",
             stroke_chain());
    add_case(&cases, "curve_spline_chain",
             "closed Catmull-Rom curve tessellated into the stroke chain at compile time",
             curve_chain());
    add_case(&cases, "relief_build_up",
             "the item is a REGION: the surface accumulated before it moves OUT along "
             "its own normal by k; rounding is the falloff width",
             relief_pair(Op::Relief));
    add_case(&cases, "relief_cut_in",
             "the same region cutting IN — one kernel branch with the sign from the "
             "mode, so a backend can reproduce one and invert the other",
             relief_pair(Op::Incise));
    add_case(&cases, "deformer_noise",
             "fractal gradient noise on an INTEGER hash; a float sin() hash diverges "
             "by O(1) between backends rather than by a tolerance",
             noise_deformer());
    add_case(&cases, "deformer_magnify",
             "radial scale about a point on the surface, swelling outward", 
             magnify_deformer(0.5f));
    add_case(&cases, "deformer_pinch",
             "the same deformation gathering inward — the sign is the whole difference",
             magnify_deformer(-0.5f));
    add_case(&cases, "composed_document",
             "two layers, a blended mirror, a nested group and a paint pass", composed());
    return cases;
}

std::string kernel_parity_fixture_json(const std::vector<FixtureCase>& cases,
                                       const FixtureTolerance& tol) {
    Version v = version();
    std::string out;
    out.reserve(1u << 16);
    out += "{\n  \"schema\": ";
    out += std::to_string(kFixtureSchema);
    out += ",\n  \"generator\": \"claycore ";
    out += std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
    out += "\",\n  \"note\": \"Evaluate each case's tape with kernels compiled from "
           "clay/kernel/*.h and compare against distance/color at every probe point. "
           "See docs/06-host-gpu-previews.md.\",\n  \"tolerance\": {\"distance_abs\": ";
    append_float(&out, tol.distance_abs);
    out += ", \"distance_rel\": ";
    append_float(&out, tol.distance_rel);
    out += ", \"color_abs\": ";
    append_float(&out, tol.color_abs);
    out += "},\n  \"cases\": [\n";
    for (std::size_t i = 0; i < cases.size(); ++i) {
        out += "    ";
        append_case(&out, cases[i]);
        out += i + 1 < cases.size() ? ",\n" : "\n";
    }
    out += "  ]\n}\n";
    return out;
}

IoStatus save_kernel_parity_fixture(const std::string& path) {
    std::vector<FixtureCase> cases = kernel_parity_cases();
    for (const FixtureCase& c : cases) {
        for (float d : c.distances)
            if (!std::isfinite(d))
                return IoStatus::fail(IoError::Malformed,
                                      "non-finite reference distance in case " + c.name);
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) return IoStatus::fail(IoError::WriteFailed, path);
    std::string json = kernel_parity_fixture_json(cases);
    f.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!f) return IoStatus::fail(IoError::WriteFailed, path);
    return IoStatus::success();
}

}  // namespace io
}  // namespace clay
