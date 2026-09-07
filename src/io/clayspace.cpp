#include "clay/io/clayspace.h"

#include <cstdio>
#include <cstring>

#include "clay/io/mesh_io.h"
#include "clay/scene/commands.h"

#include "file_bytes.h"

namespace clay {
namespace io {

namespace {

// The scene module writes its payload at its own layout version; a reader is
// told the container's. They have to be the same number or a document would be
// decoded against the wrong layout.
static_assert(kClaySpaceMinor == scene::kSceneMinor,
              "the container minor and the scene payload layout version must move together");

constexpr std::uint32_t kMagic = 0x59414C43u;  // "CLAY" little-endian
constexpr std::uint32_t fourcc(const char (&s)[5]) {
    return static_cast<std::uint32_t>(s[0]) | (static_cast<std::uint32_t>(s[1]) << 8) |
           (static_cast<std::uint32_t>(s[2]) << 16) | (static_cast<std::uint32_t>(s[3]) << 24);
}
constexpr std::uint32_t kScene = fourcc("SCNE");
constexpr std::uint32_t kVoxel = fourcc("VOXL");
constexpr std::uint32_t kMask = fourcc("MASK");
constexpr std::uint32_t kMesh = fourcc("MESH");
constexpr std::uint32_t kMultires = fourcc("MRES");
constexpr std::uint32_t kGroups = fourcc("GRUP");
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

// A mesh chunk and its layer must match, both ways: a payload whose layer is
// gone is never written, and a chunk naming no mesh layer is dropped on load.
// That one rule is what makes an orphaned map entry harmless, and layer ids
// are monotonic so an orphan can never be captured by a later, different
// layer.
bool is_mesh_layer(const scene::Document& document, scene::LayerId id) {
    const scene::Layer* layer = document.find_layer(id);
    return layer && layer->kind == scene::LayerKind::Mesh;
}

// Its own function, not a branch inline, so the mesh reader's refusals keep
// the detail they were written with rather than collapsing into one message.
IoStatus read_mesh_chunk(const std::uint8_t* payload, std::size_t len, ClaySpaceDoc* out) {
    if (len < 4) return IoStatus::fail(IoError::Malformed, "mesh chunk too small");
    std::uint32_t layer_id = 0;
    std::memcpy(&layer_id, payload, 4);
    mesh::Mesh m;
    IoStatus s = load_mesh_stream(payload + 4, len - 4, &m);
    if (!s.ok()) return s;
    // FIRST CHUNK WINS, which is the emplace this replaced and what the voxel,
    // mask and hierarchy readers below each do: a file naming one layer twice
    // is malformed, and letting a trailing chunk override geometry already read
    // is the worse of the two answers.
    if (out->mesh_layers.count(layer_id)) return IoStatus::success();
    // Through the installer, like every other path that puts triangles into a
    // layer, so a loaded document comes up with a generation per mesh layer
    // rather than with an empty map every reader would have to default.
    out->install_mesh_geometry(layer_id, std::move(m));
    return IoStatus::success();
}

// The voxel half of the same rule. It was deferred when the mesh rule landed
// (add-mesh-layers task 7.7 — "the mesh rule makes the inconsistency visible
// and a follow-up can close it") because a voxel layer could not be removed by
// anything that left the grid behind. Recording the CREATION changed that
// (#341): undoing a crossing now removes a voxel layer and deliberately keeps
// its cells, so that a redo can pick them back up, and an orphaned grid became
// an ordinary state rather than an unreachable one.
//
// Without this the orphan reaches the file, and on load the ids are no longer
// monotonic across the gap: deserialize_document derives next_layer_id_ from
// the layers PRESENT, so the next voxel layer created can take the missing id
// and come up holding a dead sculpt.
bool is_voxel_layer(const scene::Document& document, scene::LayerId id) {
    const scene::Layer* layer = document.find_layer(id);
    return layer && layer->kind == scene::LayerKind::Voxel;
}

void drop_unmatched_voxel_chunks(ClaySpaceDoc* out) {
    for (auto it = out->voxel_layers.begin(); it != out->voxel_layers.end();) {
        if (is_voxel_layer(out->document, it->first))
            ++it;
        else
            it = out->voxel_layers.erase(it);
    }
}

void drop_unmatched_mesh_chunks(ClaySpaceDoc* out) {
    for (auto it = out->mesh_layers.begin(); it != out->mesh_layers.end();) {
        if (is_mesh_layer(out->document, it->first)) {
            ++it;
        } else {
            // The generation goes with the triangles it counted. Layer ids are
            // monotonic so a dropped id can never be captured by a later
            // layer, but leaving a count behind for geometry that is gone
            // would hand the next reader a number describing nothing.
            out->mesh_geometry_revision.erase(it->first);
            it = out->mesh_layers.erase(it);
        }
    }
}

// The same rule for a hierarchy, on the same test its cage takes: a chunk
// naming a layer this document no longer holds is dropped rather than kept as a
// hierarchy nothing can reach.
void drop_unmatched_multires_chunks(ClaySpaceDoc* out) {
    for (auto it = out->multires_layers.begin(); it != out->multires_layers.end();) {
        if (is_mesh_layer(out->document, it->first))
            ++it;
        else
            it = out->multires_layers.erase(it);
    }
}

}  // namespace

bool multires_carries_detail(const mesh::MultiresSurface& surface) {
    // Levels above the cage, OR any sculpt layer. The second half is not
    // redundant: a base deformation layer writes at level 0, so a hierarchy of
    // one level can still hold work an artist did.
    return surface.level_count() > 1 || !surface.sculpt_layers().empty();
}

bool multires_matches_cage(const mesh::Mesh& cage, const mesh::MultiresSurface& surface) {
    const mesh::Mesh& base = surface.base_mesh();
    // Counts first. A retopology, a decimation and a re-import are how this
    // actually happens and every one of them changes a count, so the expensive
    // comparison is reached only by a cage that was edited in place.
    if (base.positions.size() != cage.positions.size()) return false;
    if (base.indices.size() != cage.indices.size()) return false;
    if (base.quads.size() != cage.quads.size()) return false;
    if (base.indices != cage.indices) return false;
    if (base.quads != cage.quads) return false;
    for (std::size_t i = 0; i < base.positions.size(); ++i) {
        const kernel::cfloat3& a = base.positions[i];
        const kernel::cfloat3& b = cage.positions[i];
        // Exact, not tolerant. The question is "is this the same cage", and a
        // tolerance would answer "near enough to what?" with a number nobody
        // chose -- the same refusal clay_consolidation_params makes about
        // cell_size. A host wanting a tolerant compare has both meshes.
        if (a.x != b.x || a.y != b.y || a.z != b.z) return false;
    }
    return true;
}

std::uint64_t snapshot_identity(const std::uint8_t* data, std::size_t size) {
    // EIGHT BYTES AT A TIME, and that is a measurement rather than a
    // preference. The obvious implementation is the byte-at-a-time FNV-1a
    // session/layer_digest.h uses, and on a 1.13 MB snapshot (5 000 items and
    // a 32^3 fill) it costs 1.36 ms against the 1.52 ms save that produced
    // those bytes — 90% ON TOP OF EVERY SAVE, paid by every host including the
    // ones that never journal. Over 64-bit words the same idea runs at 4.7
    // GB/s against 0.83, which is 0.24 ms and 15% of the save.
    //
    // The length is folded in at the start so a truncated snapshot cannot hash
    // like the whole one, and the murmur3 finalizer is what makes a word's
    // worth of change reach every output bit — FNV's per-byte multiply does
    // that job in the byte-at-a-time form and nothing does it in this one.
    //
    // Byte order is NOT normalised: a recovery pair is produced and consumed
    // on one machine, has never been portable, and paying a per-byte assembly
    // to make an id travel would give back the whole win above.
    constexpr std::uint64_t kPrime = 1099511628211ull;
    std::uint64_t h = 1469598103934665603ull ^ (size * 0x9e3779b97f4a7c15ull);
    const auto finalize = [](std::uint64_t x) {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdull;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ull;
        x ^= x >> 33;
        return x;
    };
    std::size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        std::uint64_t w = 0;
        std::memcpy(&w, data + i, 8);
        h = (h ^ finalize(w)) * kPrime;
    }
    std::uint64_t tail = 0;
    for (std::size_t k = 0; i + k < size; ++k)
        tail |= static_cast<std::uint64_t>(data[i + k]) << (8 * k);
    h = (h ^ finalize(tail)) * kPrime;
    // Zero MEANS "names no snapshot", so the one hash that would be read as
    // absence is nudged. One value in 2^64 loses a little distinctness; the
    // alternative is a snapshot that silently opts out of the check.
    return h == 0 ? 1ull : h;
}

std::vector<std::uint8_t> save_clayspace(const ClaySpaceDoc& doc) {
    std::vector<std::uint8_t> out;
    put_u32(out, kMagic);
    put_u16(out, kClaySpaceMajor);
    put_u16(out, kClaySpaceMinor);

    put_chunk(out, kScene, scene::serialize_document(doc.document));
    for (const auto& [layer_id, grid] : doc.voxel_layers) {
        if (!is_voxel_layer(doc.document, layer_id)) continue;
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
    for (const auto& [layer_id, m] : doc.mesh_layers) {
        if (!is_mesh_layer(doc.document, layer_id)) continue;
        std::vector<std::uint8_t> payload;
        put_u32(payload, layer_id);
        std::vector<std::uint8_t> mesh_bytes = save_mesh_stream(m);
        payload.insert(payload.end(), mesh_bytes.begin(), mesh_bytes.end());
        put_chunk(out, kMesh, payload);
    }
    // A mesh layer's hierarchy, gated on the same "is it still that kind of
    // layer" test its cage is, so an orphan left by an undone removal is held in
    // memory and not written. The surface encodes itself; nothing here adds a
    // second version to negotiate.
    for (const auto& [layer_id, surface] : doc.multires_layers) {
        if (!is_mesh_layer(doc.document, layer_id)) continue;
        std::vector<std::uint8_t> payload;
        put_u32(payload, layer_id);
        std::vector<std::uint8_t> surface_bytes = surface.encode();
        payload.insert(payload.end(), surface_bytes.begin(), surface_bytes.end());
        put_chunk(out, kMultires, payload);
    }
    // Surface groups: ONE chunk for the document, not one per layer, because
    // the lattice is per document. Skipped entirely when nothing is named, so a
    // document that never used the feature is byte-identical to what it was.
    //
    // A field with no ids but a hidden entry cannot occur — reassigning a group
    // away takes its visibility with it — so `empty()` is the whole test.
    if (doc.groups && !doc.groups->empty()) put_chunk(out, kGroups, doc.groups->serialize());
    if (!doc.thumbnail_png.empty()) put_chunk(out, kThumb, doc.thumbnail_png);
    if (!doc.camera_bookmarks.empty()) put_chunk(out, kCamera, doc.camera_bookmarks);
    // The document now knows which bytes it was last written to, so a journal
    // taken after this names THIS snapshot (survive-a-crash 2.1). Stamped here
    // rather than in each binding because a save path that forgot to stamp
    // would leave the journal naming an older snapshot, and the host would get
    // a refusal on a pair that was in fact correct.
    doc.document.snapshot_id = snapshot_identity(out.data(), out.size());
    return out;
}

IoStatus decode_document(const std::uint8_t* data, std::size_t size, scene::Document* out,
                         std::uint16_t minor) {
    std::optional<scene::Document> doc = scene::deserialize_document(data, size, minor);
    if (!doc) return IoStatus::fail(IoError::Malformed, "scene chunk parse failed");
    *out = std::move(*doc);
    return IoStatus::success();
}

IoStatus load_clayspace(const std::uint8_t* data, std::size_t size, ClaySpaceDoc* out) {
    Cursor c{data, size};
    if (c.u32() != kMagic) return IoStatus::fail(IoError::Malformed, "bad magic");
    std::uint16_t major = c.u16();
    std::uint16_t minor = c.u16();
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
                                         &result.document, minor);
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
        } else if (cc == kMesh) {
            IoStatus s = read_mesh_chunk(payload, static_cast<std::size_t>(len), &result);
            if (!s.ok()) return s;
        } else if (cc == kMultires) {
            if (len < 4) return IoStatus::fail(IoError::Malformed, "multires chunk too short");
            std::uint32_t layer_id = 0;
            std::memcpy(&layer_id, payload, 4);
            mesh::MultiresSurface surface;
            if (!mesh::MultiresSurface::decode(payload + 4, static_cast<std::size_t>(len) - 4,
                                               &surface))
                return IoStatus::fail(IoError::Malformed, "multires chunk parse failed");
            result.multires_layers.emplace(layer_id, std::move(surface));
        } else if (cc == kGroups) {
            auto groups = voxel::GroupField::deserialize(payload, static_cast<std::size_t>(len));
            if (!groups) return IoStatus::fail(IoError::Malformed, "group chunk parse failed");
            result.groups = std::move(*groups);
        } else if (cc == kThumb) {
            result.thumbnail_png.assign(payload, payload + len);
        } else if (cc == kCamera) {
            result.camera_bookmarks.assign(payload, payload + len);
        }
        // unknown chunks skipped: backward-open
    }
    if (!c.ok) return IoStatus::fail(IoError::Malformed, "truncated stream");
    if (!have_scene) return IoStatus::fail(IoError::Malformed, "missing scene chunk");
    // After the loop rather than in the branch: chunk order is the writer's
    // business, and the scene chunk a mesh chunk is matched against may not
    // have been read yet.
    drop_unmatched_mesh_chunks(&result);
    drop_unmatched_multires_chunks(&result);
    drop_unmatched_voxel_chunks(&result);
    // The other half of the pairing: a document loaded from a snapshot knows
    // which one it is, so replay can refuse a journal taken against a
    // different one. Set only on success — a document that failed to load is
    // not a snapshot of anything.
    result.document.snapshot_id = snapshot_identity(data, size);
    *out = std::move(result);
    return IoStatus::success();
}

IoStatus save_clayspace_file(const ClaySpaceDoc& doc, const std::string& path) {
    return detail::write_whole_file(path, save_clayspace(doc));
}

IoStatus load_clayspace_file(const std::string& path, ClaySpaceDoc* out,
                             const ImportBudget& budget) {
    std::vector<std::uint8_t> bytes;
    IoStatus s = detail::read_whole_file(path, &bytes, budget.max_file_bytes);
    if (!s.ok()) return s;
    return load_clayspace(bytes.data(), bytes.size(), out);
}

}  // namespace io
}  // namespace clay
