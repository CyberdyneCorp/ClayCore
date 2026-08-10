#include <doctest/doctest.h>

#include <cstdint>

#include "vulkan_dispatch.h"

// Dispatch splitting for the Vulkan backend. Built on every platform,
// device or no device: the case it guards is a grid larger than
// maxComputeWorkGroupCount, and every desktop GPU's limit runs to billions
// of groups, so hardware will not produce it. Left to a device test, this
// code would ship unexercised.

using clay::eval::vulkan_detail::DispatchChunk;
using clay::eval::vulkan_detail::dispatch_chunks;

namespace {

// What the shader sees: every element covered exactly once, at the index it
// would have had unsplit.
bool covers_exactly(const std::vector<DispatchChunk>& chunks, std::size_t elements,
                    std::uint32_t local_size) {
    std::uint64_t next = 0;
    for (const DispatchChunk& c : chunks) {
        if (c.first_element != next) return false;
        next += static_cast<std::uint64_t>(c.groups) * local_size;
    }
    // The last chunk may overrun; the shader's own bound check drops the tail.
    return next >= elements && next - elements < local_size;
}

}  // namespace

TEST_CASE("vulkan dispatch: a batch inside the limit is one chunk") {
    auto chunks = dispatch_chunks(1000, 64, 65535);
    REQUIRE(chunks.size() == 1);
    CHECK(chunks[0].first_element == 0);
    CHECK(chunks[0].groups == 16);  // ceil(1000 / 64)
    CHECK(covers_exactly(chunks, 1000, 64));
}

TEST_CASE("vulkan dispatch: a batch past the limit is split, and still covers") {
    // 256^3, the preview grid the splitting exists for, against a small limit
    const std::size_t elements = 256u * 256u * 256u;
    auto chunks = dispatch_chunks(elements, 64, 1024);
    REQUIRE(chunks.size() > 1);
    for (const DispatchChunk& c : chunks) CHECK(c.groups <= 1024);
    CHECK(covers_exactly(chunks, elements, 64));
}

TEST_CASE("vulkan dispatch: chunks carry absolute indices") {
    auto chunks = dispatch_chunks(500, 64, 2);
    REQUIRE(chunks.size() == 4);  // ceil(500/64) = 8 groups, 2 per chunk
    CHECK(chunks[0].first_element == 0);
    CHECK(chunks[1].first_element == 128);
    CHECK(chunks[2].first_element == 256);
    CHECK(chunks[3].first_element == 384);
}

TEST_CASE("vulkan dispatch: a partial group is still dispatched") {
    auto chunks = dispatch_chunks(1, 64, 65535);
    REQUIRE(chunks.size() == 1);
    CHECK(chunks[0].groups == 1);

    auto ragged = dispatch_chunks(65, 64, 65535);
    REQUIRE(ragged.size() == 1);
    CHECK(ragged[0].groups == 2);
    CHECK(covers_exactly(ragged, 65, 64));
}

TEST_CASE("vulkan dispatch: degenerate inputs produce no work rather than bad work") {
    CHECK(dispatch_chunks(0, 64, 65535).empty());
    CHECK(dispatch_chunks(100, 0, 65535).empty());
    CHECK(dispatch_chunks(100, 64, 0).empty());
}

TEST_CASE("vulkan dispatch: the split is invisible in what gets covered") {
    // The property that matters: however the limit chops it, the same
    // elements are covered in the same places.
    const std::size_t elements = 100000;
    auto whole = dispatch_chunks(elements, 64, 1u << 30);
    for (std::uint64_t limit : {1u, 7u, 64u, 1000u, 65535u}) {
        auto split = dispatch_chunks(elements, 64, limit);
        CHECK(covers_exactly(split, elements, 64));
        std::uint64_t split_groups = 0;
        for (const DispatchChunk& c : split) split_groups += c.groups;
        CHECK(split_groups == whole[0].groups);
    }
}
