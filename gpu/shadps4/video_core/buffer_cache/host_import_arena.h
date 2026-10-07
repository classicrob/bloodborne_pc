// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/types.h"

namespace VideoCore {

struct HostImportBackingRange {
    VAddr guest;
    u64 size;
    u64 backing;
};

class HostImportBackingMap {
public:
    struct Conflict {
        HostImportBackingRange requested;
        HostImportBackingRange existing;
    };

    std::optional<Conflict> Check(VAddr start, u64 size,
                                 std::span<const HostImportBackingRange> ranges) const {
        const auto overlap = [](const auto& a, const auto& b) {
            return a.backing < b.backing + b.size && b.backing < a.backing + a.size;
        };
        for (size_t i = 0; i < ranges.size(); ++i) {
            for (size_t j = 0; j < i; ++j) {
                if (overlap(ranges[i], ranges[j])) return Conflict{ranges[i], ranges[j]};
            }
            for (const auto& [address, entry] : imports) {
                // These guest views are replaced after the growth completion wait.
                if (address < start + size && start < address + entry.size) continue;
                for (const auto& previous : entry.ranges) {
                    if (overlap(ranges[i], previous)) return Conflict{ranges[i], previous};
                }
            }
        }
        return std::nullopt;
    }

    void Register(VAddr start, u64 size, std::vector<HostImportBackingRange> ranges) {
        EraseOverlapping(start, size);
        imports.emplace(start, Entry{size, std::move(ranges)});
    }

    void EraseOverlapping(VAddr start, u64 size) {
        for (auto it = imports.begin(); it != imports.end();) {
            if (it->first < start + size && start < it->first + it->second.size) {
                it = imports.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    struct Entry {
        u64 size;
        std::vector<HostImportBackingRange> ranges;
    };
    std::map<VAddr, Entry> imports;
};

// Resource must expose cpu_addr and size_bytes. Overlapping imports share the
// same guest bytes; migrate updates address metadata without copying their data.
// retire keeps replaced views alive until recorded GPU work completes.
template <typename Resource>
class HostImportArenaMap {
public:
    HostImportArenaMap(u32 page_bits_, u64 max_buffer_size_)
        : page_size{u64{1} << page_bits_}, max_buffer_size{max_buffer_size_} {}

    const Resource* Find(VAddr address) const {
        auto it = arenas.upper_bound(address);
        if (it == arenas.begin()) {
            return nullptr;
        }
        const auto& resource = *std::prev(it)->second;
        return address - resource.cpu_addr < resource.size_bytes ? &resource : nullptr;
    }

    template <typename Allocate, typename Migrate, typename Retire, typename Complete>
    const Resource* Obtain(VAddr address, u64 size, Allocate&& allocate, Migrate&& migrate,
                           Retire&& retire, Complete&& complete) {
        ASSERT_MSG(size && address < (u64{1} << 40) && size <= (u64{1} << 40) - address,
                   "macOS imported arena invalid guest range {:#x}+{:#x}", address, size);
        if (const auto* existing = Find(address);
            existing && size <= existing->size_bytes - (address - existing->cpu_addr)) {
            return existing;
        }
        VAddr start = Common::AlignDown(address, page_size);
        VAddr end = Common::AlignUp(address + size, page_size);
        auto first = arenas.lower_bound(start);
        if (first != arenas.begin()) {
            auto previous = std::prev(first);
            if (previous->first + previous->second->size_bytes > start) {
                first = previous;
                start = first->first;
            }
        }
        auto last = first;
        while (last != arenas.end() && last->first < end) {
            end = std::max<VAddr>(end, last->first + last->second->size_bytes);
            ++last;
        }
        ASSERT_MSG(end - start <= max_buffer_size,
                   "macOS imported arena {:#x}+{:#x} exceeds Vulkan maxBufferSize {:#x}", start,
                   end - start, max_buffer_size);
        // Metal imports of aliasing guest bytes require completion before a
        // replacement view is used. The caller also records the wait's cost.
        if (first != last) complete(start, end - start);
        auto next = allocate(start, end - start);
        ASSERT_MSG(next && next->cpu_addr == start && next->size_bytes == end - start,
                   "macOS imported arena allocation does not match its guest range");
        std::vector<std::unique_ptr<Resource>> old;
        for (auto it = first; it != last; ++it) {
            migrate(*it->second, *next);
            old.push_back(std::move(it->second));
        }
        arenas.erase(first, last);
        const auto* result = next.get();
        arenas.emplace(start, std::move(next));
        for (auto& resource : old) {
            retire(std::move(resource));
        }
        return result;
    }

    template <typename Retire>
    void EraseOverlapping(VAddr address, u64 size, Retire&& retire) {
        auto it = arenas.lower_bound(address);
        if (it != arenas.begin()) {
            auto previous = std::prev(it);
            if (previous->first + previous->second->size_bytes > address) it = previous;
        }
        while (it != arenas.end() && it->first < address + size) {
            auto resource = std::move(it->second);
            it = arenas.erase(it);
            retire(std::move(resource));
        }
    }

private:
    u64 page_size;
    u64 max_buffer_size;
    std::map<VAddr, std::unique_ptr<Resource>> arenas;
};

} // namespace VideoCore
