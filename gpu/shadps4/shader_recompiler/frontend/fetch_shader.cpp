// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <xxhash.h>
#include "bbport_toggles.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/fetch_shader.h"

namespace Shader::Gcn {

void NoteVertexAttributeFormat(u64 pgm_hash, const VertexAttribute& attribute, const AmdGpu::Buffer& sharp) {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_VERTEX_CENSUS");
        return value && value[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    const u32 dfmt = sharp.data_format, nfmt = sharp.num_format;
    const u32 dst_sel = sharp.dst_sel_x | sharp.dst_sel_y << 3 | sharp.dst_sel_z << 6 | sharp.dst_sel_w << 9;
    const u64 key = u64(dfmt) | u64(nfmt) << 6 | u64(sharp.stride) << 9 | u64(attribute.num_elements) << 23 |
                    u64(attribute.inst_offset) << 26 | u64(sharp.base_address & 15) << 34 | u64(dst_sel) << 38 |
                    u64(attribute.instance_data) << 50 | u64(attribute.data_format != 0) << 52 |
                    u64(sharp.swizzle_enable) << 53 | u64(sharp.add_tid_enable) << 54;
    static std::mutex mutex;
    static std::unordered_set<u64> seen;
    {
        std::scoped_lock lock{mutex};
        if (!seen.insert(key).second) {
            return;
        }
    }
    LOG_WARNING(Render_Recompiler,
                "Vertex format census: dfmt {} nfmt {} stride {} components {} inst_offset {} base%16 {} "
                "dst_sel {:o} step {} typed_inst {} swizzle {} add_tid {} num_records {} (first vs {:#x})",
                dfmt, nfmt, u32(sharp.stride), u32(attribute.num_elements), u32(attribute.inst_offset),
                u32(sharp.base_address & 15), dst_sel, u32(attribute.instance_data), attribute.data_format != 0,
                u32(sharp.swizzle_enable), u32(sharp.add_tid_enable), u32(sharp.num_records), pgm_hash);
}

/**
 * s_load_dwordx4 s[8:11], s[2:3], 0x00
 * s_load_dwordx4 s[12:15], s[2:3], 0x04
 * s_load_dwordx4 s[16:19], s[2:3], 0x08
 * s_waitcnt     lgkmcnt(0)
 * buffer_load_format_xyzw v[4:7], v0, s[8:11], 0 idxen
 * buffer_load_format_xyz v[8:10], v0, s[12:15], 0 idxen
 * buffer_load_format_xy v[12:13], v0, s[16:19], 0 idxen
 * s_waitcnt     0
 * s_setpc_b64   s[0:1]

 * s_load_dwordx4  s[4:7], s[2:3], 0x0
 * s_waitcnt       lgkmcnt(0)
 * buffer_load_format_xyzw v[4:7], v0, s[4:7], 0 idxen
 * s_load_dwordx4  s[4:7], s[2:3], 0x8
 * s_waitcnt       lgkmcnt(0)
 * buffer_load_format_xyzw v[8:11], v0, s[4:7], 0 idxen
 * s_waitcnt       vmcnt(0) & expcnt(0) & lgkmcnt(0)
 * s_setpc_b64     s[0:1]

 * A normal fetch shader looks like the above, the instructions are generated
 * using input semantics on cpu side. Load instructions can either be separate or interleaved
 * We take the reverse way, extract the original input semantics from these instructions.
 **/

static bool IsTypedBufferLoad(const Gcn::GcnInst& inst) {
    return inst.opcode == Opcode::TBUFFER_LOAD_FORMAT_X ||
           inst.opcode == Opcode::TBUFFER_LOAD_FORMAT_XY ||
           inst.opcode == Opcode::TBUFFER_LOAD_FORMAT_XYZ ||
           inst.opcode == Opcode::TBUFFER_LOAD_FORMAT_XYZW;
}

const u32* GetFetchShaderCode(const Info& info, u32 sgpr_base) {
    const u32* code;
    std::memcpy(&code, &info.UserData()[sgpr_base], sizeof(code));
    return code;
}

std::optional<FetchShaderData> ParseFetchShader(const Shader::Info& info) {
    if (!info.has_fetch_shader) {
        return std::nullopt;
    }

    const auto* code = GetFetchShaderCode(info, info.fetch_shader_sgpr_base);

    // bbport: the pipeline cache parses the fetch shader on every draw. Results are cached
    // per thread by code address and revalidated against a copy of the code, which is
    // much cheaper than decoding it again.
    struct CachedParse {
        std::vector<u32> code;
        FetchShaderData data;
    };
    thread_local std::unordered_map<const u32*, CachedParse> cache;
    if (const auto it = cache.find(code);
        !BbToggle::Disabled(BbToggle::FetchShaderCache) && it != cache.end() &&
        std::memcmp(code, it->second.code.data(), it->second.code.size() * sizeof(u32)) == 0) {
        return it->second.data;
    }

    FetchShaderData data{};
    GcnCodeSlice code_slice(code, code + std::numeric_limits<u32>::max());
    GcnDecodeContext decoder;

    struct VsharpLoad {
        u32 dword_offset{};
        u32 base_sgpr{};
    };
    std::array<VsharpLoad, 104> loads{};

    u32 semantic_index = 0;
    while (!code_slice.atEnd()) {
        const auto inst = decoder.decodeInstruction(code_slice);
        data.size += inst.length;

        if (inst.opcode == Opcode::S_SETPC_B64) {
            break;
        }

        if (inst.inst_class == InstClass::ScalarMemRd) {
            loads[inst.dst[0].code] = VsharpLoad{inst.control.smrd.offset, inst.src[0].code * 2};
            continue;
        }

        if (inst.opcode == Opcode::V_ADD_I32) {
            const auto vgpr = inst.dst[0].code;
            const auto sgpr = s8(inst.src[0].code);
            switch (vgpr) {
            case 0: // V0 is always the vertex offset
                data.vertex_offset_sgpr = sgpr;
                break;
            case 3: // V3 is always the instance offset
                data.instance_offset_sgpr = sgpr;
                break;
            default:
                UNREACHABLE();
            }
        }

        if (inst.inst_class == InstClass::VectorMemBufFmt) {
            // SRSRC is in units of 4 SPGRs while SBASE is in pairs of SGPRs
            const u32 base_sgpr = inst.src[2].code * 4;
            auto& attrib = data.attributes.emplace_back();
            attrib.semantic = semantic_index++;
            attrib.dest_vgpr = inst.src[1].code;
            attrib.num_elements = inst.control.mubuf.count;
            attrib.sgpr_base = loads[base_sgpr].base_sgpr;
            attrib.dword_offset = loads[base_sgpr].dword_offset;
            attrib.inst_offset = inst.control.mtbuf.offset;
            attrib.instance_data = inst.src[0].code;
            if (IsTypedBufferLoad(inst)) {
                attrib.data_format = inst.control.mtbuf.dfmt;
                attrib.num_format = inst.control.mtbuf.nfmt;
            }
        }
    }

    data.code_hash = XXH3_64bits(code, data.size);

    if (cache.size() >= 4096) {
        cache.clear();
    }
    cache[code] = CachedParse{std::vector<u32>(code, code_slice.position()), data};
    return data;
}

} // namespace Shader::Gcn
