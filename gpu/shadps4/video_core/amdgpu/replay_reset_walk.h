// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (perf replay capture, docs/macos/capture-replay.md): the reset walk. Over a held
// frame's command buffers in submission order it lists the memory the command processor writes
// (reset at every replay iteration) or waits on, and refuses ("blockers") what the replay could
// not reproduce. Separate from replay_perf_capture.cpp so replay-capture-test runs it on
// hand-built packets.
//
// Conservative by construction:
// - Only listed opcodes pass: the decoded writers below, and an allow-list of packets that write
//   no memory. Any other opcode is a blocker, as the parser stops on it too.
// - Each decoded packet must hold its whole structure, and address + length must not wrap.
// - A WAIT_REG_MEM not satisfied by the memory at the cut is accepted only if an earlier,
//   unconditional write by the drawing engine (DE) covers it. Writes inside a COND_EXEC window
//   (they may not run) and constant-engine (CCB) writes (the CE runs concurrently with the DE,
//   not before it) are reset, but never certify a wait.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>
#include <fmt/format.h>
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/amdgpu/replay_format.h"

namespace ReplayCapture::Perf::Detail {

/// One command buffer of a submission, as captured (CmdSnapshot::Region and its words).
struct WalkRegion {
    u64 guest;
    u32 size_dw;
    bool constant; ///< constant engine (CCB, IndirectBufferConst)
    bool copied;   ///< false: unreadable at submit
    std::span<const u32> words;
};

class ResetWalk {
public:
    /// `read`: the memory at the cut (false when unreadable). `counter_pairs`: the occlusion
    /// counter pairs a ZPASS_DONE dump writes (Liverpool state).
    ResetWalk(std::function<bool(u64, void*, u64)> read, u32 counter_pairs)
        : read_memory{std::move(read)}, pairs{counter_pairs} {}

    /// One submission: every captured region (for indirect buffer lookup), its CCB if any, then
    /// its DCB. Submissions in order.
    void Submission(std::span<const WalkRegion> regions_, const WalkRegion* ccb,
                    const WalkRegion& dcb) {
        regions = regions_;
        if (ccb) {
            Walk(*ccb, 0, false, true);
        }
        Walk(dcb, 0, false, false);
    }

    std::vector<std::string> blockers;

    struct Range {
        u64 address;
        u64 bytes;
        ReplayFormat::ResetKind kind;
        u32 opcode;
    };
    std::vector<Range> ranges;

private:
    using ResetKind = ReplayFormat::ResetKind;

    void Blocker(std::string why) {
        if (blockers.size() < 32) {
            blockers.push_back(std::move(why));
        }
    }

    /// `certifies`: an unconditional DE write, which may satisfy a later wait.
    void Add(u64 address, u64 bytes, ResetKind kind, u32 opcode, bool certifies) {
        if (!bytes) {
            return;
        }
        if (address > ~u64{0} - bytes) {
            Blocker(fmt::format("packet {:#04x}: range {:#x} + {:#x} wraps", opcode, address, bytes));
            return;
        }
        ranges.push_back({address, bytes, kind, opcode});
        if (kind == ResetKind::Written && certifies) {
            written.emplace_back(address, address + bytes);
        }
    }

    bool WrittenBefore(u64 address, u64 bytes) const {
        for (const auto& w : written) {
            if (address >= w.first && address + bytes <= w.second) {
                return true;
            }
        }
        return false;
    }

    template <class T>
    const T* As(std::span<const u32> p, u32 op) {
        if (p.size() * sizeof(u32) < sizeof(T)) {
            Blocker(fmt::format("packet {:#04x} holds {} dwords, its structure needs {}", op,
                                p.size(), (sizeof(T) + 3) / 4));
            return nullptr;
        }
        return reinterpret_cast<const T*>(p.data());
    }

    /// `conditional`: inside a COND_EXEC window. `ce`: the constant engine's stream.
    void Walk(const WalkRegion& region, int depth, bool conditional, bool ce) {
        if (!region.copied) {
            Blocker(fmt::format("command region {:#x} ({} dwords) was not copied at submit",
                                region.guest, region.size_dw));
            return;
        }
        if (depth > 8) {
            Blocker("indirect buffers nested deeper than 8");
            return;
        }
        const auto words = region.words;
        size_t cond_end = 0; // dword index where the current COND_EXEC window ends
        for (size_t i = 0; i < words.size();) {
            const u32 header = words[i];
            const u32 type = header >> 30;
            if (type == 2) {
                ++i;
                continue;
            }
            const u64 length = u64{(header >> 16) & 0x3FFF} + 2;
            if (type != 3 || length > words.size() - i) {
                Blocker(fmt::format("command region {:#x}: bad packet at dword {}", region.guest, i));
                return;
            }
            const u32 op = (header >> 8) & 0xFF;
            const bool in_window = conditional || i < cond_end;
            if (op == 0x22) { // COND_EXEC
                if (const auto* cond = As<AmdGpu::PM4CmdCondExec>(words.subspan(i, length), op)) {
                    Add(reinterpret_cast<u64>(cond->Address()), 4, ResetKind::Waited, op, false);
                    cond_end = std::max<size_t>(cond_end, i + length + cond->exec_count.Value());
                }
            } else {
                Packet(words.subspan(i, length), op, depth, in_window, ce);
            }
            i += length;
        }
    }

    void Packet(std::span<const u32> p, u32 op, int depth, bool conditional, bool ce) {
        using AmdGpu::DataSelect;
        const bool certifies = !conditional && !ce;
        switch (op) {
        case 0x3F:   // INDIRECT_BUFFER
        case 0x33: { // INDIRECT_BUFFER_CONST
            const auto* ib = As<AmdGpu::PM4CmdIndirectBuffer>(p, op);
            if (!ib) {
                return;
            }
            const u64 address = reinterpret_cast<u64>(ib->Address<const u32>());
            for (const auto& r : regions) {
                if (r.guest == address && r.size_dw == ib->ib_size.Value() &&
                    r.constant == (op == 0x33)) {
                    Walk(r, depth + 1, conditional, ce || op == 0x33);
                    return;
                }
            }
            Blocker(fmt::format("indirect buffer {:#x} ({} dwords) has no captured region", address,
                                ib->ib_size.Value()));
            return;
        }
        case 0x47: { // EVENT_WRITE_EOP
            const auto* eop = As<AmdGpu::PM4CmdEventWriteEop>(p, op);
            if (eop && eop->data_sel.Value() != DataSelect::None) {
                Add(reinterpret_cast<u64>(eop->Address<u8>()),
                    eop->data_sel.Value() == DataSelect::Data32Low ? 4 : 8, ResetKind::Written, op,
                    certifies);
            }
            return;
        }
        case 0x48: { // EVENT_WRITE_EOS
            const auto* eos = As<AmdGpu::PM4CmdEventWriteEos>(p, op);
            if (!eos) {
                return;
            }
            const u64 address = reinterpret_cast<u64>(eos->Address<u8*>());
            Add(address,
                eos->command.Value() == AmdGpu::PM4CmdEventWriteEos::Command::SignalFence
                    ? 4
                    : u64{eos->size.Value()} * 4,
                ResetKind::Written, op, certifies);
            return;
        }
        case 0x49: { // RELEASE_MEM
            const auto* rel = As<AmdGpu::PM4CmdReleaseMem>(p, op);
            if (!rel) {
                return;
            }
            const auto sel = rel->data_sel.Value();
            if (sel == DataSelect::GdsMemStore) {
                Add(rel->Address<u64>(), u64{rel->num_dw} * 4, ResetKind::Written, op, certifies);
            } else if (sel != DataSelect::None) {
                Add(rel->Address<u64>(), sel == DataSelect::Data32Low ? 4 : 8, ResetKind::Written,
                    op, certifies);
            }
            return;
        }
        case 0x37: { // WRITE_DATA
            const auto* wd = As<AmdGpu::PM4CmdWriteData>(p, op);
            if (!wd) {
                return;
            }
            const u32 sel = wd->dst_sel.Value();
            if (sel == 2 || sel == 5) {
                Add(wd->Address<u64>(), wd->wr_one_addr.Value() ? 4 : wd->Size(),
                    ResetKind::Written, op, certifies);
            } else if (sel != 0) {
                Blocker(fmt::format("WRITE_DATA with dst_sel {} is not decoded", sel));
            }
            return;
        }
        case 0x50: { // DMA_DATA, as Liverpool::ProcessGraphics and RunDmaData handle it
            const auto* dma = As<AmdGpu::PM4DmaData>(p, op);
            if (!dma || dma->dst_addr_lo == 0x3022C) {
                return; // truncated (blocker) or skipped by the parser
            }
            const auto dst = dma->dst_sel;
            if (dma->command.das) {
                Blocker(fmt::format("DMA_DATA to register space (dst {:#x}), which the parser "
                                    "asserts against",
                                    dma->dst_addr_lo));
            } else if (dst == AmdGpu::DmaDataDst::Memory ||
                       dst == AmdGpu::DmaDataDst::MemoryUsingL2) {
                Add(dma->DstAddress<u64>(), dma->NumBytes(), ResetKind::Written, op, certifies);
            } else if (dst != AmdGpu::DmaDataDst::Gds) {
                Blocker(fmt::format("DMA_DATA with dst_sel {}", u32(dst)));
            }
            return;
        }
        case 0x83: { // DUMP_CONST_RAM (constant engine)
            if (const auto* dump = As<AmdGpu::PM4DumpConstRam>(p, op)) {
                Add(dump->Address<u64>(), dump->Size(), ResetKind::Written, op, certifies);
            }
            return;
        }
        case 0x46: { // EVENT_WRITE: only a ZPASS_DONE pipe-stat dump writes memory
            const auto* ev = As<AmdGpu::PM4CmdEventWrite>(p, op);
            if (ev && ev->event_index.Value() == AmdGpu::EventIndex::ZpassDone &&
                ev->event_type.Value() == AmdGpu::EventType::PixelPipeStatDump) {
                if (p.size() < 4) {
                    Blocker("EVENT_WRITE PixelPipeStatDump lacks its two address dwords");
                } else {
                    Blocker(fmt::format("PixelPipeStatDump writes {} bytes and changes uncaptured "
                                        "Liverpool pixel_counter", u64{pairs} * 16));
                }
            }
            return;
        }
        case 0x3C: { // WAIT_REG_MEM
            const auto* wait = As<AmdGpu::PM4CmdWaitRegMem>(p, op);
            if (!wait || wait->mem_space.Value() != AmdGpu::PM4CmdWaitRegMem::MemSpace::Memory) {
                return;
            }
            const u64 address = reinterpret_cast<u64>(wait->Address());
            Add(address, 4, ResetKind::Waited, op, false);
            if (WrittenBefore(address, 4)) {
                return;
            }
            u32 value = 0;
            if (!read_memory(address, &value, 4)) {
                Blocker(fmt::format("WAIT_REG_MEM polls {:#x}, outside captured memory", address));
            } else if (!wait->TestValue(value)) {
                Blocker(fmt::format(
                    "WAIT_REG_MEM polls {:#x} (function {}, ref {:#x}, mask {:#x}): no earlier "
                    "unconditional DE packet of the frame writes it and its value at the cut "
                    "({:#x}) does not satisfy it, so the frame waits on the guest CPU, another "
                    "queue or a write that may not run",
                    address, u32(wait->function.Value()), wait->ref, wait->mask, value));
            }
            return;
        }
        case 0x11: { // SET_BASE
            const auto* base = As<AmdGpu::PM4CmdSetBase>(p, op);
            if (base &&
                base->base_index.Value() == AmdGpu::PM4CmdSetBase::BaseIndex::DrawIndexIndirPatchTable) {
                if (conditional || ce) {
                    Blocker("conditional or CE SET_BASE makes the indirect reset base ambiguous");
                    return;
                }
                indirect_base = base->Address<u64>();
            }
            return;
        }
        case 0x24:   // DRAW_INDIRECT
        case 0x25:   // DRAW_INDEX_INDIRECT
        case 0x16: { // DISPATCH_INDIRECT
            if (p.size() < 2) {
                Blocker(fmt::format("packet {:#04x} has no data offset", op));
                return;
            }
            if (!indirect_base) {
                Blocker(fmt::format("indirect packet {:#04x} before any SET_BASE in the frame", op));
                return;
            }
            const u64 bytes = op == 0x16 ? 12 : op == 0x24 ? 16 : 20;
            Add(indirect_base + p[1], bytes, ResetKind::Indirect, op, false);
            return;
        }
        // Packets that write no memory (register state, draws, cache and counter control, CE
        // RAM). Reads of memory the frame does not write need no reset.
        case 0x10: // NOP
        case 0x12: // CLEAR_STATE
        case 0x13: // INDEX_BUFFER_SIZE
        case 0x15: // DISPATCH_DIRECT
        case 0x20: // SET_PREDICATION
        case 0x26: // INDEX_BASE
        case 0x27: // DRAW_INDEX_2
        case 0x28: // CONTEXT_CONTROL
        case 0x2A: // INDEX_TYPE
        case 0x2D: // DRAW_INDEX_AUTO
        case 0x2F: // NUM_INSTANCES
        case 0x35: // DRAW_INDEX_OFFSET_2
        case 0x42: // PFP_SYNC_ME
        case 0x58: // ACQUIRE_MEM
        case 0x68: // SET_CONFIG_REG
        case 0x69: // SET_CONTEXT_REG
        case 0x76: // SET_SH_REG
        case 0x78: // SET_QUEUE_REG
        case 0x79: // SET_UCONFIG_REG
        case 0x80: // LOAD_CONST_RAM (memory -> CE RAM)
        case 0x81: // WRITE_CONST_RAM
        case 0x84: // INCREMENT_CE_COUNTER
        case 0x85: // INCREMENT_DE_COUNTER
        case 0x86: // WAIT_ON_CE_COUNTER
        case 0x88: // WAIT_ON_DE_COUNTER_DIFF
            return;
        default:
            // Memory writers v0 does not decode (ATOMIC_*, OCCLUSION_QUERY, the multi-draw
            // indirects, STRMOUT_BUFFER_UPDATE, MEM_SEMAPHORE, COND_WRITE, REWIND, COPY_DATA,
            // GET_LOD_STATS) and every opcode not listed: refuse rather than guess.
            Blocker(fmt::format("packet {:#04x} is not on the perf capture's list of decoded or "
                                "memory-free packets",
                                op));
            return;
        }
    }

    std::function<bool(u64, void*, u64)> read_memory;
    u32 pairs;
    std::span<const WalkRegion> regions;
    u64 indirect_base = 0;
    std::vector<std::pair<u64, u64>> written; ///< certifying writes, in execution order
};

} // namespace ReplayCapture::Perf::Detail
