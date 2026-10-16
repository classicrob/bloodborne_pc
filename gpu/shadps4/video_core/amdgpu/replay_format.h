// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

/// Perf replay v0 capture files (docs/macos/capture-replay.md "Perf replay v0"). Every file
/// starts with a FileHeader; all integers little-endian; records are written in order, each
/// followed by its payload. Readers check magic, version and the record counts and stop loudly
/// on any mismatch or short read.
namespace ReplayFormat {

constexpr u32 Version = 2; ///< unchanged layouts for every file except resets.bin
constexpr u32 ResetsVersion = 3; ///< named host objects; legacy allocator identity is refused

struct FileHeader {
    char magic[8];  ///< one of the *Magic values, not NUL-terminated
    u32 version;    ///< Version
    u32 reserved;
    u64 records;    ///< record count of this file
};
static_assert(sizeof(FileHeader) == 24);

constexpr char ViewsMagic[8] = {'B', 'B', 'R', 'P', 'V', 'I', 'E', 'W'};
constexpr char BackingMagic[8] = {'B', 'B', 'R', 'P', 'B', 'A', 'C', 'K'};
constexpr char ImageMagic[8] = {'B', 'B', 'R', 'P', 'E', 'B', 'O', 'T'};
constexpr char LiverpoolMagic[8] = {'B', 'B', 'R', 'P', 'L', 'P', 'O', 'O'};
constexpr char SubmissionsMagic[8] = {'B', 'B', 'R', 'P', 'S', 'U', 'B', 'S'};
constexpr char ResetsMagic[8] = {'B', 'B', 'R', 'P', 'R', 'S', 'E', 'T'};
constexpr char ImagesMagic[8] = {'B', 'B', 'R', 'P', 'I', 'M', 'G', 'S'};
constexpr char ImageMetaMagic[8] = {'B', 'B', 'R', 'P', 'I', 'M', 'T', 'A'};

/// The files of a complete capture, each with its size in manifest.json ("files"; manifest_version
/// 2). The loader (src/probe.c, which mirrors this list) refuses a directory whose manifest is
/// missing or whose files differ from it.
constexpr const char* CaptureFiles[] = {"views.bin",     "backing.bin",       "image.bin",
                                        "liverpool.bin", "submissions.bin",   "resets.bin",
                                        "images.bin",    "image_metadata.bin"};

/// views.bin: blocks first (records = blocks + views), then views.
struct BlockRecord {
    u64 start, size; ///< direct allocation (backing offset)
    s32 type;
    u32 reserved;
};
struct ViewRecord {
    u64 start, end; ///< guest VA
    u64 phys;       ///< backing offset at capture
    u32 flexible;   ///< 0 direct, 1 flexible
    s32 prot, type;
    u32 reserved;
};
struct ViewsCounts {
    u64 blocks, views;
};

/// backing.bin: runs of backing bytes that were present at capture; absent ranges are zero.
struct BackingRun {
    u64 phys;  ///< backing offset
    u64 bytes; ///< payload bytes that follow
};

/// image.bin: an ImageRecord (the eboot image range, not counted in `records`), then one
/// ImageRun + bytes per run of readable pages (`records` runs). Unreadable pages (protected
/// regions inside the image) are not captured; the replay leaves them as loaded.
struct ImageRecord {
    u64 start, size;
};
struct ImageRun {
    u64 offset; ///< from the image start
    u64 bytes;
};

/// liverpool.bin: one record per state section, then its bytes.
enum class LiverpoolSection : u32 {
    Regs = 1,           ///< AmdGpu::Liverpool::Regs
    CeCounters = 2,     ///< ce_compare_count, ce_count, de_count (3 x u32)
    CeHeap = 3,         ///< ConstantEngine::constants_heap
    QueueCsState = 4,   ///< u32 queue id, then AmdGpu::ComputeProgram
    IndirectArgs = 5,   ///< u64 indirect_args_addr
    GfxRegChecksum = 6, ///< u64 gfx_reg_checksum (rolling: draw preparation's baseline)
};
struct SectionRecord {
    LiverpoolSection section;
    u32 reserved;
    u64 bytes; ///< payload bytes that follow (checked against the reader's sizeof)
};

/// submissions.bin: per held graphics submission, a SubmissionRecord, then `regions` x
/// (RegionRecord + size_dw words when copied).
struct SubmissionRecord {
    u64 submit_number;
    u32 regions;
    s32 dcb_region; ///< index of the DCB region
    s32 ccb_region; ///< index of the CCB region, or -1
    u32 reserved;
};
struct RegionRecord {
    u64 guest;   ///< guest address of the buffer
    u32 size_dw; ///< dwords
    u32 constant; ///< constant engine (CCB, IndirectBufferConst)
    u32 copied;   ///< 0: unreadable at submit, no words follow
    u32 reserved;
};

/// resets.bin: guest ranges restored to their capture bytes before every iteration.
enum class ResetKind : u32 {
    Written = 1, ///< a packet of the frame writes it (label, DMA, copy, CE dump, query)
    Waited = 2,  ///< WAIT_REG_MEM / COND_EXEC / semaphore read it
    Indirect = 3, ///< indirect draw/dispatch arguments
};
struct ResetRecord {
    u64 address;
    u32 bytes; ///< payload bytes that follow (the value at the cut)
    ResetKind kind;
    u32 opcode; ///< the PM4 opcode that named it (first one, after merging)
    u32 host;   ///< 1: outside the guest views (host memory, e.g. an HLE object in the low arena)
};
/// After each host reset's bytes: an explicitly registered semantic range. The reset address
/// must equal start + offset and its whole payload must lie in the range. Base address still
/// has to match across builds because the command packets embed it.
struct HostObjectRecord {
    u64 start, size, offset;
    char name[64]; ///< owned name, nonempty and NUL-terminated
};
static_assert(sizeof(HostObjectRecord) == 88);

/// images.bin: per GPU-modified image (Rasterizer::CaptureImages), an ImageHeader, its state
/// blob, then `aspects` x (AspectRecord + texels).
struct ImageHeader {
    u32 version;  ///< VideoCore::CapturedImageVersion
    u32 aspects;
    u64 state_bytes;
};
struct AspectRecord {
    u32 aspect, mip, layers, width, height, depth, texel_bytes, reserved;
    u64 bytes;
};
/// image_metadata.bin: one record, then its bytes (VideoCore::CapturedMetadata).
struct MetadataRecord {
    u32 version;
    u32 reserved;
    u64 bytes;
};

} // namespace ReplayFormat
