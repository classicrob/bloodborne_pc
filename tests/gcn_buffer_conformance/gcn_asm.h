// Minimal GCN2 (Sea Islands) encoder for the conformance shaders. Encodings from the Sea Islands
// ISA chapter 13 microcode formats.
#pragma once
#include <cstdint>
#include <vector>

namespace GcnAsm {

constexpr uint32_t VCC_LO = 106, VCC_HI = 107, EXEC_LO = 126, LITERAL = 255;
constexpr uint32_t Inline(uint32_t value) { return 128 + value; } // 0..64
constexpr uint32_t InlineNeg1 = 193;
constexpr uint32_t Vgpr(uint32_t n) { return 256 + n; }

struct Code {
    std::vector<uint32_t> words;

    void Sop1(uint32_t op, uint32_t sdst, uint32_t ssrc0) {
        words.push_back(0xBE800000u | (sdst << 16) | (op << 8) | ssrc0);
    }
    void SMovLiteral(uint32_t sdst, uint32_t value) {
        Sop1(3, sdst, LITERAL);
        words.push_back(value);
    }
    void SMovB64(uint32_t sdst, uint32_t ssrc0) { Sop1(4, sdst, ssrc0); }
    void SAndSaveexecB64(uint32_t sdst, uint32_t ssrc0) { Sop1(36, sdst, ssrc0); }
    void Sopp(uint32_t op, uint32_t simm16) { words.push_back(0xBF800000u | (op << 16) | simm16); }
    void SWaitcntAll() { Sopp(12, 0); }
    void SEndpgm() { Sopp(1, 0); }

    void Vop2(uint32_t op, uint32_t vdst, uint32_t vsrc1, uint32_t src0) {
        words.push_back((op << 25) | (vdst << 17) | (vsrc1 << 9) | src0);
    }
    void Vop1(uint32_t op, uint32_t vdst, uint32_t src0) {
        words.push_back(0x7E000000u | (vdst << 17) | (op << 9) | src0);
    }
    // V_READLANE_B32 (VOP2 1): sdst = SGPR, lane select = SGPR (vsrc1 field).
    void VReadlane(uint32_t sdst, uint32_t vgpr, uint32_t lane_sgpr) { Vop2(1, sdst, lane_sgpr, Vgpr(vgpr)); }
    void VReadfirstlane(uint32_t sdst, uint32_t vgpr) { Vop1(2, sdst, Vgpr(vgpr)); }
    void VMbcnt(bool hi, uint32_t vdst, uint32_t src0, uint32_t vsrc1) { Vop2(hi ? 36 : 35, vdst, vsrc1, src0); }
    void VCndmask(uint32_t vdst, uint32_t src0, uint32_t vsrc1) { Vop2(0, vdst, vsrc1, src0); }
    void SBcnt1B64(uint32_t sdst, uint32_t ssrc0) { Sop1(16, sdst, ssrc0); }
    void VMov(uint32_t vdst, uint32_t src0) { words.push_back(0x7E000000u | (vdst << 17) | (1u << 9) | src0); }
    void VMovLiteral(uint32_t vdst, uint32_t value) {
        VMov(vdst, LITERAL);
        words.push_back(value);
    }
    void VLshlrevB32(uint32_t vdst, uint32_t shift_src0, uint32_t vsrc1) {
        words.push_back((26u << 25) | (vdst << 17) | (vsrc1 << 9) | shift_src0);
    }
    void VCmpNeU32(uint32_t src0, uint32_t vsrc1) {
        words.push_back(0x7C000000u | (197u << 17) | (vsrc1 << 9) | src0);
    }

    // SMRD: sbase is the first SGPR of the V# (multiple of 2; the field drops the LSB).
    void SBufferLoad(uint32_t count, uint32_t sdst, uint32_t sbase, bool imm, uint32_t offset) {
        uint32_t op = 8;
        for (uint32_t c = count; c > 1; c >>= 1) ++op;
        words.push_back(0xC0000000u | (op << 22) | (sdst << 15) | ((sbase >> 1) << 9) |
                        (imm ? 1u << 8 : 0) | offset);
    }

    struct BufFields {
        uint32_t offset = 0;
        bool offen = false, idxen = false, glc = false;
        uint32_t vaddr = 0, vdata = 0, srsrc = 0, soffset = Inline(0);
    };
    void Mubuf(uint32_t op, const BufFields& f) {
        words.push_back(0xE0000000u | (op << 18) | (f.glc ? 1u << 14 : 0) | (f.idxen ? 1u << 13 : 0) |
                        (f.offen ? 1u << 12 : 0) | (f.offset & 0xFFF));
        words.push_back((f.soffset << 24) | ((f.srsrc >> 2) << 16) | (f.vdata << 8) | f.vaddr);
    }
    void Mtbuf(uint32_t op, uint32_t dfmt, uint32_t nfmt, const BufFields& f) {
        words.push_back(0xE8000000u | (nfmt << 23) | (dfmt << 19) | (op << 16) |
                        (f.glc ? 1u << 14 : 0) | (f.idxen ? 1u << 13 : 0) | (f.offen ? 1u << 12 : 0) |
                        (f.offset & 0xFFF));
        words.push_back((f.soffset << 24) | ((f.srsrc >> 2) << 16) | (f.vdata << 8) | f.vaddr);
    }
};

// MUBUF opcodes (Sea Islands).
enum : uint32_t {
    BUFFER_LOAD_FORMAT_X = 0, BUFFER_LOAD_FORMAT_XY = 1, BUFFER_LOAD_FORMAT_XYZ = 2,
    BUFFER_LOAD_FORMAT_XYZW = 3, BUFFER_STORE_FORMAT_X = 4, BUFFER_STORE_FORMAT_XY = 5,
    BUFFER_STORE_FORMAT_XYZ = 6, BUFFER_STORE_FORMAT_XYZW = 7, BUFFER_LOAD_UBYTE = 8,
    BUFFER_LOAD_SBYTE = 9, BUFFER_LOAD_USHORT = 10, BUFFER_LOAD_SSHORT = 11,
    BUFFER_LOAD_DWORD = 12, BUFFER_LOAD_DWORDX2 = 13, BUFFER_LOAD_DWORDX4 = 14,
    BUFFER_LOAD_DWORDX3 = 15, BUFFER_STORE_BYTE = 24, BUFFER_STORE_SHORT = 26,
    BUFFER_STORE_DWORD = 28, BUFFER_STORE_DWORDX2 = 29, BUFFER_STORE_DWORDX4 = 30,
    BUFFER_STORE_DWORDX3 = 31, BUFFER_ATOMIC_SWAP = 48, BUFFER_ATOMIC_CMPSWAP = 49,
    BUFFER_ATOMIC_ADD = 50, BUFFER_ATOMIC_SUB = 51, BUFFER_ATOMIC_SMIN = 53,
    BUFFER_ATOMIC_UMIN = 54, BUFFER_ATOMIC_SMAX = 55, BUFFER_ATOMIC_UMAX = 56,
    BUFFER_ATOMIC_AND = 57, BUFFER_ATOMIC_OR = 58, BUFFER_ATOMIC_XOR = 59,
    BUFFER_ATOMIC_INC = 60, BUFFER_ATOMIC_DEC = 61, BUFFER_ATOMIC_ADD_X2 = 82,
    BUFFER_ATOMIC_UMAX_X2 = 88,
};
// MTBUF opcodes.
enum : uint32_t {
    TBUFFER_LOAD_FORMAT_X = 0, TBUFFER_LOAD_FORMAT_XY = 1, TBUFFER_LOAD_FORMAT_XYZ = 2,
    TBUFFER_LOAD_FORMAT_XYZW = 3, TBUFFER_STORE_FORMAT_X = 4, TBUFFER_STORE_FORMAT_XY = 5,
    TBUFFER_STORE_FORMAT_XYZ = 6, TBUFFER_STORE_FORMAT_XYZW = 7,
};

} // namespace GcnAsm
