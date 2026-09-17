#pragma once

#include "clay/mesh/topology_ops.h"

namespace clay::mesh::detail {

// Remeshing may postpone stored normals: its topology decisions read geometry.
// Keep handles with generations because a later operation can recycle a slot.
class NormalUpdates {
   public:
    void update(DynamicSurface& surface, const std::vector<FaceId>& faces);
    void flush(DynamicSurface& surface, TopologyDelta* delta);

   private:
    std::size_t operations_ = 0;
    std::vector<FaceId> faces_;
    std::vector<VertexId> vertices_;
};

SplitResult split_edge(DynamicSurface& surface, EdgeId edge, float t,
                       const TopologyOpOptions& options, TopologyDelta* delta,
                       NormalUpdates* normals);
CollapseResult collapse_edge(DynamicSurface& surface, EdgeId edge, const TopologyOpOptions& options,
                             TopologyDelta* delta, NormalUpdates* normals);
FlipResult flip_edge(DynamicSurface& surface, EdgeId edge, const TopologyOpOptions& options,
                     TopologyDelta* delta, bool force, NormalUpdates* normals);

}  // namespace clay::mesh::detail
