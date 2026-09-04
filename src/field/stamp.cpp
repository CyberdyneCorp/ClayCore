#include "clay/field/stamp.h"

#include <cstring>

#include "clay/kernel/deform.h"

namespace clay::field {

using kernel::cf3;
using kernel::cfloat3;

namespace {

// The stamp file's own header, so a truncated or foreign buffer is refused
// rather than read as samples.
constexpr std::uint32_t kStampMagic = 0x504D5453u;  // 'STMP' little-endian
constexpr std::uint32_t kStampVersion = 1u;

// FNV-1a over the payload. Not a cryptographic hash and not trying to be: it
// names content so a host can recognise the same capture twice, and a collision
// costs a host showing two assets as one in a LIST -- nothing is dispatched on
// it, and nothing is deduplicated by it either, because the payload sharing is
// pointer identity (write-a-shared-payload-once) rather than content equality.
std::uint64_t fnv1a(const std::uint8_t* data, std::size_t size) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < size; ++i) {
        h ^= static_cast<std::uint64_t>(data[i]);
        h *= 1099511628211ull;
    }
    return h;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu));
}

void put64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu));
}

void putf(std::vector<std::uint8_t>& out, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    put32(out, bits);
}

bool take32(const std::uint8_t*& p, const std::uint8_t* end, std::uint32_t* out) {
    if (static_cast<std::size_t>(end - p) < 4) return false;
    *out = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    p += 4;
    return true;
}

bool take64(const std::uint8_t*& p, const std::uint8_t* end, std::uint64_t* out) {
    if (static_cast<std::size_t>(end - p) < 8) return false;
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (i * 8);
    p += 8;
    *out = v;
    return true;
}

bool takef(const std::uint8_t*& p, const std::uint8_t* end, float* out) {
    std::uint32_t bits = 0;
    if (!take32(p, end, &bits)) return false;
    std::memcpy(out, &bits, sizeof *out);
    return true;
}

}  // namespace

math::Transform stamp_frame_transform(const StampFrame& frame) {
    cfloat3 n = cf3(0, 0, 1), t = cf3(1, 0, 0), b = cf3(0, 1, 0);
    // THE SAME resolution the scalar alpha uses, fallback included: an asset and
    // an alpha placed at one hit must not disagree about which way is up.
    kernel::calpha_frame(frame.normal, frame.tangent, CLAY_OUTARG(n), CLAY_OUTARG(t),
                         CLAY_OUTARG(b));
    math::Transform out;
    // x -> tangent, y -> bitangent, z -> normal. +Z outward is the alpha's
    // convention and the one the header states.
    out.rotation = math::Quat::from_basis(t, b, n);
    out.position = frame.origin;
    out.scale = 1.0f;
    return out;
}

StampFrame stamp_frame_from_surface(cfloat3 hit, cfloat3 normal, float azimuth) {
    StampFrame frame;
    frame.origin = hit;
    frame.normal = normal;
    // A tangent turned about the normal by the azimuth. Built by resolving the
    // frame once with an arbitrary tangent and then rotating that one, so the
    // azimuth is measured from the SAME reference `calpha_frame`'s fallback
    // picks -- otherwise "azimuth 0" would mean a different direction depending
    // on which axis the normal happened to lean on least.
    cfloat3 n = cf3(0, 0, 1), t = cf3(1, 0, 0), b = cf3(0, 1, 0);
    kernel::calpha_frame(normal, cf3(0, 0, 0), CLAY_OUTARG(n), CLAY_OUTARG(t), CLAY_OUTARG(b));
    const float c = kernel::ccos(azimuth), s = kernel::csin(azimuth);
    frame.tangent = t * c + b * s;
    return frame;
}

math::Aabb FieldStamp::local_bounds() const {
    if (!volume) return math::Aabb{};
    return volume->bounds();
}

std::size_t FieldStamp::payload_bytes() const { return volume ? volume->bytes() : 0u; }

std::uint64_t stamp_content_id(const FieldVolume& volume) {
    const std::vector<std::uint8_t> bytes = volume.serialize();
    if (bytes.empty()) return 0;
    return fnv1a(bytes.data(), bytes.size());
}

std::vector<std::uint8_t> stamp_serialize(const FieldStamp& stamp) {
    std::vector<std::uint8_t> out;
    if (!stamp.volume) return out;
    const std::vector<std::uint8_t> payload = stamp.volume->serialize();
    if (payload.empty()) return out;

    put32(out, kStampMagic);
    put32(out, kStampVersion);
    // The placement FIRST, and it is why this is a stamp file rather than a
    // volume file: a payload reloaded without it is an asset that has forgotten
    // which way it faced.
    //
    // The QUATERNION, not the normal and tangent it was built from. Storing
    // those and re-resolving them runs the frame back through `calpha_frame`
    // and a basis-to-quaternion conversion, and the rounding in that is
    // observable: measured before this changed, every sample of a reloaded
    // stamp differed from the original in the last ulp.
    const float f[8] = {stamp.placement.position.x, stamp.placement.position.y,
                        stamp.placement.position.z, stamp.placement.rotation.x,
                        stamp.placement.rotation.y, stamp.placement.rotation.z,
                        stamp.placement.rotation.w, stamp.placement.scale};
    for (float v : f) putf(out, v);
    put64(out, stamp.content_id);
    put64(out, static_cast<std::uint64_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::optional<FieldStamp> stamp_deserialize(const std::uint8_t* data, std::size_t size) {
    if (!data) return std::nullopt;
    const std::uint8_t* p = data;
    const std::uint8_t* end = data + size;

    std::uint32_t magic = 0, version = 0;
    if (!take32(p, end, &magic) || magic != kStampMagic) return std::nullopt;
    if (!take32(p, end, &version) || version != kStampVersion) return std::nullopt;

    float f[8] = {};
    for (float& v : f)
        if (!takef(p, end, &v)) return std::nullopt;

    FieldStamp stamp;
    stamp.placement.position = cf3(f[0], f[1], f[2]);
    stamp.placement.rotation = math::Quat{f[3], f[4], f[5], f[6]};
    stamp.placement.scale = f[7];

    std::uint64_t bytes = 0;
    if (!take64(p, end, &stamp.content_id)) return std::nullopt;
    if (!take64(p, end, &bytes)) return std::nullopt;
    // Checked against what is ACTUALLY there rather than trusted, so a
    // truncated file is refused instead of read past its end.
    if (static_cast<std::uint64_t>(end - p) < bytes) return std::nullopt;

    std::optional<FieldVolume> volume =
        FieldVolume::deserialize(p, static_cast<std::size_t>(bytes));
    if (!volume) return std::nullopt;
    stamp.volume = std::make_shared<const FieldVolume>(std::move(*volume));
    return stamp;
}

}  // namespace clay::field
