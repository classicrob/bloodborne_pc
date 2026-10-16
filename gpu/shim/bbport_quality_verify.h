// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace BbQuality {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_QUALITY_VERIFY");
        if (!value || !*value || !std::strcmp(value, "0")) return false;
        if (!std::strcmp(value, "1")) return true;
        std::fprintf(stderr, "STOP: BB_QUALITY_VERIFY must be 0 or 1\n");
        std::abort();
    }();
    return enabled;
}

// One owner thread per stream. Report every state change and a periodic heartbeat, with
// a hard output cap. A truncated stream cannot qualify subsequent frames. Counts are
// observations/recorded commands, never proof of GPU completion or physical display.
template <std::size_t N>
class Trace {
public:
    explicit Trace(const char* name_, bool heartbeat_ = true)
        : name{name_}, heartbeat{heartbeat_} {}
    ~Trace() {
        if (observed) {
            std::fprintf(stderr, "QUALITY stream=%s summary observed=%llu emitted=%llu "
                                 "changes=%llu cap=%llu truncated=%u\n",
                         name, observed, emitted, changes, Cap, unsigned(exhausted));
        }
    }

    bool Note(const std::array<std::uint64_t, N>& state) {
        ++observed;
        const bool changed = !have_previous || previous != state;
        changes += changed;
        previous = state;
        have_previous = true;
        if (exhausted || (!changed && (!heartbeat || observed % 120 != 0))) return false;
        if (emitted == Cap) {
            exhausted = true;
            std::fprintf(stderr, "QUALITY stream=%s exhausted observed=%llu emitted=%llu "
                                 "cap=%llu qualification_incomplete=1\n",
                         name, observed, emitted, Cap);
            return false;
        }
        ++emitted;
        return true;
    }

    unsigned long long Observed() const { return observed; }
    unsigned long long Changes() const { return changes; }
    bool Truncated() const { return exhausted; }

private:
    static constexpr unsigned long long Cap = 1024;
    const char* name;
    const bool heartbeat;
    std::array<std::uint64_t, N> previous{};
    bool have_previous = false, exhausted = false;
    unsigned long long observed = 0, emitted = 0, changes = 0;
};

} // namespace BbQuality
