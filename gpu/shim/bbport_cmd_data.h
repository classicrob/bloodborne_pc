// bbport (macOS diagnostic, BB_CMD_DATA_CHECK=1): guest data read at parse/record time, located
// relative to the submitted command memory still held.
//
// Gnm can allocate descriptor tables and constants inside a command buffer's chunk. The engine
// allocates them downward from the chunk's end (0x26b07f0, Astra-2), outside the submitted
// [begin, cmdptr) range CmdSnapshot copies at submit, and the guest may reuse a chunk once it
// believes the GPU is done with it. Each instrumented read is classified against the submitted
// regions (DCB, CCB, IBs) of the snapshots the GPU thread still holds:
// - inside: every held region containing the range is compared with the bytes just read (live).
//   Several held copies can contain one range (a resubmitted or shared buffer), so the result
//   names how many matched, how many differ, and whether one is the parser's current submission;
//   a difference shows a held copy differs from what was read, not by itself which submission's
//   bytes the PS4 would have used;
// - straddling a held region's boundary;
// - a candidate chunk tail: outside every held region, but in the same chunk of a known command
//   pool as a held region that ends below it (under the assumed pool geometry, BB_CMD_DATA_POOLS;
//   ownership of the address is not established). The chunk's status word (its last 8 bytes:
//   0 free, 1 claimed, 2 released) is then read without faulting, after the lookup;
// - not located: none of the above (it may still be in a known pool, e.g. a chunk with no held
//   region below the address; parsed snapshots are released, so this never means outside command
//   memory).
// Reads made by generated code (the SRT walker) are not instrumented.
#pragma once
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

namespace BbCmdData {

/// Strict unsigned number: decimal without a leading zero (so 010 is not octal), or 0x/0X hex.
/// Digits only: no whitespace, sign or trailing text anywhere, no overflow.
inline bool ParseU64(const char* text, std::uint64_t& out) {
    std::string_view s = text ? text : "";
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s.remove_prefix(2);
    } else if (s.empty() || (s.size() > 1 && s[0] == '0')) {
        return false;
    }
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (!(base == 16 ? std::isxdigit(u) : std::isdigit(u))) {
            return false;
        }
    }
    std::uint64_t v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v, base);
    if (ec != std::errc{} || end != s.data() + s.size()) {
        return false;
    }
    out = v;
    return true;
}

struct Pool {
    std::uint64_t base = 0, chunk_bytes = 0, chunks = 0;
};

/// "base:chunks[,base:chunks...]"; false on a malformed value.
inline bool ParsePools(const char* text, std::uint64_t chunk_bytes, std::vector<Pool>& pools) {
    pools.clear();
    const std::string s = text ? text : "";
    std::size_t at = 0;
    for (;;) {
        const std::size_t comma = s.find(',', at);
        const std::string item =
            s.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        const std::size_t colon = item.find(':');
        if (colon == std::string::npos) {
            return false;
        }
        Pool p;
        if (!ParseU64(item.substr(0, colon).c_str(), p.base) ||
            !ParseU64(item.substr(colon + 1).c_str(), p.chunks) || !p.chunks ||
            p.chunks > (UINT64_MAX - p.base) / chunk_bytes) {
            return false;
        }
        p.chunk_bytes = chunk_bytes;
        pools.push_back(p);
        if (comma == std::string::npos) {
            return true;
        }
        at = comma + 1;
    }
}

/// BB_CMD_DATA_POOLS (default: Astra-2's captured A1 and A2) with BB_CMD_DATA_CHUNK bytes per
/// chunk (default 256 KiB, q = 0x8000 qwords). A malformed value stops.
inline const std::vector<Pool>& Pools() {
    static const std::vector<Pool> pools = [] {
        std::uint64_t chunk = 0x40000;
        if (const char* env = std::getenv("BB_CMD_DATA_CHUNK"); env && *env) {
            if (!ParseU64(env, chunk) || chunk < 4096 || chunk > (1ull << 30) ||
                (chunk & (chunk - 1))) {
                std::fprintf(stderr, "STOP: BB_CMD_DATA_CHUNK must be a power of two in "
                                     "[4096, 1 GiB]\n");
                std::abort();
            }
        }
        const char* env = std::getenv("BB_CMD_DATA_POOLS");
        std::vector<Pool> parsed;
        if (!ParsePools(env && *env ? env : "0x7039400010:128,0x703b400020:384", chunk, parsed)) {
            std::fprintf(stderr, "STOP: BB_CMD_DATA_POOLS must be base:chunks[,base:chunks]\n");
            std::abort();
        }
        return parsed;
    }();
    return pools;
}

inline bool Enabled() {
    static const bool on = [] {
        const char* env = std::getenv("BB_CMD_DATA_CHECK");
        if (!env || !*env || !std::strcmp(env, "0")) {
            return false;
        }
        if (std::strcmp(env, "1")) {
            std::fprintf(stderr, "STOP: BB_CMD_DATA_CHECK must be 0 or 1\n");
            std::abort();
        }
        Pools(); // validated now
        std::printf("BB_CMD_DATA_CHECK=1: parse-time guest reads located relative to the held "
                    "command memory snapshots\n");
        std::fflush(stdout);
        return true;
    }();
    return on;
}

/// Bytes compared per region; a longer range is located but not compared.
constexpr std::size_t MaxCompared = 256;

/// A held submitted region, valid while the owner holds its snapshot lock. `copy` is null for
/// a region not copied at submit (unreadable then): located, never compared.
struct HeldRegion {
    std::uint64_t start = 0, bytes = 0, submission = 0;
    const std::uint8_t* copy = nullptr;
};

enum class Where { Outside, Inside, Straddles, ChunkTail };

struct Result {
    Where where = Where::Outside;
    bool valid_range = true;     ///< false: address + size overflows, or size 0
    bool compared = false;       ///< size <= MaxCompared
    std::uint32_t matches = 0;   ///< held regions containing the range
    std::uint32_t differing = 0; ///< of those, copies that differ from the bytes read
    std::uint32_t uncopied = 0;  ///< of those, regions with no copy (not compared)
    /// The selected copy (current, else newest) differs from the bytes read. A mismatch only, not
    /// an order: with the current one selected, other differing copies need not be older.
    bool selected_differs = false;
    bool selected_uncopied = false; ///< the selected region has no copy: not compared
    bool current = false;        ///< one of them is the parser's current submission
    std::uint64_t submission = 0, region_start = 0, region_bytes = 0; ///< a selected region
    // ChunkTail:
    std::uint32_t pool = 0;
    std::uint64_t chunk_start = 0, chunk_bytes = 0;
    bool status_read = false;
    std::uint64_t chunk_status = 0;
};

/// The parser's current graphics submission on this thread (0: none, e.g. the draw thread).
inline thread_local std::uint64_t current_submission = 0;

/// Pure: classifies `size` bytes at `address`, read as `bytes_read`, against the held regions and
/// known pools. Touches no guest memory (the status word is read separately, by ReadStatus).
inline Result Classify(std::uint64_t address, std::uint64_t size, const void* bytes_read,
                       std::span<const HeldRegion> held, std::span<const Pool> pools,
                       std::uint64_t current) {
    Result r;
    if (!size || size > UINT64_MAX - address) {
        r.valid_range = false;
        return r;
    }
    const std::uint64_t end = address + size;
    r.compared = size <= MaxCompared;
    const HeldRegion* selected = nullptr;
    const HeldRegion* straddled = nullptr; // the first; reported only when nothing contains the range
    for (const auto& h : held) {
        if (h.bytes > UINT64_MAX - h.start) {
            continue;
        }
        const std::uint64_t h_end = h.start + h.bytes;
        if (address >= h.start && end <= h_end) {
            ++r.matches;
            const bool is_current = current && h.submission == current;
            // Selected: the parser's current submission if held, else the newest.
            if (r.matches == 1 || (is_current && !r.current) ||
                (!r.current && !is_current && h.submission > r.submission)) {
                selected = &h;
                r.submission = h.submission;
                r.region_start = h.start;
                r.region_bytes = h.bytes;
            }
            r.current = r.current || is_current;
            if (!h.copy) {
                ++r.uncopied;
            } else if (r.compared &&
                       std::memcmp(h.copy + (address - h.start), bytes_read, size) != 0) {
                ++r.differing;
            }
        } else if (address < h_end && h.start < end && !straddled) {
            straddled = &h;
        }
    }
    if (r.matches) {
        r.where = Where::Inside;
        r.selected_uncopied = !selected->copy;
        r.selected_differs = r.compared && selected && selected->copy &&
                             std::memcmp(selected->copy + (address - selected->start),
                                         bytes_read, size) != 0;
        return r;
    }
    if (straddled) {
        r.where = Where::Straddles;
        r.submission = straddled->submission;
        r.region_start = straddled->start;
        r.region_bytes = straddled->bytes;
        return r;
    }
    for (std::uint32_t p = 0; p < pools.size(); ++p) {
        const Pool& pool = pools[p];
        if (address < pool.base) {
            continue;
        }
        const std::uint64_t index = (address - pool.base) / pool.chunk_bytes;
        if (index >= pool.chunks) {
            continue;
        }
        const std::uint64_t chunk_start = pool.base + index * pool.chunk_bytes;
        if (end > chunk_start + pool.chunk_bytes) {
            continue; // crosses into the next chunk
        }
        for (const auto& h : held) {
            if (h.bytes > UINT64_MAX - h.start) {
                continue;
            }
            const std::uint64_t h_end = h.start + h.bytes;
            // The newest submission with a region below the address in this chunk.
            if (h.start >= chunk_start && h_end <= address &&
                (r.where != Where::ChunkTail || h.submission > r.submission)) {
                r.where = Where::ChunkTail;
                r.submission = h.submission;
                r.region_start = h.start;
                r.region_bytes = h.bytes;
                r.pool = p;
                r.chunk_start = chunk_start;
                r.chunk_bytes = pool.chunk_bytes;
            }
        }
        if (r.where == Where::ChunkTail) {
            break;
        }
    }
    return r;
}

/// Non-faulting guest read (Mach); false when any byte is unreadable.
inline bool SafeRead(std::uint64_t address, void* out, std::size_t size) {
#ifdef __APPLE__
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, size,
                                  reinterpret_cast<mach_vm_address_t>(out), &got) ==
               KERN_SUCCESS &&
           got == size;
#else
    (void)address;
    (void)out;
    (void)size;
    return false;
#endif
}

/// Reads a candidate chunk tail's status word, after the lookup released its lock.
inline void ReadStatus(Result& r) {
    if (r.where != Where::ChunkTail) {
        return;
    }
    r.status_read = SafeRead(r.chunk_start + r.chunk_bytes - 8, &r.chunk_status, 8);
}

/// The owner's lookup (Liverpool, when the check is on): classifies under its snapshot lock.
using LookupFn = Result (*)(void* owner, std::uint64_t address, std::uint64_t size,
                            const void* bytes_read);

namespace detail {
// Lifetime: a lookup holds `registry` shared for its whole use of the owner, so Unregister
// (exclusive) returns only once no lookup uses it, and none starts after. Order: registry, then
// the owner's locks; the owner must not hold its locks while unregistering.
inline std::shared_mutex registry;
inline LookupFn lookup_fn = nullptr;
inline void* lookup_owner = nullptr;
} // namespace detail

inline void Register(LookupFn fn, void* owner) {
    std::unique_lock lk{detail::registry};
    detail::lookup_fn = fn;
    detail::lookup_owner = owner;
}

/// Blocks until in-flight lookups of `owner` return; later reads are not located.
inline void Unregister(void* owner) {
    std::unique_lock lk{detail::registry};
    if (detail::lookup_owner == owner) {
        detail::lookup_fn = nullptr;
        detail::lookup_owner = nullptr;
    }
}

/// Classifies through the registered owner (Outside, not located, when none).
inline Result Lookup(std::uint64_t address, std::uint64_t size, const void* bytes_read) {
    std::shared_lock lk{detail::registry};
    return detail::lookup_fn ? detail::lookup_fn(detail::lookup_owner, address, size, bytes_read)
                             : Result{};
}

/// One-line description of a result (no guest memory access).
inline std::string Describe(const Result& r) {
    char text[320];
    switch (r.where) {
    case Where::Outside:
        std::snprintf(text, sizeof(text), "%s",
                      r.valid_range
                          ? "not located in a held region or a candidate chunk tail (may still "
                            "be command memory)"
                          : "an invalid range (empty, or address + size overflows)");
        break;
    case Where::Straddles:
        std::snprintf(text, sizeof(text),
                      "straddling held region %#llx (%llu bytes, submission %llu)",
                      static_cast<unsigned long long>(r.region_start),
                      static_cast<unsigned long long>(r.region_bytes),
                      static_cast<unsigned long long>(r.submission));
        break;
    case Where::Inside: {
        const std::uint32_t others_differing = r.differing - (r.selected_differs ? 1 : 0);
        const std::uint32_t others_uncopied = r.uncopied - (r.selected_uncopied ? 1 : 0);
        char uncopied_text[64] = "";
        if (others_uncopied) {
            std::snprintf(uncopied_text, sizeof(uncopied_text), ", %u not copied at submit",
                          others_uncopied);
        }
        if (!r.compared) {
            std::snprintf(text, sizeof(text),
                          "inside %u held region(s) (selected %#llx, submission %llu%s); not "
                          "compared (longer than 256 bytes)",
                          r.matches, static_cast<unsigned long long>(r.region_start),
                          static_cast<unsigned long long>(r.submission),
                          r.current ? ", the parser's current one" : "");
        } else {
            std::snprintf(text, sizeof(text),
                          "the %s %s (submission %llu, region %#llx) %s; %u other held "
                          "region%s, %u of them differ%s",
                          r.current ? "current submission's" : "newest held",
                          r.selected_uncopied ? "region" : "copy",
                          static_cast<unsigned long long>(r.submission),
                          static_cast<unsigned long long>(r.region_start),
                          r.selected_uncopied
                              ? "was not copied at submit (unreadable then): not compared"
                          : r.selected_differs ? "DIFFERS from the read"
                                               : "matches the read",
                          r.matches - 1, r.matches - 1 == 1 ? "" : "s", others_differing,
                          uncopied_text);
        }
        break;
    }
    case Where::ChunkTail: {
        char status[24];
        if (r.status_read) {
            std::snprintf(status, sizeof(status), "%llu",
                          static_cast<unsigned long long>(r.chunk_status));
        } else {
            std::snprintf(status, sizeof(status), "unreadable");
        }
        std::snprintf(text, sizeof(text),
                      "a candidate chunk tail (pool %u chunk %#llx; held region %#llx of "
                      "submission %llu ends below it; assumed geometry, ownership not "
                      "established); chunk status %s",
                      r.pool, static_cast<unsigned long long>(r.chunk_start),
                      static_cast<unsigned long long>(r.region_start),
                      static_cast<unsigned long long>(r.submission), status);
        break;
    }
    }
    return text;
}

namespace detail {
struct Tally {
    std::uint64_t outside = 0, inside = 0, inside_differing = 0, inside_reused = 0,
                  inside_ambiguous = 0,
                  straddles = 0, chunk_tail = 0;
};
inline std::mutex mutex;
inline std::map<std::pair<std::string, std::uint64_t>, Tally> tallies; ///< (kind, shader hash)
inline std::uint64_t reads = 0, logged = 0;

inline void PrintSummaryLocked() {
    std::printf("CMD-DATA summary (every 65536 reads, all kinds; not located = no held region or "
                "candidate tail): %llu reads, %llu logged\n",
                static_cast<unsigned long long>(reads), static_cast<unsigned long long>(logged));
    for (const auto& [key, t] : tallies) {
        std::printf("  %s shader %#llx: not located %llu, inside %llu (selected copy differs %llu, "
                    "only other copies differ %llu, several held copies %llu), straddling %llu, "
                    "candidate chunk tail %llu\n",
                    key.first.c_str(), static_cast<unsigned long long>(key.second),
                    static_cast<unsigned long long>(t.outside),
                    static_cast<unsigned long long>(t.inside),
                    static_cast<unsigned long long>(t.inside_differing),
                    static_cast<unsigned long long>(t.inside_reused),
                    static_cast<unsigned long long>(t.inside_ambiguous),
                    static_cast<unsigned long long>(t.straddles),
                    static_cast<unsigned long long>(t.chunk_tail));
    }
    std::fflush(stdout);
}
} // namespace detail

/// This thread's latest instrumented read, as observed then (never re-read).
struct Observation {
    const char* kind = nullptr;
    std::uint64_t shader = 0, address = 0, size = 0;
    Result result;
};
inline thread_local Observation last;

/// One parse-time read of `size` bytes at `address`, whose bytes as read are `bytes_read`.
inline void NoteRead(const char* kind, std::uint64_t shader_hash, const void* address,
                     std::size_t size, const void* bytes_read) {
    const auto at = reinterpret_cast<std::uint64_t>(address);
    Result r = Lookup(at, size, bytes_read);
    ReadStatus(r);
    last = {kind, shader_hash, at, size, r};
    const bool notable = (r.where == Where::Inside && r.selected_differs) ||
                         r.where == Where::ChunkTail || r.where == Where::Straddles;
    std::scoped_lock lk{detail::mutex};
    auto& t = detail::tallies[{kind, shader_hash}];
    switch (r.where) {
    case Where::Outside:
        ++t.outside;
        break;
    case Where::Inside:
        ++t.inside;
        t.inside_differing += r.selected_differs ? 1 : 0;
        t.inside_reused += !r.selected_differs && r.differing ? 1 : 0;
        t.inside_ambiguous += r.matches > 1 ? 1 : 0;
        break;
    case Where::Straddles:
        ++t.straddles;
        break;
    case Where::ChunkTail:
        ++t.chunk_tail;
        break;
    }
    if (notable && ++detail::logged <= 16) {
        std::printf("CMD-DATA #%llu: %s for shader %#llx at %#llx (%zu bytes): %s\n",
                    static_cast<unsigned long long>(detail::logged), kind,
                    static_cast<unsigned long long>(shader_hash),
                    static_cast<unsigned long long>(at), size, Describe(r).c_str());
    }
    if ((++detail::reads & 0xffff) == 0) {
        detail::PrintSummaryLocked();
    }
}

/// Before each descriptor read (StageSpecialization): a stop then reports only reads made for
/// that descriptor.
inline void ForgetLastRead() {
    last.kind = nullptr;
}

/// The guest mapping holding an address: [start, end), whether it is mapped (false: a gap
/// [start, end) between mappings), and how many other guest mappings view the same backing
/// byte (-1: not tracked). Set by the owner (Liverpool, runtime_memory_region and
/// runtime_memory_alias_snapshot); mapping-table queries, not guest memory reads.
using RegionFn = bool (*)(std::uint64_t address, std::uint64_t& start, std::uint64_t& end,
                          bool& mapped, long long& aliases);
inline std::atomic<RegionFn> region_lookup{nullptr};

/// For a stop: this thread's latest instrumented read for the current descriptor, as observed
/// then, and the guest mapping holding it now. Empty when off.
inline std::string LastRead() {
    if (!Enabled()) {
        return {};
    }
    if (!last.kind) {
        return "; no instrumented read for this descriptor (it came from direct user data or the "
               "uninstrumented flat buffer)";
    }
    char text[160];
    std::snprintf(text, sizeof(text),
                  "; this thread's latest instrumented read: %s for shader %#llx at %#llx (%llu "
                  "bytes), ",
                  last.kind, static_cast<unsigned long long>(last.shader),
                  static_cast<unsigned long long>(last.address),
                  static_cast<unsigned long long>(last.size));
    std::string out = text + Describe(last.result);
    std::uint64_t start = 0, end = 0;
    bool mapped = false;
    long long aliases = -1;
    if (const RegionFn fn = region_lookup.load(std::memory_order_acquire);
        fn && fn(last.address, start, end, mapped, aliases)) {
        char region[192];
        std::snprintf(region, sizeof(region),
                      "; at the stop it lies in %s [%#llx, %#llx) (%llu KiB), %s%lld other guest "
                      "mapping(s) of the same backing",
                      mapped ? "guest mapping" : "the unmapped gap",
                      static_cast<unsigned long long>(start), static_cast<unsigned long long>(end),
                      static_cast<unsigned long long>((end - start) >> 10),
                      aliases < 0 ? "untracked: " : "", aliases < 0 ? 0 : aliases);
        out += region;
    }
    return out;
}

} // namespace BbCmdData
