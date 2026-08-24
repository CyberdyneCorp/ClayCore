#pragma once

// COUNTING WHAT A CONTAINER ACTUALLY HOLDS.
//
// Top-level and module-less, beside `version.h`, for the same reason that one
// is: every module from `scene` up needs the identical arithmetic to answer
// `roll-up-document-memory`, and putting it in any single module would either
// force an include that the layering table forbids or leave four copies to
// drift apart. It depends on nothing but the standard library.
//
// TWO THINGS sizeof AND size() BOTH GET WRONG, and this header exists for
// exactly those two:
//
//  - A `std::vector` that grew to a million and was cleared still HOLDS a
//    million elements' worth of address space. `size()` reports zero and the
//    allocator disagrees. Under memory pressure the allocator is the one
//    telling the truth, so everything here counts `capacity()`.
//
//  - A node-based container's cost is its BUCKET ARRAY plus one allocation per
//    entry, and `size() * sizeof(value_type)` sees neither. A map of many small
//    entries can hold more in buckets than in values.
//
// WHAT THIS IS NOT. An allocator's own per-block header, its size-class
// rounding and its arena fragmentation are invisible from here — a 24-byte node
// may cost 32. So every figure built on this header is a FLOOR on the real
// footprint, not an equality, and the interfaces that report one say so rather
// than let a host read the difference against the OS as a defect.

#include <cstddef>

namespace clay {

// What a contiguous container has checked out from the allocator.
template <typename Vec>
std::size_t vector_bytes(const Vec& v) {
    return v.capacity() * sizeof(typename Vec::value_type);
}

// A node-based container: the bucket array, plus per entry the value and the
// node's own links. libstdc++ stores a next-pointer and (for a non-trivial
// hash) a cached hash code; libc++ stores both unconditionally. Two pointers'
// worth of overhead per node is the common case and what is charged here.
//
// An ESTIMATE of allocations that certainly happened, not a guess at an unknown
// quantity — which is the line this rollup draws between what it reports and
// what it refuses to.
template <typename Map>
std::size_t map_bytes(const Map& m) {
    return m.bucket_count() * sizeof(void*) +
           m.size() * (sizeof(typename Map::value_type) + 2 * sizeof(void*));
}

}  // namespace clay
