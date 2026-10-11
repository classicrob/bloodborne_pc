// SPDX-License-Identifier: GPL-2.0-or-later
// BB_LABEL_CHECK (gpu/shim/bbport_label_check.h): the content at an EOP label sampled at parse
// and again before the write. A change that is not one of our own completed writes is reported;
// unchanged labels, our own writes in any interleaving with parse notes, unreadable memory and
// mixed widths are not (the latter two are counted). The writes are simulated in the order
// SignalEop does them: BeforeWrite, the store, AfterWrite.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include "bbport_deferred_counts.h"
#include "bbport_label_check.h"
#include "bbport_label_store.h"
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

    std::fprintf(stderr, "%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
