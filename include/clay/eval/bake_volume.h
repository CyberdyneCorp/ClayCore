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

#include "clay/eval/backend.h"
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
                                                          SampleTransform transform = {}) {
    // One scratch buffer for the whole bake rather than one per window: the
    // windows are walked in order by a single thread, so there is nothing to
    // race, and a bake at an interactive cell has thousands of them.
    auto points = std::make_shared<std::vector<float>>();
    return [&tape, transform = std::move(transform), points](
               const field::FieldVolume::BrickGrid& grid, std::size_t first, std::size_t count,
               float* out) {
        const std::size_t n = count * field::kBrickSamples;
        points->resize(n * 3);
        for (std::size_t s = 0; s < count; ++s)
            for (int i = 0; i < field::kBrickSamples; ++i) {
                const kernel::cfloat3 p = grid.sample_position(first + s, i);
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

}  // namespace eval
}  // namespace clay
