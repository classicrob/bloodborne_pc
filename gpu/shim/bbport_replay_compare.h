// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (perf replay): helpers of the image round-trip comparison (bbport_replay.cpp) that the
// replay-capture test exercises without a GPU.
#pragma once

#include <map>
#include <utility>

namespace BbReplay {

/// Inserts `value` under `key`; false (and the map unchanged) when the key is already present.
/// A round trip compared by key is a bijection only if neither side holds a key twice.
template <class Key, class Value, class Compare, class Alloc>
bool InsertUnique(std::map<Key, Value, Compare, Alloc>& map, const Key& key, Value&& value) {
    return map.emplace(key, std::forward<Value>(value)).second;
}

} // namespace BbReplay
