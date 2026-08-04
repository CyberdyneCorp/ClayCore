/* desc_version.h — the struct_size prefix rule of the C ABI (c-abi spec:
 * versioned descriptor structs).
 *
 * Internal to the bindings, not installed: it lives beside clay_c.cpp only so
 * the rule can be exercised with a descriptor whose original layout is
 * genuinely shorter than its current one. clay_mesh_params is now the first
 * shipped descriptor to have grown — the mesher fields were appended to it in
 * ABI 0.3.0 — and the synthetic pair here stays because the rule has to keep
 * working for the descriptors that have not.
 */

#ifndef CLAY_DESC_VERSION_H
#define CLAY_DESC_VERSION_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace clay_abi {

// A size no descriptor will ever reach. A caller declaring more than this is
// not naming a layout: it is passing a struct written before the convention,
// whose first word is a float or a small enum rather than a struct_size.
// Rejecting those is what keeps the library from reading past a shorter
// object it was never given.
inline constexpr std::size_t kMaxDescSize = 4096;

enum class DescStatus {
    Ok,
    TooShort,       // below the original layout: not a versioned descriptor
    NotADescriptor  // absurdly large: the first word is not a struct_size
};

// Copies the prefix the caller declared over a defaulted descriptor, so
// fields appended later keep their documented defaults and a tail this build
// does not know is ignored rather than misread. `original` is the field set
// the convention shipped with — the shortest layout that ever carried a
// struct_size, and so the shortest a caller may declare.
template <typename Desc>
DescStatus read_desc(const Desc* src, std::size_t original, Desc* out) {
    static_assert(std::is_trivially_copyable_v<Desc>);
    std::size_t declared = src->struct_size;
    if (declared < original) return DescStatus::TooShort;
    if (declared > kMaxDescSize) return DescStatus::NotADescriptor;
    if (declared > sizeof(Desc)) declared = sizeof(Desc);  // newer caller: ignore the tail
    *out = Desc{};
    std::memcpy(out, src, declared);
    out->struct_size = static_cast<std::uint32_t>(sizeof(Desc));
    return DescStatus::Ok;
}

}  // namespace clay_abi

#endif  // CLAY_DESC_VERSION_H
