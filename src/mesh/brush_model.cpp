#include "clay/mesh/brush_model.h"

#include <algorithm>

namespace clay {
namespace mesh {

// The decomposition table. Every verb is a row; a verb that could not be
// written as one would be evidence that an axis is missing, which is the test
// the brush-engine requirement states.
//
// Two rows are worth reading beside each other. DRAW and INFLATE differ in
// exactly one column — the frame — and in nothing else, which is what lets one
// kernel serve both. GRAB and SNAKEHOOK are identical here, and correctly so:
// one stamp of snakehook IS a grab, and what makes it a snakehook is the
// re-anchoring BETWEEN stamps, which is a fact about a stroke rather than about
// a brush.
BrushModel model_of(MeshBrush verb) {
    BrushModel m;
    m.verb = verb;
    m.footprint = default_geodesic(verb) ? BrushFootprint::SurfaceWalk : BrushFootprint::Ball;
    m.target = writes_color(verb) ? BrushWriteTarget::Color : BrushWriteTarget::Position;
    m.post = writes_color(verb) ? BrushPostPolicy::None : BrushPostPolicy::RecomputeNormals;
    switch (verb) {
        case MeshBrush::Grab:
        case MeshBrush::Snakehook:
            m.frame = BrushFrame::StrokeDirection;
            m.kernel = BrushKernelId::Translate;
            break;
        case MeshBrush::Draw:
            m.frame = BrushFrame::RegionNormal;
            m.kernel = BrushKernelId::Displace;
            break;
        case MeshBrush::Inflate:
            m.frame = BrushFrame::VertexNormal;
            m.kernel = BrushKernelId::Displace;
            break;
        case MeshBrush::Pinch:
            m.frame = BrushFrame::VertexNormal;  // the plane the gather slides in
            m.kernel = BrushKernelId::Gather;
            break;
        case MeshBrush::Nudge:
            m.frame = BrushFrame::StrokeDirection;
            m.kernel = BrushKernelId::Tangential;
            break;
        case MeshBrush::Flatten:
            m.frame = BrushFrame::RegionPlane;
            m.kernel = BrushKernelId::Plane;
            break;
        case MeshBrush::Clay:
            m.frame = BrushFrame::RegionNormal;
            m.kernel = BrushKernelId::PlaneDeposit;
            break;
        case MeshBrush::Crease:
            m.frame = BrushFrame::RegionNormal;
            m.kernel = BrushKernelId::CutAndGather;
            break;
        case MeshBrush::Smooth:
        case MeshBrush::Polish:
            m.frame = BrushFrame::None;
            m.kernel = BrushKernelId::Laplacian;
            break;
        case MeshBrush::Scrape:
            // The cut AND the relax, from one snapshot. The plane is the frame
            // and the Laplacian is the kernel; sequencing them as two stamps is
            // a different operation and a worse one.
            m.frame = BrushFrame::RegionPlane;
            m.kernel = BrushKernelId::Laplacian;
            break;
        case MeshBrush::Relax:
            // The same Laplacian target smooth uses, with its normal component
            // removed — which is what the vertex-normal frame names.
            m.frame = BrushFrame::VertexNormal;
            m.kernel = BrushKernelId::Laplacian;
            break;
        case MeshBrush::Layer:
            m.frame = BrushFrame::RegionNormal;
            m.kernel = BrushKernelId::DepositCeiling;
            break;
        case MeshBrush::Paint:
            m.frame = BrushFrame::None;
            m.kernel = BrushKernelId::ColorBlend;
            break;
        case MeshBrush::Smear:
            m.frame = BrushFrame::None;
            m.kernel = BrushKernelId::ColorAdvect;
            break;
    }
    return m;
}

BrushRuntimePlan compile_plan(const BrushModel& model, const MeshBrushSettings& settings) {
    BrushRuntimePlan plan;
    plan.model = model;
    plan.geodesic = settings.geodesic;
    plan.smooth_passes = std::clamp(settings.smooth_iterations, 1, kMaxSmoothIterations);

    // What the gather owes this kernel. Derived from the KERNEL rather than the
    // verb, which is the point of the axis: a second representation adding a
    // verb that reads a one-ring gets the right answer without a new row here.
    switch (model.kernel) {
        case BrushKernelId::Laplacian:
            plan.needs_neighbors = true;
            // Polish, and only polish, reads a NEIGHBOUR's own normal — its
            // gate is the mean angle between this vertex's normal and its
            // neighbours'. The other three readings of the Laplacian do not.
            plan.needs_neighbor_normals = model.verb == MeshBrush::Polish;
            break;
        case BrushKernelId::ColorAdvect:
            plan.needs_neighbors = true;
            plan.needs_neighbor_colors = true;
            break;
        case BrushKernelId::DepositCeiling:
            plan.needs_stroke_origin = true;
            break;
        default:
            break;
    }
    return plan;
}

}  // namespace mesh
}  // namespace clay
