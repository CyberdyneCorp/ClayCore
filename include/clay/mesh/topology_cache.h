#pragma once

// Sharing an `Adjacency` between the sculptors over one mesh
// (share-mesh-topology-cache).
//
// WHAT THIS IS FOR, in one measurement. On a 384x384 plane — 148,225 vertices,
// 294,912 triangles, the size the roadmap already records a construction split
// for — `MeshSculptor(mesh)` costs 120.8 ms and a SECOND one over the same
// unchanged triangles costs 123.6 ms. `Adjacency::build` is 120.2 ms of that;
// everything else a fresh sculptor owns is an empty vector. Two sessions on one
// layer, or a sculptor destroyed and recreated between strokes, pay the weld
// and the two CSR sorts again for a partition that did not change.
//
// -- WHY THE ENTRY IS VERIFIED AND NOT TRUSTED --------------------------------
//
// The obvious key is (layer identity, the layer's geometry revision), and it is
// one bug away from serving a stale adjacency over replaced triangles — which
// is exactly what #472 reports about that revision: it does not move when
// history replaces a layer's triangles. A cache keyed on a counter inherits
// every defect in every path that should have bumped it, and the failure is
// silent: a sculptor with a wrong adjacency does not crash, it moves the wrong
// vertices.
//
// So a hit is CHECKED against the mesh it is about to be served for, through a
// `TopologyFingerprint` — counts, weld epsilon and a hash of the index buffer.
// Measured at 0.25 ms against the 120 ms it decides whether to skip, which is a
// rounding error on the thing it protects.
//
// `forget` still exists and the document still calls it on replacement. The two
// are not alternatives: `forget` is the intentional, free invalidation, and the
// fingerprint is the net under it.
//
// -- WHAT IT DELIBERATELY DOES NOT INVALIDATE ON ------------------------------
//
// POSITIONS MOVING. `Adjacency` pins its weld partition at build time and
// `adjacency.h` has always said positions may move under it freely — a live
// sculptor already behaves this way across a whole stroke. Serving a cached
// entry after a sculpt therefore hands a NEW sculptor exactly what a LIVE one
// would have been holding, which is the property that makes this a cache and
// not a behaviour change.
//
// It is still a real semantic and it is stated rather than hidden: a vertex
// dragged out of a coincidence it was welded into stays in its class, where a
// rebuild would have separated it. A wholesale geometry replacement — the path
// that changes what the artist means by "this mesh" — calls `forget`.
//
// A fingerprint over positions was rejected for the reason that would make it
// wrong rather than expensive: it would invalidate on every stroke, which is
// the one case this exists to serve.
//
// -- OWNERSHIP ----------------------------------------------------------------
//
// A DOCUMENT OWNS ONE. Never a process-global map, and the reasons are the
// ordinary ones and one that is not: a document close releases it, a trim can
// see it, two documents cannot collide on one layer id — and a test can run in
// isolation, which a static map makes impossible to guarantee once two test
// files touch it.
//
// One entry per owner, not one per (owner, weld epsilon). Two sculptors on one
// layer at two different epsilons therefore replace each other's entry rather
// than accumulating, and the first one's `shared_ptr` keeps its own adjacency
// alive for as long as it needs it. That bounds the cache at the number of mesh
// layers a document holds, which is the property worth having; the thrash it
// permits is a case no host has.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

#include "clay/mesh/adjacency.h"
#include "clay/mesh/mesh_data.h"

namespace clay {
namespace mesh {

// What an `Adjacency` was built over, in the terms that decide whether it may
// be served for a different `Mesh`.
//
// NOT A KEY — the key is the owner's identity. This is the CHECK, and the
// distinction matters: a key that collides gives a wrong answer, a check that
// fails gives a rebuild.
struct TopologyFingerprint {
    std::uint64_t vertex_count = 0;
    std::uint64_t triangle_count = 0;
    // A hash of the whole index buffer. Four independent lanes rather than one
    // FNV chain, because the chain's serial multiply reads 1.0 ms on 884,736
    // indices and the lanes read 0.25 ms for the same work.
    std::uint64_t connectivity = 0;
    // Part of the fingerprint because it changes what welding MEANS, not just
    // how long it takes: an adjacency built at 1e-5 and one built exactly are
    // different partitions of the same triangles.
    float weld_epsilon = 0.0f;

    static TopologyFingerprint of(const Mesh& m, float weld_epsilon);

    bool operator==(const TopologyFingerprint& other) const {
        return vertex_count == other.vertex_count && triangle_count == other.triangle_count &&
               connectivity == other.connectivity && weld_epsilon == other.weld_epsilon;
    }
    bool operator!=(const TopologyFingerprint& other) const { return !(*this == other); }
};

// What the cache has done, for a host and for a test.
//
// THE HIT AND MISS COUNTS ARE THE POINT. A cache that never hits and a cache
// that is not there are indistinguishable from the outside — same answers, same
// timings within noise on a machine nobody is measuring carefully — and this
// library has shipped a feature before whose entire defect was that the engine
// could do it and nothing outside could see whether it did.
struct TopologyCacheStats {
    std::size_t entries = 0;
    std::size_t bytes = 0;
    std::size_t hits = 0;
    std::size_t misses = 0;
    // Entries replaced or released: a `forget`, a fingerprint mismatch that
    // rebuilt over an existing entry, a trim, a clear.
    std::size_t evictions = 0;
    // Cumulative, in nanoseconds. Building is what the cache exists to avoid;
    // verifying is what it costs. A host comparing the two is reading the
    // trade directly.
    std::uint64_t build_nanoseconds = 0;
    std::uint64_t verify_nanoseconds = 0;
};

class TopologyCache {
   public:
    // The adjacency for `m`, built if there is no live entry for `owner` that
    // fingerprints against it. Never null.
    //
    // `owner` is a stable identity for the STORAGE — a layer id. It is the
    // caller's to choose and the caller's to keep unique; the cache cannot
    // check it, which is why the fingerprint exists.
    //
    // A build happens under the lock. Calls on one document must be serialized
    // through the C ABI, so contention cannot arise there at all; the lock is
    // for a C++ embedder, and holding it across the build is what makes
    // "concurrent creation builds one authoritative entry" true rather than
    // likely.
    std::shared_ptr<const Adjacency> acquire(std::uint64_t owner, const Mesh& m,
                                             float weld_epsilon = kDefaultWeldEpsilon);

    // Drop `owner`'s entry. What a wholesale geometry replacement calls.
    // Returns the bytes released, which is zero when a live sculptor is still
    // holding the entry — dropping the cache's reference does not free storage
    // somebody is using, and reporting that it did would be a lie a host sizes
    // its next allocation against.
    std::size_t forget(std::uint64_t owner);

    // Release every entry no live sculptor holds. Returns the bytes released.
    //
    // "No live sculptor" is a FACT here rather than a heuristic: entries are
    // `shared_ptr` and a use count of one means the cache is the only holder.
    std::size_t release_unused();

    // Everything, whether held or not. The cache's own references go; a live
    // sculptor keeps working off the reference it already has.
    void clear();

    TopologyCacheStats stats() const;
    std::size_t bytes() const;
    // What ONE owner's entry costs, so a per-layer memory report can
    // attribute it to the layer that caused it rather than only to the
    // document. Zero when there is no entry.
    std::size_t bytes_of(std::uint64_t owner) const;

   private:
    struct Entry {
        TopologyFingerprint print;
        std::shared_ptr<const Adjacency> adjacency;
    };

    // Sized by the number of mesh layers a document holds, so a map's node per
    // entry is not worth avoiding and its stable iterators are worth having.
    std::map<std::uint64_t, Entry> entries_;
    mutable std::mutex mutex_;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
    std::size_t evictions_ = 0;
    std::uint64_t build_ns_ = 0;
    std::uint64_t verify_ns_ = 0;
};

}  // namespace mesh
}  // namespace clay
