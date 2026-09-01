// The only thing a level owns (mesh-multires spec, add-mesh-multires).
//
// Two properties are load-bearing far outside this file. The first is that an
// untouched vertex costs NOTHING: a level 5 over a 20k cage is twenty million
// vertices, and a hierarchy where the artist has detailed a cheek must not
// allocate a quarter of a gigabyte to say so. The second is that the checksum
// depends on the CONTENT and not on which representation is holding it, because
// "dropping the rebuildable caches changed nothing authoritative" is asserted
// by comparing two of these numbers.

#include <doctest/doctest.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "clay/mesh/detail_field.h"

using namespace clay;
using mesh::DetailField;
using mesh::LocalDetail;

namespace {

LocalDetail d(float t, float b, float n) { return LocalDetail{t, b, n}; }

}  // namespace

TEST_CASE("an untouched vertex costs nothing and reads as zero") {
    DetailField f;
    f.reset(100000);
    CHECK(f.empty());
    CHECK(f.resident_vertices() == 0);
    CHECK(f.get(0).zero());
    CHECK(f.get(99999).zero());
    // The block table and nothing else: a hundred thousand vertices, and the
    // storage is the four bytes per block that say "nothing here".
    const std::size_t idle = f.bytes();
    CHECK(idle < 100000u * sizeof(LocalDetail) / 100u);

    f.set(50000, d(0.0f, 0.0f, 0.25f));
    CHECK_FALSE(f.empty());
    CHECK(f.get(50000).normal == doctest::Approx(0.25f));
    // Exactly one block came into existence.
    CHECK(f.resident_vertices() == DetailField::kDefaultBlockSize);
    CHECK(f.bytes() > idle);
}

TEST_CASE("writing zero into a block that does not exist allocates nothing") {
    DetailField f;
    f.reset(4096);
    f.set(1000, LocalDetail{});
    CHECK(f.resident_vertices() == 0);
    CHECK(f.empty());
}

TEST_CASE("zeroing a vertex clears it, and compaction releases the block") {
    DetailField f;
    f.reset(4096);
    f.set(1000, d(1.0f, 2.0f, 3.0f));
    f.set(2000, d(0.0f, 0.0f, 1.0f));
    const std::size_t two_blocks = f.resident_vertices();
    CHECK(two_blocks == 2u * DetailField::kDefaultBlockSize);

    f.set(1000, LocalDetail{});
    CHECK(f.get(1000).zero());
    // Still resident until compaction: releasing a block on every zeroed vertex
    // would thrash a smoothing pass that flattens and re-deposits.
    CHECK(f.resident_vertices() == two_blocks);
    f.compact();
    CHECK(f.resident_vertices() == DetailField::kDefaultBlockSize);
    CHECK(f.get(2000).normal == doctest::Approx(1.0f));
    CHECK(f.get(1000).zero());
}

TEST_CASE("a field that fills up promotes to dense and still reads the same") {
    // Small enough that the promotion threshold is reachable in a test.
    const std::uint32_t n = 4 * DetailField::kDefaultBlockSize;
    DetailField f;
    f.reset(n);
    for (std::uint32_t v = 0; v < n; ++v) f.set(v, d(0.0f, 0.0f, static_cast<float>(v) * 0.001f));
    CHECK(f.dense());
    CHECK(f.coverage() == doctest::Approx(1.0f));
    for (std::uint32_t v = 0; v < n; ++v)
        CHECK(f.get(v).normal == doctest::Approx(static_cast<float>(v) * 0.001f));
}

TEST_CASE("the checksum follows the content, not the representation") {
    const std::uint32_t n = 4 * DetailField::kDefaultBlockSize;

    DetailField sparse;
    sparse.reset(n);
    sparse.set(3, d(1.0f, 0.0f, 0.0f));
    sparse.set(700, d(0.0f, 2.0f, 0.0f));
    const std::uint64_t sum = sparse.checksum();

    // The same content reached by a different route: fill everything, then
    // zero it back down. That promotes to dense on the way, so the field ends
    // holding the same values in a different container.
    DetailField dense;
    dense.reset(n);
    for (std::uint32_t v = 0; v < n; ++v) dense.set(v, d(9.0f, 9.0f, 9.0f));
    for (std::uint32_t v = 0; v < n; ++v) dense.set(v, LocalDetail{});
    dense.set(3, d(1.0f, 0.0f, 0.0f));
    dense.set(700, d(0.0f, 2.0f, 0.0f));
    CHECK(dense.dense());
    CHECK(dense.checksum() == sum);

    // And compaction — which moves blocks around and may demote — changes
    // nothing about it.
    dense.compact();
    CHECK(dense.checksum() == sum);
    sparse.compact();
    CHECK(sparse.checksum() == sum);

    // A single changed coefficient changes it.
    sparse.set(700, d(0.0f, 2.001f, 0.0f));
    CHECK(sparse.checksum() != sum);
}

TEST_CASE("a round trip is exact and deterministic") {
    DetailField f;
    f.reset(10000);
    for (std::uint32_t v = 0; v < 10000; v += 37)
        f.set(v, d(static_cast<float>(v) * 1e-4f, -static_cast<float>(v) * 2e-4f, 0.5f));

    const std::vector<std::uint8_t> bytes = f.encode();
    // Twice, byte for byte: an encoding that walked a hash map would not be.
    CHECK(f.encode() == bytes);

    DetailField back;
    REQUIRE(DetailField::decode(bytes.data(), bytes.size(), &back));
    CHECK(back.vertex_count() == f.vertex_count());
    CHECK(back.checksum() == f.checksum());
    for (std::uint32_t v = 0; v < 10000; ++v) {
        CHECK(back.get(v).tangent == f.get(v).tangent);
        CHECK(back.get(v).bitangent == f.get(v).bitangent);
        CHECK(back.get(v).normal == f.get(v).normal);
    }
    CHECK(back.encode() == bytes);
}

TEST_CASE("an empty field round-trips to an empty field") {
    DetailField f;
    f.reset(5000);
    const std::vector<std::uint8_t> bytes = f.encode();
    DetailField back;
    REQUIRE(DetailField::decode(bytes.data(), bytes.size(), &back));
    CHECK(back.empty());
    CHECK(back.vertex_count() == 5000);
    CHECK(back.resident_vertices() == 0);
}

TEST_CASE("the decoder refuses a truncated, hostile or unknown buffer") {
    DetailField f;
    f.reset(2000);
    f.set(10, d(1, 2, 3));
    f.set(1500, d(4, 5, 6));
    const std::vector<std::uint8_t> bytes = f.encode();

    DetailField out;
    CHECK_FALSE(DetailField::decode(nullptr, 0, &out));
    CHECK_FALSE(DetailField::decode(bytes.data(), 0, &out));
    CHECK_FALSE(DetailField::decode(bytes.data(), 8, &out));
    for (std::size_t cut = 16; cut < bytes.size(); cut += 313)
        CHECK_FALSE(DetailField::decode(bytes.data(), cut, &out));

    std::vector<std::uint8_t> wrong = bytes;
    wrong[0] ^= 0xFF;
    CHECK_FALSE(DetailField::decode(wrong.data(), wrong.size(), &out));

    std::vector<std::uint8_t> newer = bytes;
    newer[4] = 99;
    CHECK_FALSE(DetailField::decode(newer.data(), newer.size(), &out));

    // A BLOCK COUNT larger than the buffer can hold, which is how a reader gets
    // asked to allocate gigabytes from a few hundred bytes. Refused by
    // arithmetic, before anything is reserved.
    std::vector<std::uint8_t> hostile = bytes;
    hostile[16] = 0xff;
    hostile[17] = 0xff;
    hostile[18] = 0xff;
    hostile[19] = 0x0f;
    CHECK_FALSE(DetailField::decode(hostile.data(), hostile.size(), &out));

    // A BLOCK SIZE that is not a power of two in range describes a layout this
    // reader cannot address; taking it on trust would put the block arithmetic
    // out of step with the stream.
    for (std::uint32_t bad : {0u, 3u, 100u, 1u << 24}) {
        std::vector<std::uint8_t> wrong_block = bytes;
        for (int i = 0; i < 4; ++i)
            wrong_block[12 + i] = static_cast<std::uint8_t>((bad >> (i * 8)) & 0xffu);
        CHECK_FALSE(DetailField::decode(wrong_block.data(), wrong_block.size(), &out));
    }

    // A vertex count past the ceiling, likewise.
    std::vector<std::uint8_t> huge = bytes;
    huge[8] = 0xff;
    huge[9] = 0xff;
    huge[10] = 0xff;
    huge[11] = 0xff;
    CHECK_FALSE(DetailField::decode(huge.data(), huge.size(), &out));

    // Blocks out of order describe several overlaid fields rather than one.
    std::vector<std::uint8_t> shuffled = bytes;
    shuffled[20] = 0xff;
    shuffled[21] = 0x00;
    CHECK_FALSE(DetailField::decode(shuffled.data(), shuffled.size(), &out));
}

TEST_CASE("the block size is a parameter, and the content does not depend on it") {
    // WHAT MAKES THE DEFAULT DEFENSIBLE: the number can be swept, so a
    // benchmark can say what it costs rather than a comment asserting it is
    // reasonable. Everything a caller can observe about the CONTENT has to be
    // independent of the choice.
    const std::uint32_t n = 5000;
    std::vector<std::pair<std::uint32_t, LocalDetail>> written;
    for (std::uint32_t v = 1200; v < 1500; v += 7)
        written.emplace_back(v, d(static_cast<float>(v) * 1e-3f, 0.0f, 0.25f));

    std::uint64_t reference = 0;
    for (std::uint32_t block : {4u, 64u, 256u, 1024u, 4096u}) {
        DetailField f;
        f.reset(n, block);
        CHECK(f.block_size() == block);
        for (const auto& [vertex, value] : written) f.set(vertex, value);
        for (const auto& [vertex, value] : written) CHECK(f.get(vertex) == value);
        CHECK(f.get(0).zero());
        CHECK(f.get(n - 1).zero());

        // THE CHECKSUM IS THE CONTENT, and the block size is not part of it.
        if (reference == 0)
            reference = f.checksum();
        else
            CHECK(f.checksum() == reference);

        // A round trip carries the block size, so decoding does not silently
        // re-block a field into the reader's default and change its bytes.
        const std::vector<std::uint8_t> encoded = f.encode();
        DetailField back;
        REQUIRE(DetailField::decode(encoded.data(), encoded.size(), &back));
        CHECK(back.block_size() == block);
        CHECK(back.checksum() == reference);
        CHECK(back.encode() == encoded);
    }

    // A block size of zero takes the default; anything else is rounded up to a
    // power of two so every access stays a shift.
    DetailField defaulted;
    defaulted.reset(n, 0);
    CHECK(defaulted.block_size() == DetailField::kDefaultBlockSize);
    DetailField rounded;
    rounded.reset(n, 300);
    CHECK(rounded.block_size() == 512);
}

TEST_CASE("a smaller block wastes less on a footprint and costs more in table") {
    // The trade the sweep in `BM_MultiresDetailBlockSize` measures, asserted
    // here in the direction rather than the magnitude: a footprint that touches
    // a contiguous run of a large level allocates fewer bytes at a small block
    // size, and the block table it pays for is the other side of that.
    const std::uint32_t n = 200000;
    std::size_t small_bytes = 0, large_bytes = 0;
    for (std::uint32_t block : {64u, 4096u}) {
        DetailField f;
        f.reset(n, block);
        for (std::uint32_t v = 100000; v < 100400; ++v) f.set(v, d(0, 0, 0.1f));
        (block == 64u ? small_bytes : large_bytes) = f.resident_vertices();
    }
    CHECK(small_bytes < large_bytes);
}

// The bug this pins: `compact()` packed the CONTENT and left the CAPACITY, and
// `bytes()` reports capacity. Compaction therefore released nothing a host could
// observe -- and on the demotion path, which rebuilds through `set()`, the
// vectors grow by the standard library's own factor. libstdc++ doubles and MSVC
// grows by 1.5, so the same compaction landed on a different capacity per
// toolchain: on MSVC it landed ABOVE the dense form it replaced and the stack's
// bytes went UP after compacting, which is how CI caught it.
//
// Asserted as "no slack against a field built sparse from the start" rather than
// as "fewer bytes than before", because that is the claim that is true on every
// toolchain instead of true on the one it was written on.
TEST_CASE("detail field: compaction releases capacity, not only content") {
    const std::uint32_t vertices = 8192;
    const std::uint32_t block = 1024;

    // Dense, then emptied back to a handful of live blocks, which is the path
    // that demotes -- and the path whose rebuild overshoots.
    DetailField promoted;
    promoted.reset(vertices, block);
    for (std::uint32_t v = 0; v < vertices; ++v) promoted.set(v, d(0.001f, 0.0f, 0.002f));
    REQUIRE(promoted.dense());
    for (std::uint32_t v = block; v < vertices; ++v) promoted.set(v, d(0.0f, 0.0f, 0.0f));

    const std::size_t before = promoted.bytes();
    promoted.compact();
    CHECK_FALSE(promoted.dense());
    CHECK(promoted.bytes() < before);

    // The toolchain-independent half: what survived is one block of content, so
    // the compacted field must cost what a field that only ever held that block
    // costs. Any slack here is capacity the compaction did not give back.
    DetailField built_sparse;
    built_sparse.reset(vertices, block);
    for (std::uint32_t v = 0; v < block; ++v) built_sparse.set(v, d(0.001f, 0.0f, 0.002f));
    REQUIRE_FALSE(built_sparse.dense());
    CHECK(promoted.bytes() == built_sparse.bytes());

    // And compaction stays a memory lever rather than a change to the picture.
    for (std::uint32_t v = 0; v < vertices; ++v)
        CHECK(promoted.get(v).normal == built_sparse.get(v).normal);

    // Idempotent: a second compaction of an already-packed field must not move
    // the number either way.
    const std::size_t packed = promoted.bytes();
    promoted.compact();
    CHECK(promoted.bytes() == packed);
}
