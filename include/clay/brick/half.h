#pragma once

// Software float16 conversion (round-to-nearest-even), used for narrow-band
// brick storage. Deterministic bit-exact results on every platform — the
// locality guarantee's bit-identity tests depend on that, so no hardware
// F16C/NEON intrinsics here.

#include <cstdint>
#include <cstring>

namespace clay {
namespace brick {

inline std::uint16_t float_to_half(float f) {
    std::uint32_t x;
    std::memcpy(&x, &f, 4);
    std::uint32_t sign = (x >> 16) & 0x8000u;
    std::uint32_t em = x & 0x7FFFFFFFu;

    if (em >= 0x7F800000u) {  // inf/nan
        std::uint32_t mant = em > 0x7F800000u ? 0x0200u : 0u;
        return static_cast<std::uint16_t>(sign | 0x7C00u | mant);
    }
    if (em >= 0x47800000u) return static_cast<std::uint16_t>(sign | 0x7C00u);  // overflow -> inf
    if (em < 0x38800000u) {
        // subnormal half (or zero): shift with round-to-nearest-even
        std::uint32_t mant = (em & 0x007FFFFFu) | 0x00800000u;
        int shift = 113 - static_cast<int>(em >> 23);
        if (shift > 24) return static_cast<std::uint16_t>(sign);
        std::uint32_t v = mant >> shift;
        std::uint32_t rem = mant & ((1u << shift) - 1u);
        std::uint32_t half_ulp = 1u << (shift - 1);
        if (rem > half_ulp || (rem == half_ulp && (v & 1u))) ++v;
        return static_cast<std::uint16_t>(sign | v);
    }
    std::uint32_t e = ((em >> 23) - 112u) << 10;
    std::uint32_t m = (em >> 13) & 0x03FFu;
    std::uint32_t rem = em & 0x1FFFu;
    std::uint32_t v = sign | e | m;
    if (rem > 0x1000u || (rem == 0x1000u && (v & 1u))) ++v;
    return static_cast<std::uint16_t>(v);
}

inline float half_to_float(std::uint16_t h) {
    std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
    std::uint32_t em = h & 0x7FFFu;
    std::uint32_t x;
    if (em >= 0x7C00u) {  // inf/nan
        x = sign | 0x7F800000u | ((em & 0x03FFu) << 13);
    } else if (em >= 0x0400u) {  // normal
        x = sign | ((((em >> 10) + 112u) << 23) | ((em & 0x03FFu) << 13));
    } else if (em != 0) {  // subnormal
        std::uint32_t m = em;
        int e = -1;
        do {
            m <<= 1;
            ++e;
        } while (!(m & 0x0400u));
        x = sign | ((127u - 15u - static_cast<std::uint32_t>(e)) << 23) |
            ((m & 0x03FFu) << 13);
    } else {
        x = sign;  // zero
    }
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

}  // namespace brick
}  // namespace clay
