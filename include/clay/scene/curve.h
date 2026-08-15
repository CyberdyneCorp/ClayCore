#pragma once

// Control-point curves (scene-model spec): tessellation of a typed point list
// into the segment chain the stroke opcode already evaluates.
//
// A curve is NOT a new primitive. The stroke opcode already sweeps a sphere of
// varying radius along a chain of segments, exactly and with finite support; a
// curve is a way of AUTHORING that chain rather than a new thing to evaluate.
// Tessellating here rather than adding a tape opcode means curves cost nothing
// at evaluation time and inherit four backends, per-brick culling, exactness
// tracking, picking, undo and the file format unchanged.
//
// The consequence worth stating: a point list whose points are all hard
// corners tessellates to itself, so it compiles to a bit-identical tape. A
// stroke is a curve whose points are all hard corners.

#include <cstdint>
#include <vector>

#include "clay/scene/types.h"

namespace clay {
namespace scene {

// The deepest a span is bisected. 6 gives up to 64 segments per span, which is
// past the point where a tolerance is doing anything useful and well short of
// a tape a small tolerance could otherwise grow without limit.
inline constexpr int kMaxCurveDepth = 6;

// Below this a tolerance is treated as "as fine as the depth bound allows"
// rather than as a request for infinite subdivision.
inline constexpr float kMinCurveTolerance = 1e-5f;

// Tessellate an item's control points into the point chain the stroke opcode
// reads. Hard-only and open returns the input unchanged — not merely
// equivalent to it, the same points in the same order, which is what makes the
// tape bit-identical.
//
// `tolerance` is the maximum distance a span's midpoint may sit from its
// chord. It is a document property rather than a host setting: two builds have
// to agree on what a document means, and a per-host tolerance would make the
// field itself host-dependent.
std::vector<StrokePoint> tessellate_curve(const std::vector<StrokePoint>& points, bool closed,
                                          float tolerance);

// A guide's total arc length, and its tightest turn.
//
// Shared by the swept primitive, which carries profiles ALONG a guide, and by
// the bend-along-a-curve deformer, which reads the same geometry from the
// other end. Two copies would be two things to keep in step, and they decide
// the same safety bound.
//
// The bend radius is the CIRCUMRADIUS of each consecutive triple, not the turn
// angle over the arc: an angle estimate is fooled by tessellation density,
// reading a finely-sampled gentle curve as a tight one because short segments
// accumulate angle. Both take an ALREADY TESSELLATED guide, because what
// bounds the field is the polyline that was compiled rather than the ideal
// curve behind it.
float guide_arc_length(const std::vector<StrokePoint>& guide);
float guide_bend_radius(const std::vector<StrokePoint>& guide);

// Whether a point list needs tessellating at all. Cheap, and it lets the
// compiler and the bounds walk skip the copy for the common case.
bool curve_is_polyline(const std::vector<StrokePoint>& points, bool closed);

}  // namespace scene
}  // namespace clay
