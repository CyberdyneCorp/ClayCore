#pragma once

// A BRUSH, AS DATA (brush-engine spec, add-shared-brush-kernels).
//
// The artist-facing families other engines ship — Clay Buildup, Dam Standard,
// hPolish, Trim Dynamic, Snake Hook, Rake — are not new deformations. Each is a
// kernel plus a falloff plus a frame plus an accumulation rule plus a spacing,
// and this type is that tuple. A named brush that needs a new code path is
// evidence an axis is missing, and the axis is what gets added; nothing in
// `reference_presets()` below has an engine path of its own.
//
// WHY THE PRESET LIVES IN `brush` AND THE AXES DO NOT. `mesh` may not include
// `brush` — `brush` already depends on `mesh`, because `apply_to_mesh` is the
// stroke engine's fourth consumer — so the axes a per-vertex loop reads had to
// go below, in `mesh/brush_model.h`. A preset is the axes PAIRED WITH a stroke
// preset, and `brush` is the one module that can see both vocabularies. That is
// the same reason `apply_to_mesh` is the only place a `MaskField` becomes a
// `field::MaskGate`.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "clay/brush/stroke.h"
#include "clay/mesh/brush_model.h"
#include "clay/mesh/sculpt_common.h"

namespace clay {
namespace brush {

// From the first release rather than retrofitted, for the reason
// `StrokePreset` already records: presets outlive engine versions, and a
// library of them silently reinterpreted by a later build is the failure a
// version number exists to prevent.
inline constexpr std::uint16_t kBrushPresetVersion = 1;

struct BrushPreset {
    // What the artist calls it. Carried rather than derived, because two
    // presets can share every axis value and still be different brushes to the
    // person using them.
    std::string name;

    // How the stroke lays stamps down: spacing, pressure, taper, jitter,
    // accumulation, steady stroke.
    StrokePreset stroke;

    // What each stamp DOES: footprint, frame, kernel, write target, post
    // policy.
    mesh::BrushModel model;

    // The brush's own settings — the fields that are part of its identity
    // rather than of where a stamp landed. `centre`, `direction` and the alpha
    // pointers are placement and come from the stroke and the caller; `radius`
    // and `strength` are ignored by `apply_to_mesh`, which takes each stamp's
    // own.
    mesh::MeshBrushSettings settings;

    // NO IMAGE BYTES, EVER. An alpha, and any displacement image a later
    // change adds, stays caller-owned and borrowed for the duration of a call —
    // exactly as the mesh alpha stamp already requires. So a preset library
    // costs kilobytes, a host owns its own resource cache, and the same preset
    // can be stamped with two different alphas.
    //
    // `serialize` therefore writes no alpha pointer and no alpha extent, and
    // `deserialize` leaves them at their defaults.
    std::vector<std::uint8_t> serialize() const;

    // Accepts this version and any earlier one, taking defaults for whatever
    // the older schema did not carry, and REFUSES a newer one rather than
    // reading a prefix of it and pretending. A partially populated preset is
    // never produced.
    static std::optional<BrushPreset> deserialize(const std::uint8_t* data, std::size_t size);
};

// The named families, as data. Every one of these is axis values over existing
// kernels; none has a code path.
//
// This is the list the brush-engine requirement is checked against: if a family
// here needed an engine change to express, the model would be missing an axis.
std::vector<BrushPreset> reference_presets();

// One by name, or nullopt. Names are the library's keys and are stable.
std::optional<BrushPreset> reference_preset(const std::string& name);

}  // namespace brush
}  // namespace clay
