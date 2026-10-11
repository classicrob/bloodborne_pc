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
#include "bbport_label_check.h"

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
    std::fprintf(stderr, "%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
