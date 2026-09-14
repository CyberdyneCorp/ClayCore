#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "clay.h"

// WHETHER DECIMATION PINCHED THE EXPORT, read by a host on the C boundary
// (c-abi spec: a decimation reports itself across the ABI).
//
// What this is a regression for: mesh::DecimateReport was C++ ONLY. A host
// reaching claycore through the C ABI set clay_mesh_params.decimate, was handed
// a mesh, and had no way to be told the simplification had pinched it — it
// could only re-validate the result itself. That is a different answer. The
// validator says the mesh in hand carries an edge with four incident triangles;
// it cannot say whether decimation made it, because the input mesh is gone by
// then, nor whether the retry ladder ran.
//
// And the distinction is not academic on this fixture. Issue #575: a unit
// sphere, nothing sculpted, meshed at voxel 0.02 with CLAY_MESHER_MARCHING,
// comes back pinched at six of the sixteen ratios swept below — including 0.50,
// which is what an export panel puts in the slider by default — from an input
// with no non-manifold edge at all. Every one of those is decimation's doing
// and the report is the only thing that says so.

namespace {

struct Owned {
    clay_mesh* m = nullptr;
    ~Owned() { clay_mesh_destroy(m); }
    Owned() = default;
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
    operator clay_mesh*() const { return m; }
};

struct Doc {
    clay_document* d = nullptr;
    clay_layer_id layer = 0;
    Doc() {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "form", &layer) == CLAY_OK);
    }
    ~Doc() { clay_document_destroy(d); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
    operator clay_document*() const { return d; }

    void add(clay_prim prim, const std::vector<float>& params) {
        clay_item* it = clay_item_create(prim, params.data(), params.size());
        REQUIRE(it != nullptr);
        clay_node_id id = 0;
        const clay_result r = clay_layer_add_item(d, layer, it, &id);
        clay_item_destroy(it);
        REQUIRE(r == CLAY_OK);
    }
};

clay_validation_report validation_of(const clay_mesh* m) {
    clay_validation_report v{};
    v.struct_size = sizeof v;
    REQUIRE(clay_mesh_validation_report(m, 0, &v) == CLAY_OK);
    return v;
}

clay_decimate_report decimation_of(const clay_mesh* m) {
    clay_decimate_report d{};
    d.struct_size = sizeof d;
    REQUIRE(clay_mesh_decimate_report(m, &d) == CLAY_OK);
    return d;
}

clay_mesh_params mesh_params(float voxel, clay_mesher mesher, float ratio) {
    clay_mesh_params p{};
    p.struct_size = sizeof p;
    p.voxel_size = voxel;
    p.mesher = mesher;
    p.decimate = ratio > 0.0f ? 1 : 0;
    p.decimate_ratio = ratio;
    return p;
}

}  // namespace

TEST_CASE("c abi: a host reads the pinched sphere export of issue #575") {
    // THE REPORTED CASE, driven the way the host drove it. One layer, one node,
    // a unit sphere, nothing sculpted; clay_document_mesh at voxel 0.02 with
    // marching and decimate on.
    Doc doc;
    doc.add(CLAY_PRIM_SPHERE, {1.0f});

    // The undecimated mesh is clean, which is what makes a pinch below
    // decimation's doing rather than the mesher's. Marching is 2-manifold by
    // construction and this asserts it rather than assuming it, because the
    // whole attribution claim rests on it.
    {
        clay_mesh_params p = mesh_params(0.02f, CLAY_MESHER_MARCHING, 0.0f);
        Owned raw;
        REQUIRE(clay_document_mesh(doc, &p, &raw.m) == CLAY_OK);
        const clay_validation_report v = validation_of(raw);
        REQUIRE(v.triangles > 200000);  // the pinch needs the density it was found at
        REQUIRE(v.non_manifold_edges == 0);
        REQUIRE(v.watertight);
    }

    int pinched = 0;
    std::string sweep;
    for (int percent = 20; percent <= 95; percent += 5) {
        CAPTURE(percent);
        clay_mesh_params p =
            mesh_params(0.02f, CLAY_MESHER_MARCHING, static_cast<float>(percent) / 100.0f);
        Owned m;
        REQUIRE(clay_document_mesh(doc, &p, &m.m) == CLAY_OK);

        const clay_validation_report v = validation_of(m);
        const clay_decimate_report d = decimation_of(m);
        CAPTURE(v.non_manifold_edges);
        CAPTURE(d.manifold);
        CAPTURE(d.attempts);

        // THE REPORT NEVER LIES, and that is the part that holds on every
        // toolchain. WHICH ratios pinch does not: the collapse order is decided
        // by float comparisons inside meshoptimizer's own queue, and the mesh
        // reaching it moves with FP contraction — the same sphere at the same
        // voxel is 281,568 triangles with `-ffp-contract=on` and 281,544 with
        // it off, and the bad bands are 31 of 76 ratios on one and 0 of 76 on
        // the other (issue #575). A case pinning ratio 0.5 as non-manifold was
        // written against this fixture and failed on every Linux job while
        // passing on every macOS one. So this asserts the agreement, not the
        // band.
        CHECK(d.manifold == (v.non_manifold_edges == 0 ? 1 : 0));
        // The input was clean — asserted above — so every pinch here is
        // attributed to decimation and none to the mesher. This is the field a
        // host CANNOT recover by validating the result it was handed.
        CHECK(d.input_manifold == 1);
        // One simplification, plus at most the two-flag retry ladder.
        CHECK(d.attempts >= 1);
        CHECK(d.attempts <= 3);

        if (v.non_manifold_edges != 0) {
            ++pinched;
            // The ladder was exhausted before the pinch was returned: a report
            // saying "not manifold" after a single attempt would mean the
            // retries silently stopped running.
            CHECK(d.attempts == 3);
        }
        sweep += std::to_string(percent) + ":" + (d.manifold ? "clean " : "PINCHED ");
    }
    // Not an assertion. On AppleClang this fixture pinches at 0.45, 0.50, 0.55,
    // 0.65, 0.70 and 0.75 and is clean at the other ten; on a toolchain that
    // contracts differently it pinches nowhere. Both are the reported defect,
    // and neither is something this case can require.
    MESSAGE("issue #575 sweep: " << sweep << "(" << pinched << " of 16 pinched)");
}

TEST_CASE("c abi: a pinch decimation was handed is not attributed to decimation") {
    // THE DETERMINISTIC HALF. Whether a marching mesh pinches UNDER
    // simplification is a toolchain-dependent float question, so the case above
    // cannot be relied on to exercise the reporting path everywhere. This one
    // builds the condition out of the mesher instead of out of the collapse
    // order, and therefore fires on every platform.
    //
    // A torus whose minor radius is smaller than the voxel, meshed with
    // CLAY_MESHER_NETS. Surface nets places one vertex per cell and the header
    // says plainly that it is NOT manifold; a ring thinner than a cell is the
    // configuration where that bites. Measured here: 624 triangles carrying 24
    // non-manifold edges, and no boundary edge.
    Doc doc;
    doc.add(CLAY_PRIM_TORUS, {0.6f, 0.03f});

    clay_mesh_params raw_params = mesh_params(0.05f, CLAY_MESHER_NETS, 0.0f);
    Owned raw;
    REQUIRE(clay_document_mesh(doc, &raw_params, &raw.m) == CLAY_OK);
    const clay_validation_report in = validation_of(raw);
    CAPTURE(in.triangles);
    REQUIRE(in.non_manifold_edges > 0);  // the fixture is the point

    // Ratio 1.0 so the simplifier has nothing to take away: the result keeps
    // the pinch it was handed, which is what makes the report's verdict a fact
    // about the fixture rather than another bet on which collapses fire.
    clay_mesh_params p = mesh_params(0.05f, CLAY_MESHER_NETS, 1.0f);
    Owned m;
    REQUIRE(clay_document_mesh(doc, &p, &m.m) == CLAY_OK);
    const clay_validation_report v = validation_of(m);
    REQUIRE(v.non_manifold_edges > 0);

    const clay_decimate_report d = decimation_of(m);
    CHECK(d.manifold == 0);
    // THE FIELD THAT CANNOT BE RECOVERED DOWNSTREAM. Both meshes validate as
    // non-manifold and a host holding only the result cannot tell which of them
    // broke it. This says the mesher did.
    CHECK(d.input_manifold == 0);
    // And decimation does not spend the ladder trying to clean up something it
    // did not break — an input that arrives pinched is simplified once.
    CHECK(d.attempts == 1);
}

TEST_CASE("c abi: a mesh nobody decimated has no decimation to report") {
    // A clean report and no report at all are different facts. Answering the
    // second with the first would tell a host its export survived decimation
    // when no decimation ran, which is the failure clay_mesh_quad_report
    // refuses for the same reason.
    clay_decimate_report d{};
    d.struct_size = sizeof d;

    SUBCASE("meshed without decimate") {
        Doc doc;
        doc.add(CLAY_PRIM_SPHERE, {0.5f});
        clay_mesh_params p = mesh_params(0.05f, CLAY_MESHER_MARCHING, 0.0f);
        Owned m;
        REQUIRE(clay_document_mesh(doc, &p, &m.m) == CLAY_OK);
        CHECK(clay_mesh_decimate_report(m, &d) == CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("built from triangles") {
        const float positions[12] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                     0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
        const std::uint32_t indices[12] = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
        Owned m;
        REQUIRE(clay_mesh_from_triangles(positions, 4, indices, 12, &m.m) == CLAY_OK);
        CHECK(clay_mesh_decimate_report(m, &d) == CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("null arguments") {
        Doc doc;
        doc.add(CLAY_PRIM_SPHERE, {0.5f});
        clay_mesh_params p = mesh_params(0.05f, CLAY_MESHER_MARCHING, 0.5f);
        Owned m;
        REQUIRE(clay_document_mesh(doc, &p, &m.m) == CLAY_OK);
        CHECK(clay_mesh_decimate_report(m, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_mesh_decimate_report(nullptr, &d) == CLAY_ERROR_INVALID_ARGUMENT);
        // And a struct_size that names no layout is refused rather than read.
        clay_decimate_report short_desc{};
        short_desc.struct_size = sizeof(std::uint32_t);
        CHECK(clay_mesh_decimate_report(m, &short_desc) == CLAY_ERROR_INVALID_ARGUMENT);
    }
}

TEST_CASE("c abi: a rigid motion carries the decimation report with the mesh") {
    // A transform copies the mesh and moves its vertices; it rewrites no index,
    // so no edge changes incidence and the verdict is still true of the result.
    // Dropping it would make a host that frames or re-orients an export before
    // writing it lose the only statement it had about the export's topology.
    Doc doc;
    doc.add(CLAY_PRIM_TORUS, {0.6f, 0.03f});
    clay_mesh_params p = mesh_params(0.05f, CLAY_MESHER_NETS, 1.0f);
    Owned m;
    REQUIRE(clay_document_mesh(doc, &p, &m.m) == CLAY_OK);
    const clay_decimate_report before = decimation_of(m);

    const float position[3] = {1.0f, 2.0f, 3.0f};
    const float axis[3] = {0.0f, 1.0f, 0.0f};
    Owned moved;
    REQUIRE(clay_mesh_transform(m, position, axis, 0.7f, 2.0f, &moved.m) == CLAY_OK);
    const clay_decimate_report after = decimation_of(moved);
    CHECK(after.manifold == before.manifold);
    CHECK(after.input_manifold == before.input_manifold);
    CHECK(after.attempts == before.attempts);

    const float squash[3] = {2.0f, 0.5f, 1.0f};
    Owned squashed;
    REQUIRE(clay_mesh_transform_nonuniform(m, position, axis, 0.0f, squash, &squashed.m) ==
            CLAY_OK);
    const clay_decimate_report nonuniform = decimation_of(squashed);
    CHECK(nonuniform.manifold == before.manifold);
    CHECK(nonuniform.input_manifold == before.input_manifold);
    CHECK(nonuniform.attempts == before.attempts);
}

TEST_CASE("c abi: a weld forgets the decimation report rather than answering a stale one") {
    // A decimation report describes A PARTICULAR SET OF TRIANGLES. `clay_mesh_weld`
    // rewrites the indices of the SAME handle, so a report that survives it is not
    // stale, it is FALSE — and the whole point of the field is attribution.
    //
    // Found by an adversarial review of the PR that added the report, with a probe
    // on this shape: the report kept answering `manifold = 1` while the validator
    // found the welded mesh non-manifold. An epsilon of a fifteenth of the voxel is
    // an ordinary cleanup weld, not a contrived one.
    Doc doc;
    doc.add(CLAY_PRIM_TORUS, {0.6f, 0.2f});

    Owned m;
    const clay_mesh_params p = mesh_params(0.03f, CLAY_MESHER_MARCHING, 0.5f);
    REQUIRE(clay_document_mesh(doc, &p, &m.m) == CLAY_OK);

    // The report exists before the weld, which is what makes its absence afterwards
    // a statement rather than the default.
    const clay_decimate_report before = decimation_of(m);
    CHECK(before.attempts >= 1);

    clay_weld_desc w{};
    w.struct_size = sizeof w;
    REQUIRE(clay_mesh_weld_defaults(&w) == CLAY_OK);
    w.epsilon = 0.002f;  // a fifteenth of the voxel
    REQUIRE(clay_mesh_weld(m, &w, nullptr) == CLAY_OK);

    // Forgotten, not recomputed: a recomputed answer would be to a question the
    // caller never asked, and the reader already distinguishes "not decimated"
    // from a clean verdict.
    clay_decimate_report after{};
    after.struct_size = sizeof after;
    CHECK(clay_mesh_decimate_report(m, &after) != CLAY_OK);
}
