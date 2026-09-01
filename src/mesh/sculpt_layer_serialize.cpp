// THE BYTE FORM of a layer stack, of one gesture on it and of one property
// change to it (file-io spec, add-mesh-sculpt-layers).
//
// WHAT IS WRITTEN. Per layer: the id, the name, the KIND, visibility, lock,
// strength, and each level's coefficients and mask. Nothing derived — not the
// composed field, which is `B + Σ s·m·L` and reproducible from what is here,
// and not the coverage or byte figures, which are measurements of the
// containers rather than content.
//
// THE KIND IS WRITTEN AND AN UNKNOWN ONE IS REFUSED, not skipped. A stream
// carrying a procedural pore layer that a reader silently drops presents a
// surface missing an artist's work while claiming to be complete — the same
// failure the surface's version bump exists to prevent, one level down. Each
// layer's payload is LENGTH-PREFIXED anyway, so a later format can offer
// deliberate skipping where refusing is wrong; the bytes to make that choice
// exist from the first release and the choice itself is not made now.
//
// EVERY DECLARED COUNT IS CHECKED AGAINST THE BUFFER BEFORE THE ARRAY IT
// DESCRIBES IS RESERVED. A few hundred bytes declaring four billion layers is a
// request for more memory than a machine holds, and it has to be refused by
// arithmetic rather than by `bad_alloc`.

#include <algorithm>
#include <cstring>

#include "clay/mesh/sculpt_layer.h"

namespace clay {
namespace mesh {
namespace {

constexpr std::uint32_t kMaskMagic = 0x46574d43u;   // 'CMWF'
constexpr std::uint32_t kMaskVersion = 1u;
constexpr std::uint32_t kStackMagic = 0x534c4d43u;  // 'CMLS'
constexpr std::uint32_t kStackVersion = 1u;
constexpr std::uint32_t kDeltaMagic = 0x444c4d43u;  // 'CMLD'
constexpr std::uint32_t kDeltaVersion = 1u;
constexpr std::uint32_t kPropertyMagic = 0x504c4d43u;  // 'CMLP'
constexpr std::uint32_t kPropertyVersion = 1u;

// A ceiling on what one stream may declare per array, matching the multires
// stream's own.
constexpr std::uint32_t kMaxArray = 1u << 28;

void put_u32(std::vector<std::uint8_t>* out, std::uint32_t v) {
    out->push_back(static_cast<std::uint8_t>(v & 0xffu));
    out->push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
    out->push_back(static_cast<std::uint8_t>((v >> 16) & 0xffu));
    out->push_back(static_cast<std::uint8_t>((v >> 24) & 0xffu));
}

void put_u64(std::vector<std::uint8_t>* out, std::uint64_t v) {
    put_u32(out, static_cast<std::uint32_t>(v & 0xffffffffu));
    put_u32(out, static_cast<std::uint32_t>(v >> 32));
}

void put_f32(std::vector<std::uint8_t>* out, float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    put_u32(out, bits);
}

void put_detail(std::vector<std::uint8_t>* out, const LocalDetail& d) {
    put_f32(out, d.tangent);
    put_f32(out, d.bitangent);
    put_f32(out, d.normal);
}

void put_blob(std::vector<std::uint8_t>* out, const std::vector<std::uint8_t>& blob) {
    put_u32(out, static_cast<std::uint32_t>(blob.size()));
    out->insert(out->end(), blob.begin(), blob.end());
}

struct Reader {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t at = 0;

    std::size_t remaining() const { return size - at; }
    bool u32(std::uint32_t* out) {
        if (at + 4 > size) return false;
        *out = static_cast<std::uint32_t>(data[at]) |
               (static_cast<std::uint32_t>(data[at + 1]) << 8) |
               (static_cast<std::uint32_t>(data[at + 2]) << 16) |
               (static_cast<std::uint32_t>(data[at + 3]) << 24);
        at += 4;
        return true;
    }
    bool u64(std::uint64_t* out) {
        std::uint32_t lo = 0, hi = 0;
        if (!u32(&lo) || !u32(&hi)) return false;
        *out = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
        return true;
    }
    bool f32(float* out) {
        std::uint32_t bits = 0;
        if (!u32(&bits)) return false;
        std::memcpy(out, &bits, 4);
        return true;
    }
    bool vec3(kernel::cfloat3* out) { return f32(&out->x) && f32(&out->y) && f32(&out->z); }
    bool detail(LocalDetail* out) {
        return f32(&out->tangent) && f32(&out->bitangent) && f32(&out->normal);
    }
    // A declared count, checked against what the buffer could possibly hold
    // BEFORE the array it describes is reserved.
    bool count(std::uint32_t stride_bytes, std::uint32_t* out) {
        if (!u32(out)) return false;
        if (*out > kMaxArray) return false;
        return static_cast<std::size_t>(*out) <= remaining() / stride_bytes;
    }
    bool blob(std::vector<std::uint8_t>* out) {
        std::uint32_t n = 0;
        if (!count(1u, &n)) return false;
        out->assign(data + at, data + at + n);
        at += n;
        return true;
    }
    bool skip(std::size_t n) {
        if (remaining() < n) return false;
        at += n;
        return true;
    }
};

}  // namespace

// -- the mask -----------------------------------------------------------------

std::vector<std::uint8_t> SparseWeightField::encode() const {
    std::vector<std::uint32_t> blocks;
    const std::uint32_t total = block_count();
    for (std::uint32_t b = 0; b < total; ++b)
        if (block_stored(b)) blocks.push_back(b);

    std::vector<std::uint8_t> out;
    put_u32(&out, kMaskMagic);
    put_u32(&out, kMaskVersion);
    put_u32(&out, vertex_count_);
    put_u32(&out, block_size_);
    put_u32(&out, static_cast<std::uint32_t>(blocks.size()));
    for (std::uint32_t b : blocks) {
        put_u32(&out, b);
        const std::uint32_t begin = b * block_size_;
        for (std::uint32_t i = 0; i < block_size_; ++i) {
            const std::uint32_t v = begin + i;
            // Past the end of the level writes the IDENTITY rather than a zero:
            // a decoder that read it back would otherwise store a mask of 0 on
            // the tail of the last block, and a later resize would find the
            // layer erased there.
            put_f32(&out, v < vertex_count_ ? get(v) : 1.0f);
        }
    }
    return out;
}

bool SparseWeightField::decode(const std::uint8_t* data, std::size_t size,
                               SparseWeightField* out) {
    if (!data || !out) return false;
    Reader r{data, size, 0};
    std::uint32_t magic = 0, version = 0, vertex_count = 0, block_size = 0, block_total = 0;
    if (!r.u32(&magic) || magic != kMaskMagic) return false;
    if (!r.u32(&version) || version != kMaskVersion) return false;
    if (!r.u32(&vertex_count) || vertex_count > kMaxVertices) return false;
    if (!r.u32(&block_size)) return false;
    if (block_size < 4u || block_size > (1u << 20) || (block_size & (block_size - 1u)) != 0u)
        return false;
    if (!r.u32(&block_total)) return false;

    const std::uint32_t blocks = (vertex_count + block_size - 1) / block_size;
    if (block_total > blocks) return false;
    const std::size_t per_block = 4u + static_cast<std::size_t>(block_size) * 4u;
    if (static_cast<std::size_t>(block_total) > r.remaining() / per_block) return false;

    SparseWeightField field;
    field.reset(vertex_count, block_size);
    std::uint32_t previous = 0;
    for (std::uint32_t i = 0; i < block_total; ++i) {
        std::uint32_t block = 0;
        if (!r.u32(&block) || block >= blocks) return false;
        // Ascending and without repeats, which is what `encode` writes and what
        // makes a stream describe one field rather than several overlaid.
        if (i > 0 && block <= previous) return false;
        previous = block;
        for (std::uint32_t k = 0; k < block_size; ++k) {
            float w = 1.0f;
            if (!r.f32(&w)) return false;
            const std::uint32_t v = block * block_size + k;
            if (v < vertex_count) field.set(v, w);
        }
    }
    *out = std::move(field);
    return true;
}

// -- the stack ----------------------------------------------------------------

namespace {

std::vector<std::uint8_t> encode_layer(const SculptLayer& layer) {
    std::vector<std::uint8_t> out;
    put_u64(&out, layer.id);
    put_u32(&out, static_cast<std::uint32_t>(layer.kind));
    put_u32(&out, (layer.visible ? 1u : 0u) | (layer.locked ? 2u : 0u));
    put_f32(&out, layer.strength);
    put_u32(&out, static_cast<std::uint32_t>(layer.name.size()));
    out.insert(out.end(), layer.name.begin(), layer.name.end());
    put_u32(&out, static_cast<std::uint32_t>(layer.detail.size()));
    for (std::size_t l = 0; l < layer.detail.size(); ++l) {
        put_blob(&out, layer.detail[l].encode());
        put_blob(&out, layer.mask[l].encode());
    }
    return out;
}

bool decode_layer(Reader* r, SculptLayer* out) {
    std::uint32_t kind = 0, flags = 0, name_size = 0, levels = 0;
    if (!r->u64(&out->id) || out->id == kNoSculptLayer) return false;
    if (!r->u32(&kind)) return false;
    // AN UNKNOWN KIND IS REFUSED. See the file header: a reader that drops a
    // layer it does not understand presents a partial sculpt as a whole one.
    if (kind != static_cast<std::uint32_t>(SculptLayerKind::Sampled)) return false;
    out->kind = SculptLayerKind::Sampled;
    if (!r->u32(&flags)) return false;
    out->visible = (flags & 1u) != 0u;
    out->locked = (flags & 2u) != 0u;
    if (!r->f32(&out->strength)) return false;
    // A strength outside [0,1] describes a stack this library would not have
    // produced, and taking it on trust would let a file amplify a pass past
    // what was recorded.
    if (!(out->strength >= 0.0f) || out->strength > 1.0f) return false;
    if (!r->count(1u, &name_size) || name_size > SculptLayerStack::kMaxNameBytes) return false;
    out->name.assign(reinterpret_cast<const char*>(r->data + r->at), name_size);
    if (!r->skip(name_size)) return false;

    // Each level costs at least two four-byte lengths, so the ceiling is the
    // buffer itself rather than a preference.
    if (!r->count(8u, &levels)) return false;
    out->detail.assign(levels, DetailField{});
    out->mask.assign(levels, SparseWeightField{});
    for (std::uint32_t l = 0; l < levels; ++l) {
        std::vector<std::uint8_t> blob;
        if (!r->blob(&blob)) return false;
        if (!DetailField::decode(blob.data(), blob.size(), &out->detail[l])) return false;
        if (!r->blob(&blob)) return false;
        if (!SparseWeightField::decode(blob.data(), blob.size(), &out->mask[l])) return false;
    }
    return true;
}

}  // namespace

std::vector<std::uint8_t> SculptLayerStack::encode() const {
    std::vector<std::uint8_t> out;
    put_u32(&out, kStackMagic);
    put_u32(&out, kStackVersion);
    put_u32(&out, static_cast<std::uint32_t>(layers_.size()));
    put_u64(&out, active_);
    // THE ID COUNTER IS WRITTEN. Without it a reload would mint an id a
    // previous session had already spent, and two layers in one document would
    // answer to the same handle after the next `add`.
    put_u64(&out, next_id_);
    put_u32(&out, block_size_);
    put_u32(&out, static_cast<std::uint32_t>(level_vertices_.size()));
    for (std::uint32_t v : level_vertices_) put_u32(&out, v);
    for (const SculptLayer& layer : layers_) put_blob(&out, encode_layer(layer));
    return out;
}

bool SculptLayerStack::decode(const std::uint8_t* data, std::size_t size, SculptLayerStack* out) {
    if (!data || !out) return false;
    Reader r{data, size, 0};
    std::uint32_t magic = 0, version = 0, count = 0, block_size = 0, levels = 0;
    if (!r.u32(&magic) || magic != kStackMagic) return false;
    if (!r.u32(&version) || version != kStackVersion) return false;
    // A layer is at least a header's worth of bytes, so the declared count is
    // priced against the buffer BEFORE the vector is reserved.
    if (!r.count(24u, &count) || count > kMaxLayers) return false;

    SculptLayerStack stack;
    if (!r.u64(&stack.active_) || !r.u64(&stack.next_id_)) return false;
    if (!r.u32(&block_size)) return false;
    if (block_size < 4u || block_size > (1u << 20) || (block_size & (block_size - 1u)) != 0u)
        return false;
    stack.block_size_ = block_size;
    if (!r.count(4u, &levels)) return false;
    stack.level_vertices_.resize(levels);
    for (std::uint32_t l = 0; l < levels; ++l)
        if (!r.u32(&stack.level_vertices_[l])) return false;

    stack.dirty_.assign(levels, LevelDirty{});
    for (std::uint32_t l = 0; l < levels; ++l) {
        stack.dirty_[l].mark.assign(stack.level_block_count(l), 0);
        stack.dirty_[l].all = true;
    }

    for (std::uint32_t i = 0; i < count; ++i) {
        std::vector<std::uint8_t> payload;
        if (!r.blob(&payload)) return false;
        Reader lr{payload.data(), payload.size(), 0};
        SculptLayer layer;
        if (!decode_layer(&lr, &layer)) return false;
        // Two layers answering to one id would make every lookup ambiguous and
        // every undo record apply to whichever came first.
        if (stack.index_of(layer.id) != kNoSculptLayerIndex) return false;
        if (layer.id >= stack.next_id_) return false;
        // A layer's per-level fields must describe THIS stack's levels. A
        // stream pairing one level's coefficients with a different vertex count
        // would silently attach every wrinkle somewhere else.
        layer.detail.resize(levels);
        layer.mask.resize(levels);
        for (std::uint32_t l = 0; l < levels; ++l) {
            const std::uint32_t vertices = stack.level_vertices_[l];
            if (layer.detail[l].vertex_count() != 0 &&
                layer.detail[l].vertex_count() != vertices)
                return false;
            if (layer.mask[l].vertex_count() != 0 && layer.mask[l].vertex_count() != vertices)
                return false;
        }
        stack.layers_.push_back(std::move(layer));
    }
    if (stack.active_ != kNoSculptLayer && stack.index_of(stack.active_) == kNoSculptLayerIndex)
        return false;
    *out = std::move(stack);
    return true;
}

// -- one gesture --------------------------------------------------------------

std::vector<std::uint8_t> SculptLayerDelta::encode() const {
    std::vector<std::uint8_t> out;
    put_u32(&out, kDeltaMagic);
    put_u32(&out, kDeltaVersion);
    put_u64(&out, layer_);
    put_u32(&out, static_cast<std::uint32_t>(detail_.size()));
    put_u32(&out, static_cast<std::uint32_t>(mask_.size()));
    for (const DetailEntry& e : detail_) {
        put_u32(&out, e.level);
        put_u32(&out, e.vertex);
        put_detail(&out, e.before);
        put_detail(&out, e.after);
    }
    for (const MaskEntry& e : mask_) {
        put_u32(&out, e.level);
        put_u32(&out, e.vertex);
        put_f32(&out, e.before);
        put_f32(&out, e.after);
    }
    return out;
}

bool SculptLayerDelta::decode(const std::uint8_t* data, std::size_t size, SculptLayerDelta* out) {
    if (!data || !out) return false;
    Reader r{data, size, 0};
    std::uint32_t magic = 0, version = 0, detail_count = 0, mask_count = 0;
    if (!r.u32(&magic) || magic != kDeltaMagic) return false;
    if (!r.u32(&version) || version != kDeltaVersion) return false;
    SculptLayerDelta record;
    if (!r.u64(&record.layer_)) return false;
    if (!r.count(32u, &detail_count)) return false;
    if (!r.count(16u, &mask_count)) return false;

    for (std::uint32_t i = 0; i < detail_count; ++i) {
        DetailEntry e;
        if (!r.u32(&e.level) || !r.u32(&e.vertex)) return false;
        if (!r.detail(&e.before) || !r.detail(&e.after)) return false;
        const std::uint64_t key = key_of(e.level, e.vertex);
        // One entry a vertex, which is what coalescing produces. A stream with
        // two would describe a gesture that undoes to the middle of itself.
        if (record.detail_slot_.find(key) != record.detail_slot_.end()) return false;
        record.detail_slot_.emplace(key, static_cast<std::uint32_t>(record.detail_.size()));
        record.detail_.push_back(e);
    }
    for (std::uint32_t i = 0; i < mask_count; ++i) {
        MaskEntry e;
        if (!r.u32(&e.level) || !r.u32(&e.vertex)) return false;
        if (!r.f32(&e.before) || !r.f32(&e.after)) return false;
        const std::uint64_t key = key_of(e.level, e.vertex);
        if (record.mask_slot_.find(key) != record.mask_slot_.end()) return false;
        record.mask_slot_.emplace(key, static_cast<std::uint32_t>(record.mask_.size()));
        record.mask_.push_back(e);
    }
    *out = std::move(record);
    return true;
}

// -- one property change ------------------------------------------------------

std::vector<std::uint8_t> SculptLayerProperty::encode() const {
    std::vector<std::uint8_t> out;
    put_u32(&out, kPropertyMagic);
    put_u32(&out, kPropertyVersion);
    put_u32(&out, static_cast<std::uint32_t>(op));
    put_u64(&out, layer);
    put_u32(&out, static_cast<std::uint32_t>(name_before.size()));
    out.insert(out.end(), name_before.begin(), name_before.end());
    put_u32(&out, static_cast<std::uint32_t>(name_after.size()));
    out.insert(out.end(), name_after.begin(), name_after.end());
    put_f32(&out, strength_before);
    put_f32(&out, strength_after);
    put_u32(&out, (flag_before ? 1u : 0u) | (flag_after ? 2u : 0u));
    put_u64(&out, active_before);
    put_u64(&out, active_after);
    put_blob(&out, stack_before);
    put_blob(&out, stack_after);
    put_u32(&out, static_cast<std::uint32_t>(base_detail.size()));
    for (const DetailEntry& e : base_detail) {
        put_u32(&out, e.level);
        put_u32(&out, e.vertex);
        put_detail(&out, e.before);
        put_detail(&out, e.after);
    }
    put_u32(&out, static_cast<std::uint32_t>(base_vertices.size()));
    for (std::size_t i = 0; i < base_vertices.size(); ++i) {
        put_u32(&out, base_vertices[i]);
        put_f32(&out, base_before[i].x);
        put_f32(&out, base_before[i].y);
        put_f32(&out, base_before[i].z);
        put_f32(&out, base_after[i].x);
        put_f32(&out, base_after[i].y);
        put_f32(&out, base_after[i].z);
    }
    return out;
}

bool SculptLayerProperty::decode(const std::uint8_t* data, std::size_t size,
                                 SculptLayerProperty* out) {
    if (!data || !out) return false;
    Reader r{data, size, 0};
    std::uint32_t magic = 0, version = 0, op_value = 0, flags = 0, n = 0;
    if (!r.u32(&magic) || magic != kPropertyMagic) return false;
    if (!r.u32(&version) || version != kPropertyVersion) return false;
    if (!r.u32(&op_value) || op_value > static_cast<std::uint32_t>(Op::Structural)) return false;

    SculptLayerProperty p;
    p.op = static_cast<Op>(op_value);
    if (!r.u64(&p.layer)) return false;
    for (std::string* name : {&p.name_before, &p.name_after}) {
        if (!r.count(1u, &n) || n > SculptLayerStack::kMaxNameBytes) return false;
        name->assign(reinterpret_cast<const char*>(r.data + r.at), n);
        if (!r.skip(n)) return false;
    }
    if (!r.f32(&p.strength_before) || !r.f32(&p.strength_after)) return false;
    if (!r.u32(&flags)) return false;
    p.flag_before = (flags & 1u) != 0u;
    p.flag_after = (flags & 2u) != 0u;
    if (!r.u64(&p.active_before) || !r.u64(&p.active_after)) return false;
    if (!r.blob(&p.stack_before) || !r.blob(&p.stack_after)) return false;

    if (!r.count(32u, &n)) return false;
    p.base_detail.resize(n);
    for (DetailEntry& e : p.base_detail) {
        if (!r.u32(&e.level) || !r.u32(&e.vertex)) return false;
        if (!r.detail(&e.before) || !r.detail(&e.after)) return false;
    }
    if (!r.count(28u, &n)) return false;
    p.base_vertices.resize(n);
    p.base_before.resize(n);
    p.base_after.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (!r.u32(&p.base_vertices[i])) return false;
        if (!r.vec3(&p.base_before[i]) || !r.vec3(&p.base_after[i])) return false;
    }
    *out = std::move(p);
    return true;
}

}  // namespace mesh
}  // namespace clay
