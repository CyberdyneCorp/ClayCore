// A prefix seed is a fold suspended mid-chain (#360): compile_layer_prefix
// evaluates the first K roots of the active layer as a standalone tape, and a
// later refill folds roots[K..end) onto that value with compile_layer_suffix
// and the seeded walk -- the same machinery the append path (#306) trusts.
//
// The assertion is IDENTITY. Continuing the fold from the number it reached is
// not an approximation of running the whole chain -- it is the same
// instructions over the same floats -- so anything short of bit-equality means
// the boundary is not where it claims to be. And the whole risk is SILENCE: a
// prefix cut at the wrong place, or culled under the wrong pad, produces a
// tape that compiles, evaluates and answers with the wrong field; nothing
// errors and nothing crashes. So every reuse case here is memcmp == 0 against
// a full compile, every reuse case has teeth (the suffix must MOVE the seed),
// and every shape the compiler cannot be sure of is required to be refused.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "clay/eval/backend.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"

using namespace clay;
using kernel::cf3;
using kernel::cfloat3;

namespace {

// add_sdf_layer returns a reference INTO doc.layers, which reallocates when
// the next layer is added. The SdfContent is shared_ptr-held and does not
// move; the Layer does. Hold the id and the content, never the Layer.
struct LayerRef {
    scene::LayerId id = 0;
    scene::SdfContent* sdf = nullptr;
};

LayerRef add_layer(scene::Document& doc, const char* name) {
    scene::Layer& l = doc.add_sdf_layer(name);
    return LayerRef{l.id, l.sdf.get()};
}

scene::Node dab(float x, float y, float z, float r, scene::Op op = scene::Op::Add,
                float k = 0.0f) {
    scene::Node n;
    n.prim = scene::Prim::sphere(r);
    n.xform.position = cf3(x, y, z);
    n.op = op;
    n.blend = scene::Blend{k > 0.0f ? scene::BlendProfile::Quadratic : scene::BlendProfile::Hard,
                           k};
    return n;
}

// A sculpt: a base and `dabs` blended dabs walked over its surface. Subtract
// every 4th and Quadratic blends throughout, because a boundary bug that is
// invisible under hard-Add min-associativity is exactly the kind that ships.
scene::Document sculpt(int dabs) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("s");
    scene::Node base;
    base.prim = scene::Prim::sphere(1.0f);
    l.sdf->insert(base);
    for (int i = 1; i <= dabs; ++i) {
        scene::Node d;
        d.prim = scene::Prim::sphere(0.18f);
        const float a = 0.4f * std::sin(static_cast<float>(i) * 0.9f);
        const float b = 0.4f * std::cos(static_cast<float>(i) * 1.4f);
        d.xform.position = cf3(std::sqrt(std::max(0.0f, 1.0f - a * a - b * b)), a, b);
        d.op = (i % 4 == 0) ? scene::Op::Subtract : scene::Op::Add;
        d.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.06f};
        l.sdf->insert(d);
    }
    return doc;
}

std::vector<float> lattice(int n) {
    std::vector<float> p;
    p.reserve(static_cast<std::size_t>(n) * n * n * 3);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            for (int k = 0; k < n; ++k) {
                const float s = 2.4f / static_cast<float>(n - 1);
                p.push_back(-1.2f + s * static_cast<float>(i));
                p.push_back(-1.2f + s * static_cast<float>(j));
                p.push_back(-1.2f + s * static_cast<float>(k));
            }
    return p;
}

std::vector<float> eval_whole(const scene::Tape& tape, const std::vector<float>& pts) {
    std::vector<float> out(pts.size() / 3);
    eval::PointQuery q;
    q.points_xyz = pts.data();
    q.count = out.size();
    eval::PointResults r;
    r.distances = out.data();
    eval::eval_points_blocked(tape, q, r);
    return out;
}

// The checkpoint the C binding's plan_frontier hand-builds: the active layer's
// chain is mid-fold with a value on the stack, no earlier layer beneath it,
// and no prefix BYTES -- the suffix compiler re-states the semantics and never
// reads the lengths.
scene::TapeCheckpoint frontier_checkpoint(scene::LayerId layer) {
    scene::TapeCheckpoint cp;
    cp.valid = true;
    cp.layer = layer;
    cp.layer_have_acc = true;
    cp.doc_have_acc = false;
    return cp;
}

// Compile roots[0..K) as a prefix, fold roots[K..end) onto its value, and
// require the result to equal `want` bit for bit. Returns the seed so a caller
// can make its own teeth checks.
std::vector<float> check_split(const scene::Document& doc, scene::LayerId layer,
                               const std::vector<scene::NodeId>& roots, std::size_t K,
                               const std::vector<float>& pts, const std::vector<float>& want,
                               std::size_t whole_instrs) {
    const std::size_t count = pts.size() / 3;
    scene::Tape prefix;
    REQUIRE(scene::compile_layer_prefix(doc, K, &prefix));
    const std::vector<float> seed = eval_whole(prefix, pts);

    const std::vector<scene::NodeId> appended(roots.begin() + static_cast<std::ptrdiff_t>(K),
                                              roots.end());
    scene::Tape suffix;
    REQUIRE(scene::compile_layer_suffix(frontier_checkpoint(layer), doc, appended, &suffix,
                                        nullptr));
    REQUIRE(suffix.instrs.size() > 0);
    REQUIRE(suffix.instrs.size() < whole_instrs);  // it IS only the tail

    // Seeded in place, as the refill does (test_suffix_tape holds that this
    // is safe).
    std::vector<float> got = seed;
    eval::PointQuery q;
    q.points_xyz = pts.data();
    q.count = count;
    eval::PointResults r;
    r.distances = got.data();
    eval::eval_points_seeded(suffix, q, got.data(), nullptr, r);

    CHECK(std::memcmp(got.data(), want.data(), count * sizeof(float)) == 0);
    return seed;
}

}  // namespace

TEST_CASE("prefix plus suffix is the whole chain, to the bit") {
    const std::vector<float> pts = lattice(20);
    const std::size_t count = pts.size() / 3;
    scene::Document doc = sculpt(20);
    const scene::LayerId layer = doc.layers[0].id;
    const std::vector<scene::NodeId> roots = doc.layers[0].sdf->roots;
    const scene::Tape whole = scene::compile_document(doc);
    const std::vector<float> want = eval_whole(whole, pts);

    for (std::size_t K : {std::size_t{1}, roots.size() / 2, roots.size() - 1}) {
        CAPTURE(K);
        const std::vector<float> seed =
            check_split(doc, layer, roots, K, pts, want, whole.instrs.size());
        // Teeth: the suffix has to have MOVED the seed, or the memcmp above is
        // two copies of one buffer agreeing with each other.
        CHECK(std::memcmp(want.data(), seed.data(), count * sizeof(float)) != 0);
    }
}

TEST_CASE("a boundary lands on root granularity, groups included") {
    // Boundary N means N ROOT-LIST entries folded, and a root can be a group
    // whose children -- an Op::None group's especially, which apply inline to
    // the outer chain -- all fold before or after the cut as one unit. The cut
    // on either side of such a group must still be the whole chain to the bit;
    // the children carry a Subtract and soft blends so a fold emitted on the
    // wrong side of the boundary cannot hide behind min-associativity.
    const std::vector<float> pts = lattice(16);
    const std::size_t count = pts.size() / 3;

    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("g");
    l.sdf->insert(dab(0, 0, 0, 1.0f));
    l.sdf->insert(dab(0.6f, 0.3f, 0, 0.3f, scene::Op::Add, 0.08f));
    scene::Node g;
    g.is_group = true;
    g.op = scene::Op::None;  // children inline to the outer chain
    const scene::NodeId gid = l.sdf->insert(g);
    l.sdf->insert(dab(0.2f, 0.8f, 0, 0.28f, scene::Op::Subtract, 0.05f), gid);
    l.sdf->insert(dab(0, 0.9f, 0.3f, 0.3f, scene::Op::Add, 0.07f), gid);
    l.sdf->insert(dab(-0.5f, 0.5f, 0.4f, 0.3f, scene::Op::Add, 0.06f));

    const scene::LayerId layer = doc.layers[0].id;
    const std::vector<scene::NodeId> roots = doc.layers[0].sdf->roots;
    REQUIRE(roots.size() == 4);
    REQUIRE(roots[2] == gid);
    const scene::Tape whole = scene::compile_document(doc);
    const std::vector<float> want = eval_whole(whole, pts);

    // K = 2: the group is the FIRST suffix item. K = 3: the group's inlined
    // children have already folded INTO the prefix.
    for (std::size_t K : {std::size_t{2}, std::size_t{3}}) {
        CAPTURE(K);
        const std::vector<float> seed =
            check_split(doc, layer, roots, K, pts, want, whole.instrs.size());
        CHECK(std::memcmp(want.data(), seed.data(), count * sizeof(float)) != 0);
    }
}

TEST_CASE("a prefix seed carries colour, bit for bit") {
    // The accumulator a coloured walk folds is a CTapeValue, so the seed is a
    // distance AND the colour the prefix reached; continuing with the distance
    // alone would fold every combine against black -- a wrong answer rather
    // than a missing one.
    const std::vector<float> pts = lattice(16);
    const std::size_t count = pts.size() / 3;

    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("c");
    scene::Node base;
    base.prim = scene::Prim::sphere(1.0f);
    base.color = cf3(0.8f, 0.2f, 0.1f);
    l.sdf->insert(base);
    for (int i = 1; i <= 9; ++i) {
        scene::Node d;
        d.prim = scene::Prim::sphere(0.3f);
        const float a = 0.5f * std::sin(static_cast<float>(i) * 0.9f);
        d.xform.position = cf3(std::sqrt(std::max(0.0f, 1.0f - a * a)), a, 0.0f);
        d.color = cf3(0.1f * static_cast<float>(i % 7), 0.9f, 0.3f);
        d.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.09f};
        l.sdf->insert(d);
    }
    const scene::LayerId layer = doc.layers[0].id;
    const std::vector<scene::NodeId> roots = doc.layers[0].sdf->roots;
    const std::size_t K = 5;

    auto eval_full = [&](const scene::Tape& tape, std::vector<float>& d, std::vector<float>& c) {
        d.assign(count, 0.0f);
        c.assign(count * 3, 0.0f);
        eval::PointQuery q;
        q.points_xyz = pts.data();
        q.count = count;
        eval::PointResults r;
        r.distances = d.data();
        r.colors_rgb = c.data();
        eval::eval_points_blocked(tape, q, r);
    };

    scene::Tape prefix;
    REQUIRE(scene::compile_layer_prefix(doc, K, &prefix));
    std::vector<float> seed_d, seed_c, want_d, want_c;
    eval_full(prefix, seed_d, seed_c);
    eval_full(scene::compile_document(doc), want_d, want_c);

    const std::vector<scene::NodeId> appended(roots.begin() + K, roots.end());
    scene::Tape suffix;
    REQUIRE(scene::compile_layer_suffix(frontier_checkpoint(layer), doc, appended, &suffix,
                                        nullptr));

    eval::PointQuery q;
    q.points_xyz = pts.data();
    q.count = count;
    std::vector<float> got_d = seed_d, got_c = seed_c;
    {
        eval::PointResults r;
        r.distances = got_d.data();
        r.colors_rgb = got_c.data();
        eval::eval_points_seeded(suffix, q, got_d.data(), got_c.data(), r);
    }
    CHECK(std::memcmp(got_d.data(), want_d.data(), count * sizeof(float)) == 0);
    CHECK(std::memcmp(got_c.data(), want_c.data(), count * 3 * sizeof(float)) == 0);

    SUBCASE("and the colours vary, so the comparison has teeth") {
        bool varies = false;
        for (std::size_t i = 3; i < want_c.size(); i += 3)
            varies = varies || want_c[i] != want_c[0];
        CHECK(varies);
    }

    SUBCASE("a distance-only seed leaves the colour buffer untouched") {
        // Rather than folding every combine against black.
        std::vector<float> d = seed_d;
        std::vector<float> c(count * 3, 7.0f);
        eval::PointResults only;
        only.distances = d.data();
        only.colors_rgb = c.data();
        eval::eval_points_seeded(suffix, q, d.data(), nullptr, only);
        CHECK(std::memcmp(d.data(), want_d.data(), count * sizeof(float)) == 0);
        CHECK(c[0] == 7.0f);  // untouched
    }
}

TEST_CASE("compile_layer_prefix refuses what it cannot be sure of") {
    // Every reuse case above has its refusal mirror, for the reason the suffix
    // compiler's own refusal test states: a wrong reuse is silent, and a
    // refusal only costs the full evaluation the caller would have paid.
    scene::Tape out;

    SUBCASE("count zero: an empty prefix is the absence of one") {
        scene::Document doc = sculpt(4);
        CHECK_FALSE(scene::compile_layer_prefix(doc, 0, &out));
    }
    SUBCASE("count past the root list") {
        scene::Document doc = sculpt(4);
        const std::size_t n = doc.layers[0].sdf->roots.size();
        CHECK_FALSE(scene::compile_layer_prefix(doc, n + 1, &out));
        CHECK(scene::compile_layer_prefix(doc, n, &out));  // the whole chain is fine
    }
    SUBCASE("no layer at all") {
        scene::Document doc;
        CHECK_FALSE(scene::compile_layer_prefix(doc, 1, &out));
    }
    SUBCASE("the only SDF layer is invisible") {
        scene::Document doc = sculpt(4);
        doc.layers[0].visible = false;
        CHECK_FALSE(scene::compile_layer_prefix(doc, 1, &out));
    }
    SUBCASE("null out") {
        scene::Document doc = sculpt(4);
        CHECK_FALSE(scene::compile_layer_prefix(doc, 1, nullptr));
    }
}

TEST_CASE("a culled prefix is cut under the document pad, not the layer's own") {
    // The pad decides which items a culled compile keeps, and the value a
    // prefix produces is the seed a suffix compiled by compile_layer_suffix --
    // which culls under the DOCUMENT pad -- will be folded onto. A prefix
    // culled under compile_layer's per-layer pad would drop items the suffix's
    // world still contains: two different fields, married silently.
    //
    // The fixture makes the two pads disagree: the layer BENEATH carries wide
    // quadratic blends (document pad ~0.4), the ACTIVE layer is all hard
    // (per-layer pad 0), and the active layer's first root sits just outside
    // the cull box plus zero but inside the box plus the document pad.
    scene::Document doc;
    LayerRef below = add_layer(doc, "b");
    below.sdf->insert(dab(0.0f, 0.0f, 0.0f, 0.4f, scene::Op::Add, 0.4f));
    below.sdf->insert(dab(0.3f, 0.0f, 0.0f, 0.4f, scene::Op::Add, 0.4f));
    LayerRef active = add_layer(doc, "a");
    // Ordinal 0 -- the prefix: bound [0.65, 1.25], outside box + 0 (max 0.6)
    // and inside box + 0.4 (max 1.0).
    active.sdf->insert(dab(0.95f, 0.0f, 0.0f, 0.3f));
    // Ordinal 1 -- the suffix.
    active.sdf->insert(dab(0.0f, 0.0f, 0.0f, 0.3f));

    const math::Aabb box{cf3(-0.6f, -0.6f, -0.6f), cf3(0.6f, 0.6f, 0.6f)};
    const scene::CullRegion cull{box};

    // Sample points inside the box, close enough to its +x face that the far
    // item is the nearest surface there.
    std::vector<float> pts;
    for (int i = 0; i < 12; ++i)
        for (int j = 0; j < 5; ++j)
            for (int k = 0; k < 5; ++k) {
                pts.push_back(-0.55f + 0.1f * static_cast<float>(i));
                pts.push_back(-0.2f + 0.1f * static_cast<float>(j));
                pts.push_back(-0.2f + 0.1f * static_cast<float>(k));
            }
    const std::size_t count = pts.size() / 3;

    scene::Tape prefix;
    REQUIRE(scene::compile_layer_prefix(doc, 1, &prefix, &cull));
    const std::vector<float> seed = eval_whole(prefix, pts);

    const std::vector<scene::NodeId> appended{active.sdf->roots[1]};
    scene::Tape suffix;
    REQUIRE(scene::compile_layer_suffix(frontier_checkpoint(active.id), doc, appended, &suffix,
                                        nullptr, &cull));
    std::vector<float> got = seed;
    eval::PointQuery q;
    q.points_xyz = pts.data();
    q.count = count;
    eval::PointResults r;
    r.distances = got.data();
    eval::eval_points_seeded(suffix, q, got.data(), nullptr, r);

    // Against the ACTIVE-LAYER HALF of a whole-document compile under the same
    // region -- what the refill folds its below half against, and a compile
    // that culls under the document pad by construction.
    const std::vector<float> want =
        eval_whole(scene::compile_document_part(doc, active.id, false, &cull), pts);
    CHECK(std::memcmp(got.data(), want.data(), count * sizeof(float)) == 0);

    // Teeth: the same document with the below layer's blends HARD has a
    // document pad of zero, and the same prefix compile must then CULL the far
    // item -- so the two seeds differ. If compile_layer_prefix read the active
    // layer's own pad instead of the document's, both compiles would cull it
    // and this comparison would find nothing.
    scene::Document flat;
    LayerRef fb = add_layer(flat, "b");
    fb.sdf->insert(dab(0.0f, 0.0f, 0.0f, 0.4f));
    fb.sdf->insert(dab(0.3f, 0.0f, 0.0f, 0.4f));
    LayerRef fa = add_layer(flat, "a");
    fa.sdf->insert(dab(0.95f, 0.0f, 0.0f, 0.3f));
    fa.sdf->insert(dab(0.0f, 0.0f, 0.0f, 0.3f));
    scene::Tape flat_prefix;
    REQUIRE(scene::compile_layer_prefix(flat, 1, &flat_prefix, &cull));
    const std::vector<float> flat_seed = eval_whole(flat_prefix, pts);
    CHECK(std::memcmp(seed.data(), flat_seed.data(), count * sizeof(float)) != 0);
}
