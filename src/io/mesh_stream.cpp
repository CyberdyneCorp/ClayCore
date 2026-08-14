#include "clay/io/mesh_io.h"

#include <cstring>

#include "clay/mesh/quad_mesh.h"  // quads_consistent

namespace clay {
namespace io {

namespace {

constexpr std::size_t kHeaderBytes = 9;  // u32 vertices + u32 indices + u8 attributes

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}

void put_f32(std::vector<std::uint8_t>& out, float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);  // the bit pattern, so the round trip is exact
    put_u32(out, bits);
}

std::uint32_t take_u32(const std::uint8_t*& p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p[i]) << (i * 8);
    p += 4;
    return v;
}

float take_f32(const std::uint8_t*& p) {
    std::uint32_t bits = take_u32(p);
    float f = 0.0f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// Vertex-aligned when present, empty otherwise — the invariant mesh::Mesh
// documents, and the one the attribute mask encodes. A mismatched array is
// treated as absent rather than written short, which is what the C accessors
// already do with one.
template <typename T>
bool carries(const std::vector<T>& v, std::size_t vertex_count) {
    return !v.empty() && v.size() == vertex_count;
}

std::uint8_t attribute_mask(const mesh::Mesh& m) {
    const std::size_t n = m.positions.size();
    std::uint8_t mask = 0;
    if (carries(m.normals, n)) mask |= kMeshHasNormals;
    if (carries(m.colors, n)) mask |= kMeshHasColors;
    if (carries(m.uvs, n)) mask |= kMeshHasUvs;
    return mask;
}

// What one vertex costs given the attributes a chunk declares. The bound that
// makes an over-declared count refusable without allocating for it.
std::uint64_t bytes_per_vertex(std::uint8_t mask) {
    std::uint64_t bytes = 12;
    if (mask & kMeshHasNormals) bytes += 12;
    if (mask & kMeshHasColors) bytes += 12;
    if (mask & kMeshHasUvs) bytes += 8;
    return bytes;
}

void read_f3(const std::uint8_t*& p, std::vector<kernel::cfloat3>* out, std::size_t count) {
    out->reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        float x = take_f32(p);
        float y = take_f32(p);
        out->push_back(kernel::cf3(x, y, take_f32(p)));
    }
}

// The quad tail, once the caller has established that bytes remain for one.
// Absent is the normal case and is not an error — every mesh written before
// this section existed, and every triangle mesh written since, ends before it —
// so the absent case stays at the call site and only the present one is here.
//
// Present but malformed is refused, exactly as an out-of-range triangle index
// is: a chunk this library did not write is not trusted to be half right, and a
// quad list that contradicts the triangles beside it would travel on as a mesh
// whose two index arrays describe different surfaces.
//
// This claims the FIRST tail: a later minor that appends another section must
// write it after the quads (bytes past the quad list are ignored here, which is
// where it goes), because a section written before them would be read as a quad
// count and refused.
IoStatus read_quad_tail(const std::uint8_t*& p, std::uint64_t tail, std::uint64_t vertex_count,
                        mesh::Mesh* m) {
    if (tail < 4) return IoStatus::fail(IoError::Malformed, "truncated mesh quad section");
    const std::uint64_t quad_count = take_u32(p);
    if (quad_count * 4 * 4 > tail - 4)
        return IoStatus::fail(IoError::Malformed,
                              "mesh stream declares " + std::to_string(quad_count) +
                                  " quads but does not carry them");
    m->quads.reserve(static_cast<std::size_t>(quad_count) * 4);
    for (std::uint64_t i = 0; i < quad_count * 4; ++i) {
        std::uint32_t index = take_u32(p);
        if (index >= vertex_count)
            return IoStatus::fail(IoError::Malformed, "a mesh quad points past the vertices");
        m->quads.push_back(index);
    }
    if (!mesh::quads_consistent(*m))
        return IoStatus::fail(IoError::Malformed,
                              "a mesh quad section is not the triangulation beside it");
    return IoStatus::success();
}

}  // namespace

std::vector<std::uint8_t> save_mesh_stream(const mesh::Mesh& m) {
    const std::size_t n = m.positions.size();
    const std::uint8_t mask = attribute_mask(m);

    std::vector<std::uint8_t> out;
    out.reserve(kHeaderBytes + n * static_cast<std::size_t>(bytes_per_vertex(mask)) +
                m.indices.size() * 4);
    put_u32(out, static_cast<std::uint32_t>(n));
    put_u32(out, static_cast<std::uint32_t>(m.indices.size()));
    out.push_back(mask);

    for (const kernel::cfloat3& p : m.positions) {
        put_f32(out, p.x);
        put_f32(out, p.y);
        put_f32(out, p.z);
    }
    auto put_f3 = [&out](const std::vector<kernel::cfloat3>& v) {
        for (const kernel::cfloat3& e : v) {
            put_f32(out, e.x);
            put_f32(out, e.y);
            put_f32(out, e.z);
        }
    };
    if (mask & kMeshHasNormals) put_f3(m.normals);
    if (mask & kMeshHasColors) put_f3(m.colors);
    if (mask & kMeshHasUvs)
        for (const kernel::cfloat2& uv : m.uvs) {
            put_f32(out, uv.x);
            put_f32(out, uv.y);
        }
    for (std::uint32_t index : m.indices) put_u32(out, index);
    // The quad section, appended only when there is one. Written after every
    // byte an older reader knows how to count, which is what makes it a tail
    // that reader skips rather than a stream it refuses.
    //
    // A quad list that is NOT the triangulation beside it is not written at
    // all: the loader below refuses one, so writing it would produce a
    // document this very library cannot open. Dropping it costs the quads and
    // keeps the mesh — the same trade every operation that rewrites indices
    // makes through mesh::drop_quads.
    if (m.has_quads() && mesh::quads_consistent(m)) {
        put_u32(out, static_cast<std::uint32_t>(m.quad_count()));
        for (std::uint32_t index : m.quads) put_u32(out, index);
    }
    return out;
}

IoStatus load_mesh_stream(const std::uint8_t* data, std::size_t size, mesh::Mesh* out) {
    if (!data || !out) return IoStatus::fail(IoError::Malformed, "null mesh stream");
    if (size < kHeaderBytes) return IoStatus::fail(IoError::Malformed, "mesh stream too small");

    const std::uint8_t* p = data;
    const std::uint64_t vertex_count = take_u32(p);
    const std::uint64_t index_count = take_u32(p);
    const std::uint8_t mask = *p++;
    const std::uint64_t body = size - kHeaderBytes;

    if (index_count % 3 != 0)
        return IoStatus::fail(IoError::Malformed, "mesh index count is not whole triangles");
    // An attribute this build cannot lay out changes the stride, so the stream
    // cannot be read at all — refused rather than misread as a shorter one.
    if (mask & ~(kMeshHasNormals | kMeshHasColors | kMeshHasUvs))
        return IoStatus::fail(IoError::Malformed, "mesh stream declares an unknown attribute");
    // Both counts bounded in 64-bit against the bytes actually present, before
    // a single element is reserved: a chunk may claim four billion vertices.
    // Bounded rather than equal, so a later minor may append to the stream and
    // this reader skips the tail instead of refusing the file.
    //
    // THE QUAD SECTION DEPENDS ON THAT BEING AN INEQUALITY. Tighten this to
    // equality and every build older than the quad section starts refusing
    // whole documents that contain one, instead of reading their meshes as the
    // triangles they already are.
    const std::uint64_t declared = vertex_count * bytes_per_vertex(mask) + index_count * 4;
    if (declared > body)
        return IoStatus::fail(IoError::Malformed,
                              "mesh stream declares " + std::to_string(declared) +
                                  " bytes of geometry but carries " + std::to_string(body));

    mesh::Mesh m;
    const std::size_t n = static_cast<std::size_t>(vertex_count);
    read_f3(p, &m.positions, n);
    if (mask & kMeshHasNormals) read_f3(p, &m.normals, n);
    if (mask & kMeshHasColors) read_f3(p, &m.colors, n);
    if (mask & kMeshHasUvs) {
        m.uvs.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            float u = take_f32(p);
            m.uvs.push_back(kernel::cf2(u, take_f32(p)));
        }
    }
    m.indices.reserve(static_cast<std::size_t>(index_count));
    for (std::uint64_t i = 0; i < index_count; ++i) {
        std::uint32_t index = take_u32(p);
        if (index >= vertex_count)
            return IoStatus::fail(IoError::Malformed, "a mesh index points past the vertices");
        m.indices.push_back(index);
    }

    const std::uint64_t tail = body - declared;
    if (tail != 0) {
        const IoStatus quads = read_quad_tail(p, tail, vertex_count, &m);
        if (!quads.ok()) return quads;
    }
    *out = std::move(m);
    return IoStatus::success();
}

}  // namespace io
}  // namespace clay
