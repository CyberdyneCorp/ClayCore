#pragma once

// The adaptive surface's index audit, shared by every test that has to prove a
// held `DynamicSculptor` stayed in step with its surface: every live face is in
// a chunk, and no chunk holds a dead one.
//
// Both numbers, not one. A replay that skipped the reindex leaves live faces
// MISSING (the faces a stroke deleted come back in no chunk); one that skipped
// the unindex leaves DEAD entries (the faces it created stay put).

#include <cstddef>
#include <cstdint>

#include "clay/mesh/dynamic_sculpt.h"

namespace clay_test {

struct IndexCoverage {
    std::size_t live_missing = 0;
    std::size_t dead_indexed = 0;
};

inline IndexCoverage index_coverage(const clay::mesh::DynamicSculptor& sculptor) {
    IndexCoverage out;
    const clay::mesh::DynamicSurface& s = sculptor.surface();
    const clay::mesh::DynamicBvh& bvh = sculptor.bvh();
    s.faces().for_each_live([&](clay::mesh::FaceId f, const clay::mesh::DynamicFace&) {
        if (bvh.leaf_of(f) == clay::mesh::DynamicBvh::kNoLeaf) ++out.live_missing;
    });
    for (std::size_t i = 0; i < bvh.leaf_count(); ++i) {
        const clay::mesh::SurfaceLeaf* leaf = bvh.leaf(static_cast<std::uint32_t>(i));
        if (!leaf) continue;
        for (clay::mesh::FaceId f : leaf->faces)
            if (!s.live(f)) ++out.dead_indexed;
    }
    return out;
}

}  // namespace clay_test
