// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: BB_WRITE_LOG=1 — a ring of the writes the GPU emulation makes to guest memory (fences,
// WriteData, buffer and image downloads), dumped when the guest faults: which of them landed
// where the guest's data got corrupted. Diagnostic only.
#pragma once

#include <cstdint>

namespace BbWriteLog {
enum Source : std::uint32_t { Backing, WriteData, Fence, FenceIntent, WriteDataIntent, EosIntent };
bool Enabled();
/// BB_WRITE_LOG=1: at the write itself (changes the timing of those writes).
void Note(std::uint64_t address, const void* data, std::uint64_t size, Source source);
/// BB_WRITE_LOG=2: where the GPU command thread decodes a fence or WriteData, off the write
/// path (the address and value it will write later).
void NoteIntent(std::uint64_t address, const void* data, std::uint64_t size, Source source);
/// Logged writes overlapping [begin, end) (newest first, at most 200) and the 16 latest, on
/// stderr, for a failure that is not a guest fault (a malformed PM4 packet). Nothing if off.
void DumpNear(std::uint64_t begin, std::uint64_t end, const char* why);
}

extern "C" void bbgpu_dump_guest_writes(void* ucontext);
