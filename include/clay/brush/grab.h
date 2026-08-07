#pragma once

// Document grab — the Move brush (brush-engine spec, add-document-grab).
//
// ZBrush's Move drags the SURFACE. The `grab` deformer drags ONE ITEM'S OWN
// FIELD, and takes its centre in that item's LOCAL frame. On a single-item form
// those coincide, which is why the gap is easy to miss; on a form smooth-unioned
// from several items — the normal case for a blocked-out sculpt — grabbing one
// item pulls its share and leaves the rest behind.
//
// So this is a RESOLVER, the same shape as cut_item and snakehook: a world drag
// in, a plan of ordinary edits out. It reads the document and never modifies it,
// so a host can preview a Move before committing it.
//
// It is EXACT rather than an approximation, for two reasons:
//
//  - Combine ops are pointwise in the deformed point, so warping every operand
//    identically is the same as warping their combination. One world warp on
//    every item IS a field-level grab, not an imitation of one.
//  - math::Transform's scale is uniform by design, so a spherical falloff stays
//    spherical under it and the mapping is always exactly expressible.

#include <cstdint>
#include <vector>

#include "clay/scene/document.h"
#include "clay/scene/types.h"

namespace clay {
namespace brush {

// A drag, entirely in WORLD units — which is the point: a host has the drag in
// world space already, and turning it into per-item local frames is the
// error-prone step this exists to own.
struct GrabSettings {
    kernel::cfloat3 centre = kernel::cf3(0, 0, 0);
    float radius = 0.25f;
    kernel::cfloat3 displacement = kernel::cf3(0, 0, 0);
    std::uint8_t ease = 0;
    // Gate the pull on the half-space it heads into, so the far side of a form
    // does not travel with the near side.
    bool front_only = false;
};

// The same drag expressed in the frame of an item whose world transform is
// `item_world` (its layer's transform composed with its own).
//
// The pure core of the whole row, and the part worth testing on its own: the
// centre maps through the inverse transform, the displacement through the
// inverse rotation and the scale, and the radius through the scale.
scene::Deformer grab_local(const math::Transform& item_world, const GrabSettings& settings);

// One item's share of a drag: the WHOLE new chain, ready for SetDeformersCmd.
//
// The whole chain rather than the one deformer to append, because a drag has to
// COALESCE. During one drag the centre and radius are fixed and only the
// displacement grows, so appending per frame would grow the chain without bound
// and degrade evaluation cost and the declared Lipschitz with every frame. A
// trailing grab carrying the same centre and radius is replaced instead — the
// discipline AppendStroke/TrimStroke coalescing already applies to strokes.
struct GrabTarget {
    scene::LayerId layer = 0;
    scene::NodeId node = scene::kNoNode;
    std::vector<scene::Deformer> deformers;
};

// Resolve a world drag into the plan of per-item edits that performs it.
//
// Only items the drag can actually touch are returned. A grab breaks exactness
// and raises the Lipschitz bound, so putting one on every item would cost a
// whole document its safe step scale for a local gesture — and outside its
// radius the warp is the identity, so an item whose influence bound misses the
// drag's sphere is provably unaffected.
//
// Hidden layers and layers protected from edits are skipped, so the plan only
// ever contains edits that would be accepted.
std::vector<GrabTarget> grab_document(const scene::Document& doc,
                                      const GrabSettings& settings);

}  // namespace brush
}  // namespace clay
