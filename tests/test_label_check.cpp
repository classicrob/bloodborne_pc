// SPDX-License-Identifier: GPL-2.0-or-later
// BB_LABEL_CHECK (gpu/shim/bbport_label_check.h): the content at an EOP label sampled at parse
// and again before the write. A change that is not one of our own completed writes is reported;
// unchanged labels, our own writes in any interleaving with parse notes, unreadable memory and
// mixed widths are not (the latter two are counted). The writes are simulated in the order
// SignalEop does them: BeforeWrite, the store, AfterWrite.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include "bbport_cmd_data.h"
#include "bbport_deferred_counts.h"
#include "bbport_label_check.h"
#include "bbport_label_mode.h"
#include "bbport_label_store.h"
#include "bbport_parse_split.h"
#include "bbport_compute_sh.h"
#include "bbport_frame_marks.h"
#include "bbport_label_sequencer.h"

namespace {
unsigned failures = 0;
void Expect(const char* what, bool ok) {
    std::fprintf(stderr, "%-74s %s\n", what, ok ? "ok" : "FAIL");
    failures += ok ? 0 : 1;
}

// SignalEop's order: check, store, completed.
bool Write(std::uint64_t* label, std::uint64_t value, std::uint32_t bytes = 8) {
    const auto address = reinterpret_cast<std::uint64_t>(label);
    const bool reported = BbLabelCheck::BeforeWrite(address, bytes);
    std::memcpy(label, &value, bytes);
    BbLabelCheck::AfterWrite(address, value, bytes);
    return reported;
}
std::uint64_t Addr(const void* p) {
    return reinterpret_cast<std::uint64_t>(p);
}
} // namespace

static int owner_origin_test = 0;

int main() {
    using namespace BbLabelCheck;
    alignas(16) static std::uint64_t block[6] = {0, 0x1111, 0x2222, 0x3333, 0x4444, 0x5555};
    std::uint64_t* label = &block[0];

    NoteParsed(Addr(label), 8);
    Expect("label unchanged until written: no report", !Write(label, 1));

    NoteParsed(Addr(label), 8);
    block[0] = 0x7012345670; // the allocator links the freed block at offset 0
    Expect("content changed before the write (a free): reported", Write(label, 0x5300000000));

    // Sol-2's interleaving: A is the only pending EOP and is being written; B is parsed after
    // A's check but before A's store, so B samples A's old value; A stores; B finds A's value.
    block[0] = 2;
    NoteParsed(Addr(label), 8); // A
    const bool a_reported = BeforeWrite(Addr(label), 8);
    NoteParsed(Addr(label), 8); // B samples 2
    block[0] = 3;               // A's store
    AfterWrite(Addr(label), 3, 8);
    Expect("parse note between our check and our store: our value is not a report",
           !a_reported && !Write(label, 4));

    // Two EOPs in flight, the guest frees the block between the two writes: reported.
    NoteParsed(Addr(label), 8);
    NoteParsed(Addr(label), 8);
    const bool first = Write(label, 5);
    block[0] = 0x7012345670;
    Expect("freed while a second EOP is pending: reported", !first && Write(label, 6));

    // A 32-bit label compares only its 4 bytes.
    alignas(16) static std::uint64_t small[6] = {0xAAAA'BBBB'0000'0000ull};
    NoteParsed(Addr(&small[0]), 4);
    small[0] = 0xCCCC'DDDD'0000'0000ull;
    Expect("32-bit label: a change in the other half is not reported", !Write(&small[0], 1, 4));

    // A 32-bit EOP then a 64-bit EOP on one label: mixed, counted, not compared.
    alignas(16) static std::uint64_t both[6] = {0xFFFF'0000'0000'0000ull};
    const auto mixed_before = counts.mixed.load();
    NoteParsed(Addr(&both[0]), 4);
    NoteParsed(Addr(&both[0]), 8);
    const bool m1 = Write(&both[0], 7, 4);
    const bool m2 = Write(&both[0], 8, 8);
    Expect("32- and 64-bit EOPs on one label: mixed, counted, not reported",
           !m1 && !m2 && counts.mixed.load() == mixed_before + 2);

    // Unreadable memory: a label in an unmapped page, and one whose 48-byte block crosses into
    // one. Nothing faults; the unreadable label is counted, the readable one is still checked.
    const long page = sysconf(_SC_PAGESIZE);
    auto* pages = static_cast<std::uint8_t*>(
        mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    munmap(pages + page, page);
    const auto unreadable_before = counts.unreadable.load();
    NoteParsed(Addr(pages + page), 8);
    Expect("label in an unmapped page: no fault, no report",
           !BeforeWrite(Addr(pages + page), 8));
    Expect("  ... counted as unreadable", counts.unreadable.load() == unreadable_before + 1);
    auto* edge = reinterpret_cast<std::uint64_t*>(pages + page - 16);
    *edge = 1;
    NoteParsed(Addr(edge), 8);
    *edge = 0x7012345670;
    Expect("label at a page end whose 48-byte block crosses into an unmapped page: reported, "
           "no fault",
           BeforeWrite(Addr(edge), 8));

    const auto unmatched_before = counts.unmatched.load();
    Expect("a write without a parse note is not reported", !Write(&block[3], 9));
    Expect("  ... counted as unmatched", counts.unmatched.load() == unmatched_before + 1);
    // BB_LABELS_AT_GPU_COMPLETE's writer order (bbport_label_sequencer.h). Label A is issued
    // deferred (ready only after the copy threads), B direct (ready at once): B must not be
    // written before A, however long A's prerequisites take.
    {
        BbLabels::Sequencer sequencer;
        std::vector<char> written;
        std::mutex written_mutex;
        const auto label = [&](char name) {
            return [&, name] {
                std::scoped_lock lk{written_mutex};
                written.push_back(name);
            };
        };
        const auto a = sequencer.Issue();
        const auto b = sequencer.Issue();
        std::atomic<int> taken{0};
        std::jthread writer([&](std::stop_token stop) {
            while (auto next = sequencer.Next(stop)) {
                next->write();
                ++taken;
            }
        });
        sequencer.Ready(b, 2, label('B')); // the direct label, ready first
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        Expect("a later label ready first is held until the earlier one is ready",
               taken.load() == 0);
        sequencer.Ready(a, 1, label('A'));
        for (int i = 0; i < 200 && taken.load() < 2; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Expect("then both are written in issue order (A, B)",
               written == std::vector<char>{'A', 'B'});
        // Teardown with labels pending: one issued and ready behind one not ready.
        const auto c = sequencer.Issue();
        const auto d = sequencer.Issue();
        (void)c;
        sequencer.Ready(d, 4, label('D'));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        writer.request_stop();
        writer.join();
        Expect("stopping with labels pending writes none past the gap and reports both as "
               "outstanding",
               written.size() == 2 && sequencer.Outstanding() == 2);
    }

    // A label stored through the guest mapping (bbport_label_store.h): its bytes only.
    {
        alignas(16) static std::uint8_t bytes[32];
        std::memset(bytes, 0xEE, sizeof(bytes));
        BbLabelStore::StoreOnce(&bytes[8], 0x1122334455667788ull, 8);
        BbLabelStore::StoreOnce(&bytes[20], 0xFFFF'FFFF'99AA'BBCCull, 4);
        BbLabelStore::StoreOnce(&bytes[26], 0xDDEEull, 2);
        std::uint64_t q;
        std::uint32_t d;
        std::uint16_t h;
        std::memcpy(&q, &bytes[8], 8);
        std::memcpy(&d, &bytes[20], 4);
        std::memcpy(&h, &bytes[26], 2);
        Expect("label stores of 8, 4 and 2 bytes write their value and nothing around it",
               q == 0x1122334455667788ull && d == 0x99AABBCCu && h == 0xDDEE &&
                   bytes[7] == 0xEE && bytes[16] == 0xEE && bytes[24] == 0xEE &&
                   bytes[28] == 0xEE);
        // WRITE_DATA payloads: 1 or 2 aligned dwords as one store, others copied.
        std::memset(bytes, 0xEE, sizeof(bytes));
        const std::uint32_t one = 0x01020304;
        const std::uint32_t two[2] = {0x05060708, 0x090A0B0C};
        const std::uint32_t three[3] = {1, 2, 3};
        BbLabelStore::WriteData(&bytes[0], &one, 4);
        BbLabelStore::WriteData(&bytes[8], two, 8);
        BbLabelStore::WriteData(&bytes[17], &one, 4); // unaligned: copied
        BbLabelStore::WriteData(&bytes[22], two, 8);  // two dwords, not 8-aligned: copied
        std::uint8_t three_at[12];
        BbLabelStore::WriteData(three_at, three, 12);
        Expect("WRITE_DATA payloads land exactly: 1 dword (aligned, unaligned), 2 dwords "
               "(aligned, unaligned), 3 dwords",
               !std::memcmp(&bytes[0], &one, 4) && !std::memcmp(&bytes[8], two, 8) &&
                   !std::memcmp(&bytes[17], &one, 4) && !std::memcmp(&bytes[22], two, 8) &&
                   !std::memcmp(three_at, three, 12) && bytes[4] == 0xEE &&
                   bytes[16] == 0xEE && bytes[21] == 0xEE && bytes[30] == 0xEE);
    }

    // The recorder-side and label counts are separate (bbport_deferred_counts.h): a recorder
    // write still held is not complete, whichever way a concurrent label's two counters move.
    {
        BbDeferred::Count recorder, labels;
        std::atomic<std::uint64_t> total_done{0};
        const std::uint64_t held = recorder.Issue(); // its callback has not run
        labels.Issue();
        labels.Complete(); // label counted written, its total not yet
        const bool written_first = !recorder.CompleteThrough(held);
        total_done.fetch_add(1);
        const bool both = !recorder.CompleteThrough(held);
        Expect("a held recorder write is not complete while a label is written, then counted",
               written_first && both);
        labels.Issue();
        total_done.fetch_add(1); // the other order: total first
        labels.Complete();
        Expect("  ... nor with the label's counters moving in the other order",
               !recorder.CompleteThrough(held));
        std::atomic<bool> release{false};
        std::thread callback([&] {
            while (!release.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            recorder.Complete();
            total_done.fetch_add(1);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const bool blocked = !recorder.CompleteThrough(held);
        release.store(true);
        bool completed = false;
        for (int i = 0; i < 1000 && !completed; ++i) {
            completed = recorder.CompleteThrough(held);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        callback.join();
        Expect("  ... the recorder wait stays blocked until its callback completes, then ends",
               blocked && completed);
    }

    // When labels are written (bbport_label_mode.h): at GPU completion by default, toggle bit 59
    // switches to at submit live, BB_LABELS_AT_GPU_COMPLETE=0/1 fixes the mode for the run.
    {
        using BbLabels::RunMode;
        const RunMode unset = BbLabels::ParseRunMode(nullptr);
        const RunMode empty = BbLabels::ParseRunMode("");
        const RunMode on = BbLabels::ParseRunMode("1");
        const RunMode off = BbLabels::ParseRunMode("0");
        Expect("label mode: unset or empty is the default, 1 and 0 fix the run",
               unset == RunMode::Default && empty == RunMode::Default &&
                   on == RunMode::AtGpuComplete && off == RunMode::AtSubmit);
        Expect("  ... default: at GPU completion, bit 59 set: at submit",
               BbLabels::AtGpuComplete(unset, false) && !BbLabels::AtGpuComplete(unset, true));
        Expect("  ... =1 and =0 ignore bit 59",
               BbLabels::AtGpuComplete(on, true) && BbLabels::AtGpuComplete(on, false) &&
                   !BbLabels::AtGpuComplete(off, false) && !BbLabels::AtGpuComplete(off, true));
        std::fflush(stderr);
        const pid_t child = fork();
        if (child == 0) {
            std::freopen("/dev/null", "w", stderr);
            BbLabels::ParseRunMode("yes");
            std::_Exit(0);
        }
        int status = 0;
        waitpid(child, &status, 0);
        Expect("  ... any other value stops", WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    }

    // BB_CMD_DATA_CHECK (bbport_cmd_data.h): the production classifier and its inputs.
    {
        using namespace BbCmdData;
        // Two pools as captured (A1 starts 0x10 past 256 KiB alignment, A2 0x20).
        std::vector<Pool> pools;
        const bool parsed = ParsePools("0x7039400010:128,0x703b400020:384", 0x40000, pools);
        static const std::uint8_t copy_a[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        static std::uint8_t copy_b[16];
        std::memcpy(copy_b, copy_a, 16);
        copy_b[4] = 99; // a second held copy of the same range, different bytes
        const std::uint64_t a1_chunk = 0x7039400010 + 3 * 0x40000; // A1 chunk 3
        const std::uint64_t region = a1_chunk + 0x100;
        const std::vector<HeldRegion> held = {
            {region, 16, 7, copy_a},
            {region, 16, 9, copy_b}, // resubmitted
        };
        const Result same = Classify(region + 4, 4, copy_a + 4, held, pools, 9);
        const Result newest = Classify(region + 4, 4, copy_a + 4, held, pools, 0);
        const Result reused = Classify(region + 4, 4, copy_b + 4, held, pools, 9);
        Expect("cmd data: inside two held copies, one differs, the current one named",
               parsed && same.where == Where::Inside && same.matches == 2 &&
                   same.differing == 1 && same.current && same.submission == 9 &&
                   !newest.current && newest.submission == 9);
        Expect("  ... the selected copy differing is told apart from another copy differing",
               same.selected_differs && // current 9 (copy_b) vs the read of copy_a
                   Describe(same).find("DIFFERS") != std::string::npos &&
                   !reused.selected_differs && reused.differing == 1 && // only 7 (copy_a) differs
                   Describe(reused).find("matches the read; 1 other held region, 1 of them differ") !=
                       std::string::npos);
        // A region not copied at submit has no copy: located, never compared (Verifier).
        const std::vector<HeldRegion> uncopied_held = {
            {region, 16, 7, copy_a},
            {region, 16, 9, nullptr}, // unreadable at submit
        };
        const Result uncopied_current = Classify(region + 4, 4, copy_b + 4, uncopied_held, pools, 9);
        const Result uncopied_other = Classify(region + 4, 4, copy_a + 4, uncopied_held, pools, 7);
        Expect("  ... an uncopied region is never compared, and is named as such",
               uncopied_current.where == Where::Inside && uncopied_current.selected_uncopied &&
                   !uncopied_current.selected_differs && uncopied_current.uncopied == 1 &&
                   uncopied_current.differing == 1 && // copy_a vs the read of copy_b
                   Describe(uncopied_current).find("not copied at submit (unreadable then): not "
                                                   "compared") != std::string::npos &&
                   !uncopied_other.selected_uncopied && !uncopied_other.differing &&
                   Describe(uncopied_other).find("matches the read; 1 other held region, 0 of "
                                                 "them differ, 1 not copied") != std::string::npos);
        // A containing region is never renamed by a straddling one, in either order (Astra-2).
        bool kept = true;
        for (const std::uint64_t cur : {std::uint64_t{7}, std::uint64_t{0}}) {
            const HeldRegion contain{0x1000, 0x100, 7, nullptr}, partial{0x1044, 8, 9, nullptr};
            const std::vector<HeldRegion> orders[2] = {{contain, partial}, {partial, contain}};
            for (const auto& order : orders) {
                const Result x = Classify(0x1040, 8, copy_a, order, pools, cur);
                kept = kept && x.where == Where::Inside && x.matches == 1 && x.submission == 7 &&
                       x.region_start == 0x1000 && x.region_bytes == 0x100 &&
                       x.current == (cur == 7);
            }
        }
        const std::vector<HeldRegion> two_partial = {{0x1044, 8, 9, nullptr},
                                                     {0x1030, 0x12, 4, nullptr}};
        const Result only_straddle = Classify(0x1040, 8, copy_a, two_partial, pools, 0);
        Expect("  ... a straddling region never renames the containing selection (both orders, "
               "current and newest); straddling alone names the first",
               kept && only_straddle.where == Where::Straddles &&
                   only_straddle.submission == 9 && only_straddle.region_start == 0x1044);
        const Result straddle = Classify(region + 12, 8, copy_a, held, pools, 0);
        const Result tail = Classify(a1_chunk + 0x3ff00, 16, copy_a, held, pools, 0);
        const Result next_chunk = Classify(a1_chunk + 0x40000 + 8, 8, copy_a, held, pools, 0);
        const Result crossing = Classify(a1_chunk + 0x3fff8, 16, copy_a, held, pools, 0);
        Expect("  ... straddling; a candidate tail in A1's chunk (offset 0x10); none in the next "
               "chunk or across a chunk end",
               straddle.where == Where::Straddles && tail.where == Where::ChunkTail &&
                   tail.chunk_start == a1_chunk && tail.pool == 0 &&
                   next_chunk.where == Where::Outside && crossing.where == Where::Outside &&
                   Describe(next_chunk).find("not located") != std::string::npos &&
                   Describe(next_chunk).find("known pools") == std::string::npos);
        const Result overflow = Classify(UINT64_MAX - 4, 16, copy_a, held, pools, 0);
        static std::uint8_t big[512];
        const std::vector<HeldRegion> big_held = {{0x1000, 512, 1, big}};
        const Result uncompared = Classify(0x1000, 300, big, big_held, pools, 0);
        Expect("  ... an overflowing range is invalid; over 256 bytes is located, not compared",
               !overflow.valid_range && uncompared.where == Where::Inside &&
                   !uncompared.compared && !uncompared.differing &&
                   Describe(uncompared).find("not compared") != std::string::npos);
        std::uint64_t v = 0;
        std::vector<Pool> bad;
        std::uint64_t d = 0, z = 0;
        Expect("  ... strict numbers: no whitespace or sign anywhere, no octal, no trailing text, "
               "no overflow, no empty pool",
               ParseU64("0x40000", v) && v == 0x40000 && ParseU64("10", d) && d == 10 &&
                   ParseU64("0", z) && z == 0 && !ParseU64("010", v) && !ParseU64("0x", v) &&
                   !ParseU64("", v) && !ParseU64("-1", v) && !ParseU64("+5", v) &&
                   !ParseU64(" 5", v) && !ParseU64("\t-1", v) && !ParseU64("\n+5", v) &&
                   !ParseU64("0x-1", v) && !ParseU64("0x 1", v) && !ParseU64("5 ", v) &&
                   !ParseU64("12x", v) &&
                   !ParseU64("0x1ffffffffffffffff", v) && !ParsePools("0x1000:0", 0x40000, bad) &&
                   !ParsePools("0x1000", 0x40000, bad) &&
                   !ParsePools("0xffffffffffffff00:2", 0x40000, bad));
        // The status word read after the lookup: readable, and in an unmapped page.
        const long page = sysconf(_SC_PAGESIZE);
        auto* two = static_cast<std::uint8_t*>(
            mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
        const std::uint64_t status_value = 2;
        std::memcpy(two + page - 8, &status_value, 8);
        Result readable;
        readable.where = Where::ChunkTail;
        readable.chunk_start = reinterpret_cast<std::uint64_t>(two);
        readable.chunk_bytes = page;
        ReadStatus(readable);
        Result unreadable = readable;
        unreadable.chunk_start = reinterpret_cast<std::uint64_t>(two) + page;
        munmap(two + page, page);
        ReadStatus(unreadable);
        Expect("  ... the chunk status is read without faulting, unreadable when unmapped",
               readable.status_read && readable.chunk_status == 2 && !unreadable.status_read &&
                   Describe(unreadable).find("unreadable") != std::string::npos);
        // The latest read is formatted as observed, without touching its (now unmapped) address.
        setenv("BB_CMD_DATA_CHECK", "1", 1);
        last = {"ud table", 0xabc, reinterpret_cast<std::uint64_t>(two) + page, 16, same};
        const std::string text = LastRead();
        Expect("  ... the stop's latest-read text comes from the observation, not a re-read",
               text.find("latest instrumented read: ud table") != std::string::npos &&
                   text.find("DIFFERS") != std::string::npos);
        // The stop names the guest mapping holding the read (a mapping-table query, faked here).
        region_lookup.store([](std::uint64_t, std::uint64_t& lo, std::uint64_t& hi, bool& mapped,
                               long long& aliases) {
            lo = 0x7043000000;
            hi = 0x704b000000;
            mapped = true;
            aliases = 1;
            return true;
        });
        last.flips_parsed = 7;
        last.pointer = {true, true, 0x7039500100, 41, 5};
        last.pointer_high = last.pointer;
        last.dump = {true, 0x70437a7200, 0x100, 40, 6};
        const std::string named = LastRead();
        last.pointer_high = {true, true, 0x7039500180, 41, 6};
        last.dump = {true, 0x70437a7200, 0x100, 40, 9}; // recorded after the read's sample
        const std::string split = LastRead();
        last.pointer_high = last.pointer;
        last.dump = {true, 0x70437a7200, 0x100, 40, 6};
        last.pointer = {true, true, 0x7039500200, 0, 7, 3};
        last.pointer_high = last.pointer;
        const std::string from_compute = LastRead();
        last.pointer = {true, false, 0, 0, 0};
        last.pointer_high = last.pointer;
        last.dump = {};
        const std::string unwritten = LastRead();
        last.pointer = {};
        const std::string elsewhere = LastRead();
        region_lookup.store(nullptr);
        Expect("  ... the stop says which SET_SH_REG last wrote the table pointer and how long ago, "
               "and any DUMP_CONST_RAM of ours over the bytes",
               named.find("(low word) was last written by a SET_SH_REG of the graphics stream "
                          "at 0x7039500100 (submission 41), 2 flip packet(s) before this read "
                          "(an EARLIER frame") != std::string::npos &&
                   from_compute.find("a SET_SH_REG of compute queue 3 at") != std::string::npos &&
                   named.find("went to [0x70437a7200, +256) (submission 40), 1 flip "
                              "packet(s) before") != std::string::npos &&
                   named.find("high word") == std::string::npos &&
                   split.find("high word was written separately, by a SET_SH_REG of the "
                              "graphics stream at 0x7039500180") != std::string::npos &&
                   split.find("after this read's frame sample") != std::string::npos &&
                   named.find("retained state is a candidate, not by itself a defect)") !=
                       std::string::npos &&
                   unwritten.find("not written by any tracked SET_SH_REG since the check "
                                  "started") != std::string::npos &&
                   elsewhere.find("not a tracked graphics user-data register") !=
                       std::string::npos);
        Expect("  ... the stop names the guest mapping holding the read and its other views",
               named.find("guest mapping [0x7043000000, 0x704b000000) (131072 KiB), 1 other") !=
                   std::string::npos);
        {
            // A flat-buffer stop: every user-data word's writer and the pointer pairs' mappings.
            static const std::uint32_t ud[4] = {0x10, 0x0, 0x437a72a0, 0x70}; // s[0:1] not an address
            Register(
                nullptr,
                [](void*, const void* word, std::uint64_t, std::uint64_t, PointerOrigin& low,
                   PointerOrigin& high, DumpOrigin&, std::uint64_t& flips_now) {
                    const auto index =
                        static_cast<std::uint64_t>(static_cast<const std::uint32_t*>(word) - ud);
                    flips_now = 9;
                    low = {true, index != 1, 0x7039500000 + index * 0x10, 50, 9 - index};
                    high = low;
                },
                &owner_origin_test);
            region_lookup.store([](std::uint64_t, std::uint64_t& lo, std::uint64_t& hi,
                                   bool& mapped, long long& aliases) {
                lo = 0x7043400040;
                hi = 0x704d400040;
                mapped = true;
                aliases = 0;
                return true;
            });
            const std::string origins = UserDataOrigins(ud, 4);
            region_lookup.store(nullptr);
            Unregister(&owner_origin_test);
            Expect("  ... a flat-buffer stop names each user-data word's writer and where pointer "
                   "pairs lie",
                   origins.find("s0 gfx 0x7039500000 (submission 50) 0 flip(s) ago;") !=
                           std::string::npos &&
                       origins.find("s1 none tracked;") != std::string::npos &&
                       origins.find("s3 gfx 0x7039500030 (submission 50) 3 flip(s) ago;") !=
                           std::string::npos &&
                       origins.find("s[2:3] 0x70437a72a0 lies in guest mapping [0x7043400040, "
                                    "0x704d400040)") != std::string::npos &&
                       origins.find("s[0:1]") == std::string::npos);
        }
        ForgetLastRead();
        Expect("  ... after a new descriptor starts, a stop names no earlier read",
               LastRead().find("no instrumented read for this descriptor") != std::string::npos);
        munmap(two, page);
        // Owner lifetime: Unregister waits for a lookup in flight; none runs after it.
        static std::atomic<int> entered{0}, release{0}, calls{0};
        static int owner_object = 0;
        const LookupFn blocking = [](void* owner, std::uint64_t, std::uint64_t,
                                     const void*) -> Result {
            calls.fetch_add(1);
            entered.store(owner == &owner_object ? 1 : -1);
            while (!release.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Result r;
            r.where = Where::Inside;
            return r;
        };
        Register(blocking, nullptr, &owner_object);
        Result in_flight;
        std::thread reader([&] { in_flight = Lookup(1, 1, copy_a); });
        while (!entered.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::atomic<bool> unregistered{false};
        std::thread destroyer([&] {
            Unregister(&owner_object);
            unregistered.store(true);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const bool waited = !unregistered.load();
        release.store(1);
        reader.join();
        destroyer.join();
        const Result after = Lookup(1, 1, copy_a);
        Expect("  ... unregistering waits for a lookup in flight; no lookup runs after it",
               entered.load() == 1 && waited && unregistered.load() &&
                   in_flight.where == Where::Inside && after.where == Where::Outside &&
                   calls.load() == 1);
    }

    // Parse split (bbport_parse_split.h): every transition charges the owner of the time since
    // the previous one, so a thread's time is partitioned exactly, whatever the nesting.
    std::thread([] {
        using namespace BbSplit;
        const auto spin = [] {
            const std::uint64_t until = __rdtsc() + 20000;
            while (__rdtsc() < until) {
            }
        };
        const auto total = [] {
            std::uint64_t sum = 0;
            for (const auto& c : stage_a.bucket) sum += c.load();
            for (const auto& c : stage_a.scope) sum += c.load();
            return sum;
        };
        const auto in = [](K kind, std::uint32_t bucket) {
            return stage_a.scope_in[kind * MaxBuckets + bucket].load();
        };
        Attach(stage_a, ALoop);
        const std::uint64_t attached = stage_a.last;
        spin();
        bool scope_ok = true;
        {
            Scope deferred{KDeferred}; // Sol-2's case: a mark while a scope is open
            spin();
            Mark(ADraw);
            spin();
            {
                Scope tick{KGpuTick};
                spin();
            }
            spin();
            scope_ok = Open() == ADraw;
        }
        spin();
        bool section_ok = true;
        {
            Section active{stage_a, AHostCmds};
            section_ok = Open() == AHostCmds;
            spin();
            {
                Section other{stage_b, BBind}; // stage B's: inactive here
                section_ok = section_ok && Open() == AHostCmds;
                spin();
            }
        }
        section_ok = section_ok && Open() == ADraw;
        // A packet yields; the scheduler loop and a child task run; the packet resumes.
        Mark(AWaitRegMem);
        spin();
        const std::uint32_t saved = Suspend();
        Mark(ALoop);
        spin();
        {
            const std::uint32_t parent = Suspend(); // RESUME of a CE child from the loop's task
            Mark(AConstEng);
            spin();
            Resume(parent);
        }
        const bool child_ok = Open() == ALoop;
        Resume(saved);
        const bool resumed_ok = Open() == AWaitRegMem;
        spin();
        Mark(ALoop);
        Flush();
        Expect("parse split: buckets and scopes partition the thread's time exactly",
               total() == stage_a.last - attached);
        Expect("  ... a mark inside a scope moves only the bucket its time is listed under; inner "
               "scopes take their own time",
               scope_ok && in(KDeferred, ALoop) > 0 && in(KDeferred, ADraw) > 0 &&
                   in(KDeferred, ALoop) + in(KDeferred, ADraw) == stage_a.scope[KDeferred].load() &&
                   in(KGpuTick, ADraw) == stage_a.scope[KGpuTick].load() &&
                   stage_a.scope[KGpuTick].load() > 0);
        Expect("  ... an active section reopens the previous bucket; another thread's does nothing",
               section_ok && stage_a.bucket[AHostCmds].load() > 0 &&
                   stage_b.bucket[BBind].load() == 0);
        Expect("  ... a yielded packet and a resumed child each get back their bucket",
               child_ok && resumed_ok && stage_a.bucket[AConstEng].load() > 0 &&
                   stage_a.bucket[AWaitRegMem].load() > 0);
    }).join();
    {
        const pid_t child = fork();
        if (child == 0) {
            std::freopen("/dev/null", "w", stderr);
            BbSplit::Attach(BbSplit::stage_a, BbSplit::ALoop);
            BbSplit::Scope open{BbSplit::KDeferred};
            BbSplit::Suspend(); // a scope across a yield
            std::_Exit(0);
        }
        int status = 0;
        waitpid(child, &status, 0);
        Expect("  ... a scope open across a yield stops",
               WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    }

    {
        const std::uint32_t vs_pointer[2] = {0x43400040, 0x70};
        BbComputeSh::Note(2, 0x4E, 2, vs_pointer); // VS user data s[2:3]
        BbComputeSh::Note(2, 0x260, 1, vs_pointer); // above the compute program (0x200..0x250)
        Expect("compute SET_SH_REG: writes into graphics user data are counted and named",
               BbComputeSh::IsGraphicsUserData(0x4E) && !BbComputeSh::IsGraphicsUserData(0x4B) &&
                   !BbComputeSh::IsGraphicsUserData(0x15C) && BbComputeSh::into_graphics == 1 &&
                   BbComputeSh::into_user_data == 1 && BbComputeSh::above_compute == 1 &&
                   BbComputeSh::Describe().find("into graphics user data 1, latest: queue 2, SH "
                                                "0x4e, 2 word(s), first 0x43400040") !=
                       std::string::npos);
    }
    BbFrameMarks::attempted.Note(12);
    BbFrameMarks::attempted.Note(13);
    BbFrameMarks::parsed.Note(0);
    BbFrameMarks::prepared.Note(12);
    BbFrameMarks::published.Note(11);
    Expect("frame marks: the stop line gives each side's flip count and latest argument",
           BbFrameMarks::Describe().find("guest API calls 2 (latest arg 13), flip packets parsed "
                                         "1, flip preparation entered 1 (latest arg 12), status "
                                         "published 1 (latest arg 11)") != std::string::npos);

    {
        using namespace BbSplit;
        detail::FlushLine(0); // baseline
        BbSplit::NoteFlush(true); // no reason set: other
        {
            FlushReason frame{FFrameEnd};
            NoteFlush(true);
            {
                FlushReason label{FLabel};
                NoteFlush(false);
            }
            NoteFlush(false); // back to the frame end
        }
        NoteLabelBatch(300000);
        NoteLabelBatch(700000);
        const std::string line = detail::FlushLine(2);
        Expect("flush census: reasons nest and restore; per frame by reason, passes ended, batch age",
               line.find("2.0 per frame = frame end 1.0, unbatched labels 0.5") != std::string::npos &&
                   line.find("other 0.5") != std::string::npos &&
                   line.find("1.0 per frame ended an open render pass") != std::string::npos &&
                   line.find("label batches 1.0 per frame, oldest label's age at submit 500 us "
                             "mean, 700 us max") != std::string::npos &&
                   flush_reason == FOther);
    }

    std::fprintf(stderr, "%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
