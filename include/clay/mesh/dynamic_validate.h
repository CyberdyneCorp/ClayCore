#pragma once

// EVERY INVARIANT A HALF-EDGE SURFACE HAS, checkable in one call
// (dynamic-topology spec, add-dynamic-topology).
//
// WHY THIS IS A FIRST-CLASS FILE and not a test helper. The failure mode of a
// half-edge implementation is not a crash: it is a surface that still renders,
// still exports, still takes another brush stroke, and is quietly wrong in one
// corner of one fan. That defect surfaces minutes later somewhere else, and the
// operator that caused it has long since returned success.
//
// So the operators are checked STRUCTURALLY rather than by their results. The
// debug build validates after every single split, collapse and flip, and the
// fuzz target validates after thousands of interleaved ones. That is the only
// way rare local connectivity failures surface at all.

#include <cstddef>
#include <string>
#include <vector>

#include "clay/mesh/dynamic_surface.h"

namespace clay {
namespace mesh {

struct DynamicValidationIssue {
    // What broke, in a sentence, with the slot that broke it.
    std::string what;
    std::uint32_t slot = 0;
};

struct DynamicValidationReport {
    bool ok = true;
    std::vector<DynamicValidationIssue> issues;

    // Bounded on purpose: a corrupted surface can produce one issue per element,
    // and a report with a million entries helps nobody find the first one.
    static constexpr std::size_t kMaxIssues = 32;

    void add(std::string what, std::uint32_t slot) {
        ok = false;
        if (issues.size() < kMaxIssues) issues.push_back(DynamicValidationIssue{std::move(what), slot});
    }
    // The first issue, which is the one worth reading: later ones are usually
    // the same corruption seen from other elements.
    std::string summary() const {
        if (ok) return "ok";
        return issues.empty() ? "invalid" : issues.front().what;
    }
};

// Every invariant:
//
//   - twin symmetry: twin(twin(h)) == h, and a half-edge is never its own twin
//   - the two half-edges of an edge are twins of each other
//   - `next` closes in exactly three steps for a face, and every corner of a
//     face names that face
//   - no live element references a dead slot
//   - no face repeats a vertex
//   - an edge's two half-edges run between the same pair of vertices, opposite
//     ways
//   - a vertex's `outgoing` is live and actually originates there
//   - every vertex ring closes or terminates on a boundary
//   - boundary half-edges form closed loops
//   - no NaN or infinity in a position or a normal
//   - the boundary constraint flag agrees with the actual incidence
DynamicValidationReport validate_dynamic_surface(const DynamicSurface& surface);

// The cheap subset, for the after-every-operator check in a debug build: twin
// symmetry, face loops and dead references, over the elements an operator
// touched rather than the whole surface.
DynamicValidationReport validate_local(const DynamicSurface& surface,
                                const std::vector<FaceId>& faces);

}  // namespace mesh
}  // namespace clay
