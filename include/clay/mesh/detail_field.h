#pragma once

// WHAT A LEVEL ACTUALLY OWNS (mesh-multires spec, add-mesh-multires).
//
// A level's positions are its subdivided parent's positions PLUS this — three
// coefficients per vertex, in the transported frame `surface_frame.h` builds.
// Everything else about a level is derived and can be thrown away; this cannot.
// It is the user's work.
//
// SPARSE, AND THE REASON IS ARITHMETIC. Catmull-Clark multiplies faces by four
// a level, so a 20k-quad cage reaches 5.1M faces at level 4 and 20.5M at level
// 5. Twelve bytes of detail on every vertex of every level is 60 MB and 240 MB
// respectively — for a hierarchy where the artist has touched a cheek. Detail
// is where a subdivision hierarchy is at its most local: an artist adds pores
// to a face and nothing else on the model has any. So storage is blocked: a
// block of `block_size()` vertices exists only once something in it is non-zero,
// and returns to nothing when it is zeroed again.
//
// DENSE ABOVE A THRESHOLD, and that is a SPEED decision rather than a memory
// one — worth saying, because the obvious reading is wrong. The block table
// costs four bytes per block, so at the default block size the sparse form is
// smaller than the dense one until coverage passes 99.9%; it is never memory
// that argues for
// promotion. What argues for it is the indirection on every read, which a
// smoothing pass over a fully-detailed level pays a million times. The
// threshold and the measurement behind it are on `kDensePromotionCoverage`.
//
// FP32 AND NOT QUANTIZED, deliberately, and this is the one place the change
// says no to a compression that would obviously "work". High-frequency detail
// is exactly where a quantization artefact appears first and where it is least
// forgivable — a pore field is a signal at the scale of the quantization step.
// Compression waits on a measured error bound, and this is the editing
// representation rather than the transport one.
//
// DESIGNED FOR THE LAYER STACK THAT FOLLOWS IT. `add-mesh-sculpt-layers` is
// the next change, and its layers store the same thing this does: a sparse
// field of frame coefficients over a level. Building the two independently is
// how a library ends up with two displacement systems that disagree, so this
// type is the shared one — a layer will be another `DetailField`, composed, not
// a second mechanism.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace clay {
namespace mesh {

// Tangent, bitangent and normal coefficients. Zero means "this vertex sits
// exactly where the subdivision put it", which is what an untouched vertex is
// and what makes the sparse form possible.
struct LocalDetail {
    float tangent = 0.0f;
    float bitangent = 0.0f;
    float normal = 0.0f;

    bool zero() const { return tangent == 0.0f && bitangent == 0.0f && normal == 0.0f; }
    bool operator==(const LocalDetail& o) const {
        return tangent == o.tangent && bitangent == o.bitangent && normal == o.normal;
    }
    bool operator!=(const LocalDetail& o) const { return !(*this == o); }
};

class DetailField {
   public:
    // 1024 vertices, 12 KB of coefficients.
    //
    // A PARAMETER RATHER THAN A CONSTANT, and that is what makes the number
    // defensible rather than merely plausible: `BM_MultiresDetailBlockSize`
    // sweeps 64 / 256 / 1024 / 4096 across three footprints on a level of
    // 1,048,576 vertices and reports what each choice costs to hold the same
    // detail — the blocks it allocates plus the block table it needs over the
    // level. Total KiB, measured:
    //
    //     reached      64      256     1024     4096
    //     ------------------------------------------
    //         400   72.25       25       16       49
    //       4,000  139.75       94       88       97
    //      40,000  814.75      769      760      769
    //
    // 1024 is the least at every footprint, and the shape says why: below it
    // the block TABLE over the level dominates (64 spends 64 KiB of table to
    // save 12 KiB of payload on the narrow dab), above it the last partly-used
    // BLOCK does (4096 wastes 36 KiB on a 400-vertex footprint). The minimum is
    // broad rather than sharp — 256 and 1024 are within 7% at the two larger
    // footprints — which is why this is a default rather than a tuning
    // parameter, and why it is a parameter at all: a constant nobody can sweep
    // is a number nobody can re-derive when the footprint changes. Sculpt
    // layers, which store the same type over the same levels, can choose
    // differently without touching this file.
    static constexpr std::uint32_t kDefaultBlockSize = 1024;

    // At this fraction of blocks allocated, the field switches to a flat array.
    //
    // ONE, AND THE MEASUREMENT IS WHY. The reason to promote was supposed to be
    // the per-read indirection, and `BM_MultiresDetailAccess` says there is
    // none worth having: reading a stroke-sized index set out of a sparse field
    // runs at 611 M/s against 605 M/s dense — the block table entry a local
    // footprint keeps re-reading is simply always in cache. So promotion buys
    // no measurable speed, while promoting EARLY costs real memory: a field
    // promoted at three-quarters coverage allocates a third more than the
    // sparse form it replaced.
    //
    // A threshold with nothing on the benefit side belongs where it cannot
    // cost anything either. At 1.0 the field promotes only once every block is
    // allocated — the one case where the table and the indirection are pure
    // overhead and the dense array is not one byte larger.
    static constexpr float kDensePromotionCoverage = 1.0f;

    static constexpr std::uint32_t kNoBlock = 0xffffffffu;

    // Size the field to a level. Clears everything. A block size of zero takes
    // the default; anything else is rounded up to a power of two, so the
    // division and modulo in every access stay shifts.
    void reset(std::uint32_t vertex_count, std::uint32_t block_size = kDefaultBlockSize);
    std::uint32_t vertex_count() const { return vertex_count_; }
    std::uint32_t block_size() const { return block_size_; }

    // Zero outside anything stored, which is the whole point: an unwritten
    // vertex costs no memory and reads as "no detail here".
    LocalDetail get(std::uint32_t vertex) const;
    // Writing a zero RELEASES rather than stores — a vertex smoothed back flat
    // must not keep a block alive, or a stroke that undoes itself leaves the
    // memory it took. The block itself is released by `compact`.
    void set(std::uint32_t vertex, const LocalDetail& value);

    // Nothing stored anywhere. A hierarchy whose levels are all empty is
    // exactly its base subdivided, which is what `from_mesh` produces.
    bool empty() const;
    // How many vertices live in an allocated block — the storage cost's unit,
    // NOT how many are non-zero.
    std::size_t resident_vertices() const;
    float coverage() const;
    bool dense() const { return dense_; }

    // Release every all-zero block and demote a dense field whose coverage fell
    // back below the threshold.
    //
    // NEVER CALLED INSIDE A POINTER EVENT. It walks every stored block, which
    // is proportional to the level rather than to the dab, and a stroke that
    // paid that per stamp would stutter exactly where it must not. The owner
    // calls it between gestures.
    void compact();

    std::size_t bytes() const;

    // -- block presence, for a composer reading several fields at once --------
    //
    // Exposed for `sculpt_layer.h`'s composer and for nothing else: a layer's
    // field, a layer's mask and a level's composed field share this blocking,
    // so block `b` names the same `block_size()` vertices in all of them. That
    // shared index is what turns "a strength change costs the layer's coverage
    // rather than the surface" from an optimisation into a property of the data
    // structure — the composer walks the blocks a layer HAS, and a layer that
    // does not reach a block is an O(1) miss rather than a scan.
    //
    // No storage changes here. `stored_block_at` walks `slot_block_`, which
    // already existed so that a walk over stored blocks costs the stored ones.
    std::uint32_t block_count() const;
    bool block_stored(std::uint32_t block) const;
    std::uint32_t stored_block_count() const;
    std::uint32_t stored_block_at(std::uint32_t index) const;

    // A representation-INDEPENDENT hash of the content: the same value for a
    // sparse field and the dense field holding the same coefficients, and the
    // same value before and after a `compact`. That is what lets a test assert
    // "dropping the rebuildable caches changed nothing authoritative" without
    // comparing two megabytes.
    std::uint64_t checksum() const;

    // -- encoding -------------------------------------------------------------
    // Its own versioned form rather than a member of the surface's, because a
    // level's detail is the one thing in a hierarchy that is neither derivable
    // nor small, and a future layer stack encodes the same type.
    std::vector<std::uint8_t> encode() const;
    // Refuses a truncated, hostile or newer buffer rather than returning a
    // field that reads out of bounds. `out` is untouched on failure.
    static bool decode(const std::uint8_t* data, std::size_t size, DetailField* out);

    // The ceiling a decoder refuses above, before allocating. A few hundred
    // bytes declaring four billion vertices is a request for more memory than a
    // machine holds, and it must be refused by arithmetic rather than by
    // `bad_alloc`.
    static constexpr std::uint32_t kMaxVertices = 1u << 30;

   private:
    // Ensure the block holding `vertex` exists, and return the storage index of
    // that vertex.
    std::size_t reserve_slot(std::uint32_t vertex);
    void promote_to_dense();

    std::uint32_t vertex_count_ = 0;
    bool dense_ = false;
    // block -> slot in `storage_`, kNoBlock when the block has never been
    // written. Empty when dense.
    std::vector<std::uint32_t> block_slot_;
    // slot -> block, so a walk over stored blocks costs the stored ones. Empty
    // when dense.
    std::vector<std::uint32_t> slot_block_;
    // `slots * block_size_` entries when sparse, `vertex_count_` when dense.
    std::vector<LocalDetail> storage_;
    std::uint32_t block_size_ = kDefaultBlockSize;
};

}  // namespace mesh
}  // namespace clay
