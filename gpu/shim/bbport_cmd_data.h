// bbport (macOS diagnostic, BB_CMD_DATA_CHECK=1): guest data read at parse/record time from
// inside submitted command memory.
//
// Gnm can allocate descriptor tables and constants from the command buffer's chunk itself. The
// engine allocates them downward from the command chunk's end (0x26b07f0, Astra-2), outside the
// submitted [begin, cmdptr) range that CmdSnapshot copies at submit. The command processor reads
// them when it executes, but the guest may reuse the chunk once it believes the GPU is done with
// it. This check classifies each instrumented guest read against the snapshots still held:
// - inside a submitted region (DCB, CCB, IB), comparing the bytes just read (live) with the
//   submitted bytes;
// - straddling a region boundary;
// - in a submitted region's chunk tail: past the region's end in the same command chunk
//   (BB_CMD_DATA_CHUNK bytes, a power of two, default 262144; chunks start at
//   BB_CMD_DATA_CHUNK_OFFSET modulo that size, default 0x20: Astra-2's pool base 0x703b400020),
//   reporting the chunk's status word (end - 8: 0 free, 1 claimed, 2 released) as read now. The tail's submitted bytes are not copied, so it cannot compare them;
// - outside command memory.
// Counts per read kind and shader hash, and the first 16 changed or chunk-tail reads in full,
// go to stdout.
//
// Supporting evidence only: a difference shows the live bytes changed after submit; it does not
// show which bytes the PS4 command processor would have read. Reads made by generated code (the
// SRT walker) are not instrumented.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace BbCmdData {

enum class Where { Outside, Inside, Straddles, ChunkTail };

/// Submitted bytes are copied out (the snapshot may be released once the lookup returns), up
/// to this many; a longer range is classified but not compared.
constexpr std::size_t MaxCompared = 256;

struct Found {
    Where where = Where::Outside;
    bool copied = false;             ///< `submitted` holds the range's submitted bytes
    std::uint8_t submitted[MaxCompared];
    std::uint64_t submission = 0;    ///< the snapshot's submit number
    std::uint64_t region_start = 0;  ///< guest address of the region
    std::uint64_t region_bytes = 0;
    std::uint64_t chunk_status = 0;  ///< ChunkTail: the chunk's status word, read now
};

/// BB_CMD_DATA_CHUNK_OFFSET: where chunks start, modulo the chunk size.
inline std::uint64_t ChunkOffset();

/// The command chunk holding `address`.
inline std::uint64_t ChunkStart(std::uint64_t address);

/// BB_CMD_DATA_CHUNK: command chunk size in bytes (a power of two).
inline std::uint64_t ChunkBytes() {
    static const std::uint64_t bytes = [] {
        const char* env = std::getenv("BB_CMD_DATA_CHUNK");
        if (!env || !*env) {
            return std::uint64_t{262144};
        }
        char* end = nullptr;
        const unsigned long long v = std::strtoull(env, &end, 0);
        if (*end || v < 4096 || (v & (v - 1))) {
            std::fprintf(stderr, "STOP: BB_CMD_DATA_CHUNK must be a power of two >= 4096\n");
            std::abort();
        }
        return std::uint64_t{v};
    }();
    return bytes;
}

inline std::uint64_t ChunkOffset() {
    static const std::uint64_t offset = [] {
        const char* env = std::getenv("BB_CMD_DATA_CHUNK_OFFSET");
        if (!env || !*env) {
            return std::uint64_t{0x20};
        }
        char* end = nullptr;
        const unsigned long long v = std::strtoull(env, &end, 0);
        if (*end) {
            std::fprintf(stderr, "STOP: BB_CMD_DATA_CHUNK_OFFSET must be a number\n");
            std::abort();
        }
        return std::uint64_t{v} & (ChunkBytes() - 1);
    }();
    return offset;
}

inline std::uint64_t ChunkStart(std::uint64_t address) {
    const std::uint64_t chunk = ChunkBytes(), offset = ChunkOffset();
    return ((address - offset) & ~(chunk - 1)) + offset;
}

/// Set by the GPU command thread's owner (Liverpool) when the check is on.
using LookupFn = Found (*)(std::uint64_t address, std::uint64_t size);
inline std::atomic<LookupFn> lookup{nullptr};

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
        std::printf("BB_CMD_DATA_CHECK=1: parse-time guest reads classified against the "
                    "submitted command memory snapshots\n");
        std::fflush(stdout);
        return true;
    }();
    return on;
}

inline Found Lookup(std::uint64_t address, std::uint64_t size) {
    const LookupFn fn = lookup.load(std::memory_order_acquire);
    return fn ? fn(address, size) : Found{};
}

namespace detail {
struct Tally {
    std::uint64_t outside = 0, inside = 0, straddles = 0, differs = 0, chunk_tail = 0;
};
inline std::mutex mutex;
inline std::map<std::pair<std::string, std::uint64_t>, Tally> tallies; ///< (kind, shader hash)
inline std::uint64_t reads = 0, differences = 0;

inline void PrintSummaryLocked() {
    std::printf("CMD-DATA summary: %llu reads, %llu logged (changed inside, or chunk tail)\n",
                static_cast<unsigned long long>(reads),
                static_cast<unsigned long long>(differences));
    for (const auto& [key, t] : tallies) {
        std::printf("  %s shader %#llx: outside %llu, inside %llu (changed %llu), straddling "
                    "%llu, chunk tail %llu\n",
                    key.first.c_str(), static_cast<unsigned long long>(key.second),
                    static_cast<unsigned long long>(t.outside),
                    static_cast<unsigned long long>(t.inside),
                    static_cast<unsigned long long>(t.differs),
                    static_cast<unsigned long long>(t.straddles),
                    static_cast<unsigned long long>(t.chunk_tail));
    }
    std::fflush(stdout);
}
} // namespace detail

/// This thread's latest instrumented read (for a stop that follows it).
inline thread_local std::uint64_t last_address = 0, last_size = 0;
inline thread_local const char* last_kind = nullptr;

/// One parse-time read of `size` bytes at `address`, whose bytes as read are `value_read`.
inline void NoteRead(const char* kind, std::uint64_t shader_hash, const void* address,
                     std::size_t size, const void* value_read) {
    const auto at = reinterpret_cast<std::uint64_t>(address);
    last_address = at;
    last_size = size;
    last_kind = kind;
    const Found found = Lookup(at, size);
    const bool differs = found.where == Where::Inside && found.copied &&
                         std::memcmp(found.submitted, value_read, size) != 0;
    std::scoped_lock lk{detail::mutex};
    auto& t = detail::tallies[{kind, shader_hash}];
    switch (found.where) {
    case Where::Outside:
        ++t.outside;
        break;
    case Where::Inside:
        ++t.inside;
        break;
    case Where::Straddles:
        ++t.straddles;
        break;
    case Where::ChunkTail:
        ++t.chunk_tail;
        if (++detail::differences <= 16) {
            std::printf("CMD-DATA #%llu: %s for shader %#llx at %#llx (%zu bytes) in the chunk "
                        "tail of submission %llu (region %#llx, %llu bytes, %llu bytes past its "
                        "end); chunk status now %llu\n",
                        static_cast<unsigned long long>(detail::differences), kind,
                        static_cast<unsigned long long>(shader_hash),
                        static_cast<unsigned long long>(at), size,
                        static_cast<unsigned long long>(found.submission),
                        static_cast<unsigned long long>(found.region_start),
                        static_cast<unsigned long long>(found.region_bytes),
                        static_cast<unsigned long long>(at - found.region_start -
                                                        found.region_bytes),
                        static_cast<unsigned long long>(found.chunk_status));
        }
        break;
    }
    if (differs) {
        ++t.differs;
        if (++detail::differences <= 16) {
            std::uint32_t live[4]{}, sub[4]{};
            const std::size_t shown = size < sizeof(live) ? size : sizeof(live);
            std::memcpy(live, value_read, shown);
            std::memcpy(sub, found.submitted, shown);
            std::printf("CMD-DATA #%llu: %s for shader %#llx at %#llx (%zu bytes) inside "
                        "submission %llu's command memory (region %#llx, %llu bytes): live "
                        "%08x %08x %08x %08x, submitted %08x %08x %08x %08x (rewritten after "
                        "submit)\n",
                        static_cast<unsigned long long>(detail::differences), kind,
                        static_cast<unsigned long long>(shader_hash),
                        static_cast<unsigned long long>(at), size,
                        static_cast<unsigned long long>(found.submission),
                        static_cast<unsigned long long>(found.region_start),
                        static_cast<unsigned long long>(found.region_bytes), live[0], live[1],
                        live[2], live[3], sub[0], sub[1], sub[2], sub[3]);
        }
    }
    if ((++detail::reads & 0xffff) == 0) {
        detail::PrintSummaryLocked();
    }
}

/// For stop messages: where `size` bytes at `address` lie, and whether live differs.
inline std::string Describe(std::uint64_t address, std::uint64_t size) {
    const Found found = Lookup(address, size);
    char text[192];
    switch (found.where) {
    case Where::Outside:
        std::snprintf(text, sizeof(text), "outside submitted command memory");
        break;
    case Where::Straddles:
        std::snprintf(text, sizeof(text),
                      "straddling a submitted command memory region (submission %llu)",
                      static_cast<unsigned long long>(found.submission));
        break;
    case Where::ChunkTail:
        std::snprintf(text, sizeof(text),
                      "in the chunk tail of submission %llu's command memory (not copied at "
                      "submit; chunk status now %llu)",
                      static_cast<unsigned long long>(found.submission),
                      static_cast<unsigned long long>(found.chunk_status));
        break;
    case Where::Inside: {
        const bool differs = found.copied && std::memcmp(found.submitted,
                                                         reinterpret_cast<const void*>(address),
                                                         static_cast<std::size_t>(size));
        std::snprintf(text, sizeof(text),
                      "inside submission %llu's command memory (snapshot %s live: %s)",
                      static_cast<unsigned long long>(found.submission),
                      differs ? "differs from" : "equals",
                      differs ? "rewritten after submit" : "unchanged");
        break;
    }
    }
    return text;
}

/// For a stop: this thread's latest instrumented read and where it lies (empty when off).
inline std::string LastRead() {
    if (!Enabled() || !last_kind) {
        return {};
    }
    char text[96];
    std::snprintf(text, sizeof(text), "; latest %s read at %#llx (%llu bytes), ", last_kind,
                  static_cast<unsigned long long>(last_address),
                  static_cast<unsigned long long>(last_size));
    return text + Describe(last_address, last_size);
}

} // namespace BbCmdData
