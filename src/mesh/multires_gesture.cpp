#include "clay/mesh/multires_gesture.h"

#include <algorithm>
#include <utility>

#include "clay/mesh/dynamic_surface.h"  // SurfaceMark: the per-process seed

namespace clay {
namespace mesh {

namespace {

constexpr std::uint32_t kGestureMagic = 0x44524D43u;  // "CMRD", little-endian
constexpr std::uint32_t kGestureVersion = 1;
constexpr std::size_t kHeaderBytes = 40;

// One random value per process. Borrowed from the adaptive surface's lineage,
// which exists for the same reason: a record from another process must match
// nothing here, however its counters happen to line up.
std::uint64_t process_origin() {
    static const std::uint64_t origin = SurfaceMark::fresh().lineage;
    return origin;
}

void put_u32(std::vector<std::uint8_t>* out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put_u64(std::vector<std::uint8_t>* out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out->push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put_half(std::vector<std::uint8_t>* out, const std::vector<std::uint8_t>& half) {
    put_u64(out, half.size());
    out->insert(out->end(), half.begin(), half.end());
}

struct Reader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t at = 0;

    bool u32(std::uint32_t* v) {
        if (size - at < 4) return false;
        *v = 0;
        for (int i = 0; i < 4; ++i) *v |= static_cast<std::uint32_t>(data[at + i]) << (8 * i);
        at += 4;
        return true;
    }
    bool u64(std::uint64_t* v) {
        if (size - at < 8) return false;
        *v = 0;
        for (int i = 0; i < 8; ++i) *v |= static_cast<std::uint64_t>(data[at + i]) << (8 * i);
        at += 8;
        return true;
    }
    // A length-prefixed half, bounded by what is actually left before it is
    // trusted: a hostile length is refused, never allocated.
    bool half(const std::uint8_t** begin, std::size_t* length) {
        std::uint64_t n = 0;
        if (!u64(&n) || n > size - at) return false;
        *begin = data + at;
        *length = static_cast<std::size_t>(n);
        at += *length;
        return true;
    }
};

// The magic and the version word, which decide whether the rest is ours to read.
GestureDecode read_version(Reader* r) {
    std::uint32_t magic = 0, version = 0;
    if (!r->u32(&magic) || magic != kGestureMagic) return GestureDecode::Malformed;
    if (!r->u32(&version)) return GestureDecode::Malformed;
    if (version > kGestureVersion) return GestureDecode::ForwardVersion;
    return version == kGestureVersion ? GestureDecode::Ok : GestureDecode::Malformed;
}

// One half: absent, or exactly its record's encoding of a NON-EMPTY record.
// `encode` never writes an empty half's bytes, and a half with bytes left over
// would be a second record hiding behind the first.
template <class Record>
bool decode_half(const std::uint8_t* data, std::size_t length, Record* out) {
    if (length == 0) return true;
    if (!Record::decode(data, length, out)) return false;
    return !out->empty() && out->encoded_size() == length;
}

}  // namespace

void MultiresGesture::clear() {
    base_.clear();
    layer_.clear();
    origin_ = 0;
    structure_ = 0;
}

bool MultiresGesture::bound_to(const MultiresSurface& surface) const {
    return origin_ == process_origin() && structure_ == surface.structure_revision();
}

bool MultiresGesture::accepts(const MultiresSurface& surface) const {
    if (empty()) return true;
    if (!bound_to(surface)) return false;
    // The next stamp writes the active pass, or the base when there is none. A
    // base write joins either half; a pass write joins only its own.
    const SculptLayerId active = surface.sculpt_layers().active();
    return layer_.empty() || active == kNoSculptLayer || active == layer_.layer();
}

void MultiresGesture::bind(const MultiresSurface& surface) {
    if (empty()) return;
    origin_ = process_origin();
    structure_ = surface.structure_revision();
}

bool MultiresGesture::replayable(const MultiresSurface& surface) const {
    if (empty()) return true;
    if (!bound_to(surface)) return false;
    if (!base_.empty() && !base_.matches(surface)) return false;
    return layer_.empty() || layer_.matches(surface.sculpt_layers());
}

bool MultiresGesture::replay(MultiresSurface& surface, bool forward) const {
    if (!replayable(surface)) return false;
    if (!base_.empty()) {
        // Checked by `replayable` above, so neither can refuse here.
        const bool wrote = forward ? base_.apply(surface) : base_.revert(surface);
        (void)wrote;
    }
    if (layer_.empty()) return true;
    surface.apply_sculpt_layer_delta(layer_, forward);
    // A BASE write marks its patches as it lands; a PASS write only queues its
    // blocks for recomposition, and nothing is marked until a level is next
    // evaluated. Measured: a reverted pass left clay_multires_dirty_blocks
    // EMPTY, so a host re-copying its dirty blocks after an undo redrew
    // nothing. Evaluating the display level here is the recomposition the
    // host's next copy would have paid anyway, moved to where it can be seen.
    surface.positions_at(surface.display_level());
    return true;
}

bool MultiresGesture::revert(MultiresSurface& surface) const { return replay(surface, false); }
bool MultiresGesture::apply(MultiresSurface& surface) const { return replay(surface, true); }

std::vector<std::uint32_t> MultiresGesture::levels() const {
    std::vector<std::uint32_t> out = base_.levels();
    const std::vector<std::uint32_t> layer = layer_.levels();
    out.insert(out.end(), layer.begin(), layer.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::size_t MultiresGesture::encoded_size() const {
    return kHeaderBytes + (base_.empty() ? 0 : base_.encoded_size()) +
           (layer_.empty() ? 0 : layer_.encoded_size());
}

std::size_t MultiresGesture::bytes() const {
    return sizeof(*this) + base_.bytes() + layer_.bytes();
}

// The envelope: magic, version, origin, structure, then each half as a 64-bit
// length and that record's own encoding -- zero length for an empty half. The
// halves are the records' own bytes, so their decoders keep doing the
// validation they already do.
std::vector<std::uint8_t> MultiresGesture::encode() const {
    std::vector<std::uint8_t> out;
    out.reserve(encoded_size());
    put_u32(&out, kGestureMagic);
    put_u32(&out, kGestureVersion);
    put_u64(&out, origin_);
    put_u64(&out, structure_);
    put_half(&out, base_.empty() ? std::vector<std::uint8_t>{} : base_.encode());
    put_half(&out, layer_.empty() ? std::vector<std::uint8_t>{} : layer_.encode());
    return out;
}

GestureDecode MultiresGesture::decode(const std::uint8_t* data, std::size_t size,
                                      MultiresGesture* out) {
    if (!data || !out) return GestureDecode::Malformed;
    Reader r{data, size};
    const GestureDecode version = read_version(&r);
    if (version != GestureDecode::Ok) return version;

    MultiresGesture g;
    const std::uint8_t* base = nullptr;
    const std::uint8_t* layer = nullptr;
    std::size_t base_n = 0, layer_n = 0;
    const bool framed = r.u64(&g.origin_) && r.u64(&g.structure_) && r.half(&base, &base_n) &&
                        r.half(&layer, &layer_n) && r.at == size;  // nothing trailing
    if (!framed) return GestureDecode::Malformed;
    if (!decode_half(base, base_n, &g.base_) || !decode_half(layer, layer_n, &g.layer_))
        return GestureDecode::Malformed;
    // A gesture holding anything carries a binding, and an empty one none.
    if (g.empty() != (g.origin_ == 0)) return GestureDecode::Malformed;
    *out = std::move(g);
    return GestureDecode::Ok;
}

}  // namespace mesh
}  // namespace clay
