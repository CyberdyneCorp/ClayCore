#pragma once

// The Tube tool (brush-engine spec, add-tube-tool) — Nomad Sculpt's Tubes.
//
// Tap or draw a path, get a rope, pipe, tentacle or hair strand along it. Every
// ingredient was already here and none of them were joined:
//
//  - control points and the smooth/sharp toggle are StrokePointType, already
//    Hard, Spline, BSpline and Bezier, tessellated to a document tolerance
//  - a round tube with a varying radius is the stroke opcode, which sweeps a
//    sphere along a segment chain with a radius PER POINT and stays EXACT
//  - a cross-section that is not a circle is Prim::swept, which carries profiles
//    along a guide on parallel-transported frames
//  - closed tubes are stroke_closed
//
// What did not exist is the step that turns a drawn path plus a few settings
// into the right item. Leaving it to each caller means each answers "how does
// the radius vary", "when is this a stroke and when a sweep" and "what does
// closed mean for a tapered tube" differently — the same argument the cut tool
// and snakehook were built on.

#include <optional>
#include <vector>

#include "clay/scene/types.h"

namespace clay {
namespace brush {

struct TubeSettings {
    // Nomad's B-spline toggle, which is just the curve's point type: Hard turns
    // sharply at each control point, BSpline rounds through them. Not a separate
    // kind of curve — a tube's path is the curve every other item takes.
    scene::StrokePointType point_type = scene::StrokePointType::BSpline;

    // The radius at the start, the middle and the end, as Nomad's handles are.
    // Interpolated by ARC LENGTH, so a path whose control points bunch does not
    // bunch the taper. Equal values give a uniform tube.
    float radius_start = 0.08f;
    float radius_mid = 0.08f;
    float radius_end = 0.08f;

    // The last point joins back to the first, with no cap between them.
    bool closed = false;

    // Tessellation tolerance for the path — the same document-level quantity a
    // curve uses.
    float tolerance = 0.01f;

    // Smoothing between consecutive segments of the swept chain. Zero is the
    // plain union of round cones, which is already smooth for a tube.
    float blend_k = 0.0f;
};

// Resolve a path into a ROUND tube: a swept sphere, which is an exact distance
// field, so the document's safe step scale stays 1.
//
// Returns nothing for fewer than two points, or a radius that is positive
// nowhere — an item that would contribute nothing is not worth producing.
std::optional<scene::Node> tube(const std::vector<kernel::cfloat3>& path,
                                const TubeSettings& settings = {});

// The same path with a CROSS-SECTION that is not a circle: a swept item, which
// is a bound field rather than an exact one, so the safe step scale falls. That
// is the price of a square or custom profile, and it is charged here rather than
// discovered later.
//
// `profiles` are carried along the guide and distributed by arc length, as
// Prim::swept does; one profile is a constant cross-section.
std::optional<scene::Node> tube_with_profile(const std::vector<kernel::cfloat3>& path,
                                             const std::vector<scene::Profile>& profiles,
                                             const TubeSettings& settings = {});

}  // namespace brush
}  // namespace clay
