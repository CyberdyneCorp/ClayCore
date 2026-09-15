#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

namespace clay::mesh::detail {

struct EdgeKey {
    std::uint64_t a, b;
    bool operator==(const EdgeKey&) const = default;
};

struct EdgeKeyHash {
    std::size_t operator()(EdgeKey key) const {
        // Packed lattice coordinates need mixing before a power-of-two mask.
        std::uint64_t h = key.a * 0x9E3779B185EBCA87ull ^
                          (key.b + 0xC2B2AE3D27D4EB4Full + (key.a << 6));
        h ^= h >> 30;
        h *= 0xbf58476d1ce4e5b9ull;
        h ^= h >> 27;
        h *= 0x94d049bb133111ebull;
        return static_cast<std::size_t>(h ^ (h >> 31));
    }
};

// Per-builder scratch storage. Neither hashing nor bucket order determines
// mesh order: the caller supplies the index at the first encounter of an edge.
// Hash is a test seam for forced collisions; equality always compares both keys.
template <class Hash = EdgeKeyHash>
class EdgeVertexMap {
  public:
    EdgeVertexMap() = default;
    EdgeVertexMap(const EdgeVertexMap&) = delete;
    EdgeVertexMap& operator=(const EdgeVertexMap&) = delete;

    std::pair<std::uint32_t, bool> intern(EdgeKey key, std::uint32_t next) {
        if (slots_.empty()) slots_.resize(16);
        std::size_t at = locate(slots_, key);
        if (slots_[at].occupied) return {slots_[at].value, false};
        // Keep a quarter of the buckets empty to bound ordinary probing.
        if (count_ == slots_.size() - slots_.size() / 4) {
            grow();
            at = locate(slots_, key);
        }
        slots_[at] = {key, next, true};
        ++count_;
        return {next, true};
    }

  private:
    struct Slot {
        EdgeKey key{};
        std::uint32_t value = 0;
        bool occupied = false;
    };

    static std::size_t locate(const std::vector<Slot>& slots, EdgeKey key) {
        std::size_t at = Hash{}(key) & (slots.size() - 1);
        while (slots[at].occupied && !(slots[at].key == key))
            at = (at + 1) & (slots.size() - 1);
        return at;
    }

    void grow() {
        if (slots_.size() > slots_.max_size() / 2)
            std::abort();  // Same fatal allocation policy as this no-exceptions build.
        std::vector<Slot> larger(slots_.size() * 2);
        for (const Slot& entry : slots_)
            if (entry.occupied) larger[locate(larger, entry.key)] = entry;
        slots_.swap(larger);
    }

    std::vector<Slot> slots_;
    std::size_t count_ = 0;
};

}  // namespace clay::mesh::detail
