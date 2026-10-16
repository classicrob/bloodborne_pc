// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (perf replay): helpers of the image round-trip comparison (bbport_replay.cpp) that the
// replay-capture test exercises without a GPU.
#pragma once

#include <map>
#include <cstring>
#include "video_core/amdgpu/replay_format.h"
#include <utility>

namespace BbReplay {

/// Inserts `value` under `key`; false (and the map unchanged) when the key is already present.
/// A round trip compared by key is a bijection only if neither side holds a key twice.
template <class Key, class Value, class Compare, class Alloc>
bool InsertUnique(std::map<Key, Value, Compare, Alloc>& map, const Key& key, Value&& value) {
    return map.emplace(key, std::forward<Value>(value)).second;
}

inline bool HostSpanValid(const ReplayFormat::HostObjectRecord& object,
                          std::uint64_t address, std::uint64_t bytes) {
    return object.name[0] && std::memchr(object.name, 0, sizeof(object.name)) &&
           object.size && object.size <= UINT64_MAX - object.start && bytes &&
           object.offset <= object.size && bytes <= object.size - object.offset &&
           address >= object.start && address - object.start == object.offset;
}

inline bool HostMatches(const ReplayFormat::HostObjectRecord& object, std::uint64_t address,
                        std::uint64_t bytes, std::uint64_t start, std::uint64_t size) {
    return HostSpanValid(object, address, bytes) && start == object.start && size == object.size;
}

} // namespace BbReplay
