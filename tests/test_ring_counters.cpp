// Constant-ring counters of the parse split (gpu/shim/bbport_parse_split.h, "Constant ring per
// frame"): activity before the first frame end is the baseline and is not reported; the first
// window reports exactly the counts made inside it, per frame. Prints the block to stdout for
// tests/test_ring_counters.sh, which checks the line.
#include <cstdio>
#include "bbport_parse_split.h"

int main() {
    using namespace BbSplit;
    // Before the first frame end: startup activity, must not appear.
    for (int i = 0; i < 1000; ++i) {
        NoteRingCopy(4096, true);
        NoteRingSkip(RingSkip::Pending, true);
        NotePendingNote(HugeNote);
    }
    NoteFrameEnd(0); // the baseline
    // Inside the first window (600 frame ends): known counts.
    for (int i = 0; i < 600; ++i) {
        NoteRingCopy(1024, true); // 600 copies, 1 KiB each
    }
    for (int i = 0; i < 60; ++i) {
        NoteRingCopy(1024, false); // ring full
        NoteRingSkip(RingSkip::Large, false);
    }
    for (int i = 0; i < 120; ++i) {
        NoteRingSkip(RingSkip::Pending, i % 2 == 0); // 60 behind a huge range
        NotePendingNote(i % 4 == 0 ? HugeNote : 64);  // 30 huge
    }
    for (int i = 0; i < 60; ++i) {
        NoteHugeShaderNote();
    }
    for (int i = 0; i < 600; ++i) {
        NoteFrameEnd(0);
    }
    return 0;
}
