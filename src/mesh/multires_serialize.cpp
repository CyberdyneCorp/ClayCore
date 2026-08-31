// The byte form of a hierarchy and of one gesture on it (file-io spec,
// add-mesh-multires).
//
// WHAT IS WRITTEN AND WHAT IS NOT. The cage, the rule, the level count, which
// levels were active, and each level's detail. NOT the per-level face lists:
// they follow from the cage and the rule, and a level-4 face list over a 20k
// cage is eighty megabytes of something the reader can derive in a fraction of
// the time it would take to read. NOT the evaluated positions, for the same
// reason and a stronger one — they are a function of the two things above, so
// storing them would create a second answer that a corrupt file could make
// disagree with the first.
//
// THE RULE IS RECORDED RATHER THAN ASSUMED. A hierarchy reconstructed with a
// different subdivision rule than it was authored with is a different surface,
// and nothing else in the stream reveals the substitution — the counts, the
// detail and the cage would all still line up.
//
// THE DECODER PRICES THE HIERARCHY BEFORE BUILDING IT. A few hundred bytes can
// declare a twelve-level hierarchy over a large cage, which is a request for
// more memory than a machine holds. The level counts follow from the cage by
// arithmetic, so they are computed and checked against a ceiling BEFORE the
// first level is allocated.

#include <cstring>

#include "multires_internal.h"
#include "clay/mesh/multires_sculpt.h"

namespace clay {
namespace mesh {
namespace {

constexpr std::uint32_t kSurfaceMagic = 0x53524d43u;  // 'CMRS'
constexpr std::uint32_t kSurfaceVersion = 1u;
constexpr std::uint32_t kDeltaMagic = 0x44524d43u;  // 'CMRD'
constexpr std::uint32_t kDeltaVersion = 1u;

// A ceiling on what a stream may declare per array, so a hostile count is
// refused by arithmetic rather than by `bad_alloc`.
constexpr std::uint32_t kMaxArray = 1u << 28;

void put_u32(std::vector<std::uint8_t>* out, std::uint32_t v) {
    out->push_back(static_cast<std::uint8_t>(v & 0xffu));
    out->push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
    out->push_back(static_cast<std::uint8_t>((v >> 16) & 0xffu));
    out->push_back(static_cast<std::uint8_t>((v >> 24) & 0xffu));
}

void put_f32(std::vector<std::uint8_t>* out, float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    put_u32(out, bits);
}

void put_vec3(std::vector<std::uint8_t>* out, kernel::cfloat3 v) {
    put_f32(out, v.x);
    put_f32(out, v.y);
    put_f32(out, v.z);
}

struct Reader {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t at = 0;

    bool u32(std::uint32_t* out) {
        if (at + 4 > size) return false;
        *out = static_cast<std::uint32_t>(data[at]) |
               (static_cast<std::uint32_t>(data[at + 1]) << 8) |
               (static_cast<std::uint32_t>(data[at + 2]) << 16) |
               (static_cast<std::uint32_t>(data[at + 3]) << 24);
        at += 4;
        return true;
    }
    bool f32(float* out) {
        std::uint32_t bits = 0;
        if (!u32(&bits)) return false;
        std::memcpy(out, &bits, 4);
        return true;
    }
    bool vec3(kernel::cfloat3* out) { return f32(&out->x) && f32(&out->y) && f32(&out->z); }
    bool vec2(kernel::cfloat2* out) { return f32(&out->x) && f32(&out->y); }
    std::size_t remaining() const { return size - at; }
    // A declared count, checked against what the buffer could possibly hold
    // BEFORE the array it describes is reserved.
    bool count(std::uint32_t stride_bytes, std::uint32_t* out) {
        if (!u32(out)) return false;
        if (*out > kMaxArray) return false;
        return static_cast<std::size_t>(*out) <= remaining() / stride_bytes;
    }
};

void put_positions(std::vector<std::uint8_t>* out, const std::vector<kernel::cfloat3>& v) {
    put_u32(out, static_cast<std::uint32_t>(v.size()));
    for (const kernel::cfloat3& p : v) put_vec3(out, p);
}

bool take_positions(Reader* r, std::vector<kernel::cfloat3>* out) {
    std::uint32_t n = 0;
    if (!r->count(12u, &n)) return false;
    out->resize(n);
    for (std::uint32_t i = 0; i < n; ++i)
        if (!r->vec3(&(*out)[i])) return false;
    return true;
}

void put_indices(std::vector<std::uint8_t>* out, const std::vector<std::uint32_t>& v) {
    put_u32(out, static_cast<std::uint32_t>(v.size()));
    for (std::uint32_t i : v) put_u32(out, i);
}

bool take_indices(Reader* r, std::vector<std::uint32_t>* out) {
    std::uint32_t n = 0;
    if (!r->count(4u, &n)) return false;
    out->resize(n);
    for (std::uint32_t i = 0; i < n; ++i)
        if (!r->u32(&(*out)[i])) return false;
    return true;
}

// Would this cage, subdivided this many times, exceed what this build will
// reconstruct? Pure arithmetic on the level-0 counts, so the answer arrives
// before the first allocation.
bool depth_affordable(std::uint64_t vertices, std::uint64_t edges, std::uint64_t faces,
                      std::uint64_t corners, std::uint32_t levels) {
    for (std::uint32_t l = 1; l < levels; ++l) {
        const std::uint64_t v = vertices + edges + faces;
        if (v > MultiresSurface::kMaxLevelVertices) return false;
        const std::uint64_t f = corners;
        const std::uint64_t e = 2ull * edges + corners;
        vertices = v;
        faces = f;
        edges = e;
        corners = 4ull * f;
    }
    return true;
}

}  // namespace

std::vector<std::uint8_t> MultiresSurface::encode() const {
    std::vector<std::uint8_t> out;
    put_u32(&out, kSurfaceMagic);
    put_u32(&out, kSurfaceVersion);
    if (!state_) {
        put_u32(&out, 0);  // rule
        put_u32(&out, 0);  // level count
        return out;
    }
    put_u32(&out, static_cast<std::uint32_t>(state_->options.rule));
    put_u32(&out, static_cast<std::uint32_t>(state_->levels.size()));
    put_u32(&out, state_->sculpt_level);
    put_u32(&out, state_->display_level);
    put_f32(&out, state_->options.weld_epsilon);

    const Mesh& m = state_->base;
    put_positions(&out, m.positions);
    put_positions(&out, m.normals);
    put_positions(&out, m.colors);
    put_u32(&out, static_cast<std::uint32_t>(m.uvs.size()));
    for (const kernel::cfloat2& uv : m.uvs) {
        put_f32(&out, uv.x);
        put_f32(&out, uv.y);
    }
    put_indices(&out, m.indices);
    put_indices(&out, m.quads);

    for (std::size_t l = 1; l < state_->levels.size(); ++l) {
        const std::vector<std::uint8_t> blob = state_->levels[l].detail.encode();
        put_u32(&out, static_cast<std::uint32_t>(blob.size()));
        out.insert(out.end(), blob.begin(), blob.end());
    }
    return out;
}

namespace {

// The cage's arrays, with every declared count checked against what the buffer
// could hold BEFORE the array it describes is reserved.
bool read_base_mesh(Reader* r, Mesh* base) {
    if (!take_positions(r, &base->positions)) return false;
    if (!take_positions(r, &base->normals)) return false;
    if (!take_positions(r, &base->colors)) return false;
    std::uint32_t uv_count = 0;
    if (!r->count(8u, &uv_count)) return false;
    base->uvs.resize(uv_count);
    for (std::uint32_t i = 0; i < uv_count; ++i)
        if (!r->vec2(&base->uvs[i])) return false;
    if (!take_indices(r, &base->indices)) return false;
    if (!take_indices(r, &base->quads)) return false;
    // The optional arrays are "empty or one per vertex" on `mesh::Mesh`, and a
    // stream saying otherwise describes a mesh nothing in this library would
    // have produced.
    const std::size_t n = base->positions.size();
    if (!base->normals.empty() && base->normals.size() != n) return false;
    if (!base->colors.empty() && base->colors.size() != n) return false;
    if (!base->uvs.empty() && base->uvs.size() != n) return false;
    return true;
}

// The header, up to but not including the cage.
struct SurfaceHeader {
    std::uint32_t levels = 0;
    std::uint32_t sculpt = 0;
    std::uint32_t display = 0;
    float weld = kDefaultWeldEpsilon;
    SubdivisionRule rule = SubdivisionRule::CatmullClark;
};

bool read_header(Reader* r, SurfaceHeader* out) {
    std::uint32_t magic = 0, version = 0, rule = 0;
    if (!r->u32(&magic) || magic != kSurfaceMagic) return false;
    if (!r->u32(&version) || version != kSurfaceVersion) return false;
    if (!r->u32(&rule)) return false;
    // THE RULE IS READ RATHER THAN ASSUMED. A hierarchy reconstructed with a
    // different rule than it was authored with is a different surface, and
    // nothing else in the stream reveals the substitution.
    if (rule != static_cast<std::uint32_t>(SubdivisionRule::CatmullClark)) return false;
    out->rule = static_cast<SubdivisionRule>(rule);
    if (!r->u32(&out->levels)) return false;
    if (out->levels == 0) return true;
    if (out->levels > MultiresSurface::kMaxLevels) return false;
    if (!r->u32(&out->sculpt) || !r->u32(&out->display) || !r->f32(&out->weld)) return false;
    return out->sculpt < out->levels && out->display < out->levels;
}

}  // namespace

bool MultiresSurface::decode(const std::uint8_t* data, std::size_t size, MultiresSurface* out) {
    if (!data || !out) return false;
    Reader r{data, size, 0};
    SurfaceHeader header;
    if (!read_header(&r, &header)) return false;
    if (header.levels == 0) {
        *out = MultiresSurface{};
        return true;
    }

    Mesh base;
    if (!read_base_mesh(&r, &base)) return false;

    MultiresOptions options;
    options.rule = header.rule;
    if (header.weld >= 0.0f) options.weld_epsilon = header.weld;
    MultiresError err = MultiresError::None;
    std::optional<MultiresSurface> surface = from_mesh(base, options, &err);
    if (!surface) return false;

    // THE DEPTH IS PRICED BEFORE IT IS BUILT. The counts follow from the cage by
    // arithmetic, so a stream declaring a hierarchy nothing could hold is
    // refused here rather than after eleven levels have been allocated.
    const MultiresLevel& level0 = surface->state_->levels[0];
    if (!depth_affordable(level0.topology.vertex_count, level0.edge_count,
                          level0.topology.face_count, level0.topology.corners.size(),
                          header.levels))
        return false;

    for (std::uint32_t l = 1; l < header.levels; ++l)
        if (!surface->add_level(&err)) return false;

    for (std::uint32_t l = 1; l < header.levels; ++l) {
        std::uint32_t blob_size = 0;
        if (!r.count(1u, &blob_size)) return false;
        DetailField field;
        if (!DetailField::decode(data + r.at, blob_size, &field)) return false;
        r.at += blob_size;
        // A level's detail must describe THAT level. A stream pairing one
        // level's coefficients with a different vertex count would silently
        // attach every wrinkle somewhere else.
        if (field.vertex_count() != surface->topology_at(l).vertex_count) return false;
        surface->state_->levels[l].detail = std::move(field);
    }

    surface->state_->sculpt_level = header.sculpt;
    surface->state_->display_level = header.display;
    *out = std::move(*surface);
    return true;
}

// -- one gesture --------------------------------------------------------------

std::vector<std::uint8_t> MultiresDelta::encode() const {
    std::vector<std::uint8_t> out;
    put_u32(&out, kDeltaMagic);
    put_u32(&out, kDeltaVersion);
    put_u32(&out, static_cast<std::uint32_t>(detail_.size()));
    put_u32(&out, static_cast<std::uint32_t>(base_vertices_.size()));
    for (const DetailEntry& e : detail_) {
        put_u32(&out, e.level);
        put_u32(&out, e.vertex);
        put_f32(&out, e.before.tangent);
        put_f32(&out, e.before.bitangent);
        put_f32(&out, e.before.normal);
        put_f32(&out, e.after.tangent);
        put_f32(&out, e.after.bitangent);
        put_f32(&out, e.after.normal);
    }
    for (std::size_t i = 0; i < base_vertices_.size(); ++i) {
        put_u32(&out, base_vertices_[i]);
        put_vec3(&out, base_before_[i]);
        put_vec3(&out, base_after_[i]);
    }
    return out;
}

bool MultiresDelta::decode(const std::uint8_t* data, std::size_t size, MultiresDelta* out) {
    if (!data || !out) return false;
    Reader r{data, size, 0};
    std::uint32_t magic = 0, version = 0, detail_count = 0, base_count = 0;
    if (!r.u32(&magic) || magic != kDeltaMagic) return false;
    if (!r.u32(&version) || version != kDeltaVersion) return false;
    if (!r.count(32u, &detail_count)) return false;
    if (!r.count(28u, &base_count)) return false;

    MultiresDelta record;
    for (std::uint32_t i = 0; i < detail_count; ++i) {
        DetailEntry e;
        if (!r.u32(&e.level) || !r.u32(&e.vertex)) return false;
        if (!r.f32(&e.before.tangent) || !r.f32(&e.before.bitangent) || !r.f32(&e.before.normal))
            return false;
        if (!r.f32(&e.after.tangent) || !r.f32(&e.after.bitangent) || !r.f32(&e.after.normal))
            return false;
        if (e.level == 0) return false;  // the cage is recorded as positions, not coefficients
        const std::uint64_t key = key_of(e.level, e.vertex);
        if (record.detail_slot_.find(key) != record.detail_slot_.end()) return false;
        record.detail_slot_.emplace(key, static_cast<std::uint32_t>(record.detail_.size()));
        record.detail_.push_back(e);
    }
    for (std::uint32_t i = 0; i < base_count; ++i) {
        std::uint32_t vertex = 0;
        kernel::cfloat3 before, after;
        if (!r.u32(&vertex) || !r.vec3(&before) || !r.vec3(&after)) return false;
        if (record.base_slot_.find(vertex) != record.base_slot_.end()) return false;
        record.base_slot_.emplace(vertex, static_cast<std::uint32_t>(record.base_vertices_.size()));
        record.base_vertices_.push_back(vertex);
        record.base_before_.push_back(before);
        record.base_after_.push_back(after);
    }
    *out = std::move(record);
    return true;
}

}  // namespace mesh
}  // namespace clay
