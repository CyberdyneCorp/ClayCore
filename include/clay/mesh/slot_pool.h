#pragma once

// STABLE SLOTS: storage whose indices survive unrelated deletions
// (dynamic-topology spec, add-dynamic-topology).
//
// WHY THIS EXISTS. A dynamic surface's whole reason for being is that a local
// edit is local. The obvious storage — a `std::vector` compacted on erase —
// destroys that in the first line: removing one element renumbers everything
// after it, so a handle held by the spatial index, the undo record, the brush's
// candidate list or the host's upload buffer silently comes to mean a different
// element. The renumbering is also O(n) per erase, on a path that runs
// thousands of times per stroke.
//
// So nothing is ever moved. An erased slot goes on a free list and is reused;
// live slots keep their index for as long as they live.
//
// THE GENERATION IS THE HALF THAT MATTERS. Reusing a slot is what makes the
// storage bounded, and it is also what makes a stale handle DANGEROUS rather
// than merely wrong: without a generation, a handle to a deleted vertex whose
// slot has since been reused dereferences happily and returns a completely
// different vertex. That failure is silent, plausible and appears at a distance
// from its cause — the worst combination there is. Every handle carries the
// generation its slot had when the handle was made, `get` compares them, and a
// handle that outlived its element returns null.
//
// NOT A GENERAL-PURPOSE CONTAINER. It has no iterators over live elements by
// design: every caller here walks the surface through its own connectivity —
// half-edges, rings, faces — and a caller that wants to sweep everything wants
// `for_each_live`, which says so.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace clay {
namespace mesh {

// The handle every element of a dynamic surface is addressed by.
//
// A POD of two 32-bit words rather than a pointer: it survives storage growth,
// it is trivially copyable into an undo record or across the C ABI, and it is
// comparable and hashable without knowing anything about the pool it came from.
//
// The tag parameter makes a `VertexId` and a `FaceId` different TYPES even
// though both are two integers, so passing one where the other belongs does not
// compile. That is worth more here than anywhere else in the library: the four
// element kinds are constantly converted between, and half of the interesting
// bugs in a half-edge implementation are one of them used as another.
template <typename Tag>
struct SlotId {
    static constexpr std::uint32_t kInvalid = std::numeric_limits<std::uint32_t>::max();

    std::uint32_t slot = kInvalid;
    std::uint32_t generation = 0;

    bool valid() const { return slot != kInvalid; }
    explicit operator bool() const { return valid(); }

    friend bool operator==(const SlotId& a, const SlotId& b) {
        return a.slot == b.slot && a.generation == b.generation;
    }
    friend bool operator!=(const SlotId& a, const SlotId& b) { return !(a == b); }
    // ORDERED BY SLOT, and this is load-bearing rather than incidental: the
    // determinism rule is that every candidate set is sorted by stable id
    // before any operator runs, and `slot` is that identity. Ordering by
    // generation first would make the order depend on the history of edits,
    // which is exactly what the rule exists to remove.
    friend bool operator<(const SlotId& a, const SlotId& b) { return a.slot < b.slot; }
};

// Storage of `T` addressed by `Id`, with a free list and a generation per slot.
template <typename T, typename Id>
class SlotPool {
   public:
    Id create(const T& value) {
        if (free_head_ != Id::kInvalid) {
            const std::uint32_t slot = free_head_;
            free_head_ = slots_[slot].next_free;
            slots_[slot].value = value;
            slots_[slot].live = true;
            --dead_;
            return Id{slot, slots_[slot].generation};
        }
        slots_.push_back(Slot{value, 0u, Id::kInvalid, true});
        return Id{static_cast<std::uint32_t>(slots_.size() - 1), 0u};
    }

    // Returns false for a handle that is already dead or was never live, so a
    // double erase is a no-op rather than a corruption of the free list.
    bool erase(Id id) {
        if (!live(id)) return false;
        Slot& s = slots_[id.slot];
        s.live = false;
        // BUMPED ON ERASE, not on create, so that every handle made before this
        // point stops matching immediately. Bumping on reuse would leave a
        // window where a stale handle to a freed slot still compares equal.
        ++s.generation;
        s.next_free = free_head_;
        free_head_ = id.slot;
        ++dead_;
        return true;
    }

    bool live(Id id) const {
        return id.valid() && id.slot < slots_.size() && slots_[id.slot].live &&
               slots_[id.slot].generation == id.generation;
    }

    T* get(Id id) { return live(id) ? &slots_[id.slot].value : nullptr; }
    const T* get(Id id) const { return live(id) ? &slots_[id.slot].value : nullptr; }

    // For the hot paths that have already established liveness — an operator
    // walking a face's own half-edges, say. Undefined for a dead handle, which
    // is why the checked form is the default and this one says so in its name.
    T& at(Id id) { return slots_[id.slot].value; }
    const T& at(Id id) const { return slots_[id.slot].value; }

    // The id currently naming a slot, whatever generation it is on. For a
    // decoder rebuilding a surface from bytes, and for the validator, which has
    // to look at slots rather than at handles.
    Id id_at(std::uint32_t slot) const {
        if (slot >= slots_.size() || !slots_[slot].live) return Id{};
        return Id{slot, slots_[slot].generation};
    }

    bool live_at(std::uint32_t slot) const { return slot < slots_.size() && slots_[slot].live; }

    std::size_t capacity_slots() const { return slots_.size(); }
    std::size_t size() const { return slots_.size() - dead_; }
    bool empty() const { return size() == 0; }

    // Every live element, in SLOT ORDER. The order is the point: it is the
    // stable identity order the determinism rule names, and it does not depend
    // on the sequence of edits that produced the pool.
    template <typename Fn>
    void for_each_live(Fn&& fn) const {
        for (std::uint32_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].live) fn(Id{i, slots_[i].generation}, slots_[i].value);
    }
    template <typename Fn>
    void for_each_live_mutable(Fn&& fn) {
        for (std::uint32_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].live) fn(Id{i, slots_[i].generation}, slots_[i].value);
    }

    void clear() {
        slots_.clear();
        free_head_ = Id::kInvalid;
        dead_ = 0;
    }

    void reserve(std::size_t n) { slots_.reserve(n); }

    // What the pool owns, for a memory report. Not `sizeof`: the slots are the
    // payload, and a pool holding a stroke's worth of geometry costs nothing
    // like one holding a model's.
    std::size_t bytes() const { return slots_.capacity() * sizeof(Slot); }

    // How much of the storage is dead, so a caller can decide whether an
    // incremental compaction outside the pointer-event path is worth running.
    std::size_t dead_slots() const { return dead_; }

    // -- decoding ------------------------------------------------------------
    //
    // A decoder needs to rebuild a pool with its slots exactly where they were,
    // because every handle in the encoded connectivity names a slot. So it
    // sizes the pool once and fills slots directly, rather than replaying a
    // sequence of creates and erases that would have to produce the same free
    // list by luck.
    void decode_resize(std::size_t slot_count) {
        slots_.assign(slot_count, Slot{T{}, 0u, Id::kInvalid, false});
        free_head_ = Id::kInvalid;
        dead_ = slot_count;
    }
    void decode_set(std::uint32_t slot, const T& value, std::uint32_t generation) {
        Slot& s = slots_[slot];
        s.value = value;
        s.generation = generation;
        s.live = true;
        --dead_;
    }
    // Rebuild the free list over whatever slots were left dead. Descending, so
    // the list hands out the LOWEST slot first and a decoded pool allocates in
    // the same order a freshly built one would.
    void decode_finish() {
        free_head_ = Id::kInvalid;
        for (std::uint32_t i = static_cast<std::uint32_t>(slots_.size()); i-- > 0;)
            if (!slots_[i].live) {
                slots_[i].next_free = free_head_;
                free_head_ = i;
            }
    }

   private:
    struct Slot {
        T value;
        std::uint32_t generation = 0;
        std::uint32_t next_free = Id::kInvalid;
        bool live = false;
    };

    std::vector<Slot> slots_;
    std::uint32_t free_head_ = Id::kInvalid;
    std::size_t dead_ = 0;
};

}  // namespace mesh
}  // namespace clay
