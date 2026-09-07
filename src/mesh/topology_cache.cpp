#include "clay/mesh/topology_cache.h"

#include <chrono>

namespace clay {
namespace mesh {

namespace {

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Four independent multiply-xor lanes, combined at the end.
//
// A single FNV chain over the same 884,736 indices reads 1.0 ms because every
// multiply waits for the one before it; four lanes read 0.25 ms, which is what
// makes verifying a hit a rounding error on the 120 ms build it is deciding
// whether to skip. It is not a cryptographic hash and does not need to be:
// what it has to catch is a DIFFERENT index buffer, not an adversarial one.
std::uint64_t hash_indices(const std::vector<std::uint32_t>& indices) {
    constexpr std::uint64_t kPrime = 1099511628211ull;
    std::uint64_t lane[4] = {1469598103934665603ull, 1099511628211ull, 14695981039346656037ull,
                             1234567890123456789ull};
    const std::size_t n = indices.size();
    const std::size_t quads = n / 4;
    for (std::size_t i = 0; i < quads; ++i) {
        lane[0] = (lane[0] ^ indices[i * 4 + 0]) * kPrime;
        lane[1] = (lane[1] ^ indices[i * 4 + 1]) * kPrime;
        lane[2] = (lane[2] ^ indices[i * 4 + 2]) * kPrime;
        lane[3] = (lane[3] ^ indices[i * 4 + 3]) * kPrime;
    }
    std::uint64_t tail = 0xcbf29ce484222325ull;
    for (std::size_t i = quads * 4; i < n; ++i) tail = (tail ^ indices[i]) * kPrime;
    // The lanes are combined with a rotate each so that a permutation of the
    // four positions within a group is not invisible.
    std::uint64_t h = tail;
    for (int i = 0; i < 4; ++i) {
        const std::uint64_t v = lane[i];
        h ^= (v << (i * 7)) | (v >> (64 - i * 7 - 1) >> 1);
        h *= kPrime;
    }
    return h ^ (static_cast<std::uint64_t>(n) * kPrime);
}

}  // namespace

TopologyFingerprint TopologyFingerprint::of(const Mesh& m, float weld_epsilon) {
    TopologyFingerprint print;
    print.vertex_count = m.positions.size();
    print.triangle_count = m.triangle_count();
    print.connectivity = hash_indices(m.indices);
    print.weld_epsilon = weld_epsilon;
    return print;
}

std::shared_ptr<const Adjacency> TopologyCache::acquire(std::uint64_t owner, const Mesh& m,
                                                        float weld_epsilon) {
    std::lock_guard<std::mutex> lock(mutex_);

    const std::uint64_t verify_started = now_ns();
    const TopologyFingerprint print = TopologyFingerprint::of(m, weld_epsilon);
    verify_ns_ += now_ns() - verify_started;

    auto it = entries_.find(owner);
    if (it != entries_.end() && it->second.print == print && it->second.adjacency) {
        ++hits_;
        return it->second.adjacency;
    }
    ++misses_;
    // An entry that was here and did not match is an eviction whether or not
    // anything still holds it: the cache's reference to it is going.
    if (it != entries_.end()) ++evictions_;

    const std::uint64_t build_started = now_ns();
    auto built = std::make_shared<const Adjacency>(Adjacency::build(m, weld_epsilon));
    build_ns_ += now_ns() - build_started;

    entries_[owner] = Entry{print, built};
    return built;
}

std::size_t TopologyCache::forget(std::uint64_t owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(owner);
    if (it == entries_.end()) return 0;
    // Only what the cache's own reference frees. A sculptor still holding the
    // entry keeps the storage, and reporting it as released would be a figure a
    // host sizes its next allocation against.
    const std::size_t released =
        it->second.adjacency.use_count() == 1 ? it->second.adjacency->bytes() : 0;
    entries_.erase(it);
    ++evictions_;
    return released;
}

std::size_t TopologyCache::release_unused() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t released = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.adjacency && it->second.adjacency.use_count() == 1) {
            released += it->second.adjacency->bytes();
            it = entries_.erase(it);
            ++evictions_;
        } else {
            ++it;
        }
    }
    return released;
}

void TopologyCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    evictions_ += entries_.size();
    entries_.clear();
}

TopologyCacheStats TopologyCache::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    TopologyCacheStats s;
    s.entries = entries_.size();
    for (const auto& kv : entries_)
        if (kv.second.adjacency) s.bytes += kv.second.adjacency->bytes();
    s.hits = hits_;
    s.misses = misses_;
    s.evictions = evictions_;
    s.build_nanoseconds = build_ns_;
    s.verify_nanoseconds = verify_ns_;
    return s;
}

std::size_t TopologyCache::bytes() const { return stats().bytes; }

std::size_t TopologyCache::bytes_of(std::uint64_t owner) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(owner);
    return it != entries_.end() && it->second.adjacency ? it->second.adjacency->bytes() : 0;
}

}  // namespace mesh
}  // namespace clay
