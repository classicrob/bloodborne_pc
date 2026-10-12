// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal Sea Islands (GCN2) encoder for the geometry-emulation conformance shaders: the
// instructions an ES, a GS and a copy shader need (ring buffer accesses with glc+slc, exec-masked
// control flow, s_sendmsg gs emit/cut). Encodings from the Sea Islands ISA microcode formats.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace GsAsm {

constexpr uint32_t VCC_LO = 106, VCC_HI = 107, EXEC_LO = 126, LITERAL = 255;
constexpr uint32_t Inline(uint32_t value) { return 128 + value; } // 0..64
constexpr uint32_t InlineF1 = 242;                                 // 1.0f
constexpr uint32_t Vgpr(uint32_t n) { return 256 + n; }

inline uint32_t FloatBits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

struct Code {
    std::vector<uint32_t> words;

    void Sop1(uint32_t op, uint32_t sdst, uint32_t ssrc0) {
        words.push_back(0xBE800000u | (sdst << 16) | (op << 8) | ssrc0);
    }
    /// s_mov_b32 sdst, literal
    void SMovLiteral(uint32_t sdst, uint32_t value) {
        Sop1(3, sdst, LITERAL);
        words.push_back(value);
    }
    void SMovB64(uint32_t sdst, uint32_t ssrc0) { Sop1(4, sdst, ssrc0); }
    void SAndSaveexecB64(uint32_t sdst, uint32_t ssrc0) { Sop1(36, sdst, ssrc0); }
    void SOrSaveexecB64(uint32_t sdst, uint32_t ssrc0) { Sop1(37, sdst, ssrc0); }
    void Sopp(uint32_t op, uint32_t simm16) { words.push_back(0xBF800000u | (op << 16) | simm16); }
    void SEndpgm() { Sopp(1, 0); }
    void SWaitcnt() { Sopp(12, 0); }
    /// s_sendmsg gs: op 1 = cut, 2 = emit (stream 0); 3 = gs_done
    void SSendmsgGs(uint32_t op) { Sopp(16, 2u | (op << 4)); }
    void SSendmsgGsDone() { Sopp(16, 3u); }
    /// s_cbranch_execz; returns the word index to patch with PatchBranch.
    size_t SCbranchExecz() {
        Sopp(8, 0);
        return words.size() - 1;
    }
    void PatchBranch(size_t at) {
        const int32_t delta = int32_t(words.size()) - int32_t(at + 1);
        words[at] = (words[at] & 0xFFFF0000u) | (uint32_t(delta) & 0xFFFF);
    }

    void Vop1(uint32_t op, uint32_t vdst, uint32_t src0) {
        words.push_back(0x7E000000u | (vdst << 17) | (op << 9) | src0);
    }
    void VMov(uint32_t vdst, uint32_t src0) { Vop1(1, vdst, src0); }
    void VMovLiteral(uint32_t vdst, uint32_t value) {
        Vop1(1, vdst, LITERAL);
        words.push_back(value);
    }
    void VCvtF32U32(uint32_t vdst, uint32_t vsrc) { Vop1(6, vdst, Vgpr(vsrc)); }

    void Vop2(uint32_t op, uint32_t vdst, uint32_t vsrc1, uint32_t src0) {
        words.push_back((op << 25) | (vdst << 17) | (vsrc1 << 9) | src0);
    }
    void VAddF32Literal(uint32_t vdst, float literal, uint32_t vsrc1) {
        Vop2(3, vdst, vsrc1, LITERAL);
        words.push_back(FloatBits(literal));
    }
    void VMulF32Literal(uint32_t vdst, float literal, uint32_t vsrc1) {
        Vop2(8, vdst, vsrc1, LITERAL);
        words.push_back(FloatBits(literal));
    }
    void VLshrrevB32(uint32_t vdst, uint32_t shift_src0, uint32_t vsrc1) { Vop2(22, vdst, vsrc1, shift_src0); }
    void VLshlrevB32(uint32_t vdst, uint32_t shift_src0, uint32_t vsrc1) { Vop2(26, vdst, vsrc1, shift_src0); }
    void VAndB32Literal(uint32_t vdst, uint32_t literal, uint32_t vsrc1) {
        Vop2(27, vdst, vsrc1, LITERAL);
        words.push_back(literal);
    }

    void Vopc(uint32_t op, uint32_t src0, uint32_t vsrc1) {
        words.push_back(0x7C000000u | (op << 17) | (vsrc1 << 9) | src0);
    }
    /// vcc = src0 < vsrc1 (unsigned)
    void VCmpLtU32(uint32_t src0, uint32_t vsrc1) { Vopc(0xC1, src0, vsrc1); }
    /// vcc = src0 == vsrc1
    void VCmpEqU32(uint32_t src0, uint32_t vsrc1) { Vopc(0xC2, src0, vsrc1); }
    /// vcc = src0 != vsrc1
    void VCmpNeU32(uint32_t src0, uint32_t vsrc1) { Vopc(0xC5, src0, vsrc1); }

    /// MUBUF dword load/store with glc+slc (a ring access); `soffset` is an SGPR or inline.
    void RingDword(bool store, uint32_t vdata, uint32_t vaddr, bool offen, uint32_t offset,
                   uint32_t soffset, uint32_t srsrc = 0) {
        const uint32_t op = store ? 28u : 12u;
        words.push_back(0xE0000000u | (op << 18) | (1u << 14) | (offen ? 1u << 12 : 0) |
                        (offset & 0xFFF));
        words.push_back((soffset << 24) | (1u << 22) | ((srsrc >> 2) << 16) | (vdata << 8) | vaddr);
    }

    /// s_load_dwordx4 sdst, s[sbase:sbase+1], offset (dwords)
    void SLoadDwordx4(uint32_t sdst, uint32_t sbase, uint32_t offset) {
        words.push_back(0xC0000000u | (2u << 22) | (sdst << 15) | ((sbase / 2) << 9) | (1u << 8) |
                        offset);
    }
    void Sop2(uint32_t op, uint32_t sdst, uint32_t ssrc0, uint32_t ssrc1) {
        words.push_back(0x80000000u | (op << 23) | (sdst << 16) | (ssrc1 << 8) | ssrc0);
    }
    void SLshlB32(uint32_t sdst, uint32_t ssrc0, uint32_t ssrc1) { Sop2(30, sdst, ssrc0, ssrc1); }
    /// s_bfe_u32 sdst, ssrc0, (width << 16) | offset
    void SBfeU32(uint32_t sdst, uint32_t ssrc0, uint32_t offset, uint32_t width) {
        Sop2(39, sdst, ssrc0, LITERAL);
        words.push_back((width << 16) | offset);
    }
    void Vop3(uint32_t op, uint32_t vdst, uint32_t src0, uint32_t src1) {
        words.push_back(0xD0000000u | (op << 17) | vdst);
        words.push_back(src0 | (src1 << 9));
    }
    /// vdst = the lane id in the wave (v_mbcnt_lo/hi of -1, as copy shaders compute it)
    void LaneId(uint32_t vdst) {
        Vop3(0x123, vdst, Inline(0) + 65 /* -1 */, Inline(0));
        Vop3(0x124, vdst, Inline(0) + 65, Vgpr(vdst));
    }
    /// vdst = bits of s[mask:mask+1] below this lane (v_mbcnt_lo/hi)
    void MaskRank(uint32_t vdst, uint32_t mask) {
        Vop3(0x123, vdst, mask, Inline(0));
        Vop3(0x124, vdst, mask + 1, Vgpr(vdst));
    }
    /// MUBUF buffer_store_dword (no glc/slc), the index from vaddr with idxen.
    void BufferStoreDword(uint32_t vdata, uint32_t vaddr, bool idxen, uint32_t offset,
                          uint32_t soffset, uint32_t srsrc) {
        words.push_back(0xE0000000u | (28u << 18) | (idxen ? 1u << 13 : 0) | (offset & 0xFFF));
        words.push_back((soffset << 24) | ((srsrc >> 2) << 16) | (vdata << 8) | vaddr);
    }
    /// MTBUF tbuffer_store_format_* (op 4 = x .. 7 = xyzw) with glc+slc, as Gnm's stream-out
    /// stores; the index from vaddr with idxen.
    void TBufferStore(uint32_t op, uint32_t dfmt, uint32_t nfmt, uint32_t vdata, uint32_t vaddr,
                      bool idxen, uint32_t offset, uint32_t soffset, uint32_t srsrc) {
        words.push_back((0x3Au << 26) | (nfmt << 23) | (dfmt << 19) | (op << 16) | (1u << 14) |
                        (idxen ? 1u << 13 : 0) | (offset & 0xFFF));
        words.push_back((soffset << 24) | (1u << 22) | ((srsrc >> 2) << 16) | (vdata << 8) | vaddr);
    }

    /// EXP: target 12 = pos0, 32 + n = param n.
    void Exp(uint32_t target, bool done, uint32_t v0) {
        words.push_back(0xF8000000u | (done ? 1u << 11 : 0) | (target << 4) | 0xF);
        words.push_back(((v0 + 3) << 24) | ((v0 + 2) << 16) | ((v0 + 1) << 8) | v0);
    }
};

} // namespace GsAsm
