// SPDX-License-Identifier: GPL-2.0-or-later
// BB_LABEL_CHECK (gpu/shim/bbport_label_check.h): a label the guest changed between its EOP being
// parsed and written means the guest acted on the work as done too early (the 0x263b8e7 heap
// corruption: a freed 48-byte block whose free-list link at offset 0 is the label). Unchanged
// labels, labels never noted, and several EOPs pending on one label are not reports.
#include <cstdint>
#include <cstdio>
#include "bbport_label_check.h"

namespace {
unsigned failures = 0;
void Expect(const char* what, bool ok) {
    std::fprintf(stderr, "%-70s %s\n", what, ok ? "ok" : "FAIL");
    failures += ok ? 0 : 1;
}
} // namespace

int main() {
    alignas(16) static std::uint64_t block[6] = {0, 0x1111, 0x2222, 0x3333, 0x4444, 0x5555};
    const auto label = reinterpret_cast<std::uint64_t>(&block[0]);

    // In order: parsed with 0, the guest waits, the GPU writes 1.
    BbLabelCheck::NoteParsed(label, 8);
    Expect("label unchanged until written: no report", !BbLabelCheck::BeforeWrite(label, 1, 8));
    block[0] = 1;

    // The guest frees the block before the label lands: the allocator links it at offset 0.
    BbLabelCheck::NoteParsed(label, 8);
    block[0] = 0x7012345670; // free-list next pointer
    Expect("guest freed the block before the label landed: reported",
           BbLabelCheck::BeforeWrite(label, 0x5300000000, 8));

    // Two EOPs pending on one label (two frames in flight), the guest leaves it alone: each
    // write is checked against its own parse.
    block[0] = 2;
    BbLabelCheck::NoteParsed(label, 8);
    BbLabelCheck::NoteParsed(label, 8);
    const bool first = BbLabelCheck::BeforeWrite(label, 3, 8);
    block[0] = 3; // the first write landed
    const bool second = BbLabelCheck::BeforeWrite(label, 4, 8);
    // The second was parsed while the label still held 2 and now finds the first write's 3:
    // that is the GPU's own write, not the guest.
    Expect("first of two pending writes: no report", !first);
    Expect("second of two pending writes finds the first one's value: no report", !second);
    block[0] = 4;
    // Two pending, the guest frees the block after the first landed: reported.
    BbLabelCheck::NoteParsed(label, 8);
    BbLabelCheck::NoteParsed(label, 8);
    const bool a = BbLabelCheck::BeforeWrite(label, 5, 8);
    block[0] = 5;
    block[0] = 0x7012345670; // freed while the second EOP is still pending
    Expect("freed while a second EOP is pending: reported",
           !a && BbLabelCheck::BeforeWrite(label, 6, 8));

    // A 32-bit label compares only its 4 bytes.
    alignas(16) static std::uint64_t small[6] = {0xAAAA'BBBB'0000'0000ull};
    const auto small_label = reinterpret_cast<std::uint64_t>(&small[0]);
    BbLabelCheck::NoteParsed(small_label, 4);
    small[0] = 0xCCCC'DDDD'0000'0000ull; // the upper half is another field
    Expect("32-bit label: a change in the other half is not reported",
           !BbLabelCheck::BeforeWrite(small_label, 1, 4));

    Expect("a write without a parse note is not reported",
           !BbLabelCheck::BeforeWrite(reinterpret_cast<std::uint64_t>(&block[3]), 9, 8));
    std::fprintf(stderr, "%s: %u failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
