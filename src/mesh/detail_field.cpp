#include "clay/mesh/detail_field.h"

#include <algorithm>
#include <cstring>

namespace clay {
namespace mesh {
namespace {

constexpr std::uint32_t kMagic = 0x46444d43u;  // 'CMDF'
constexpr std::uint32_t kVersion = 1u;

void put_u32(std::vector<std::uint8_t>* out, std::uint32_t v) {
    out->push_back(static_cast<std::uint8_t>(v & 0xffu));
    out->push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
    out->push_back(static_cast<std::uint8_t>((v >> 16) & 0xffu));
    out->push_back(static_cast<std::uint8_t>((v >> 24) & 0xffu));
}

bool take_u32(const std::uint8_t* data, std::size_t size, std::size_t* at, std::uint32_t* out) {
    if (*at + 4 > size) return false;
    *out = static_cast<std::uint32_t>(data[*at]) | (static_cast<std::uint32_t>(data[*at + 1]) << 8) |
           (static_cast<std::uint32_t>(data[*at + 2]) << 16) |
           (static_cast<std::uint32_t>(data[*at + 3]) << 24);
    *at += 4;
    return true;
}

void put_detail(std::vector<std::uint8_t>* out, const LocalDetail& d) {
    std::uint32_t bits[3];
    std::memcpy(&bits[0], &d.tangent, 4);
    std::memcpy(&bits[1], &d.bitangent, 4);
    std::memcpy(&bits[2], &d.normal, 4);
    for (std::uint32_t b : bits) put_u32(out, b);
}

bool take_detail(const std::uint8_t* data, std::size_t size, std::size_t* at, LocalDetail* out) {
    std::uint32_t bits[3];
    for (std::uint32_t& b : bits)
        if (!take_u32(data, size, at, &b)) return false;
    std::memcpy(&out->tangent, &bits[0], 4);
    std::memcpy(&out->bitangent, &bits[1], 4);
    std::memcpy(&out->normal, &bits[2], 4);
    return true;
}

void hash_u32(std::uint64_t* h, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        *h ^= static_cast<std::uint64_t>((v >> (i * 8)) & 0xffu);
        *h *= 0x100000001b3ull;
    }
}

void hash_detail(std::uint64_t* h, const LocalDetail& d) {
    std::uint32_t bits[3];
    std::memcpy(&bits[0], &d.tangent, 4);
    std::memcpy(&bits[1], &d.bitangent, 4);
    std::memcpy(&bits[2], &d.normal, 4);
    for (std::uint32_t b : bits) hash_u32(h, b);
}

}  // namespace

std::uint32_t DetailField::block_count() const {
    return (vertex_count_ + block_size_ - 1) / block_size_;
}

namespace {

// Rounded up to a power of two so the division and modulo in every access stay
// shifts, and floored at four so a "block" is never one vertex with a four-byte
// table entry beside it.
std::uint32_t normalize_block_size(std::uint32_t requested) {
    if (requested == 0) return DetailField::kDefaultBlockSize;
    std::uint32_t n = 4;
    while (n < requested && n < (1u << 20)) n <<= 1;
    return n;
}

}  // namespace

void DetailField::reset(std::uint32_t vertex_count, std::uint32_t block_size) {
    vertex_count_ = vertex_count;
    block_size_ = normalize_block_size(block_size);
    dense_ = false;
    block_slot_.assign(block_count(), kNoBlock);
    slot_block_.clear();
    storage_.clear();
}

LocalDetail DetailField::get(std::uint32_t vertex) const {
    if (vertex >= vertex_count_) return LocalDetail{};
    if (dense_) return storage_[vertex];
    const std::uint32_t slot = block_slot_[vertex / block_size_];
    if (slot == kNoBlock) return LocalDetail{};
    return storage_[static_cast<std::size_t>(slot) * block_size_ + (vertex % block_size_)];
}

std::size_t DetailField::reserve_slot(std::uint32_t vertex) {
    if (dense_) return vertex;
    const std::uint32_t block = vertex / block_size_;
    std::uint32_t slot = block_slot_[block];
    if (slot == kNoBlock) {
        slot = static_cast<std::uint32_t>(slot_block_.size());
        block_slot_[block] = slot;
        slot_block_.push_back(block);
        storage_.resize(storage_.size() + block_size_);
        // `>=`, not `>`: the threshold is 1.0, and "every block allocated" has
        // to be reachable rather than one block short of it forever.
        if (static_cast<float>(slot_block_.size()) >=
            kDensePromotionCoverage * static_cast<float>(block_count())) {
            promote_to_dense();
            return vertex;
        }
    }
    return static_cast<std::size_t>(slot) * block_size_ + (vertex % block_size_);
}

void DetailField::promote_to_dense() {
    std::vector<LocalDetail> flat(vertex_count_);
    for (std::size_t s = 0; s < slot_block_.size(); ++s) {
        const std::uint32_t begin = slot_block_[s] * block_size_;
        const std::uint32_t end = std::min(begin + block_size_, vertex_count_);
        for (std::uint32_t v = begin; v < end; ++v)
            flat[v] = storage_[s * block_size_ + (v - begin)];
    }
    storage_ = std::move(flat);
    block_slot_.clear();
    slot_block_.clear();
    dense_ = true;
}

void DetailField::set(std::uint32_t vertex, const LocalDetail& value) {
    if (vertex >= vertex_count_) return;
    // A zero written into a block that does not exist stays nothing: allocating
    // three kilobytes to record "unchanged" is exactly the cost this storage
    // exists to avoid.
    if (!dense_ && value.zero() && block_slot_[vertex / block_size_] == kNoBlock) return;
    storage_[reserve_slot(vertex)] = value;
}

bool DetailField::empty() const {
    // One loop for both representations: `storage_` holds exactly the entries
    // that could be non-zero either way, and everything outside it is zero by
    // construction.
    for (const LocalDetail& d : storage_)
        if (!d.zero()) return false;
    return true;
}

std::size_t DetailField::resident_vertices() const {
    return dense_ ? storage_.size() : slot_block_.size() * block_size_;
}

float DetailField::coverage() const {
    if (vertex_count_ == 0) return 0.0f;
    if (dense_) return 1.0f;
    const std::uint32_t blocks = block_count();
    if (blocks == 0) return 0.0f;
    return static_cast<float>(slot_block_.size()) / static_cast<float>(blocks);
}

void DetailField::compact() {
    if (dense_) {
        // Demote only when the content would actually fit back into a
        // meaningfully smaller sparse form; otherwise the walk below is wasted
        // work on the exact field that promoted for speed.
        std::uint32_t live = 0;
        const std::uint32_t blocks = block_count();
        for (std::uint32_t b = 0; b < blocks; ++b) {
            const std::uint32_t begin = b * block_size_;
            const std::uint32_t end = std::min(begin + block_size_, vertex_count_);
            for (std::uint32_t v = begin; v < end; ++v)
                if (!storage_[v].zero()) {
                    ++live;
                    break;
                }
        }
        if (blocks == 0 ||
            static_cast<float>(live) >= kDensePromotionCoverage * static_cast<float>(blocks))
            return;
        std::vector<LocalDetail> flat = std::move(storage_);
        reset(vertex_count_, block_size_);
        for (std::uint32_t v = 0; v < vertex_count_; ++v)
            if (!flat[v].zero()) set(v, flat[v]);
        return;
    }

    std::vector<std::uint32_t> keep;
    keep.reserve(slot_block_.size());
    for (std::size_t s = 0; s < slot_block_.size(); ++s) {
        const LocalDetail* block = storage_.data() + s * block_size_;
        for (std::uint32_t i = 0; i < block_size_; ++i)
            if (!block[i].zero()) {
                keep.push_back(static_cast<std::uint32_t>(s));
                break;
            }
    }
    if (keep.size() == slot_block_.size()) return;

    std::vector<LocalDetail> packed(keep.size() * block_size_);
    std::vector<std::uint32_t> new_slot_block(keep.size());
    std::fill(block_slot_.begin(), block_slot_.end(), kNoBlock);
    for (std::size_t i = 0; i < keep.size(); ++i) {
        const std::size_t src = static_cast<std::size_t>(keep[i]) * block_size_;
        std::copy(storage_.begin() + static_cast<std::ptrdiff_t>(src),
                  storage_.begin() + static_cast<std::ptrdiff_t>(src + block_size_),
                  packed.begin() + static_cast<std::ptrdiff_t>(i * block_size_));
        new_slot_block[i] = slot_block_[keep[i]];
        block_slot_[new_slot_block[i]] = static_cast<std::uint32_t>(i);
    }
    storage_ = std::move(packed);
    slot_block_ = std::move(new_slot_block);
}

bool DetailField::block_stored(std::uint32_t block) const {
    if (dense_) return block < block_count();
    return block < block_slot_.size() && block_slot_[block] != kNoBlock;
}

std::uint32_t DetailField::stored_block_count() const {
    // A DENSE field stores every block, which is exactly what promotion means:
    // the block table is gone because there is nothing left for it to say.
    return dense_ ? block_count() : static_cast<std::uint32_t>(slot_block_.size());
}

std::uint32_t DetailField::stored_block_at(std::uint32_t index) const {
    if (dense_) return index < block_count() ? index : kNoBlock;
    return index < slot_block_.size() ? slot_block_[index] : kNoBlock;
}

std::size_t DetailField::bytes() const {
    return storage_.capacity() * sizeof(LocalDetail) +
           block_slot_.capacity() * sizeof(std::uint32_t) +
           slot_block_.capacity() * sizeof(std::uint32_t);
}

std::uint64_t DetailField::checksum() const {
    std::uint64_t h = 0xcbf29ce484222325ull;
    hash_u32(&h, vertex_count_);
    // Walked PER NON-ZERO VERTEX rather than per block, so the value depends on
    // the content and on nothing about the container holding it — not which
    // representation, not the order blocks were allocated in, and not the BLOCK
    // SIZE. The last of those is why this is not a per-block walk: the block
    // size is a parameter now, and a checksum that changed with it could not be
    // used to say "releasing the caches changed nothing authoritative".
    for (std::uint32_t v = 0; v < vertex_count_; ++v) {
        const LocalDetail d = get(v);
        if (d.zero()) continue;
        hash_u32(&h, v);
        hash_detail(&h, d);
    }
    return h;
}

std::vector<std::uint8_t> DetailField::encode() const {
    // Encoded in the SPARSE shape whatever the live representation is, so a
    // saved file does not record a speed decision the reader has no reason to
    // inherit. The BLOCK SIZE is recorded, and a reader adopts it: it is a
    // storage parameter rather than a property of the content, and carrying it
    // is what keeps a decode-then-encode byte-identical to the original.
    std::vector<std::uint32_t> blocks;
    const std::uint32_t total = block_count();
    for (std::uint32_t b = 0; b < total; ++b) {
        const std::uint32_t begin = b * block_size_;
        const std::uint32_t end = std::min(begin + block_size_, vertex_count_);
        for (std::uint32_t v = begin; v < end; ++v)
            if (!get(v).zero()) {
                blocks.push_back(b);
                break;
            }
    }

    std::vector<std::uint8_t> out;
    out.reserve(24 + blocks.size() * (4 + static_cast<std::size_t>(block_size_) * 12));
    put_u32(&out, kMagic);
    put_u32(&out, kVersion);
    put_u32(&out, vertex_count_);
    put_u32(&out, block_size_);
    put_u32(&out, static_cast<std::uint32_t>(blocks.size()));
    for (std::uint32_t b : blocks) {
        put_u32(&out, b);
        const std::uint32_t begin = b * block_size_;
        for (std::uint32_t i = 0; i < block_size_; ++i) {
            const std::uint32_t v = begin + i;
            put_detail(&out, v < vertex_count_ ? get(v) : LocalDetail{});
        }
    }
    return out;
}

bool DetailField::decode(const std::uint8_t* data, std::size_t size, DetailField* out) {
    if (!data || !out) return false;
    std::size_t at = 0;
    std::uint32_t magic = 0, version = 0, vertex_count = 0, block_size = 0, block_total = 0;
    if (!take_u32(data, size, &at, &magic) || magic != kMagic) return false;
    if (!take_u32(data, size, &at, &version) || version != kVersion) return false;
    if (!take_u32(data, size, &at, &vertex_count) || vertex_count > kMaxVertices) return false;
    // A block size that is not a power of two in range describes a layout this
    // reader cannot address, and taking it on trust would put the arithmetic
    // below out of step with the stream.
    if (!take_u32(data, size, &at, &block_size)) return false;
    if (block_size < 4u || block_size > (1u << 20) || (block_size & (block_size - 1u)) != 0u)
        return false;
    if (!take_u32(data, size, &at, &block_total)) return false;

    // THE COUNT IS CHECKED AGAINST THE BUFFER BEFORE ANYTHING IS ALLOCATED.
    // Without this a header declaring four million blocks is a request for
    // twelve gigabytes, and the failure is an abort rather than a refusal.
    const std::uint32_t blocks = (vertex_count + block_size - 1) / block_size;
    if (block_total > blocks) return false;
    const std::size_t per_block = 4u + static_cast<std::size_t>(block_size) * 12u;
    if (static_cast<std::size_t>(block_total) > (size - at) / per_block) return false;

    DetailField field;
    field.reset(vertex_count, block_size);
    std::uint32_t previous = 0;
    for (std::uint32_t i = 0; i < block_total; ++i) {
        std::uint32_t block = 0;
        if (!take_u32(data, size, &at, &block) || block >= blocks) return false;
        // Ascending and without repeats, which is what `encode` writes and what
        // makes a stream describe one field rather than several overlaid.
        if (i > 0 && block <= previous) return false;
        previous = block;
        for (std::uint32_t k = 0; k < block_size; ++k) {
            LocalDetail d;
            if (!take_detail(data, size, &at, &d)) return false;
            const std::uint32_t v = block * block_size + k;
            if (v < vertex_count) field.set(v, d);
        }
    }
    *out = std::move(field);
    return true;
}

}  // namespace mesh
}  // namespace clay
