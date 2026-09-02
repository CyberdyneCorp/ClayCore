#include "clay/io/memory.h"

#include "clay/bytes.h"
#include "clay/scene/memory.h"

namespace clay {
namespace io {

namespace {

// The one place the fields are summed. Every entry point calls it, so a field
// added to MemoryReport without being listed here shows up as a total that no
// longer matches the parts — which is what test 6.2 asserts by summing rather
// than by restating the number.
void finish(MemoryReport* r) {
    r->total = r->edit_list + r->voxel_content + r->mesh_layers + r->masks +
               r->voxel_sculpt_layers + r->history + r->passthrough + r->transient +
               r->surface_content + r->multires_detail + r->sculpt_layers +
               r->surface_caches + r->surface_scratch + r->surface_undo;
}

void add_voxel(MemoryReport* r, const voxel::VoxelGrid& g) {
    r->voxel_content += g.content_bytes();
    r->voxel_sculpt_layers += g.sculpt_layer_total_bytes();
    ++r->voxel_layers;
}

void add_mask(MemoryReport* r, const voxel::MaskField& m) {
    r->masks += m.content_bytes();
    r->transient += m.step_snapshot_bytes();
    ++r->mask_count;
}

void add_mesh(MemoryReport* r, const mesh::Mesh& m) {
    r->mesh_layers += m.bytes();
    ++r->mesh_layer_count;
}

// The surface tier, from the ledger the host filled. One place, so the mapping
// from a category to a report line is written once and the two cannot drift.
void add_surfaces(MemoryReport* r, const memory::MemoryLedger& ledger) {
    using memory::MemoryCategory;
    r->surface_content += ledger.of(MemoryCategory::BaseGeometry) +
                          ledger.of(MemoryCategory::Topology) + ledger.of(MemoryCategory::Masks);
    r->multires_detail += ledger.of(MemoryCategory::MultiresDetail);
    r->sculpt_layers += ledger.of(MemoryCategory::SculptLayers);
    r->surface_caches += ledger.of(MemoryCategory::ChunkIndex) +
                         ledger.of(MemoryCategory::EvaluatedCache) +
                         ledger.of(MemoryCategory::LevelRuntimeCache) +
                         ledger.of(MemoryCategory::LayerEvalCache) +
                         ledger.of(MemoryCategory::DerivedPositions);
    r->surface_scratch +=
        ledger.of(MemoryCategory::Scratch) + ledger.of(MemoryCategory::PreviewStaging);
    r->surface_undo += ledger.of(MemoryCategory::UndoHistory);
}

}  // namespace

std::size_t MemoryReport::essential() const {
    return edit_list + voxel_content + mesh_layers + masks + surface_content + multires_detail +
           sculpt_layers;
}

std::size_t MemoryReport::rebuildable() const {
    // `transient` is here rather than in `essential` because it is a stroke's
    // copy-on-write shadow: it is going away when the gesture ends whether or
    // not anybody releases it.
    return surface_caches + surface_scratch + passthrough + transient;
}

std::size_t MemoryReport::undoable() const {
    return history + voxel_sculpt_layers + surface_undo;
}

MemoryReport document_memory(const ClaySpaceDoc& doc, const session::History* history,
                             const memory::MemoryLedger* surfaces) {
    MemoryReport r;
    r.edit_list = scene::document_bytes(doc.document);
    for (const auto& [id, grid] : doc.voxel_layers) add_voxel(&r, grid);
    for (const auto& [id, mask] : doc.masks) add_mask(&r, mask);
    for (const auto& [id, m] : doc.mesh_layers) add_mesh(&r, m);
    if (history) r.history = history->bytes().total;
    r.passthrough = vector_bytes(doc.thumbnail_png) + vector_bytes(doc.camera_bookmarks);
    if (surfaces != nullptr) add_surfaces(&r, *surfaces);
    finish(&r);
    return r;
}

bool layer_memory(const ClaySpaceDoc& doc, scene::LayerId layer, MemoryReport* out) {
    if (!out) return false;
    const scene::Layer* l = doc.document.find_layer(layer);
    if (!l) return false;

    MemoryReport r;
    // The edit list is per layer for an SDF layer: its own record, its name and
    // the content it points at. An INSTANCE reports its source's content in
    // full, which is the one place the per-layer view and the document view
    // deliberately disagree — the document counts shared content once, and a
    // per-layer figure that reported zero for an instance would tell a host the
    // layer is free when displaying it costs an evaluation like any other.
    r.edit_list = sizeof(scene::Layer) + l->name.capacity();
    if (l->sdf) {
        // A set scoped to THIS layer: two of its own nodes sampling one volume
        // is one allocation, while the same volume in another layer is that
        // layer's business — which is the per-layer view this call exists for.
        scene::SharedSeen seen;
        r.edit_list += sizeof(scene::SdfContent);
        r.edit_list += vector_bytes(l->sdf->roots);
        r.edit_list += map_bytes(l->sdf->nodes());
        for (const auto& [id, node] : l->sdf->nodes())
            r.edit_list += scene::node_bytes(node, &seen) - sizeof(scene::Node);
    }

    // Voxel, mask and mesh content are keyed by layer id, so a layer has at
    // most one of each and a lookup miss simply contributes nothing.
    auto grid = doc.voxel_layers.find(layer);
    if (grid != doc.voxel_layers.end()) add_voxel(&r, grid->second);
    auto mask = doc.masks.find(layer);
    if (mask != doc.masks.end()) add_mask(&r, mask->second);
    auto m = doc.mesh_layers.find(layer);
    if (m != doc.mesh_layers.end()) add_mesh(&r, m->second);

    // history and passthrough stay zero: both are document-wide, and this is
    // what makes the layers plus the document-wide lines reconstruct the total.
    finish(&r);
    *out = r;
    return true;
}

}  // namespace io
}  // namespace clay
