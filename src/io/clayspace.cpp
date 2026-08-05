#include "clay/io/clayspace.h"

#include <cstdio>
#include <cstring>

#include "clay/scene/commands.h"

namespace clay {
namespace io {

namespace {

constexpr std::uint32_t kMagic = 0x59414C43u;  // "CLAY" little-endian
constexpr std::uint32_t fourcc(const char (&s)[5]) {
    return static_cast<std::uint32_t>(s[0]) | (static_cast<std::uint32_t>(s[1]) << 8) |
           (static_cast<std::uint32_t>(s[2]) << 16) | (static_cast<std::uint32_t>(s[3]) << 24);
}
constexpr std::uint32_t kScene = fourcc("SCNE");
constexpr std::uint32_t kVoxel = fourcc("VOXL");
constexpr std::uint32_t kMask = fourcc("MASK");
constexpr std::uint32_t kThumb = fourcc("THMB");
constexpr std::uint32_t kCamera = fourcc("CAMB");

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}
void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}
void put_chunk(std::vector<std::uint8_t>& out, std::uint32_t cc,
               const std::vector<std::uint8_t>& payload) {
    put_u32(out, cc);
    put_u64(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

struct Cursor {
    const std::uint8_t* p;
    std::size_t remaining;
    bool ok = true;
    std::uint16_t u16() {
        if (remaining < 2) {
            ok = false;
            return 0;
        }
        std::uint16_t v = static_cast<std::uint16_t>(p[0] | (p[1] << 8));
        p += 2;
        remaining -= 2;
        return v;
    }
    std::uint32_t u32() {
        if (remaining < 4) {
            ok = false;
            return 0;
        }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p[i]) << (i * 8);
        p += 4;
        remaining -= 4;
        return v;
    }
    std::uint64_t u64() {
        if (remaining < 8) {
            ok = false;
            return 0;
        }
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (i * 8);
        p += 8;
        remaining -= 8;
        return v;
    }
};

}  // namespace

std::vector<std::uint8_t> save_clayspace(const ClaySpaceDoc& doc) {
    std::vector<std::uint8_t> out;
    put_u32(out, kMagic);
    put_u16(out, kClaySpaceMajor);
    put_u16(out, kClaySpaceMinor);

    put_chunk(out, kScene, scene::serialize_document(doc.document));
    for (const auto& [layer_id, grid] : doc.voxel_layers) {
        std::vector<std::uint8_t> payload;
        put_u32(payload, layer_id);
        std::vector<std::uint8_t> grid_bytes = grid.serialize();
        payload.insert(payload.end(), grid_bytes.begin(), grid_bytes.end());
        put_chunk(out, kVoxel, payload);
    }
    for (const auto& [layer_id, mask] : doc.masks) {
        if (mask.empty()) continue;
        std::vector<std::uint8_t> payload;
        put_u32(payload, layer_id);
        std::vector<std::uint8_t> mask_bytes = mask.serialize();
        payload.insert(payload.end(), mask_bytes.begin(), mask_bytes.end());
        put_chunk(out, kMask, payload);
    }
    if (!doc.thumbnail_png.empty()) put_chunk(out, kThumb, doc.thumbnail_png);
    if (!doc.camera_bookmarks.empty()) put_chunk(out, kCamera, doc.camera_bookmarks);
    return out;
}

IoStatus decode_document(const std::uint8_t* data, std::size_t size, scene::Document* out) {
    std::optional<scene::Document> doc = scene::deserialize_document(data, size);
    if (!doc) return IoStatus::fail(IoError::Malformed, "scene chunk parse failed");
    *out = std::move(*doc);
    return IoStatus::success();
}

IoStatus load_clayspace(const std::uint8_t* data, std::size_t size, ClaySpaceDoc* out) {
    Cursor c{data, size};
    if (c.u32() != kMagic) return IoStatus::fail(IoError::Malformed, "bad magic");
    std::uint16_t major = c.u16();
    std::uint16_t minor = c.u16();
    (void)minor;
    if (!c.ok) return IoStatus::fail(IoError::Malformed, "truncated header");
    if (major > kClaySpaceMajor)
        return IoStatus::fail(IoError::ForwardVersion,
                              "file requires a newer claycore (major " + std::to_string(major) +
                                  ")");

    ClaySpaceDoc result;
    bool have_scene = false;
    while (c.ok && c.remaining > 0) {
        std::uint32_t cc = c.u32();
        std::uint64_t len = c.u64();
        if (!c.ok || len > c.remaining)
            return IoStatus::fail(IoError::Malformed, "truncated chunk");
        const std::uint8_t* payload = c.p;
        c.p += len;
        c.remaining -= static_cast<std::size_t>(len);

        if (cc == kScene) {
            IoStatus s = decode_document(payload, static_cast<std::size_t>(len),
                                         &result.document);
            if (!s.ok()) return s;
            have_scene = true;
        } else if (cc == kVoxel) {
            if (len < 4) return IoStatus::fail(IoError::Malformed, "voxel chunk too small");
            std::uint32_t layer_id = 0;
            std::memcpy(&layer_id, payload, 4);
            auto grid = voxel::VoxelGrid::deserialize(payload + 4,
                                                      static_cast<std::size_t>(len) - 4);
            if (!grid) return IoStatus::fail(IoError::Malformed, "voxel chunk parse failed");
            result.voxel_layers.emplace(layer_id, std::move(*grid));
        } else if (cc == kMask) {
            if (len < 4) return IoStatus::fail(IoError::Malformed, "mask chunk too small");
            std::uint32_t layer_id = 0;
            std::memcpy(&layer_id, payload, 4);
            auto mask = voxel::MaskField::deserialize(payload + 4,
                                                      static_cast<std::size_t>(len) - 4);
            if (!mask) return IoStatus::fail(IoError::Malformed, "mask chunk parse failed");
            result.masks.emplace(layer_id, std::move(*mask));
        } else if (cc == kThumb) {
            result.thumbnail_png.assign(payload, payload + len);
        } else if (cc == kCamera) {
            result.camera_bookmarks.assign(payload, payload + len);
        }
        // unknown chunks skipped: backward-open
    }
    if (!c.ok) return IoStatus::fail(IoError::Malformed, "truncated stream");
    if (!have_scene) return IoStatus::fail(IoError::Malformed, "missing scene chunk");
    *out = std::move(result);
    return IoStatus::success();
}

IoStatus save_clayspace_file(const ClaySpaceDoc& doc, const std::string& path) {
    std::vector<std::uint8_t> bytes = save_clayspace(doc);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return IoStatus::fail(IoError::WriteFailed, path);
    std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (written != bytes.size()) return IoStatus::fail(IoError::WriteFailed, path);
    return IoStatus::success();
}

IoStatus load_clayspace_file(const std::string& path, ClaySpaceDoc* out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return IoStatus::fail(IoError::FileNotFound, path);
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(f);
        return IoStatus::fail(IoError::ReadFailed, path);
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::size_t read = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (read != bytes.size()) return IoStatus::fail(IoError::ReadFailed, path);
    return load_clayspace(bytes.data(), bytes.size(), out);
}

}  // namespace io
}  // namespace clay
