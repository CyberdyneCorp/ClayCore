#pragma once

// Splitting a dispatch against the device's workgroup limit.
//
// Pulled out of the backend so it can be tested without a device: the case
// it exists for -- a grid larger than maxComputeWorkGroupCount -- is one no
// desktop GPU will produce (their limits run to billions of groups), so a
// test that waits for real hardware to exercise it never runs at all.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace clay {
namespace eval {
namespace vulkan_detail {

struct DispatchChunk {
    std::uint32_t first_element;  // absolute index this chunk starts at
    std::uint32_t groups;         // workgroups to dispatch for it
};

// Chunks covering `elements` items at `local_size` per group, with no chunk
// exceeding `max_groups`. Every chunk carries an ABSOLUTE first element, so
// each writes where it would have written unsplit.
inline std::vector<DispatchChunk> dispatch_chunks(std::size_t elements,
                                                  std::uint32_t local_size,
                                                  std::uint64_t max_groups) {
    std::vector<DispatchChunk> chunks;
    if (elements == 0 || local_size == 0 || max_groups == 0) return chunks;
    const std::uint64_t total = (elements + local_size - 1) / local_size;
    for (std::uint64_t done = 0; done < total;) {
        const std::uint64_t take = (total - done) < max_groups ? (total - done) : max_groups;
        chunks.push_back(DispatchChunk{static_cast<std::uint32_t>(done * local_size),
                                       static_cast<std::uint32_t>(take)});
        done += take;
    }
    return chunks;
}

}  // namespace vulkan_detail
}  // namespace eval
}  // namespace clay
