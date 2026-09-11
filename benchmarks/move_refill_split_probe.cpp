// How much of a live Move drag's per-event cost is RECOMPUTED FROM UNCHANGED
// INPUTS. NOT a gated benchmark.
//
// THE QUESTION, and it is not the obvious one. Two independent measurements put
// the cost of a Move drag frame in the same place:
//
//   benchmarks/move_preview_probe.cpp (this repo, Mac, 200 stamps, voxel 0.05)
//       ~14.2 ms per pointer event, and the draw-then-undo pattern versus
//       clay_sdf_move_preview_document is 1.02x -- so the PATTERN is noise and
//       the refill is everything.
//
//   ClaySpaceDesktop, same drag and radius, varying only the form underneath
//       (macOS aarch64, Release, against ClayCore 0.84.0):
//
//           stamps   ms/event   bricks/event
//                0       1.97    294 -> 540
//               50       9.58    294 -> 540
//              200      22.62    294 -> 540
//              400      40.70    294 -> 540
//
//       The BRICK COUNTS ARE IDENTICAL down the column. Every scene refills the
//       same bricks over the same region, because the dirty region is the swept
//       ball of the drag and nothing about scene content touches it. What moves
//       is the cost of ONE BRICK: ~0.005 ms empty, ~0.10 ms at 400 stamps. A
//       factor of twenty for a region that did not move.
//
// So the per-event cost is not the region and not the pattern. It is per-brick
// cost scaling with scene size. The useful split is therefore NOT "compile
// versus evaluate" for its own sake -- it is:
//
//       during a drag, only the dragged layer changes between pointer events.
//       How much of what each event pays is rebuilt from inputs that did not?
//
// Every brick request compiles its own culled tape (compile_request_tape,
// bindings/c/clay_c.cpp), and that compile walks the whole document: a cost
// model fitted by a separate agent puts the cull walk at ~0.067 us per item
// PAID HOWEVER TIGHT THE BOX. At 400 items that floor is ~0.027 ms per brick
// before a single sample is evaluated. If that share is large, it is a cache
// with an obvious invalidation key and hoisting it removes a constant from
// every event rather than shaving a percentage off one.
//
// THE PREDICTION THIS TESTS, stated before the measurement so it can be wrong:
// if the compile share RISES with item count, the hoist is worth most on
// exactly the scenes where the problem is. If it FALLS, the per-brick cost is
// evaluation scaling with tape length, there is nothing to hoist, and the
// honest answer is that a Move drag over a dense form is expensive because the
// field under it is expensive -- which would close this line of work.
//
// THE DESIGN. Replicates compile_request_tape exactly -- same CullRegion (the
// brick box dilated by the band), same CullIndex and CullPlan sharing that the
// batch path uses -- and times the compile apart from the grid evaluation of
// the same brick, through the same CPU backend the refill uses. Reported at
// four scene sizes so the SHAPE is visible rather than one ratio.
//
// Exits non-zero if the split cannot be trusted: if the two halves do not add
// up to the measured whole, or if the tapes compiled at different scene sizes
// do not actually differ in length (which would mean the fixture never grew).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "clay/eval/backend.h"
#include "clay/math/geom.h"
#include "clay/scene/cull_index.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"

using namespace clay;
using kernel::cf3;
using kernel::cfloat3;

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

constexpr int kDim = 8;          // lattice samples per brick axis
constexpr float kVoxel = 0.05f;  // world units between samples
constexpr int kBandVoxels = 2;
constexpr float kDragRadius = 0.40f;

// The form: `stamps` blended spheres along an arc, plus ONE grab standing for
// the drag in progress. Deliberately one grab and not many: the accumulation
// question is settled elsewhere, and stacking warps here would confound scene
// size with chain depth, which is the variable this probe is holding still.
// `spread` moves the same items APART into a volume instead of packing them
// along one arc, keeping the count, the chain and the blend identical. This is
// the control that decides whether a per-brick tape carrying nearly the whole
// document is a CULL THAT FAILED or a CULL THAT CORRECTLY KEPT material which
// really is in range. Proposed by ClaySpaceDesktop, who refuted the cull
// hypothesis on their own fixture with an angular sweep -- 385 ms/event at 0
// degrees against 1.86 at 150, with the drag subtending ~25 degrees, so their
// region test turns off almost exactly where the drag stops reaching.
scene::Document form(int stamps, bool spread) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("form");
    for (int i = 0; i < stamps; ++i) {
        const float t = stamps > 1 ? static_cast<float>(i) / static_cast<float>(stamps - 1) : 0.0f;
        const float angle = t * 3.14159265f;
        scene::Node n;
        n.id = l.sdf->reserve_id();
        n.prim = scene::Prim::sphere(0.22f);
        // Packed: along one arc, so neighbours overlap and everything is near
        // everything. Spread: the same count over a wide lattice, so a brick in
        // the drag region has only a few items genuinely in range.
        if (spread) {
            const int side = 8;  // 8^3 = 512 cells, enough for 400
            const int ix = i % side, iy = (i / side) % side, iz = (i / (side * side)) % side;
            n.xform.position = cf3((ix - 3.5f) * 0.9f, (iy - 3.5f) * 0.9f, (iz - 3.5f) * 0.9f);
        } else {
            n.xform.position = cf3(std::cos(angle) * 1.2f, std::sin(angle) * 0.6f, 0.0f);
        }
        n.op = scene::Op::Add;
        n.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.05f};
        if (i == stamps / 2)
            n.deformers.push_back(
                scene::Deformer::grab(cf3(0.0f, 0.6f, 0.35f), kDragRadius, cf3(0.2f, 0.1f, 0.0f)));
        l.sdf->insert(n);
    }
    return doc;
}

// The bricks a drag's swept ball dirties, as brick boxes in world space.
std::vector<math::Aabb> swept_bricks() {
    const float span = static_cast<float>(kDim) * kVoxel;  // one brick's world width
    std::vector<math::Aabb> out;
    // A ball of kDragRadius at the anchor, swept by the displacement: the same
    // conservative region clay_sdf_move_update reports as its dirty bounds.
    const cfloat3 lo = cf3(-0.45f, 0.15f, -0.10f);
    const cfloat3 hi = cf3(0.45f, 1.05f, 0.80f);
    for (float x = lo.x; x < hi.x; x += span)
        for (float y = lo.y; y < hi.y; y += span)
            for (float z = lo.z; z < hi.z; z += span)
                out.push_back(math::Aabb{cf3(x, y, z), cf3(x + span, y + span, z + span)});
    return out;
}

struct Split {
    int stamps = 0;
    double compile_ms = 0.0;   // per brick, median
    double eval_ms = 0.0;      // per brick, median
    double whole_ms = 0.0;     // per brick, compile+eval timed together
    std::size_t instrs = 0;    // longest per-brick tape, to prove the fixture grew
    std::size_t instrs_min = 0;
    std::size_t instrs_med = 0;
    std::size_t bricks = 0;
};

Split measure(int stamps, int repeats, bool spread) {
    Split s;
    s.stamps = stamps;
    const scene::Document doc = form(stamps, spread);
    const std::vector<math::Aabb> boxes = swept_bricks();
    s.bricks = boxes.size();

    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    if (!cpu) return s;

    const float band = static_cast<float>(kBandVoxels) * kVoxel;

    // The index and plan are built ONCE per batch on the real path, so building
    // them per brick here would charge the compile something the engine does
    // not pay. Same sharing as eval_requests_in_chunks.
    scene::CullIndex index(doc);
    math::Aabb batch;
    for (const math::Aabb& b : boxes) batch.expand(b.dilated(band));
    const scene::CullPlan plan = index.plan(batch);

    std::vector<double> compiles, evals, wholes;
    std::vector<std::size_t> lengths;
    std::vector<float> values(static_cast<std::size_t>(kDim) * kDim * kDim, 0.0f);

    for (int r = 0; r < repeats; ++r) {
        for (const math::Aabb& box : boxes) {
            const scene::CullRegion cull{box.dilated(band)};

            eval::GridQuery q;
            q.origin = box.min;
            q.spacing = kVoxel;
            q.nx = kDim;
            q.ny = kDim;
            q.nz = kDim;

            // compile alone
            const Clock::time_point t0 = Clock::now();
            scene::Tape tape = scene::compile_document(doc, &cull, &index, &plan);
            compiles.push_back(ms_since(t0));
            if (r == 0) lengths.push_back(tape.instrs.size());

            // evaluate alone, against the tape just built
            const Clock::time_point t1 = Clock::now();
            cpu->eval_grid(tape, q, values.data(), nullptr);
            evals.push_back(ms_since(t1));

            // and both together, which is what the refill actually pays
            const Clock::time_point t2 = Clock::now();
            scene::Tape t = scene::compile_document(doc, &cull, &index, &plan);
            cpu->eval_grid(t, q, values.data(), nullptr);
            wholes.push_back(ms_since(t2));
        }
    }
    if (!lengths.empty()) {
        std::sort(lengths.begin(), lengths.end());
        s.instrs_min = lengths.front();
        s.instrs_med = lengths[lengths.size() / 2];
        s.instrs = lengths.back();
    }
    s.compile_ms = median(compiles);
    s.eval_ms = median(evals);
    s.whole_ms = median(wholes);
    return s;
}

// -- DOES BATCH SIZE MOVE THE COARSE CULL? ----------------------------------
//
// ClaySpaceDesktop measured a 4x cost from 400 items spread far from the
// pointer that a tight far cluster does not pay, and I suggested the mechanism
// was CullIndex::plan testing against the UNION of every brick box in the
// batch rather than against one brick. They tested the cheap consequence --
// shrink the batch, shrink the union -- by varying take_dirty(512) 64-fold,
// and the delta did not move (5.85 / 4.26 / 4.17 / 4.75 ms at 512 / 128 / 32 /
// 8). Two readings survived that, and they could not separate them from the
// host:
//
//   1. the batch union is not the mechanism; or
//   2. it is, but take_dirty hands bricks back in an order that is not
//      spatial, so a batch of 8 spans nearly the same box as a batch of 512.
//
// READING 2 IS ALREADY REFUTED BY THE SOURCE, which is worth saying before
// measuring anything: BrickCache::mark_dirty fills dirty_ with a z->y->x
// nested loop (src/brick/cache.cpp:57), take_dirty preserves that order, and
// the C paging hands out a CONTIGUOUS SLICE of it (clay_c.cpp). So a batch of
// 8 is 8 x-adjacent bricks -- a thin strip, not a spanning sample. The union
// really does shrink.
//
// Which leaves reading 1, and this measures it directly: the union's volume
// against what a brick actually costs, at four batch sizes on their fixture
// shape. If the volume collapses while the cost does not, the union is not
// what retains the far items and my mechanism was wrong.
void batch_union_probe() {
    std::printf("\n  BATCH UNION -- does shrinking the batch shrink what the coarse cull keeps?\n");
    // Their fixture: 400 items spread over a wide cube, nowhere near the drag.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("far");
    for (int i = 0; i < 400; ++i) {
        const int side = 8;
        const int ix = i % side, iy = (i / side) % side, iz = (i / (side * side)) % side;
        scene::Node n;
        n.id = l.sdf->reserve_id();
        n.prim = scene::Prim::sphere(0.22f);
        // Centred far from the drag region (which sits around y = 0.6, |x| < 0.5).
        n.xform.position = cf3((ix - 3.5f) * 2.0f, (iy - 3.5f) * 2.0f - 14.0f, (iz - 3.5f) * 2.0f);
        n.op = scene::Op::Add;
        n.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.05f};
        l.sdf->insert(n);
    }
    // A brick set the size a real drag dirties. swept_bricks() gives 27, which
    // cannot discriminate: at batch 32 and above it collapses to ONE plan and
    // every row prints the same number. ClaySpaceDesktop's fixture reports
    // 294-540 bricks per event, so this generates ~600 in the SAME raster
    // order mark_dirty produces (z outer, x inner), which is what makes a
    // contiguous slice of it a spatially compact strip.
    std::vector<math::Aabb> boxes;
    {
        const float span = static_cast<float>(kDim) * kVoxel;
        for (int z = 0; z < 8; ++z)
            for (int y = 0; y < 9; ++y)
                for (int x = 0; x < 9; ++x)
                    boxes.push_back(math::Aabb{cf3(-1.8f + x * span, 0.15f + y * span,
                                                   -1.6f + z * span),
                                               cf3(-1.8f + (x + 1) * span, 0.15f + (y + 1) * span,
                                                   -1.6f + (z + 1) * span)});
    }
    const float band = static_cast<float>(kBandVoxels) * kVoxel;
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    if (!cpu) return;
    const scene::CullIndex index(doc);

    std::printf("  %zu bricks, raster order\n  batch   union volume   per-brick whole ms\n", boxes.size());
    std::vector<float> values(static_cast<std::size_t>(kDim) * kDim * kDim, 0.0f);
    for (const std::size_t batch : {std::size_t(8), std::size_t(32), std::size_t(128),
                                    std::size_t(512)}) {
        double volume_sum = 0.0;
        int plans = 0;
        std::vector<double> per_brick;
        {   // warm: one untimed pass, so batch 8 does not pay first-touch for all
            const scene::CullRegion c0{boxes[0].dilated(band)};
            const scene::CullPlan p0 = index.plan(boxes[0].dilated(band));
            eval::GridQuery q0;
            q0.origin = boxes[0].min; q0.spacing = kVoxel; q0.nx = q0.ny = q0.nz = kDim;
            scene::Tape t0 = scene::compile_document(doc, &c0, &index, &p0);
            cpu->eval_grid(t0, q0, values.data(), nullptr);
        }
        for (std::size_t base = 0; base < boxes.size(); base += batch) {
            const std::size_t n = std::min(batch, boxes.size() - base);
            math::Aabb region;
            for (std::size_t i = base; i < base + n; ++i) region.expand(boxes[i].dilated(band));
            const cfloat3 e = region.extent();
            volume_sum += static_cast<double>(e.x) * e.y * e.z;
            ++plans;
            const scene::CullPlan plan = index.plan(region);
            for (std::size_t i = base; i < base + n; ++i) {
                const scene::CullRegion cull{boxes[i].dilated(band)};
                eval::GridQuery q;
                q.origin = boxes[i].min;
                q.spacing = kVoxel;
                q.nx = q.ny = q.nz = kDim;
                const Clock::time_point t0 = Clock::now();
                scene::Tape t = scene::compile_document(doc, &cull, &index, &plan);
                cpu->eval_grid(t, q, values.data(), nullptr);
                per_brick.push_back(ms_since(t0));
            }
        }
        std::printf("  %5zu   %12.4f   %8.5f   (%d plans)\n", batch,
                    volume_sum / (plans ? plans : 1), median(per_brick), plans);
    }
}

}  // namespace

int main() {
    std::printf("move_refill_split_probe: how much of a drag frame is rebuilt from\n"
                "unchanged inputs (brick dim %d, voxel %.3f, band %d, drag radius %.2f)\n\n",
                kDim, static_cast<double>(kVoxel), kBandVoxels, static_cast<double>(kDragRadius));

    const int sizes[] = {0, 50, 200, 400};
    std::vector<Split> rows;
    for (const int n : sizes) {
        // Fewer repeats on the big scenes: the timed unit grows with them, and
        // the point is the SHAPE across sizes, not a tight CI on any one row.
        rows.push_back(measure(n, n >= 200 ? 3 : 8, /*spread=*/false));
        const Split& s = rows.back();
        // THE CULL'S OWN EFFECTIVENESS. A per-brick tape that carries nearly
        // every item means the region test dropped almost nothing, which would
        // make "evaluation dominates" a statement about a cull that is not
        // working rather than about evaluation being expensive.
        std::printf("  %4d stamps: %zu bricks, per-brick tape instrs min %zu / med %zu / max %zu\n",
                    s.stamps, s.bricks, s.instrs_min, s.instrs_med, s.instrs);
    }

    std::printf("\n  per brick, median ms\n");
    std::printf("  stamps   compile      eval     whole   compile share   per-event (x%zu bricks)\n",
                rows.empty() ? 0 : rows[0].bricks);
    int failures = 0;
    for (const Split& s : rows) {
        const double sum = s.compile_ms + s.eval_ms;
        const double share = sum > 0.0 ? 100.0 * s.compile_ms / sum : 0.0;
        std::printf("  %4d   %8.5f  %8.5f  %8.5f   %9.1f%%   %10.3f\n", s.stamps, s.compile_ms,
                    s.eval_ms, s.whole_ms, share,
                    s.whole_ms * static_cast<double>(s.bricks));
        // The two halves must account for the whole, or one of the three timers
        // is measuring something other than what it is labelled.
        if (s.whole_ms > 0.0) {
            const double ratio = sum / s.whole_ms;
            if (ratio < 0.75 || ratio > 1.35) {
                std::printf("       FAIL: compile+eval = %.5f but whole = %.5f (%.2fx) -- the\n"
                            "             split does not account for the measured cost.\n",
                            sum, s.whole_ms, ratio);
                ++failures;
            }
        }
    }

    // The fixture must actually have grown, or every row above is the same
    // measurement printed four times with different labels.
    if (rows.size() >= 2 && rows.back().instrs <= rows.front().instrs) {
        std::printf("\nFAIL: the tape did not get longer as stamps grew (%zu -> %zu), so scene\n"
                    "      size was never the variable and this table means nothing.\n",
                    rows.front().instrs, rows.back().instrs);
        ++failures;
    }

    // -- THE CONTROL --------------------------------------------------------
    //
    // Same count, same chain, same blend; only the geometry spread out. If the
    // per-brick tape falls here, the packed table above was the cull correctly
    // keeping material that really is in range, and there is no defect. If it
    // stays high, the region test is failing regardless of geometry.
    std::printf("\n  CONTROL -- the same items spread into a volume instead of packed on an arc\n");
    std::printf("  stamps   per-brick tape (min/med/max)   compile      eval     whole\n");
    std::vector<Split> spread_rows;
    for (const int n : sizes) {
        spread_rows.push_back(measure(n, n >= 200 ? 3 : 8, /*spread=*/true));
        const Split& s = spread_rows.back();
        std::printf("  %4d        %5zu / %5zu / %5zu       %8.5f  %8.5f  %8.5f\n", s.stamps,
                    s.instrs_min, s.instrs_med, s.instrs, s.compile_ms, s.eval_ms, s.whole_ms);
    }
    if (!spread_rows.empty() && !rows.empty()) {
        const std::size_t packed = rows.back().instrs_med;
        const std::size_t loose = spread_rows.back().instrs_med;
        std::printf("\n  median per-brick tape at %d stamps: packed %zu, spread %zu\n",
                    rows.back().stamps, packed, loose);
        if (loose * 3 < packed)
            std::printf("  -> THE CULL WORKS. The packed table is the geometry, not a defect:\n"
                        "     spreading the same items drops the tape by %.1fx. The cost of a\n"
                        "     Move over a dense form is the material that is really there.\n",
                        loose ? static_cast<double>(packed) / static_cast<double>(loose) : 0.0);
        else
            std::printf("  -> THE CULL DOES NOT WORK. The same count spread over a wide volume\n"
                        "     still compiles %zu instrs per brick, so the region test is not\n"
                        "     dropping what it should and the pad is worth measuring.\n", loose);
    }

    // The verdict, stated as the prediction it was written to test.
    if (rows.size() >= 2) {
        const auto share_of = [](const Split& s) {
            const double sum = s.compile_ms + s.eval_ms;
            return sum > 0.0 ? s.compile_ms / sum : 0.0;
        };
        const double lo = share_of(rows[1]);   // 50 stamps
        const double hi = share_of(rows.back());
        std::printf("\n  compile share at 50 stamps %.1f%%, at %d stamps %.1f%%\n", lo * 100.0,
                    rows.back().stamps, hi * 100.0);
        if (hi > lo * 1.15)
            std::printf("  -> RISES with scene size. A per-gesture tape cache keyed on the\n"
                        "     undragged layers removes a constant that grows with the form,\n"
                        "     which is where the problem is. Worth building.\n");
        else if (hi < lo * 0.85)
            std::printf("  -> FALLS with scene size. The per-brick cost is evaluation scaling\n"
                        "     with tape length; there is no constant worth hoisting, and a\n"
                        "     dense form is expensive because its field is. Closes this line.\n");
        else
            std::printf("  -> FLAT. Compile and eval scale together; a hoist saves its share\n"
                        "     and no more. Decide on the absolute number, not the trend.\n");
    }

    batch_union_probe();

    if (failures) {
        std::printf("\n%d invariant(s) failed; do not quote the table above.\n", failures);
        return 1;
    }
    std::printf("\nok\n");
    return 0;
}
