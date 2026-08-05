#pragma once

// .clayspace document container (file-io spec): binary chunked format.
//
//   magic "CLAY" | u16 major | u16 minor | chunks...
//   chunk: fourcc u32 | u64 payload size | payload
//
// Chunks: 'SCNE' scene command/document payload, 'VOXL' one voxel layer
// (u32 layer id + VoxelGrid stream), 'MASK' one layer's mask field (u32 layer
// id + MaskField stream), 'THMB' thumbnail bytes (PNG,
// passthrough), 'CAMB' camera bookmarks (passthrough). Unknown chunks are
// skipped (backward-open); a higher major version refuses to load
// (forward-refuse) with no partial document.
//
// Minor 1 packs a layer's ghost and lock flags into the byte its visibility
// flag already occupied. A minor-0 document therefore loads with both off,
// with no version handling; a minor-1 document read by a build that predates
// the flags loses them, and a layer that is both hidden and ghosted reads as
// visible there. That is the whole extent of the incompatibility, and it is
// why the minor moved.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "clay/io/result.h"
#include "clay/scene/document.h"
#include "clay/voxel/grid.h"
#include "clay/voxel/mask.h"

namespace clay {
namespace io {

inline constexpr std::uint16_t kClaySpaceMajor = 1;
inline constexpr std::uint16_t kClaySpaceMinor = 1;

// The document bundle a .clayspace file holds. Voxel layer content is keyed
// by layer id (the scene module stays voxel-agnostic by layering rule).
struct ClaySpaceDoc {
    scene::Document document;
    std::map<scene::LayerId, voxel::VoxelGrid> voxel_layers;
    // Masks sit beside voxel content rather than inside scene::Document, so a
    // mask's presence cannot change what the document evaluates to — the
    // structural version of "masking gates authoring, not evaluation".
    std::map<scene::LayerId, voxel::MaskField> masks;
    std::vector<std::uint8_t> thumbnail_png;      // optional passthrough
    std::vector<std::uint8_t> camera_bookmarks;   // optional passthrough
};

std::vector<std::uint8_t> save_clayspace(const ClaySpaceDoc& doc);
IoStatus load_clayspace(const std::uint8_t* data, std::size_t size, ClaySpaceDoc* out);

IoStatus save_clayspace_file(const ClaySpaceDoc& doc, const std::string& path);
IoStatus load_clayspace_file(const std::string& path, ClaySpaceDoc* out);

// Scene payload codec shared with the command vocabulary (scene chunk =
// serialize_document; exposed for tests and the C ABI).
IoStatus decode_document(const std::uint8_t* data, std::size_t size, scene::Document* out);

}  // namespace io
}  // namespace clay
