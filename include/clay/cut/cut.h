#pragma once

// The cut tool (cut-tool spec): a shape drawn over the model becomes an
// ordinary edit item that cuts through it.
//
// Every ingredient for this already existed — Extrude, 2D profiles, Subtract
// and Intersect, rounding for bevelled walls. What did not exist was the step
// that turns a drawn shape into that item, and leaving that to each caller
// means each one answers "how deep" and "which side" differently. Same
// argument as the stroke engine.
//
// Two things are deliberate:
//
// THE CUT IS A PRISM, NOT A FRUSTUM. A shape drawn on screen under a
// perspective camera sweeps a converging wedge, and cutting with one would be
// wrong: the cut face would not be flat and the result would depend on where
// the camera was standing — the same rectangle drawn from two positions would
// give two different solids. A trim is a straight cut. Both ZBrush and 3DCoat
// cut a prism; so does this.
//
// NO CAMERA ENTERS THE ENGINE. The caller gives a frame it already has, since
// it needed one to draw the overlay, and shape coordinates are in WORLD units
// on that frame — not pixels, not normalized device coordinates. The engine
// has no viewport and should not learn about one.

#include <optional>
#include <vector>

#include "clay/math/geom.h"
#include "clay/scene/types.h"

namespace clay {
namespace cut {

// The plane the shape was drawn on and the direction it sweeps. `right` and
// `up` span the plane and are the units shape coordinates are measured in;
// `forward` is the sweep. The three must be orthonormal — a frame that is not
// is a caller bug rather than something to silently orthonormalize, because
// the shape the user saw was drawn in the frame they think they have.
struct CutFrame {
    kernel::cfloat3 origin = kernel::cf3(0, 0, 0);
    kernel::cfloat3 right = kernel::cf3(1, 0, 0);
    kernel::cfloat3 up = kernel::cf3(0, 1, 0);
    kernel::cfloat3 forward = kernel::cf3(0, 0, 1);

    bool is_orthonormal(float tolerance = 1e-3f) const;
};

enum class CutShapeType : std::uint8_t { Rect = 0, Circle, Polygon };

// The drawn shape, in world units on the frame. A polygon's vertices are in
// frame coordinates and its outline is implicitly closed.
struct CutShape {
    CutShapeType type = CutShapeType::Rect;
    float half_width = 1.0f;   // Rect
    float half_height = 1.0f;  // Rect
    float radius = 1.0f;       // Circle
    std::vector<kernel::cfloat2> polygon;

    static CutShape rect(float half_width, float half_height);
    static CutShape circle(float radius);
    static CutShape from_polygon(std::vector<kernel::cfloat2> vertices);

    // A closed control-point curve drawn in the cut plane, flattened through
    // the same tessellator curves use. Offered so a spline lasso does not have
    // each caller writing its own flattening — and so it honours the same
    // tolerance a curve would.
    static CutShape from_curve(const std::vector<scene::StrokePoint>& control_points,
                               float tolerance = 0.01f);
};

struct CutOptions {
    // Bevels the cut's edges, in world units. The node's own rounding.
    float rounding = 0.0f;
    // How far the sweep runs either side of the frame origin, along forward.
    // Absent means "derive it from the region", which is what makes a cut go
    // all the way through instead of stopping inside. Giving it explicitly is
    // how a deliberate partial cut is expressed.
    std::optional<float> near_extent;
    std::optional<float> far_extent;
};

// Resolve a cut into an ordinary edit item. `region` is what is being cut —
// normally the document's own bounds — and is used only to size the sweep.
//
// Pure: no document is read or touched, so a UI can preview a cut before
// committing it. The caller places the result with whichever op it wants, and
// THAT is where "keep the outside" versus "keep the inside" is decided:
// Subtract removes what the shape covers, Intersect keeps only that. A
// separate flag would be a second way to say one thing.
//
// Returns nullopt for a frame that is not orthonormal or a shape with no area,
// rather than an item that would quietly cut nothing or everything.
std::optional<scene::Node> cut_item(const CutFrame& frame, const CutShape& shape,
                                    const math::Aabb& region, const CutOptions& options = {});

}  // namespace cut
}  // namespace clay
