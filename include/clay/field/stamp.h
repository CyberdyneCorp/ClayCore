#pragma once

// A captured region of a field, reusable as an asset (stamp-a-captured-field).
//
// WHAT THIS ADDS, AND WHAT IT DELIBERATELY DOES NOT. An audit of the
// implementation guide against the tree found most of what it asks for already
// shipped: `PrimType::Volume` is a sampled field that compiles through the tape,
// `clay_item_volume_from_document` captures a finite world region banded and
// redistanced, and `write-a-shared-payload-once` (format minor 17) made a
// placement cost a reference on disk as well as in memory. So this is not a
// capture pipeline. It is the parts capture does not give you:
//
//   AN ORIENTED FRAME. Today's capture region is a world-axis-aligned box. An
//   artist stamping a detail onto a curved surface needs the capture taken about
//   the SURFACE -- +Z outward, X/Y the tangent plane -- or the asset is only
//   reusable at the orientation it was taken at.
//
//   AN IDENTITY. Two placements share a payload, but nothing NAMES the asset, so
//   nothing can list what a document uses or keep one on disk on its own.
//
// THE FRAME IS NEVER INFERRED, and that is a decision rather than an omission.
// An orientation derived from the samples moves when the region moves, so
// re-capturing the same detail yields an asset that no longer agrees with the
// placements already made from it. The caller supplies the frame, and
// `stamp_frame_from_surface` builds one from a hit, a normal and an azimuth
// through the SAME `kernel::calpha_frame` the scalar alpha uses -- so a stamp
// and an alpha placed at one hit cannot disagree about which way is up.

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "clay/field/volume.h"
#include "clay/kernel/shim.h"
#include "clay/math/transform.h"

namespace clay::field {

// The frame a capture is taken in: where it sits, which way it faces, and which
// way is "along" in its tangent plane.
//
// `normal` is +Z of the asset's own axes -- the direction it pushes, the same
// meaning `dir` has for an alpha -- and `tangent` is a ROUGH +X that is
// re-orthogonalised, so a caller may hand in a stylus azimuth without
// normalising it against the normal first.
struct StampFrame {
    kernel::cfloat3 origin = kernel::cf3(0.0f, 0.0f, 0.0f);
    kernel::cfloat3 normal = kernel::cf3(0.0f, 0.0f, 1.0f);
    kernel::cfloat3 tangent = kernel::cf3(1.0f, 0.0f, 0.0f);
};

// The frame as a placement: the transform taking the asset's local coordinates
// to the world it was captured from. Placing a capture under exactly this
// reproduces the source.
//
// Through `kernel::calpha_frame`, including its fallback when the tangent is
// parallel to the normal, so an asset and an alpha resolve the same hit to the
// same axes.
math::Transform stamp_frame_transform(const StampFrame& frame);

// A frame from what a host actually has: a surface hit, the surface normal
// there, and the stylus azimuth in radians about that normal.
//
// Azimuth ROTATES THE TANGENT about the normal, which is what a rake or a
// chisel needs and what makes an azimuth worth carrying at all -- without it a
// stamp has no way to be turned by the wrist that placed it.
StampFrame stamp_frame_from_surface(kernel::cfloat3 hit, kernel::cfloat3 normal, float azimuth);

// A captured field, its frame, and a name for its content.
//
// The samples are the `FieldVolume` capture already produces -- this owns
// nothing new. What it adds is the frame the capture was taken in and an id
// derived from the payload, so two captures of the same region are recognisably
// the same asset and a host can keep one on disk.
struct FieldStamp {
    // In the stamp's OWN coordinates: the lattice was sampled about the origin
    // of the capture frame, not about the world origin.
    std::shared_ptr<const FieldVolume> volume;
    // The frame AS RESOLVED -- the transform taking local coordinates to the
    // world the capture was taken from, which is exactly what a placement wants.
    //
    // The resolved form rather than the caller's `StampFrame`, and that is a
    // round-trip property rather than a convenience: re-deriving a frame from a
    // stored normal and tangent runs them back through `calpha_frame` and a
    // quaternion, and the rounding in that showed up as a reloaded stamp whose
    // field differed from the original in the last ulp at every point. A
    // quaternion stored and restored is the same quaternion.
    math::Transform placement;
    // A content id over the payload's bytes. NOT a uuid: two captures that
    // produce identical samples are the same asset, and a host that captured
    // the same detail twice should be told so rather than accumulating
    // duplicates it cannot recognise. Zero when there is no payload.
    std::uint64_t content_id = 0;

    bool empty() const { return !volume || volume->brick_count() == 0; }
    // The captured region in the stamp's own coordinates.
    math::Aabb local_bounds() const;
    // What the payload costs, separately from the placements referencing it --
    // the number a host needs to decide whether to keep an asset resident.
    std::size_t payload_bytes() const;
};

// The id `FieldStamp::content_id` carries, over a volume's serialized bytes.
// Exposed so a caller holding a bare volume can ask the same question.
std::uint64_t stamp_content_id(const FieldVolume& volume);

// A stamp on its own, so a host library can keep one outside a document.
//
// The volume's own `serialize` plus the placement and the id, which is what
// makes this a stamp file rather than a volume file: a payload reloaded without
// its frame is an asset that has forgotten which way it faced.
std::vector<std::uint8_t> stamp_serialize(const FieldStamp& stamp);
std::optional<FieldStamp> stamp_deserialize(const std::uint8_t* data, std::size_t size);

}  // namespace clay::field
