#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "clay/scene/document.h"
#include "clay/scene/tape.h"

using namespace clay;
using namespace clay::scene;

// Appending an item to a document must not re-emit the whole tape. The prefix
// is reusable because the compiler emits items left to right and nothing it
// has written ever moves — so `param_offset` and the blob handles inside the
// prefix are still correct with the appended item's bytes after them.
//
// The whole risk of that reuse is SILENCE. A reused prefix that should not
// have been reused produces a tape that compiles, evaluates, and answers with
// the wrong field; nothing errors and nothing crashes. So these tests do not
// check that reuse "works" — they require the reused tape to be BIT-IDENTICAL
// to a full compile of the same document, and separately require the compiler
// to REFUSE every shape where the prefix would have moved.
//
// Bit-identical includes info and bounds, not just the three byte arrays: a
// tape that matches byte for byte but folded a different Lipschitz bound
// evaluates right and steps wrong, which is the subtlest way this could fail.

namespace {

// add_sdf_layer returns a reference INTO doc.layers, which reallocates when
// the next layer is added. The SdfContent is shared_ptr-held and does not
// move; the Layer does. Hold the id and the content, never the Layer.
struct LayerRef {
    LayerId id = 0;
    SdfContent* sdf = nullptr;
};

LayerRef add_layer(Document& doc, const char* name) {
    Layer& l = doc.add_sdf_layer(name);
    return LayerRef{l.id, l.sdf.get()};
}

Node dab(float x, float y, float z, float r, Op op = Op::Add, float k = 0.0f) {
    Node n;
    n.prim = Prim::sphere(r);
    n.xform.position = math::cfloat3{x, y, z};
    n.op = op;
    n.blend = Blend{k > 0.0f ? BlendProfile::Quadratic : BlendProfile::Hard, k};
    return n;
}

// memcmp is declared never-null even for a zero length, and an empty vector's
// data() is null — a document of plain prims has no blob at all — so the sizes
// gate the compares rather than being checked alongside them. UBSan is right
// about this and the guard is not defensive noise.
template <typename T>
bool same_bytes(const std::vector<T>& a, const std::vector<T>& b) {
    if (a.size() != b.size()) return false;
    return a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0;
}

void require_identical(const Tape& reused, const Tape& full) {
    REQUIRE(reused.instrs.size() == full.instrs.size());
    REQUIRE(reused.params.size() == full.params.size());
    REQUIRE(reused.blob.size() == full.blob.size());
    CHECK(same_bytes(reused.instrs, full.instrs));
    CHECK(same_bytes(reused.params, full.params));
    CHECK(same_bytes(reused.blob, full.blob));
    CHECK(reused.info.is_exact == full.info.is_exact);
    CHECK(reused.info.lipschitz == doctest::Approx(full.info.lipschitz));
    CHECK(reused.bounds.min.x == doctest::Approx(full.bounds.min.x));
    CHECK(reused.bounds.max.z == doctest::Approx(full.bounds.max.z));
}

// Compile, append, resume, and require the result to equal a full compile.
void check_append(Document& doc, LayerRef layer, std::vector<Node> add) {
    TapeCheckpoint cp;
    Tape prefix = compile_document_resumable(doc, &cp);
    std::vector<NodeId> ids;
    for (Node& n : add) ids.push_back(layer.sdf->insert(std::move(n)));

    Tape reused;
    TapeCheckpoint next;
    REQUIRE(compile_document_append(prefix, cp, doc, ids, &reused, &next));
    require_identical(reused, compile_document(doc));

    // Different bytes from the prefix, so a different identity: a backend
    // keyed on compile_id would otherwise serve the pre-append upload.
    CHECK(reused.compile_id != 0);
    CHECK(reused.compile_id != prefix.compile_id);
}

}  // namespace

TEST_CASE("prefix reuse: one layer") {
    SUBCASE("smooth append onto a hard base") {
        Document d;
        LayerRef a = add_layer(d, "a");
        a.sdf->insert(dab(0, 0, 0, 1.0f));
        a.sdf->insert(dab(0.5f, 0, 0, 0.3f, Op::Add, 0.05f));
        check_append(d, a, {dab(0, 0.5f, 0, 0.3f, Op::Add, 0.05f)});
    }
    SUBCASE("appending a subtract") {
        Document d;
        LayerRef a = add_layer(d, "a");
        a.sdf->insert(dab(0, 0, 0, 1.0f));
        check_append(d, a, {dab(0.5f, 0, 0, 0.3f, Op::Subtract, 0.08f)});
    }
    SUBCASE("several appends consumed at once") {
        Document d;
        LayerRef a = add_layer(d, "a");
        a.sdf->insert(dab(0, 0, 0, 1.0f));
        check_append(d, a,
                     {dab(0.2f, 0, 0, 0.3f, Op::Add, 0.05f),
                      dab(0, 0.2f, 0, 0.3f, Op::Subtract, 0.02f), dab(0, 0, 0.2f, 0.3f)});
    }
    SUBCASE("the first item of an empty document") {
        Document d;
        LayerRef a = add_layer(d, "a");
        check_append(d, a, {dab(0, 0, 0, 1.0f)});
    }
}

// Layers do NOT simply concatenate: each compiles against its own accumulator
// and is folded into the layers below by a hard union emitted AFTER its chain.
// So with more than one visible layer the cached tape ends in that union, and
// an appended item belongs IN FRONT of it. Emitting it after would combine it
// against every layer — invisible for a hard Add, because min is associative,
// and simply the wrong field for a smooth blend or a subtract. That is why
// these cases carry soft blends and a subtract: a hard-Add-only test would
// pass against the bug.
TEST_CASE("prefix reuse: the appended item goes in front of the layer union") {
    SUBCASE("two layers, smooth append") {
        Document d;
        LayerRef a = add_layer(d, "a");
        LayerRef b = add_layer(d, "b");
        a.sdf->insert(dab(0, 0, 0, 1.0f));
        b.sdf->insert(dab(1, 0, 0, 0.5f));
        check_append(d, b, {dab(1.0f, 0.4f, 0, 0.3f, Op::Add, 0.05f)});
    }
    SUBCASE("two layers, subtract append") {
        Document d;
        LayerRef a = add_layer(d, "a");
        LayerRef b = add_layer(d, "b");
        a.sdf->insert(dab(0, 0, 0, 1.0f));
        b.sdf->insert(dab(1, 0, 0, 0.5f));
        check_append(d, b, {dab(1.0f, 0.4f, 0, 0.3f, Op::Subtract, 0.06f)});
    }
    SUBCASE("three layers") {
        Document d;
        LayerRef a = add_layer(d, "a");
        LayerRef b = add_layer(d, "b");
        LayerRef c = add_layer(d, "c");
        a.sdf->insert(dab(0, 0, 0, 1.0f));
        b.sdf->insert(dab(1, 0, 0, 0.5f));
        c.sdf->insert(dab(2, 0, 0, 0.5f));
        check_append(d, c, {dab(2.0f, 0.4f, 0, 0.3f, Op::Add, 0.05f)});
    }
    SUBCASE("the last layer is empty") {
        Document d;
        LayerRef a = add_layer(d, "a");
        LayerRef b = add_layer(d, "b");
        a.sdf->insert(dab(0, 0, 0, 1.0f));
        check_append(d, b, {dab(1, 0, 0, 0.5f)});
    }
    SUBCASE("the last layer is hidden, so an earlier one is resumable") {
        Document d;
        LayerRef a = add_layer(d, "a");
        LayerRef b = add_layer(d, "b");
        a.sdf->insert(dab(0, 0, 0, 1.0f));
        b.sdf->insert(dab(1, 0, 0, 0.5f));
        d.find_layer(b.id)->visible = false;
        check_append(d, a, {dab(0, 0.5f, 0, 0.3f, Op::Add, 0.05f)});
    }
}

// Mirror and radial copies are emitted inside emit_item, so an appended item
// brings its own copies with it and the prefix is untouched. The layer
// transform is folded per item for the same reason.
TEST_CASE("prefix reuse: layer symmetry and transform") {
    SUBCASE("mirror with a smooth seam") {
        Document d;
        LayerRef a = add_layer(d, "a");
        Layer* l = d.find_layer(a.id);
        l->mirror_axes = 1;
        l->mirror_k = 0.04f;
        a.sdf->insert(dab(0, 0, 0, 1.0f));
        Node n = dab(0.5f, 0, 0, 0.3f, Op::Add, 0.05f);
        n.mirror = true;
        check_append(d, a, {n});
    }
    SUBCASE("radial") {
        Document d;
        LayerRef a = add_layer(d, "a");
        Layer* l = d.find_layer(a.id);
        l->radial_count = 6;
        l->radial_k = 0.03f;
        a.sdf->insert(dab(0, 0, 0, 1.0f));
        Node n = dab(0.5f, 0, 0, 0.3f, Op::Add, 0.05f);
        n.mirror = true;
        check_append(d, a, {n});
    }
    SUBCASE("mirror and radial compose, under a scaled layer") {
        Document d;
        LayerRef a = add_layer(d, "a");
        Layer* l = d.find_layer(a.id);
        l->mirror_axes = 3;
        l->mirror_k = 0.04f;
        l->radial_count = 4;
        l->radial_k = 0.02f;
        l->xform.position = math::cfloat3{0.3f, 0.0f, 0.0f};
        l->xform.scale = 1.7f;
        a.sdf->insert(dab(0, 0, 0, 1.0f));
        Node n = dab(0.5f, 0, 0, 0.3f, Op::Add, 0.05f);
        n.mirror = true;
        check_append(d, a, {n});
    }
}

TEST_CASE("prefix reuse: an appended group is still an append") {
    Document d;
    LayerRef a = add_layer(d, "a");
    a.sdf->insert(dab(0, 0, 0, 1.0f));

    SUBCASE("a group with children") {
        TapeCheckpoint cp;
        Tape prefix = compile_document_resumable(d, &cp);
        Node g;
        g.is_group = true;
        g.op = Op::Add;
        g.blend = Blend{BlendProfile::Quadratic, 0.05f};
        NodeId gid = a.sdf->insert(g);
        a.sdf->insert(dab(0.5f, 0, 0, 0.3f, Op::Add, 0.02f), gid);
        a.sdf->insert(dab(0.5f, 0.3f, 0, 0.3f, Op::Add, 0.02f), gid);
        Tape reused;
        REQUIRE(compile_document_append(prefix, cp, d, {gid}, &reused, nullptr));
        require_identical(reused, compile_document(d));
    }
    SUBCASE("an empty group, which compile_group rolls back") {
        TapeCheckpoint cp;
        Tape prefix = compile_document_resumable(d, &cp);
        Node g;
        g.is_group = true;
        g.op = Op::Add;
        NodeId gid = a.sdf->insert(g);
        Tape reused;
        REQUIRE(compile_document_append(prefix, cp, d, {gid}, &reused, nullptr));
        require_identical(reused, compile_document(d));
    }
}

// A stroke is many dabs, not one. Each has to resume from the tape the last
// one produced, or the fast path would fire once and the rest of the stroke
// would pay the full compile.
TEST_CASE("prefix reuse: a stroke resumes from itself, dab after dab") {
    Document d;
    LayerRef a = add_layer(d, "a");
    LayerRef b = add_layer(d, "b");
    a.sdf->insert(dab(0, 0, 0, 1.0f));
    b.sdf->insert(dab(1, 0, 0, 0.5f));

    TapeCheckpoint cp;
    Tape tape = compile_document_resumable(d, &cp);
    for (int i = 1; i <= 12; ++i) {
        NodeId id = b.sdf->insert(dab(1.0f + 0.05f * float(i), 0.1f * float(i), 0, 0.2f,
                                      i % 3 ? Op::Add : Op::Subtract, 0.02f * float(i % 4)));
        Tape grown;
        TapeCheckpoint next;
        REQUIRE(compile_document_append(tape, cp, d, {id}, &grown, &next));
        require_identical(grown, compile_document(d));
        tape = std::move(grown);
        cp = next;
    }
}

// The lineage a reused tape carries is a claim that it and its ancestor are
// byte-identical below three offsets. A backend patches on that claim without
// checking it, so a false one is silent: it transfers the wrong suffix and
// evaluates a field that never existed.
//
// These tests therefore check the BYTES, not that the field looks right.
TEST_CASE("prefix reuse: the lineage a tape claims is true") {
    Document d;
    LayerRef a = add_layer(d, "a");
    LayerRef b = add_layer(d, "b");
    a.sdf->insert(dab(0, 0, 0, 1.0f));
    b.sdf->insert(dab(1, 0, 0, 0.5f));

    TapeCheckpoint cp;
    Tape prefix = compile_document_resumable(d, &cp);
    NodeId id = b.sdf->insert(dab(1.0f, 0.4f, 0, 0.3f, Op::Add, 0.05f));
    Tape reused;
    REQUIRE(compile_document_append(prefix, cp, d, {id}, &reused, nullptr));

    SUBCASE("it names the tape it grew from") {
        CHECK(reused.parent_id == prefix.compile_id);
        CHECK(reused.parent_id != 0);
        CHECK(reused.compile_id != reused.parent_id);
    }
    SUBCASE("the two tapes really are identical below the offsets") {
        REQUIRE(reused.agree_instrs <= prefix.instrs.size());
        REQUIRE(reused.agree_params <= prefix.params.size());
        REQUIRE(reused.agree_blob <= prefix.blob.size());
        REQUIRE(reused.agree_instrs <= reused.instrs.size());
        REQUIRE(reused.agree_params <= reused.params.size());
        REQUIRE(reused.agree_blob <= reused.blob.size());
        CHECK(std::equal(prefix.instrs.begin(), prefix.instrs.begin() + (long)reused.agree_instrs,
                         reused.instrs.begin(),
                         [](const kernel::CTapeInstr& x, const kernel::CTapeInstr& y) {
                             return std::memcmp(&x, &y, sizeof(x)) == 0;
                         }));
        CHECK(std::equal(prefix.params.begin(), prefix.params.begin() + (long)reused.agree_params,
                         reused.params.begin()));
        CHECK(std::equal(prefix.blob.begin(), prefix.blob.begin() + (long)reused.agree_blob,
                         reused.blob.begin()));
    }
    SUBCASE("the agreement stops short of the whole tape") {
        // Otherwise the claim would be vacuous: a backend told everything
        // agrees would transfer nothing and show the pre-append field. With
        // two layers the ancestor's trailing layer union is re-emitted after
        // the appended item, so the agreement ends before the tape does.
        CHECK(reused.agree_instrs < prefix.instrs.size());
        CHECK(reused.agree_instrs < reused.instrs.size());
    }
}

TEST_CASE("prefix reuse: a tape that did not grow from another claims no lineage") {
    Document d;
    LayerRef a = add_layer(d, "a");
    a.sdf->insert(dab(0, 0, 0, 1.0f));
    a.sdf->insert(dab(0.5f, 0, 0, 0.3f, Op::Add, 0.05f));

    SUBCASE("a whole-document compile") {
        CHECK(compile_document(d).parent_id == 0);
    }
    SUBCASE("a resumable whole-document compile") {
        TapeCheckpoint cp;
        CHECK(compile_document_resumable(d, &cp).parent_id == 0);
    }
    SUBCASE("a culled per-brick compile") {
        const CullRegion cull{math::Aabb{math::cfloat3{-1, -1, -1}, math::cfloat3{1, 1, 1}}};
        CHECK(compile_document(d, &cull).parent_id == 0);
    }
    SUBCASE("a layer compile") {
        CHECK(compile_layer(*d.find_layer(a.id)).parent_id == 0);
    }
    SUBCASE("a prefix with no identity cannot be named") {
        // A hand-assembled tape carries compile_id 0, so a tape grown from
        // one has nothing to name and must not claim it did.
        TapeCheckpoint cp;
        Tape prefix = compile_document_resumable(d, &cp);
        prefix.compile_id = 0;
        NodeId id = a.sdf->insert(dab(0, 0.5f, 0, 0.3f));
        Tape reused;
        REQUIRE(compile_document_append(prefix, cp, d, {id}, &reused, nullptr));
        CHECK(reused.parent_id == 0);
    }
}

// Refusing costs a recompile, which is what the caller paid before any of this
// existed. Reusing when the prefix has moved is silent and wrong. So every
// shape that is not a tail append onto the resumable layer must be refused,
// and these are the shapes a host actually produces.
TEST_CASE("prefix reuse: everything that is not a tail append is refused") {
    Tape out;

    SUBCASE("an append to a layer that is not the last visible one") {
        Document d;
        LayerRef a = add_layer(d, "a");
        LayerRef b = add_layer(d, "b");
        a.sdf->insert(dab(0, 0, 0, 1.0f));
        b.sdf->insert(dab(1, 0, 0, 0.5f));
        TapeCheckpoint cp;
        Tape prefix = compile_document_resumable(d, &cp);
        NodeId id = a.sdf->insert(dab(0, 0.5f, 0, 0.3f));
        CHECK_FALSE(compile_document_append(prefix, cp, d, {id}, &out, nullptr));
    }
    SUBCASE("an insert at the front of the list") {
        Document d;
        LayerRef a = add_layer(d, "a");
        a.sdf->insert(dab(0, 0, 0, 1.0f));
        a.sdf->insert(dab(0.5f, 0, 0, 0.3f));
        TapeCheckpoint cp;
        Tape prefix = compile_document_resumable(d, &cp);
        NodeId id = a.sdf->insert(dab(0, 0.5f, 0, 0.3f), kNoNode, 0);
        CHECK_FALSE(compile_document_append(prefix, cp, d, {id}, &out, nullptr));
    }
    SUBCASE("a new last layer added after the checkpoint") {
        Document d;
        LayerRef a = add_layer(d, "a");
        a.sdf->insert(dab(0, 0, 0, 1.0f));
        TapeCheckpoint cp;
        Tape prefix = compile_document_resumable(d, &cp);
        NodeId id = a.sdf->insert(dab(0.5f, 0, 0, 0.3f));
        add_layer(d, "z").sdf->insert(dab(2, 0, 0, 0.5f));
        CHECK_FALSE(compile_document_append(prefix, cp, d, {id}, &out, nullptr));
    }
    SUBCASE("the resumable layer removed outright") {
        Document d;
        LayerRef a = add_layer(d, "a");
        LayerRef b = add_layer(d, "b");
        a.sdf->insert(dab(0, 0, 0, 1.0f));
        b.sdf->insert(dab(1, 0, 0, 0.5f));
        TapeCheckpoint cp;
        Tape prefix = compile_document_resumable(d, &cp);
        NodeId id = b.sdf->insert(dab(1, 0.4f, 0, 0.3f));
        d.remove_layer(b.id);
        CHECK_FALSE(compile_document_append(prefix, cp, d, {id}, &out, nullptr));
    }
    SUBCASE("an empty append list, and an invalid checkpoint") {
        Document d;
        LayerRef a = add_layer(d, "a");
        a.sdf->insert(dab(0, 0, 0, 1.0f));
        TapeCheckpoint cp;
        Tape prefix = compile_document_resumable(d, &cp);
        CHECK_FALSE(compile_document_append(prefix, cp, d, {}, &out, nullptr));
        NodeId id = a.sdf->insert(dab(0.5f, 0, 0, 0.3f));
        CHECK_FALSE(compile_document_append(prefix, TapeCheckpoint{}, d, {id}, &out, nullptr));
    }
    SUBCASE("ids that are not the ones at the tail") {
        Document d;
        LayerRef a = add_layer(d, "a");
        NodeId first = a.sdf->insert(dab(0, 0, 0, 1.0f));
        TapeCheckpoint cp;
        Tape prefix = compile_document_resumable(d, &cp);
        a.sdf->insert(dab(0.5f, 0, 0, 0.3f));
        // `first` is at the HEAD, so claiming it was appended is a lie the
        // compiler can catch for itself in O(appended).
        CHECK_FALSE(compile_document_append(prefix, cp, d, {first}, &out, nullptr));
    }
}
