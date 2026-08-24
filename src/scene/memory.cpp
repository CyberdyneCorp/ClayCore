#include "clay/scene/memory.h"

#include "clay/bytes.h"

namespace clay {
namespace scene {

namespace {

// True the first time this payload is seen, so a shared allocation is charged
// once per report. A null set means "no report is in progress": charge it.
bool first_sight(SharedSeen* seen, const void* p) {
    return !p ? false : (!seen || seen->insert(p).second);
}

}  // namespace

std::size_t node_bytes(const Node& n, SharedSeen* seen) {
    std::size_t b = sizeof(Node);
    b += vector_bytes(n.stroke);
    b += vector_bytes(n.children);
    // The armature binding: two parallel arrays, one entry per bound bone.
    b += vector_bytes(n.armature_parents);
    b += vector_bytes(n.armature_signs);

    // A deformer chain, and the two deformers that carry geometry of their own
    // — a bend guide and a lattice cage. These are the ones whose cost is not
    // sizeof, and a heavily deformed node is mostly this.
    b += vector_bytes(n.deformers);
    for (const Deformer& d : n.deformers) {
        b += vector_bytes(d.guide);
        b += vector_bytes(d.cage);
    }

    // Profiles, for swept and lofted prims. profile_polygons is a vector OF
    // vectors, so its capacity counts the outer handles and each inner polygon
    // must be walked — the one member here that sizeof gets wrong twice.
    b += vector_bytes(n.profile_points);
    b += vector_bytes(n.profiles);
    b += vector_bytes(n.profile_polygons);
    for (const auto& poly : n.profile_polygons) b += vector_bytes(poly);

    // The sampled volumes, which are usually the largest thing here by two
    // orders of magnitude and were entirely unaccounted before this. Shared, so
    // charged once per report.
    if (first_sight(seen, n.volume.get())) b += n.volume->bytes();
    if (first_sight(seen, n.gate.get())) b += n.gate->bytes();
    return b;
}

std::size_t document_bytes(const Document& doc) {
    std::size_t b = sizeof(Document);
    b += vector_bytes(doc.layers);
    b += vector_bytes(doc.selection);

    // Instance layers share one SdfContent. Counted once by address: the whole
    // point of an instance is that the content was not copied, and reporting it
    // per layer would tell a host to free memory that does not exist.
    // ONE set for the whole walk, so an instance's shared content and a volume
    // two nodes sample are each charged once.
    SharedSeen seen;
    for (const Layer& l : doc.layers) {
        b += l.name.capacity();
        const SdfContent* content = l.sdf.get();
        if (!first_sight(&seen, content)) continue;
        b += sizeof(SdfContent);
        b += vector_bytes(content->roots);
        b += map_bytes(content->nodes());
        // The map_bytes line above already counted sizeof(Node) per entry as
        // part of value_type, so only the heap hanging off each node remains.
        for (const auto& [id, node] : content->nodes())
            b += node_bytes(node, &seen) - sizeof(Node);
    }
    return b;
}

}  // namespace scene
}  // namespace clay
