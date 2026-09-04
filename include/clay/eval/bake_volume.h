#pragma once

// Baking a tape into a sampled volume through the CPU backend's pool.
//
// `FieldVolume::sample` takes a callable and asks it for ONE POINT AT A TIME.
// That is the right contract for a leaf module — `field` sits below `scene`
// and cannot name a tape, let alone a backend — and it is the wrong way to
// evaluate a document, which is what every `*_from` verb in the bindings does:
// a tape instruction costs about ten nanoseconds and its arithmetic costs one,
// so the interpreter is most of a bake and the interpreter is per point.
//
// `scene::bake_layer` already knew this. It goes through `sample_blocks` with
// a window fill that hands whole blocks of points to an injected
// `scene::BakePointEval`, and every caller above the layering boundary passes
// `eval::pooled_bake_eval()`. The benchmark pair that gates it —
// `BM_ConsolidateGrownDoc` against `BM_ConsolidateSerialGrownDoc` — measures
// the difference at 7.5x on a twelve-core machine.
//
// This is that window fill, as a value the other bake paths can pass. It lives
// HERE for the same reason `pooled_bake_eval` does: the layering runs
// eval -> scene, so a fill that names both a tape and a backend belongs above
// both.
//
// Byte-identity with the serial walk is the contract, not a tolerance:
// `eval_points` writes to disjoint output slices and each point goes through
// the same scalar reference evaluation, so the pool changes speed and nothing
// else.

#include <cstddef>
#include <vector>

#include <memory>
#include <tuple>

#include "clay/eval/backend.h"
#include "clay/parallel/thread_pool.h"
#include "clay/scene/cull_index.h"
#include "clay/scene/document.h"
#include "clay/field/volume.h"
#include "clay/scene/tape.h"

namespace clay {
namespace eval {

// A per-sample transform applied to the source distance before it is stored,
// taking the sample's world position and what the tape said there.
//
// It exists because the document-sourced verbs are not all "bake, then
// operate". Flatten blends toward a plane INSIDE the sampled callable, and it
// has to: `sample_blocks` decides which bricks to keep from the values it is
// given, and flatten moves the surface by many band widths, so a volume built
// from the source and rewritten afterwards would have kept the bricks around
// the surface the source had rather than the one flatten made.
//
// Empty means "store what the tape said", which is the plain bake.
using SampleTransform = std::function<float(kernel::cfloat3 p, float source)>;

// Where a sample's LATTICE position sits in the world the tape describes.
//
// Empty means "the lattice IS world", which is every bake but one: a STAMP
// CAPTURE samples an oriented box, so its lattice is the asset's own frame and
// each sample must be asked about the world point that frame puts it at. The
// captured volume then holds the region in local coordinates, and placing it
// under that same frame reproduces the source exactly.
//
// A `SampleTransform` alongside this still receives the WORLD position, because
// what it is for -- flatten drawing samples onto a plane -- is a statement about
// the world and not about a lattice.
using SamplePlacement = std::function<kernel::cfloat3(kernel::cfloat3 lattice)>;

// The block fill `FieldVolume::sample_blocks` wants, evaluating `tape` at each
// window's points through the pool.
//
// `tape` is BORROWED and must outlive the `sample_blocks` call this is handed
// to — every caller holds it as a `shared_ptr` on the stack across that call,
// which is the only shape this is for.
//
// Falls back to the tape's own scalar walk when no CPU backend is registered,
// so a build without one bakes slower rather than not at all.
inline field::FieldVolume::BrickBlockFill tape_block_fill(const scene::Tape& tape,
                                                          SampleTransform transform = {},
                                                          SamplePlacement placement = {}) {
    // One scratch buffer for the whole bake rather than one per window: the
    // windows are walked in order by a single thread, so there is nothing to
    // race, and a bake at an interactive cell has thousands of them.
    auto points = std::make_shared<std::vector<float>>();
    return [&tape, transform = std::move(transform), placement = std::move(placement), points](
               const field::FieldVolume::BrickGrid& grid, std::size_t first, std::size_t count,
               float* out) {
        const std::size_t n = count * field::kBrickSamples;
        points->resize(n * 3);
        for (std::size_t s = 0; s < count; ++s)
            for (int i = 0; i < field::kBrickSamples; ++i) {
                kernel::cfloat3 p = grid.sample_position(first + s, i);
                // The lattice position, put where the tape can be asked about
                // it. Identity for every bake; a frame for a stamp capture.
                if (placement) p = placement(p);
                const std::size_t at =
                    (s * field::kBrickSamples + static_cast<std::size_t>(i)) * 3;
                (*points)[at] = p.x;
                (*points)[at + 1] = p.y;
                (*points)[at + 2] = p.z;
            }

        bool batched = false;
        if (Backend* cpu = Registry::instance().find("cpu")) {
            PointQuery q;
            q.points_xyz = points->data();
            q.count = n;
            PointResults res;
            res.distances = out;
            res.colors_rgb = nullptr;  // a bake fills colour in its own pass
            batched = cpu->eval_points(tape, q, res) == Status::Ok;
        }
        if (!batched)
            for (std::size_t i = 0; i < n; ++i)
                out[i] = tape.eval(kernel::cf3((*points)[i * 3], (*points)[i * 3 + 1],
                                               (*points)[i * 3 + 2]))
                             .d;

        if (!transform) return;
        for (std::size_t i = 0; i < n; ++i)
            out[i] = transform(kernel::cf3((*points)[i * 3], (*points)[i * 3 + 1],
                                           (*points)[i * 3 + 2]),
                               out[i]);
    };
}

// The same evaluator, for a source asked at ARBITRARY points rather than at a
// sample lattice. `move_topological` is the case: where an output sample takes
// its material from depends on the geodesic weight there, so the query
// positions are not the grid's and no fill that only knows the grid can answer
// them.
//
// Same fallback and same borrowing rule as `tape_block_fill` above: the tape
// must outlive every call made through the returned callable.
inline std::function<void(const float*, std::size_t, float*)> tape_point_batch(
    const scene::Tape& tape) {
    return [&tape](const float* points_xyz, std::size_t count, float* out) {
        if (Backend* cpu = Registry::instance().find("cpu")) {
            PointQuery q;
            q.points_xyz = points_xyz;
            q.count = count;
            PointResults res;
            res.distances = out;
            res.colors_rgb = nullptr;
            if (cpu->eval_points(tape, q, res) == Status::Ok) return;
        }
        for (std::size_t i = 0; i < count; ++i)
            out[i] = tape.eval(kernel::cf3(points_xyz[i * 3], points_xyz[i * 3 + 1],
                                           points_xyz[i * 3 + 2]))
                         .d;
    };
}

// Baking a DOCUMENT with the tape culled per brick, exactly.
//
// A per-brick tape is enormously shorter than the whole document's where items
// are spread out: a 600-dab sphere hard-unioned compiles to 1,199 instructions
// and any one brick needs 5.4 of them. The brick cache has worked this way
// since it was written; the bake never has.
//
// The result is BYTE-IDENTICAL to evaluating the whole tape everywhere, and
// that is not a tolerance, it is a consequence of two facts about culling:
//
//   1. `culled >= true` always. Culling drops items from a minimum, and
//      dropping from a minimum can only raise it.
//   2. `culled <= band` implies `true <= band` implies the two are EQUAL --
//      because an item is only dropped when its influence bound is more than a
//      band from the brick, so inside the brick it cannot be the nearest thing
//      while the nearest thing is within the band. That is the CullRegion
//      contract (see tape.h, and scene::cull_pad for what makes it hold across
//      a smooth-union chain).
//
// From those: a sample the culled tape puts INSIDE the band is already the
// truth. A brick with no such sample stores nothing at all -- only whether it
// is near the surface, which is false, and which SIDE it is on, and (1) gives
// the sign, since an item that could make a point inside is within a band of
// it and therefore kept. So most bricks are finished by the culled tape alone.
//
// What is left is the samples a KEPT brick stores beyond the band. Those are
// stored raw, so they must be the full tape's -- a culled value there is too
// LARGE, and a field that overstates its own distance is one a marcher steps
// through. Measured, storing them would overstate by 0.033 against the plain
// bake's 0.002, which is 1.65 cells where the interpolation overshoot is 0.1.
// They are 27% of the samples, collected across the window and evaluated in ONE
// batch: doing them as scattered scalar calls instead measured 1.78x where the
// batch measures 3.48x, because it gives up the vectorised path.
//
// Culling is NOT always worth it, and the blend decides. A smooth union's cull
// pad grows with k, so a big enough k keeps every item in every brick's tape
// and the per-brick compile is pure overhead. On a 0.02 cell, whose bricks are
// 0.16 across:
//
//     k      culled/full     bake
//     0.00      0.5%        3.48x
//     0.04      7%          2.40x
//     0.06     16%          1.31x
//     0.10     49%          0.80x   <- a loss
//
// So the decision is MEASURED, from one sample of the lattice, rather than
// guessed from k.
// ONLY FOR A FILL WHOSE VALUES ARE THE ONES STORED. The refinement above
// decides what to pay for from whether a sample lands in the band, and that is
// the same question `sample_blocks` asks when it decides whether to keep the
// brick -- so the two agree, and every stored sample ends up exact.
//
// A caller that TRANSFORMS the block after this fill returns breaks that
// agreement. `field::flatten` does: it draws the samples onto a plane, so a
// brick this fill saw as empty can come back near the surface, and its samples
// -- culled, never refined, too large -- would be stored. The symptom is not
// subtle once you look for it: the volume's declared Lipschitz rises and its
// safe step scale collapses, which `test_c_volume.cpp`'s "the document-sourced
// field is far cheaper to march" caught on the first run.
//
// So flatten's document form keeps `tape_block_fill`. Relax's does not need to:
// it samples first and relaxes the volume afterwards, so what this fill
// produces is what gets stored.
inline field::FieldVolume::BrickBlockFill document_block_fill(const scene::Document& doc,
                                                              const scene::Tape& tape) {
    // -1 undecided, 0 evaluate the whole tape, 1 cull per brick. Decided once,
    // on the first window, from the bricks actually being asked for.
    auto mode = std::make_shared<int>(-1);
    auto index = std::make_shared<std::shared_ptr<const scene::CullIndex>>();
    auto plain = tape_block_fill(tape);
    // Held across windows rather than allocated per window: the windows are
    // walked in order by one thread, and a window is 512 bricks of samples.
    auto scratch = std::make_shared<std::tuple<std::vector<char>, std::vector<float>,
                                               std::vector<std::size_t>, std::vector<float>>>();

    return [&doc, &tape, mode, index, plain, scratch](
               const field::FieldVolume::BrickGrid& grid, std::size_t first, std::size_t count,
               float* out) {
        if (*mode < 0) {
            // Spread across the WHOLE lattice, not across this window. A window
            // is 512 consecutive slots in x-fastest order, so the first one is
            // a slab at the bottom of the region -- nowhere near the surface,
            // where every tape culls to almost nothing. Probing it said "cull"
            // for documents that then ran 1.7x SLOWER, which is the mistake
            // this comment exists to stop someone making again.
            const std::size_t slots = static_cast<std::size_t>(grid.bcount[0]) *
                                      static_cast<std::size_t>(grid.bcount[1]) *
                                      static_cast<std::size_t>(grid.bcount[2]);
            constexpr std::size_t kProbes = 16;
            const std::size_t step = slots > kProbes ? slots / kProbes : 1;
            std::size_t sampled = 0, instrs = 0;
            for (std::size_t slot = 0; slot < slots && sampled < kProbes;
                 slot += step, ++sampled) {
                scene::CullRegion cr{grid.brick_box(slot).dilated(grid.band)};
                instrs += scene::compile_document(doc, &cr).instrs.size();
            }
            const std::size_t whole = tape.instrs.size();
            // A third of the document's tape or less. The crossover measures
            // between a sixth and a half of it; a third sits inside that, and
            // was chosen by measuring both ends -- a fifth left the k=0.06 case
            // (1.49x) on the table, and a half would reach into the losses.
            *mode = (sampled && whole && instrs * 3 <= whole * sampled) ? 1 : 0;
            if (*mode == 1) *index = std::make_shared<const scene::CullIndex>(doc);
        }
        if (*mode == 0) {
            plain(grid, first, count, out);
            return;
        }

        auto& [needs, points, slots_out, distances] = *scratch;
        const std::size_t n = count * field::kBrickSamples;
        needs.assign(n, 0);

        // One coarse plan for the window, then a tape per brick across the
        // pool. Per BRICK rather than per window on purpose: a window is 512
        // consecutive slots, which is a slab spanning the region rather than a
        // compact box, and culling one measured 30x WORSE than culling none.
        math::Aabb window;
        for (std::size_t s = 0; s < count; ++s)
            window.expand(grid.brick_box(first + s).dilated(grid.band));
        const scene::CullIndex& idx = **index;
        const scene::CullPlan plan = idx.plan(window);
        parallel::ThreadPool::instance().parallel_for(
            count, 1, [&](std::size_t lo, std::size_t hi) {
                for (std::size_t s = lo; s < hi; ++s) {
                    scene::CullRegion cr{grid.brick_box(first + s).dilated(grid.band)};
                    const scene::Tape brick = scene::compile_document(doc, &cr, &idx, &plan);
                    float* block = out + s * field::kBrickSamples;
                    bool near = false;
                    for (int i = 0; i < field::kBrickSamples; ++i) {
                        block[i] = brick.eval(grid.sample_position(first + s, i)).d;
                        if (kernel::cabs(block[i]) <= grid.band) near = true;
                    }
                    // Nothing within the band: this brick stores no samples, so
                    // only "not near the surface" and the sign are read from it,
                    // and the culled tape has both right. Done.
                    if (!near) continue;
                    // A kept brick's samples are stored raw. In the band they
                    // are already the truth; beyond it they are too large, and
                    // storing that would make the volume overstate its own
                    // distance.
                    for (int i = 0; i < field::kBrickSamples; ++i)
                        if (kernel::cabs(block[i]) > grid.band)
                            needs[s * field::kBrickSamples + static_cast<std::size_t>(i)] = 1;
                }
            });

        points.clear();
        slots_out.clear();
        for (std::size_t j = 0; j < n; ++j) {
            if (!needs[j]) continue;
            const kernel::cfloat3 p = grid.sample_position(
                first + j / field::kBrickSamples, static_cast<int>(j % field::kBrickSamples));
            points.push_back(p.x);
            points.push_back(p.y);
            points.push_back(p.z);
            slots_out.push_back(j);
        }
        if (slots_out.empty()) return;

        distances.assign(slots_out.size(), 0.0f);
        bool batched = false;
        if (Backend* cpu = Registry::instance().find("cpu")) {
            PointQuery q;
            q.points_xyz = points.data();
            q.count = slots_out.size();
            PointResults res;
            res.distances = distances.data();
            res.colors_rgb = nullptr;
            batched = cpu->eval_points(tape, q, res) == Status::Ok;
        }
        if (!batched)
            for (std::size_t j = 0; j < slots_out.size(); ++j)
                distances[j] = tape.eval(kernel::cf3(points[j * 3], points[j * 3 + 1],
                                                     points[j * 3 + 2]))
                                   .d;
        for (std::size_t j = 0; j < slots_out.size(); ++j) out[slots_out[j]] = distances[j];
    };
}

}  // namespace eval
}  // namespace clay
