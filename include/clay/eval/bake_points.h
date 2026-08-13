#pragma once

// The pooled evaluator for scene::bake_layer's lattice samples: the same
// points through the CPU backend's reference arithmetic, spread across its
// thread pool. It lives HERE and is handed to the bake as an injected
// scene::BakePointEval — by the bindings, the benchmark, anything that sits
// above both modules — because the layering runs eval -> scene and a bake in
// scene cannot name a backend. Byte-identity with the bake's serial walk is
// the contract either way: eval_points slices its output disjointly and each
// point goes through the same scalar reference evaluation, so the injected
// pool changes speed and nothing else.

#include <cstddef>

#include "clay/eval/backend.h"
#include "clay/scene/consolidate.h"
#include "clay/scene/tape.h"

namespace clay {
namespace eval {

inline scene::BakePointEval pooled_bake_eval() {
    return [](const scene::Tape& tape, const float* points_xyz, std::size_t count,
              float* out_distances, float* out_colors_rgb) {
        Backend* cpu = Registry::instance().find("cpu");
        if (!cpu) return false;
        PointQuery q;
        q.points_xyz = points_xyz;
        q.count = count;
        PointResults res;
        res.distances = out_distances;
        res.colors_rgb = out_colors_rgb;  // null when only distances were asked for
        return cpu->eval_points(tape, q, res) == Status::Ok;
    };
}

}  // namespace eval
}  // namespace clay
