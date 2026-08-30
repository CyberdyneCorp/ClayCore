#include <doctest/doctest.h>

#include <vector>

#include "clay/eval/backend.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"

using namespace clay;
using namespace clay::scene;

// A group resume carries a STACK -- the open chains the checkpoint stopped
// inside -- and the producer and the consumer of that stack have to agree how
// many planes it has. They disagreed once, and the way they disagreed is worth
// pinning: `frames.size() + 1` reads as the obvious count and is wrong.
//
// A group emits a combine only when there is something for it to combine WITH,
// which `TapeCheckpointFrame::emits` records. A group that is the first thing
// in its layer has nothing beneath it, emits nothing, and so opens no outer
// stack plane -- the walk reaches the checkpoint one plane shallower than the
// frame count suggests.
//
// The failure was SILENT and cost only speed: the refill sized a buffer for
// one count, the walk reported another, the shapes did not match, and the
// stack was dropped. Every dab then took the full walk the resume exists to
// avoid, and the answers stayed exactly right the whole time. So this checks
// the two counts against each other directly rather than checking a field.

namespace {

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

Node group_node(Op op = Op::Add, float k = 0.0f) {
    Node g;
    g.is_group = true;
    g.op = op;
    g.blend = Blend{k > 0.0f ? BlendProfile::Quadratic : BlendProfile::Hard, k};
    return g;
}

// The count the checkpoint claims, against the depth the walk actually holds
// when it reaches that instruction. `snapshot_at` is what a refill passes.
void require_levels_agree(const Document& doc, const TapeCheckpoint& cp, const Tape& tape) {
    REQUIRE(cp.valid);
    const std::vector<math::cfloat3> pts = {
        math::cfloat3{0.0f, 0.0f, 0.0f},  math::cfloat3{0.2f, 0.1f, 0.0f},
        math::cfloat3{-0.3f, 0.2f, 0.1f}, math::cfloat3{0.5f, -0.4f, 0.2f},
    };
    std::vector<float> xyz;
    for (const math::cfloat3& p : pts) {
        xyz.push_back(p.x);
        xyz.push_back(p.y);
        xyz.push_back(p.z);
    }
    eval::PointQuery q;
    q.points_xyz = xyz.data();
    q.count = pts.size();

    // Room for the deepest the tape can reach, so a disagreement is REPORTED
    // rather than written past the end of a buffer sized by the wrong count.
    const std::size_t room = eval::tape_stack_depth(tape);
    std::vector<float> stack(q.count * (room + 2), 0.0f);
    std::vector<float> field(q.count, 0.0f);
    eval::PointResults out;
    out.distances = field.data();

    std::size_t levels = 0;
    eval::eval_points_stack(tape, q, stack.data(), nullptr, &levels, cp.instrs, 0, &out);
    CHECK(levels == checkpoint_stack_levels(cp.frames));

    // The same walk's field is the tape's field: asking for both is what makes
    // one walk enough, so a caller that stopped trusting it would walk twice.
    std::vector<float> alone(q.count, 0.0f);
    eval::PointResults solo;
    solo.distances = alone.data();
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    REQUIRE(cpu != nullptr);
    REQUIRE(cpu->eval_points(tape, q, solo) == eval::Status::Ok);
    for (std::size_t i = 0; i < q.count; ++i) CHECK(field[i] == alone[i]);
    (void)doc;
}

}  // namespace

TEST_CASE("checkpoint stack levels match the walk's depth") {
    SUBCASE("a group that is the first thing in its layer opens no outer plane") {
        Document d;
        LayerRef a = add_layer(d, "a");
        const NodeId g = a.sdf->insert(group_node(Op::Add, 0.05f));
        a.sdf->insert(dab(0.0f, 0.0f, 0.0f, 0.3f, Op::Add, 0.05f), g);
        a.sdf->insert(dab(0.1f, 0.0f, 0.0f, 0.3f, Op::Add, 0.05f), g);

        TapeCheckpoint cp;
        const Tape t = compile_document_resumable(d, &cp);
        REQUIRE(cp.frames.size() == 1);
        // The heart of it: one frame, and it emits nothing.
        CHECK_FALSE(cp.frames[0].emits);
        CHECK(checkpoint_stack_levels(cp.frames) == 1);
        CHECK(checkpoint_stack_levels(cp.frames) != cp.frames.size() + 1);
        require_levels_agree(d, cp, t);
    }

    SUBCASE("a group standing on something beneath it opens one") {
        Document d;
        LayerRef a = add_layer(d, "a");
        a.sdf->insert(dab(-0.5f, 0.0f, 0.0f, 0.3f, Op::Add, 0.05f));
        const NodeId g = a.sdf->insert(group_node(Op::Add, 0.05f));
        a.sdf->insert(dab(0.0f, 0.0f, 0.0f, 0.3f, Op::Add, 0.05f), g);

        TapeCheckpoint cp;
        const Tape t = compile_document_resumable(d, &cp);
        REQUIRE(cp.frames.size() == 1);
        CHECK(cp.frames[0].emits);
        CHECK(checkpoint_stack_levels(cp.frames) == 2);
        require_levels_agree(d, cp, t);
    }

    SUBCASE("nested all-Add groups with nothing beneath stay one plane deep") {
        Document d;
        LayerRef a = add_layer(d, "a");
        NodeId parent = kNoNode;
        for (int i = 0; i < 4; ++i)
            parent = parent == kNoNode ? a.sdf->insert(group_node(Op::Add, 0.05f))
                                       : a.sdf->insert(group_node(Op::Add, 0.05f), parent);
        a.sdf->insert(dab(0.0f, 0.0f, 0.0f, 0.3f, Op::Add, 0.05f), parent);

        TapeCheckpoint cp;
        const Tape t = compile_document_resumable(d, &cp);
        REQUIRE(cp.frames.size() == 4);
        // Four frames, one plane: the count the obvious formula would give is
        // five, and sizing a buffer by it drops every stack the refill takes.
        CHECK(checkpoint_stack_levels(cp.frames) == 1);
        require_levels_agree(d, cp, t);
    }

    SUBCASE("a carving group beneath nothing is seeded, and does open a plane") {
        Document d;
        LayerRef a = add_layer(d, "a");
        const NodeId g = a.sdf->insert(group_node(Op::Subtract, 0.05f));
        a.sdf->insert(dab(0.0f, 0.0f, 0.0f, 0.3f, Op::Add, 0.05f), g);

        TapeCheckpoint cp;
        const Tape t = compile_document_resumable(d, &cp);
        if (cp.valid && cp.frames.size() == 1) {
            // Seeded against empty space: the empty IS the outer plane, so
            // `emits` is true here for a reason that is not `outer_have_acc`.
            CHECK(cp.frames[0].seeded);
            CHECK(cp.frames[0].emits);
            CHECK(checkpoint_stack_levels(cp.frames) == 2);
            require_levels_agree(d, cp, t);
        }
    }
}
