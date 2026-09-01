#pragma once

// THE PER-STAMP SCRATCH ARENA (meshing spec, add-shared-brush-runtime).
//
// WHAT THIS IS FOR. "A stamp costs what it touches" is the rule the whole mesh
// brush path is built around, and the sculptors keep it by owning a
// `std::vector` member for every buffer a stamp needs — cleared, never freed,
// so a stroke allocates on its first dab and never again. That works, and it
// stops working the moment a buffer is needed DEEP inside the call rather than
// at the top of it: the automask's breadth-first frontier, the adaptive
// region's sort permutation, the ball query's face list. Each of those is a
// local `std::vector` today, each allocates and frees on every dab, and each is
// invisible to every test that only reads results — which is exactly the defect
// `tests/unit/test_sculpt_allocation.cpp` exists to catch and did not, because
// the gate never set an automask factor.
//
// Threading one more `std::vector` member through five call layers for each of
// them would work and would be worse: it makes every intermediate signature
// carry storage it does not use, and it has to be redone for the next
// transient. One arena passed down instead is the same discipline said once.
//
// WHAT IT IS. A bump pointer over an owned block. `allocate<T>(count)` returns
// aligned storage and moves the pointer; `reset()` returns the pointer to zero
// and KEEPS the block. Nothing is freed per allocation and nothing is tracked
// per allocation — that is the whole of why it is cheap enough to sit inside a
// per-dab path.
//
// WHAT IT REFUSES, AT COMPILE TIME. `reset()` runs no destructors — that is
// what makes it a pointer store instead of a walk — so a type that owns memory
// would leak silently, once per stamp, at pointer rates. `allocate` therefore
// `static_assert`s on trivial destructibility rather than tracking destructors,
// which would make this a general allocator: a much larger thing than the one
// this needs to be, for scratch that is `float`, `std::uint32_t`,
// `kernel::cfloat3`, `WorkItemId`, `FaceId`, `VertexId` and
// `std::pair<float, std::uint32_t>` — every one of them trivially
// destructible.
//
// ONE PER SCULPTOR, NEVER A PROCESS-GLOBAL. Three sculptors can be live at once
// — a `MultiresSculptor` OWNS a `MeshSculptor` — and a document can hold
// several mesh layers. A shared mutable arena would make two stamps on two
// layers alias each other's scratch, would make a sculptor's cost depend on
// what else the host was doing, and would be a data race the first time a host
// stamped two layers on two threads. Nothing in the current design forbids
// that, so nothing here may assume it.
//
// WHY IT REPORTS ITS OWN STATISTICS. An allocation count alone cannot see the
// opposite leak: scratch that grows a little every stamp and is never reset
// allocates nothing after warm-up and consumes memory without bound.
// `high_water_bytes()` and `growths()` are what let a test assert the arena
// STOPS growing rather than merely that a stamp stopped allocating.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace clay {
namespace mesh {

// A growable view over a block of arena storage: a size, a fixed capacity, and
// a refusal past it.
//
// The automask's frontiers and the region's sort permutation want a thing they
// can `push_back` into without an allocator behind it. Every one of them has a
// capacity that is KNOWN AND EXACT before the loop runs — a slot enters a
// breadth-first frontier at most once, so `workset.size()` bounds it — which is
// what makes a fixed capacity the right shape rather than a compromise.
//
// PAST THE CAPACITY IT REFUSES AND SAYS SO. It does not grow (there is nowhere
// to grow into: the next arena allocation already sits behind it) and it does
// not silently drop (a dropped frontier entry is a hole in an automask that
// nothing would report). `overflowed()` is the caller's signal that a bound it
// stated was wrong, which is a defect in the bound and not a condition to
// handle at runtime.
template <typename T>
class ScratchVector {
   public:
    ScratchVector() = default;
    ScratchVector(T* data, std::size_t capacity) : data_(data), capacity_(capacity) {}

    bool push_back(const T& value) {
        if (size_ >= capacity_) {
            overflowed_ = true;
            return false;
        }
        data_[size_++] = value;
        return true;
    }

    // Undefined on an empty view, exactly as `std::vector::pop_back` is. The
    // two callers are depth-first floods that just read the top.
    void pop_back_unchecked() { --size_; }

    void clear() { size_ = 0; }
    bool empty() const { return size_ == 0; }
    std::size_t size() const { return size_; }
    std::size_t capacity() const { return capacity_; }
    bool overflowed() const { return overflowed_; }

    T& operator[](std::size_t i) { return data_[i]; }
    const T& operator[](std::size_t i) const { return data_[i]; }
    T* data() { return data_; }
    const T* data() const { return data_; }
    T* begin() { return data_; }
    T* end() { return data_ + size_; }
    const T* begin() const { return data_; }
    const T* end() const { return data_ + size_; }

    // Fill the whole capacity and count it as the size. What a dense per-slot
    // array — the automask's `depth`, its `reached` marks — wants, since those
    // are indexed rather than appended to.
    void assign_all(const T& value) {
        for (std::size_t i = 0; i < capacity_; ++i) data_[i] = value;
        size_ = capacity_;
    }

   private:
    T* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
    bool overflowed_ = false;
};

class BrushScratchArena {
   public:
    BrushScratchArena() = default;

    // Storage for `count` objects of `T`, aligned for `T`, uninitialized.
    //
    // A request that does not fit in the current block takes a NEW block rather
    // than reallocating the old one, because reallocating would invalidate
    // every pointer this stamp has already been handed — a bump allocator's one
    // real hazard, and one that would surface as a use-after-free deep inside a
    // brush. The overflow blocks are folded back into one at the next `reset()`,
    // so a stroke pays for the growth once and converges.
    template <typename T>
    T* allocate(std::size_t count) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "the arena runs no destructors: `reset` is a pointer store, so a type "
                      "that owns memory would leak once per stamp");
        if (count == 0) return nullptr;
        return static_cast<T*>(take(count * sizeof(T), alignof(T)));
    }

    // The same block, wrapped in a growable view of exactly that capacity.
    template <typename T>
    ScratchVector<T> vector(std::size_t capacity) {
        return ScratchVector<T>(allocate<T>(capacity), capacity);
    }

    // Return the bump pointer to zero and KEEP the storage. Runs no
    // destructors — see the header note.
    void reset();

    // What the arena OWNS, which is the number a memory budget cares about.
    std::size_t capacity_bytes() const { return capacity_; }
    // The most any single stamp has asked for, which is the number that says
    // whether the arena has converged. A CURRENT usage would not: it reads zero
    // between stamps whatever the arena is doing.
    std::size_t high_water_bytes() const { return high_water_; }
    // How many times the arena has had to take more storage. The assertion a
    // stroke makes is that this STOPS climbing, which is the one failure an
    // allocation count cannot see: scratch that grows a little every stamp
    // allocates nothing after warm-up and consumes memory without bound.
    std::size_t growths() const { return growths_; }
    // What has been handed out since the last reset. For a scope marker and for
    // a diagnostic; a budget wants `high_water_bytes`.
    std::size_t used_bytes() const;

   private:
    friend class BrushArenaScope;

    struct Block {
        std::unique_ptr<std::byte[]> bytes;
        std::size_t size = 0;
    };

    // Where the bump pointer stands: which block, and how far into it. A pair
    // rather than one offset, because an overflow block is a separate
    // allocation and a single offset could not name a position inside it.
    struct Mark {
        std::size_t block = 0;
        std::size_t offset = 0;
    };

    void* take(std::size_t bytes, std::size_t align);
    Mark mark() const { return Mark{block_, offset_}; }
    void release(Mark m);

    std::vector<Block> blocks_;
    std::size_t block_ = 0;   // the block the bump pointer is in
    std::size_t offset_ = 0;  // how far into it
    std::size_t capacity_ = 0;
    std::size_t high_water_ = 0;
    std::size_t growths_ = 0;
};

// A NESTED reset, for scratch whose life is shorter than the stamp's.
//
// Without one, an arena's capacity converges on the SUM of everything a stamp
// allocates rather than on its peak — the automask's five arrays stay reserved
// while the region sort runs, although neither can see the other. The scope
// makes `high_water_bytes()` mean what it says.
//
// It restores a POSITION, not a state: anything allocated inside the scope is
// dangling when it ends, exactly as if the arena had been reset. That is the
// same contract `reset()` has and the reason both are named for what they do to
// the pointer.
class BrushArenaScope {
   public:
    explicit BrushArenaScope(BrushScratchArena& arena) : arena_(arena), mark_(arena.mark()) {}
    ~BrushArenaScope() { arena_.release(mark_); }

    BrushArenaScope(const BrushArenaScope&) = delete;
    BrushArenaScope& operator=(const BrushArenaScope&) = delete;

   private:
    BrushScratchArena& arena_;
    BrushScratchArena::Mark mark_;
};

}  // namespace mesh
}  // namespace clay
