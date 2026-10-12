// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Vulkan::Motion {

struct VertexRange {
    uint32_t first{}, count{};
    bool operator==(const VertexRange&) const = default;
};

// Reserve only the vertices referenced by this draw, not the whole shared vertex buffer.
// Primitive restart markers are not vertex shader invocations. Reject signed overflow.
template <class Index, size_t Extent>
VertexRange IndexedRange(std::span<const Index, Extent> indices, int32_t base_vertex, bool restart) {
    uint32_t lo = UINT32_MAX, hi = 0;
    for (const auto index : indices) {
        if (restart && index == std::numeric_limits<Index>::max()) {
            continue;
        }
        lo = std::min(lo, uint32_t(index));
        hi = std::max(hi, uint32_t(index));
    }
    if (lo > hi || int64_t(lo) + base_vertex < 0 ||
        int64_t(hi) + base_vertex > INT32_MAX) {
        return {};
    }
    return {uint32_t(int64_t(lo) + base_vertex), hi - lo + 1};
}

// Vertex shader constant buffers (stride 16) of Bloodborne's G-buffer shaders: the 864-byte
// scene constants (camera), 416-byte model constants and palettes of 3x4 bone matrices
// (48 bytes per bone). Characters have 656+ bytes; weapons and props 2-8 bones (96-384).
enum class BufferRole { Other, Skeleton, SmallSkeleton };
inline BufferRole ClassifyBuffer(uint32_t size) {
    if (size == 864 || size > 16384) {
        return BufferRole::Other;
    }
    if (size >= 640) {
        return BufferRole::Skeleton;
    }
    return size >= 96 && size % 48 == 0 ? BufferRole::SmallSkeleton : BufferRole::Other;
}

// Scanning and hashing the index list of every stored draw each frame took ~10% of the GPU
// thread. Static meshes keep their index buffers, so a result is reused and fully verified
// again every `Revalidate` frames. A stale result only costs camera-quality vectors: the
// shader bounds every history access by the stored range.
class IndexRangeCache {
public:
    static constexpr uint64_t Revalidate = 32, Unused = 600;
    struct Key {
        uint64_t address{};
        uint32_t count{}, index_size{};
        int32_t base_vertex{};
        bool restart{};
        bool operator==(const Key&) const = default;
    };
    struct Result {
        VertexRange range;
        uint64_t topology{};
    };
    struct Stats {
        uint64_t hits{}, scans{}, stale{};
    } stats;

    template <class Scan>
    Result Get(const Key& key, uint64_t frame, Scan&& scan) {
        auto& entry = entries[key];
        entry.last_use = frame;
        if (entry.valid && frame - entry.verified < Revalidate) {
            ++stats.hits;
            return entry.result;
        }
        ++stats.scans;
        const Result fresh = scan();
        if (entry.valid && (fresh.topology != entry.result.topology ||
                            !(fresh.range == entry.result.range))) {
            ++stats.stale; // the cached result was wrong for up to Revalidate frames
        }
        entry.result = fresh;
        entry.verified = frame;
        entry.valid = true;
        return entry.result;
    }
    void Trim(uint64_t frame) {
        std::erase_if(entries, [frame](const auto& e) { return frame - e.second.last_use > Unused; });
    }
    size_t Size() const { return entries.size(); }

private:
    struct Entry {
        Result result;
        uint64_t verified{}, last_use{};
        bool valid{};
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            uint64_t h = k.address * 0x9e3779b97f4a7c15ULL;
            h ^= (uint64_t(k.count) << 32 | uint32_t(k.base_vertex)) + (h << 6) + (h >> 2);
            h ^= uint64_t(k.index_size << 1 | uint32_t(k.restart)) + (h << 6) + (h >> 2);
            return size_t(h);
        }
    };
    std::unordered_map<Key, Entry, KeyHash> entries;
};

// Open-addressing map for the per-frame tables of History: entries are keyed per draw and
// the tables are cleared every frame. std::unordered_map allocated a node per insert (four
// per draw) and freed them all at the frame end; here clear() only starts a new generation.
template <class Key, class Value, class Hash>
class FlatMap {
public:
    FlatMap() : slots(64) {}

    Value& operator[](const Key& key) {
        return Insert(key).first->value;
    }
    /// Inserts `value` when `key` is absent (like std::unordered_map::emplace).
    void emplace(const Key& key, const Value& value) {
        const auto [slot, inserted] = Insert(key);
        if (inserted) {
            slot->value = value;
        }
    }
    const Value* find(const Key& key) const {
        const size_t mask = slots.size() - 1;
        for (size_t i = Start(key) & mask;; i = (i + 1) & mask) {
            const Slot& slot = slots[i];
            if (slot.generation != generation) {
                return nullptr;
            }
            if (slot.key == key) {
                return &slot.value;
            }
        }
    }
    void clear() {
        size = 0;
        if (++generation == 0) { // wrapped: stale slots could look current
            for (auto& slot : slots) {
                slot.generation = 0;
            }
            generation = 1;
        }
    }
    void swap(FlatMap& other) noexcept {
        slots.swap(other.slots);
        std::swap(size, other.size);
        std::swap(generation, other.generation);
    }

private:
    struct Slot {
        Key key{};
        Value value{};
        uint32_t generation = 0;
    };
    static size_t Start(const Key& key) {
        const uint64_t h = uint64_t(Hash{}(key)) * 0x9e3779b97f4a7c15ULL;
        return size_t(h ^ (h >> 29));
    }
    std::pair<Slot*, bool> Insert(const Key& key) {
        if ((size + 1) * 2 > slots.size()) {
            Grow();
        }
        const size_t mask = slots.size() - 1;
        for (size_t i = Start(key) & mask;; i = (i + 1) & mask) {
            Slot& slot = slots[i];
            if (slot.generation != generation) {
                slot = {key, Value{}, generation};
                ++size;
                return {&slot, true};
            }
            if (slot.key == key) {
                return {&slot, false};
            }
        }
    }
    void Grow() {
        std::vector<Slot> old(slots.size() * 2);
        old.swap(slots);
        const uint32_t live = generation;
        generation = 1;
        size = 0;
        for (const auto& slot : old) {
            if (slot.generation == live) {
                *Insert(slot.key).first = {slot.key, slot.value, generation};
            }
        }
    }

    std::vector<Slot> slots;
    size_t size = 0;
    uint32_t generation = 1;
};

inline uint32_t JitterPhases(uint32_t render_width, uint32_t output_width) {
    if (!render_width || !output_width) {
        return 8;
    }
    const double ratio = double(output_width) / render_width;
    return uint32_t(std::clamp(std::ceil(8.0 * ratio * ratio), 8.0, 256.0));
}

struct Draw {
    uint64_t shader{}, geometry{}, indices{}, topology{};
    uint32_t index_count{}, instances{}, first_instance{};
    VertexRange vertices;
    bool operator==(const Draw& rhs) const = default;
};

// CPU bookkeeping shared with the regression test. Draw identity includes topology and
// offsets: a reused index buffer or another submesh must never read uninitialised history.
class History {
public:
    explicit History(uint32_t capacity) : capacity{capacity} {}

    struct Allocation {
        uint32_t store{}, load{}, vertices{}, instances{}, first_vertex{}, first_instance{};
    };
    struct Stats {
        uint64_t draws{}, stored{}, loaded{}, unmatched{}, exhausted{}, invalid{}, still{};
        /// No history loaded because the previous copy is not identified as this instance:
        /// none or several of the mesh's previous copies near this one (ambiguous), or no
        /// instance anchor (no_identity). Draw order is not an identity: culling reorders
        /// copies of one mesh, and loading by order gave other copies' positions, hundreds of
        /// pixels off.
        uint64_t ambiguous{}, no_identity{};
    } stats;

    /// An instance's world position: bone 0's translation (3x4 rows, the .w of each row) of the
    /// draw's palette, which stays in world space while the camera moves. Copies of one mesh
    /// are told apart by it.
    using Anchor = std::array<float, 3>;
    /// The furthest an instance moves between two frames and is still recognised (world
    /// units). A history loads only when exactly one of the mesh's previous copies lies within
    /// 2 * MaxStep of the anchor and it lies within MaxStep; otherwise none does. Too small
    /// only loses history (camera vectors), never takes another copy's.
    static constexpr float MaxStep = 0.5f;
    /// Copies of one mesh tracked per frame; with more, none of them loads history.
    static constexpr uint32_t MaxCopies = 16;

    // A small skeleton (weapon, prop; most are static world pieces) only needs history
    // while its bone palette changes: with last frame's bones its motion is the camera's.
    // Checked before the index scan of Prepare, with a cheap identity. The first frame
    // of a change stores positions (nothing to load yet), the next one loads them.
    // Copies of one mesh share the identity, and their draw order changes with culling. So a
    // draw counts as still when its palette is ANY of last frame's palettes for the mesh (a
    // set, not the copy at the same position in the order). This is a conservative eligibility
    // decision for history (a still copy gets camera vectors), not proof that the instance is
    // motionless or the same as last frame's.
    struct GateKey {
        uint64_t shader{}, geometry{}, indices{};
        uint32_t index_count{}, instances{}, first_instance{};
        uint32_t occurrence{}; ///< unused (kept for the callers' designated initializers)
        bool operator==(const GateKey&) const = default;
    };
    bool Moving(GateKey key, uint64_t palette) {
        key.occurrence = 0;
        const bool seen = gate_previous_seen.find(key) != nullptr;
        const bool moving = seen && gate_previous.find({key, palette}) == nullptr;
        gate_current_seen.emplace(key, 1);
        gate_current.emplace({key, palette}, 1);
        stats.still += moving ? 0 : 1;
        return moving;
    }

    void NextFrame() {
        previous.swap(current);
        current.clear();
        gate_previous.swap(gate_current);
        gate_current.clear();
        gate_previous_seen.swap(gate_current_seen);
        gate_current_seen.clear();
        ++frame;
        used = 0;
    }

    Allocation Prepare(const Draw& draw, const std::optional<Anchor>& anchor) {
        ++stats.draws;
        const uint64_t count = uint64_t(draw.vertices.count) * draw.instances;
        if (!count || uint64_t(draw.vertices.first) + draw.vertices.count - 1 > INT32_MAX ||
            uint64_t(draw.first_instance) + draw.instances > UINT32_MAX) {
            ++stats.invalid;
            return {};
        }
        Allocation result{0, 0, draw.vertices.count, draw.instances,
                          draw.vertices.first, draw.first_instance};
        if (previous.find(draw) == nullptr) {
            ++stats.unmatched;
        } else if (!anchor) {
            ++stats.no_identity;
        } else if (const auto match = Identify(previous[draw], *anchor)) {
            result.load = *match;
            ++stats.loaded;
        } else {
            ++stats.ambiguous;
        }
        if (count <= capacity - used) {
            result.store = 1 + uint32_t(frame & 1) * capacity + used;
            used += uint32_t(count);
            auto& copies = current[draw];
            if (copies.count < MaxCopies) {
                copies.items[copies.count++] = {result.store, anchor.value_or(Anchor{}),
                                                anchor.has_value(), false};
            } else {
                copies.overflow = true;
            }
            ++stats.stored;
        } else {
            ++stats.exhausted;
        }
        return result;
    }

    uint32_t Used() const { return used; }

private:
    struct Copy {
        uint32_t store{};
        Anchor anchor{};
        bool anchored{}, taken{};
    };
    struct Copies {
        std::array<Copy, MaxCopies> items{};
        uint32_t count{};
        bool overflow{};
    };
    /// The previous copy that is this instance: the only one within 2 * MaxStep, and within
    /// MaxStep; not taken by another draw this frame.
    static std::optional<uint32_t> Identify(Copies& copies, const Anchor& anchor) {
        if (copies.overflow) {
            return std::nullopt;
        }
        const float near = MaxStep * MaxStep, around = 4.0f * near;
        Copy* found = nullptr;
        uint32_t nearby = 0;
        for (uint32_t i = 0; i < copies.count; ++i) {
            Copy& copy = copies.items[i];
            if (!copy.anchored) {
                return std::nullopt; // a copy without identity could be this instance
            }
            const float dx = copy.anchor[0] - anchor[0], dy = copy.anchor[1] - anchor[1],
                        dz = copy.anchor[2] - anchor[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (!(d2 <= around)) { // NaN: not nearby
                continue;
            }
            ++nearby;
            if (d2 <= near) {
                found = &copy;
            }
        }
        if (nearby != 1 || !found || found->taken) {
            return std::nullopt;
        }
        found->taken = true;
        return found->store;
    }
    struct DrawHash {
        size_t operator()(const Draw& d) const {
            uint64_t h = 0;
            const auto mix = [&](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
            mix(d.shader); mix(d.geometry); mix(d.indices); mix(d.topology);
            mix(d.index_count); mix(d.instances); mix(d.first_instance);
            mix(d.vertices.first); mix(d.vertices.count);
            return size_t(h);
        }
    };
    uint32_t capacity{}, used{};
    uint64_t frame{};
    FlatMap<Draw, Copies, DrawHash> previous, current;
    struct GateHash {
        size_t operator()(const GateKey& key) const {
            uint64_t h = 0;
            const auto mix = [&](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
            mix(key.shader); mix(key.geometry); mix(key.indices); mix(key.index_count);
            mix(key.instances); mix(key.first_instance); mix(key.occurrence);
            return size_t(h);
        }
    };
    struct GatePalette {
        GateKey key;
        uint64_t palette{};
        bool operator==(const GatePalette&) const = default;
    };
    struct GatePaletteHash {
        size_t operator()(const GatePalette& p) const {
            return GateHash{}(p.key) ^ size_t(p.palette * 0x9e3779b97f4a7c15ULL);
        }
    };
    FlatMap<GatePalette, uint8_t, GatePaletteHash> gate_previous, gate_current;
    FlatMap<GateKey, uint8_t, GateHash> gate_previous_seen, gate_current_seen;
};

} // namespace Vulkan::Motion
