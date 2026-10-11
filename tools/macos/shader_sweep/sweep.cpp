// SPDX-License-Identifier: GPL-2.0-or-later
// Shader corpus sweep (Apple): compiles every shader the game ships through the production
// PipelineCache::GetProgram (the device's Apple profile, BuildRuntimeInfo from registers, the
// recompiler, SPIR-V emission, the MoltenVK shader module), the way RefreshGraphicsStages and
// RefreshComputeKey select programs. Registers come from each shader's Gnmx stage struct; user
// data is synthesized from its input usage slots the way the Gnm driver fills them: buffers,
// textures and samplers as valid descriptors, tables and extended user data in memory, and a
// fetch shader generated from the vertex input semantics. ES/GS draws go through the production
// GsEmulationUnsupported check first, as RefreshGraphicsStages does.
//
//   shader-sweep JOBS FIRST END [HULL_JOB]    (JOBS from extract.py; sweep.py runs this)
//   shader-sweep --disasm CODE.bin DWORDS     (GCN disassembly with the production decoder)
//
// stderr carries the production log; "@@JOB <index> <hash> <kind>" precedes a job and
// "@@DONE <index>" follows it. A stop (assertion, exit 23) or a crash ends the process inside a
// job; sweep.py records it and restarts at the next job. Sweep setup errors print "@@SETUP".
#include <sys/mman.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "common/path_util.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/tessellation.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_gs_emulation.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace {

using Shader::HwStage;
using Shader::SwStage;

[[noreturn]] void Setup(const std::string& message) {
    std::fprintf(stderr, "@@SETUP %s\n", message.c_str());
    std::exit(3);
}

/// Guest memory for code, descriptors and tables: below 2^40 like guest addresses (V# and
/// program addresses hold 40 bits), never freed (programs keep pointers to their user data).
struct Arena {
    u8* base{};
    size_t used{};
    size_t size{};
    void Init(size_t bytes) {
        void* want = reinterpret_cast<void*>(0x2000000000ull);
        void* got = mmap(want, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (got == MAP_FAILED || reinterpret_cast<u64>(got) + bytes > (1ull << 40)) {
            Setup("cannot map the guest arena below 2^40");
        }
        base = static_cast<u8*>(got);
        size = bytes;
    }
    template <typename T = u32>
    T* Alloc(size_t bytes, size_t align = 256) {
        used = (used + align - 1) & ~(align - 1);
        if (used + bytes > size) {
            Setup("guest arena exhausted");
        }
        T* p = reinterpret_cast<T*>(base + used);
        used += bytes;
        return p;
    }
} arena;

struct Job {
    size_t index{};
    u64 hash{};
    std::string kind;
    std::string code_path;
    u32 dwords{};
    std::map<std::string, std::string> kv;
    std::vector<u32> slots, sems, copy_slots;
    u32 Get(const char* key) const {
        const auto it = kv.find(key);
        if (it == kv.end()) {
            Setup(std::string("job without ") + key);
        }
        return u32(std::strtoul(it->second.c_str(), nullptr, 0));
    }
};

std::vector<Job> ReadJobs(const char* path) {
    std::ifstream f(path);
    if (!f) {
        Setup(std::string("cannot read ") + path);
    }
    std::vector<Job> jobs;
    for (std::string line; std::getline(f, line);) {
        std::istringstream in(line);
        Job job;
        std::string hash;
        in >> hash >> job.kind >> job.code_path >> job.dwords;
        job.hash = std::strtoull(hash.c_str(), nullptr, 0);
        job.index = jobs.size();
        for (std::string field; in >> field;) {
            const auto eq = field.find('=');
            const auto key = field.substr(0, eq), value = field.substr(eq + 1);
            const u32 word = u32(std::strtoul(value.c_str(), nullptr, 0));
            if (key == "slot") {
                job.slots.push_back(word);
            } else if (key == "sem") {
                job.sems.push_back(word);
            } else if (key == "copy_slot") {
                job.copy_slots.push_back(word);
            } else {
                job.kv[key] = value;
            }
        }
        jobs.push_back(std::move(job));
    }
    return jobs;
}

/// Code plus its binary info, at a 256-byte aligned guest address (SearchBinaryInfo needs the
/// info after the code; program registers hold address >> 8).
std::span<const u32> LoadCode(const std::string& path, u32 dwords) {
    std::ifstream f(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
    if (bytes.size() < dwords * 4) {
        Setup("short code file " + path);
    }
    u32* code = arena.Alloc(bytes.size());
    std::memcpy(code, bytes.data(), bytes.size());
    return {code, dwords};
}

/// Memory behind every synthesized descriptor (compiles never read it, except the tessellation
/// constants, which get their own).
u64 SharedBacking() {
    static const u64 backing = reinterpret_cast<u64>(arena.Alloc(256 * 256 * 4));
    return backing;
}

AmdGpu::Buffer VSharp(u32 components, u64 address = SharedBacking()) {
    static constexpr AmdGpu::DataFormat formats[] = {
        AmdGpu::DataFormat::Format32, AmdGpu::DataFormat::Format32_32,
        AmdGpu::DataFormat::Format32_32_32, AmdGpu::DataFormat::Format32_32_32_32};
    AmdGpu::Buffer sharp{};
    sharp.base_address = address;
    sharp.stride = components * 4;
    sharp.num_records = 1024;
    sharp.data_format = u32(formats[components - 1]);
    sharp.num_format = u32(AmdGpu::NumberFormat::Float);
    sharp.dst_sel_x = 4;
    sharp.dst_sel_y = components > 1 ? 5 : 0;
    sharp.dst_sel_z = components > 2 ? 6 : 0;
    sharp.dst_sel_w = components > 3 ? 7 : 1;
    return sharp;
}

/// Stream-out vertex size of the synthesized stream-out buffers (V# stride, VGT vertex stride).
constexpr u32 SoVertexBytes = 64;

/// A 256x256 RGBA8 2D texture.
AmdGpu::Image TSharp() {
    auto image = AmdGpu::Image::Null(false);
    image.base_address = SharedBacking() >> 8;
    image.width = 255;
    image.height = 255;
    image.pitch = 255;
    return image;
}

template <typename T>
void Put(u32* at, const T& sharp) {
    std::memcpy(at, &sharp, sizeof(T));
}

void PutPointer(u32* at, const void* pointer) {
    const u64 address = reinterpret_cast<u64>(pointer);
    std::memcpy(at, &address, sizeof(address));
}

/// A Gnm-style fetch shader for the vertex input semantics (semantic, vgpr, size): one V# per
/// semantic from the vertex buffer table, loaded with buffer_load_format by vertex index (v0).
const u32* FetchShader(const std::vector<u32>& sems, u32 table_sgpr) {
    std::vector<u32> code;
    for (size_t k = 0; k < sems.size(); ++k) {
        const u32 semantic = sems[k] & 0xFF, sgpr = 8 + 4 * u32(k);
        if (sgpr + 4 > 104) {
            Setup("more vertex inputs than the fetch shader has SGPRs");
        }
        // s_load_dwordx4 s[sgpr:sgpr+3], s[table:table+1], semantic * 4
        code.push_back(0xC0000000u | (2u << 22) | (sgpr << 15) | ((table_sgpr / 2) << 9) |
                       (1u << 8) | (semantic * 4));
    }
    code.push_back(0xBF8C0000u); // s_waitcnt 0
    for (size_t k = 0; k < sems.size(); ++k) {
        const u32 vgpr = (sems[k] >> 8) & 0xFF, size = (sems[k] >> 16) & 0xFF;
        if (size < 1 || size > 4) {
            Setup("vertex input semantic with " + std::to_string(size) + " components");
        }
        // buffer_load_format_{x,xy,xyz,xyzw} v[vgpr..], v0, s[sgpr:sgpr+3], 0 idxen
        code.push_back((0x38u << 26) | ((size - 1) << 18) | (1u << 13));
        code.push_back(0u | (vgpr << 8) | (((8 + 4 * u32(k)) / 4) << 16) | (0x80u << 24));
    }
    code.push_back(0xBF8C0000u);
    code.push_back(0xBE802000u); // s_setpc_b64 s[0:1]
    u32* out = arena.Alloc(code.size() * 4);
    std::memcpy(out, code.data(), code.size() * 4);
    return out;
}

constexpr u32 TableEntries = 128;

struct UserData {
    std::array<u32, 16>* ud{};
    u32* tess_constants{}; ///< V# memory of the tess constant buffer (slot 19), if any
};

/// User data from input usage slots (usage, api slot, start register, flags: bit 0 = 8 dwords).
UserData SynthesizeUserData(const std::vector<u32>& slots, const std::vector<u32>& sems) {
    UserData out;
    out.ud = new (arena.Alloc(sizeof(std::array<u32, 16>))) std::array<u32, 16>{};
    u32* ext = arena.Alloc(256 * 4);
    std::memset(ext, 0, 256 * 4);
    const auto reg = [&](u32 r) -> u32* { return r < 16 ? out.ud->data() + r : ext + (r - 16); };
    u32 vb_table_sgpr = ~0u;
    for (const u32 slot : slots) {
        if ((slot & 0xFF) == 0x17) {
            vb_table_sgpr = (slot >> 16) & 0xFF;
        }
    }
    for (const u32 slot : slots) {
        const u32 usage = slot & 0xFF, api = (slot >> 8) & 0xFF, start = (slot >> 16) & 0xFF;
        const bool eight = (slot >> 24) & 1;
        u32* at = reg(start);
        switch (usage) {
        case 0x00: // ImmResource
        case 0x04: // ImmRwResource
            eight ? Put(at, TSharp()) : Put(at, VSharp(4));
            break;
        case 0x01: // ImmSampler
            Put(at, AmdGpu::Sampler{});
            break;
        case 0x02: { // ImmConstBuffer
            if (api == 19) {
                // The tessellation constant buffer (TessellationDataConstantBuffer).
                out.tess_constants = arena.Alloc(256);
                Put(at, VSharp(4, reinterpret_cast<u64>(out.tess_constants)));
            } else {
                Put(at, VSharp(4));
            }
            break;
        }
        case 0x03: // ImmVertexBuffer
            Put(at, VSharp(4));
            break;
        case 0x05: // ImmAluFloatConst
        case 0x06: // ImmAluBool32Const
        case 0x0A: // ImmShaderResourceTable
        case 0x0D: // ImmLdsEsGsSize
            break;
        case 0x07: // ImmGdsCounterRange: M0 = size << 16 | base
        case 0x08: // ImmGdsMemoryRange
            *at = 0x00400000u;
            break;
        case 0x12: { // SubPtrFetchShader
            if (vb_table_sgpr == ~0u) {
                Setup("fetch shader without a vertex buffer table slot");
            }
            PutPointer(at, FetchShader(sems, vb_table_sgpr));
            break;
        }
        case 0x13: // PtrResourceTable
        case 0x14: // PtrInternalResourceTable
        case 0x19: // PtrRwResourceTable
        case 0x1C: // PtrIndirectResourceTable
        case 0x1D: // PtrIndirectInternalResourceTable
        case 0x1E: { // PtrIndirectRwResourceTable
            u32* table = arena.Alloc(TableEntries * 32);
            for (u32 i = 0; i < TableEntries; ++i) {
                Put(table + i * 8, TSharp());
            }
            PutPointer(at, table);
            break;
        }
        case 0x15: { // PtrSamplerTable
            u32* table = arena.Alloc(TableEntries * 16);
            std::memset(table, 0, TableEntries * 16);
            PutPointer(at, table);
            break;
        }
        case 0x18: { // PtrSoBufferTable: stream-out V#s address their vertex by add_tid
            u32* table = arena.Alloc(TableEntries * 16);
            for (u32 i = 0; i < TableEntries; ++i) {
                // A 64-byte vertex holds any copy shader's stores (SOPos: xyz at 0 and 12).
                auto sharp = VSharp(4);
                sharp.stride = SoVertexBytes;
                sharp.add_tid_enable = 1;
                Put(table + i * 4, sharp);
            }
            PutPointer(at, table);
            break;
        }
        case 0x16: // PtrConstBufferTable
        case 0x1A: { // PtrInternalGlobalTable (rings)
            u32* table = arena.Alloc(TableEntries * 16);
            for (u32 i = 0; i < TableEntries; ++i) {
                Put(table + i * 4, VSharp(4));
            }
            PutPointer(at, table);
            break;
        }
        case 0x17: { // PtrVertexBufferTable: one V# per semantic index
            u32* table = arena.Alloc(64 * 16);
            for (u32 i = 0; i < 64; ++i) {
                Put(table + i * 4, VSharp(4));
            }
            for (const u32 sem : sems) {
                Put(table + (sem & 0xFF) * 4, VSharp((sem >> 16) & 0xFF));
            }
            PutPointer(at, table);
            break;
        }
        case 0x1B: // PtrExtendedUserData
            PutPointer(at, ext);
            break;
        default: {
            char text[64];
            std::snprintf(text, sizeof(text), "input usage slot %#010x not synthesized", slot);
            Setup(text);
        }
        }
    }
    return out;
}

void SetProgram(AmdGpu::ShaderProgram& program, std::span<const u32> code, u32 rsrc1, u32 rsrc2) {
    program.address = reinterpret_cast<u64>(code.data()) >> 8;
    const u32 settings[2] = {rsrc1, rsrc2};
    std::memcpy(&program.settings, settings, sizeof(settings));
}

template <typename T>
void SetRaw(T& reg, u32 value) {
    static_assert(sizeof(T) == sizeof(u32));
    std::memcpy(&reg, &value, sizeof(value));
}

/// Representative render target per export format (PsColorBuffer for the graphics key).
Shader::PsColorBuffer ColorBuffer(AmdGpu::ShaderExportFormat format) {
    using DF = AmdGpu::DataFormat;
    using NF = AmdGpu::NumberFormat;
    Shader::PsColorBuffer cb{};
    cb.export_format = format;
    cb.swizzle = AmdGpu::Image::Null(false).DstSelect();
    const auto set = [&](DF df, NF nf) {
        cb.data_format = df;
        cb.num_format = nf;
    };
    switch (format) {
    case AmdGpu::ShaderExportFormat::Zero:
        return Shader::PsColorBuffer{};
    case AmdGpu::ShaderExportFormat::R_32:
        set(DF::Format32, NF::Float);
        break;
    case AmdGpu::ShaderExportFormat::GR_32:
    case AmdGpu::ShaderExportFormat::AR_32:
        set(DF::Format32_32, NF::Float);
        break;
    case AmdGpu::ShaderExportFormat::ABGR_FP16:
        set(DF::Format16_16_16_16, NF::Float);
        break;
    case AmdGpu::ShaderExportFormat::ABGR_UNORM16:
        set(DF::Format16_16_16_16, NF::Unorm);
        break;
    case AmdGpu::ShaderExportFormat::ABGR_SNORM16:
        set(DF::Format16_16_16_16, NF::Snorm);
        break;
    case AmdGpu::ShaderExportFormat::ABGR_UINT16:
        set(DF::Format16_16_16_16, NF::Uint);
        break;
    case AmdGpu::ShaderExportFormat::ABGR_SINT16:
        set(DF::Format16_16_16_16, NF::Sint);
        break;
    case AmdGpu::ShaderExportFormat::ABGR_32:
        set(DF::Format32_32_32_32, NF::Float);
        break;
    default:
        Setup("export format " + std::to_string(u32(format)));
    }
    return cb;
}

/// GS input topology from the vertex offset VGPRs its ESGS ring loads use (v0; v0 v1; v0 v1 v3).
AmdGpu::PrimitiveType GsInputTopology(std::span<const u32> code) {
    Shader::Gcn::GcnCodeSlice slice(code.data(), code.data() + code.size());
    Shader::Gcn::GcnDecodeContext decoder;
    u32 vaddrs = 0;
    while (!slice.atEnd()) {
        const auto inst = decoder.decodeInstruction(slice);
        using Shader::Gcn::Opcode;
        const bool ring_load = inst.opcode >= Opcode::BUFFER_LOAD_DWORD &&
                               inst.opcode <= Opcode::BUFFER_LOAD_DWORDX4;
        if (ring_load && inst.src[0].field == Shader::Gcn::OperandField::VectorGPR &&
            inst.src[0].code < 8) {
            vaddrs |= 1u << inst.src[0].code;
        }
        if (inst.opcode == Shader::Gcn::Opcode::S_ENDPGM) {
            break;
        }
    }
    switch (vaddrs) {
    case 0b1:
        return AmdGpu::PrimitiveType::PointList;
    case 0b11:
        return AmdGpu::PrimitiveType::LineList;
    case 0b1011:
        return AmdGpu::PrimitiveType::TriangleList;
    default:
        Setup("GS ring loads use vertex offset VGPR mask " + std::to_string(vaddrs));
    }
}

struct Sweep {
    Vulkan::Instance& instance;
    AmdGpu::Liverpool& liverpool;
    Vulkan::PipelineCache& cache;
    Shader::Info* hull_info{}; ///< a compiled HS, the hull an LS reads its tess constants through
    u32* hull_tess_constants{};

    AmdGpu::Regs& Regs() {
        return liverpool.regs;
    }

    void Reset() {
        auto& regs = Regs();
        regs.stage_enable.raw = AmdGpu::ShaderStageEnable::VgtStages::Vs;
        regs.primitive_type = AmdGpu::PrimitiveType::TriangleList;
        regs.clipper_control.clip_space = AmdGpu::ClipSpace::ZeroToW;
        SetRaw(regs.vs_output_control, 0);
        regs.vgt_gs_mode.mode = AmdGpu::GsScenario::Off;
        // Tessellation: the corpus' HS all declare VGT_TF_PARAM 0x42 or 0x49; DS jobs use the
        // job's own value (from its HS when paired) and default to triangles.
        SetRaw(regs.tess_config, 0x49);
    }

    Vulkan::PipelineCache::Result Program(Vulkan::PipelineSelection& sel, HwStage hw, SwStage sw,
                                          std::span<const u32> code, const UserData& ud,
                                          u64 hash) {
        Shader::Backend::Bindings binding{};
        const Shader::ShaderParams params{.user_data = std::span<const u32, 16>(*ud.ud),
                                          .code = code,
                                          .hash = hash};
        return cache.GetProgram(sel, hw, sw, params, binding);
    }

    /// TessellationDataConstantBuffer as the Gnm runtime derives it from the hull constants
    /// (see frontend/tessellation.h), for one patch per threadgroup. cp_stride 0 is a passthrough
    /// hull: its outputs are its inputs.
    static void FillTessConstants(u32* at, u32 ls_stride, u32 input_cp, u32 output_cp,
                                  u32 cp_stride) {
        if (!at) {
            return;
        }
        Shader::TessellationDataConstantBuffer c{};
        c.ls_stride = ls_stride;
        c.hs_cp_stride = cp_stride != 0 ? cp_stride : ls_stride;
        c.num_patches = 1;
        c.hs_output_base = cp_stride != 0 ? input_cp * ls_stride * c.num_patches : 0;
        c.patch_output_size = c.hs_cp_stride * output_cp;
        c.patch_const_size = 16 * 6;
        c.patch_const_base = c.hs_output_base + c.patch_output_size;
        c.off_chip_tessellation_factor_threshold = 1.0f;
        c.first_edge_tess_factor_index = 0;
        std::memcpy(at, &c, sizeof(c));
    }

    void Run(const Job& job) {
        Reset();
        auto& regs = Regs();
        Vulkan::PipelineSelection sel{};
        sel.regs = &regs;
        const auto code = LoadCode(job.code_path, job.dwords);
        const auto ud = SynthesizeUserData(job.slots, job.sems);
        const u32 rsrc1 = job.Get("rsrc1"), rsrc2 = job.Get("rsrc2");
        const auto& k = job.kind;
        if (k == "cs") {
            auto& cs = liverpool.GetCsRegs();
            cs = {};
            cs.address = reinterpret_cast<u64>(code.data()) >> 8;
            const u32 settings[2] = {rsrc1, rsrc2};
            std::memcpy(&cs.settings, settings, sizeof(settings));
            cs.num_thread_x.full = u16(job.Get("threads_x"));
            cs.num_thread_y.full = u16(job.Get("threads_y"));
            cs.num_thread_z.full = u16(job.Get("threads_z"));
            Program(sel, HwStage::Compute, SwStage::Compute, code, ud, job.hash);
        } else if (k == "vs" || k == "ds") {
            SetProgram(regs.vs_program, code, rsrc1, rsrc2);
            SetRaw(regs.vs_output_control, job.Get("vs_out_cntl"));
            if (k == "ds") {
                regs.stage_enable.raw = AmdGpu::ShaderStageEnable::VgtStages::LsHs;
                FillTessConstants(ud.tess_constants, 64, 3, 3, 64);
                Program(sel, HwStage::Vertex, SwStage::TessellationEval, code, ud, job.hash);
            } else {
                Program(sel, HwStage::Vertex, SwStage::Vertex, code, ud, job.hash);
            }
        } else if (k == "ps") {
            SetProgram(regs.ps_program, code, rsrc1, rsrc2);
            SetRaw(regs.ps_input_ena, job.Get("input_ena"));
            SetRaw(regs.ps_input_addr, job.Get("input_addr"));
            regs.num_interp = job.Get("in_control") & 0x3F;
            // SPI_PS_INPUT_CNTL as Gnmx fills it from the pixel input semantics (bit 10: flat).
            for (u32 i = 0; i < regs.num_interp; ++i) {
                regs.ps_inputs[i] = {};
                regs.ps_inputs[i].input_offset = i;
                regs.ps_inputs[i].flat_shade = i < job.sems.size() && (job.sems[i] >> 10) & 1;
            }
            SetRaw(regs.barycentric_control, job.Get("baryc_cntl"));
            regs.z_export_format = AmdGpu::ShaderExportFormat(job.Get("z_format"));
            regs.color_export_format.raw = job.Get("col_format");
            SetRaw(regs.depth_shader_control, job.Get("db_shader_control"));
            for (u32 i = 0; i < AmdGpu::NUM_COLOR_BUFFERS; ++i) {
                sel.graphics_key.color_buffers[i] =
                    ColorBuffer(regs.color_export_format.GetFormat(i));
            }
            Program(sel, HwStage::Fragment, SwStage::Fragment, code, ud, job.hash);
        } else if (k == "es") {
            SetProgram(regs.es_program, code, rsrc1, rsrc2);
            regs.stage_enable.raw = AmdGpu::ShaderStageEnable::VgtStages::EsGs;
            regs.vgt_esgs_ring_itemsize = job.Get("esgs_dwords");
            Program(sel, HwStage::Export, SwStage::Vertex, code, ud, job.hash);
        } else if (k == "gs") {
            RunGs(job, sel, code, ud, rsrc1, rsrc2);
        } else if (k == "hs") {
            SetProgram(regs.hs_program, code, rsrc1, rsrc2);
            regs.stage_enable.raw = AmdGpu::ShaderStageEnable::VgtStages::LsHs;
            SetRaw(regs.tess_config, job.Get("tf_param"));
            // Gnm hull constants: threads (output control points), input control points, -,
            // control point stride (0: passthrough), ...
            std::vector<u32> hull;
            std::istringstream fields(job.kv.at("hull"));
            for (std::string field; std::getline(fields, field, ',');) {
                hull.push_back(u32(std::strtoul(field.c_str(), nullptr, 16)));
            }
            if (hull.size() != 7) {
                Setup("hull constants with " + std::to_string(hull.size()) + " words");
            }
            const u32 threads = hull[0], input_cp = hull[1], cp_stride = hull[3];
            SetRaw(regs.ls_hs_config, (input_cp << 8) | (threads << 14) | 1);
            FillTessConstants(ud.tess_constants, 64, input_cp, threads, cp_stride);
            const auto program = Program(sel, HwStage::Hull, SwStage::TessellationControl, code, ud,
                                         job.hash);
            if (!hull_info && ud.tess_constants) {
                hull_info = const_cast<Shader::Info*>(std::get<0>(program));
                hull_tess_constants = ud.tess_constants;
                std::fprintf(stderr, "@@HULL %zu\n", job.index);
            }
        } else if (k == "ls") {
            if (!hull_info) {
                Setup("LS without a hull (pass HULL_JOB: an HS job with a tess constant buffer)");
            }
            SetProgram(regs.ls_program, code, rsrc1, rsrc2);
            regs.stage_enable.raw = AmdGpu::ShaderStageEnable::VgtStages::LsHs;
            FillTessConstants(hull_tess_constants, job.Get("ls_stride"), 3, 3, job.Get("ls_stride"));
            sel.infos[u32(SwStage::TessellationControl)] = hull_info;
            Program(sel, HwStage::Local, SwStage::Vertex, code, ud, job.hash);
        } else {
            Setup("job kind " + k);
        }
    }

    void RunGs(const Job& job, Vulkan::PipelineSelection& sel, std::span<const u32> code,
               const UserData& ud, u32 rsrc1, u32 rsrc2) {
        auto& regs = Regs();
        const auto copy = LoadCode(job.kv.at("copy_code"), job.Get("copy_dwords"));
        const auto copy_ud = SynthesizeUserData(job.copy_slots, {});
        SetProgram(regs.gs_program, code, rsrc1, rsrc2);
        SetProgram(regs.vs_program, copy, job.Get("copy_rsrc1"), job.Get("copy_rsrc2"));
        SetRaw(regs.vs_output_control, job.Get("copy_vs_out_cntl"));
        regs.stage_enable.raw = AmdGpu::ShaderStageEnable::VgtStages::EsGs;
        regs.vgt_gs_mode = {};
        regs.vgt_gs_mode.mode = AmdGpu::GsScenario::ScenarioG;
        regs.vgt_strmout_config.raw = job.Get("strmout_config");
        // Stream-out: the file does not carry VGT_STRMOUT_BUFFER_CONFIG or the strides; take the
        // buffers the copy shader has offsets for, and 4-dword vertices.
        regs.vgt_strmout_buffer_config = {};
        regs.vgt_strmout_buffer_config.stream_0_buf_en = regs.vs_program.settings.so_base_en;
        for (u32 buffer = 0; buffer < 4; ++buffer) {
            regs.vgt_strmout_buffers[buffer] = {.size_dw = 4096, .vtx_stride_dw = SoVertexBytes / 4};
        }
        // Rasterized GS points take PA_SU_POINT_SIZE: 1 px (half sizes in 12.4).
        regs.point_size.half_width = 8;
        regs.point_size.half_height = 8;
        regs.vgt_gs_out_prim_type.raw = job.Get("out_prim_type");
        regs.vgt_gs_instance_cnt.raw = job.Get("instance_cnt");
        regs.vgt_gs_max_vert_out = job.Get("max_vert_out");
        regs.vgt_gs_vert_itemsize[0] = job.Get("vert_itemsize0");
        regs.vgt_gs_vert_itemsize[1] = job.Get("vert_itemsize1");
        regs.vgt_gs_vert_itemsize[2] = job.Get("vert_itemsize2");
        regs.vgt_gs_vert_itemsize[3] = job.Get("vert_itemsize3");
        regs.vgt_esgs_ring_itemsize = job.Get("esgs");
        regs.primitive_type = GsInputTopology(code);
        std::fprintf(stderr, "@@GS %zu in_prim %u\n", job.index, u32(regs.primitive_type));
        // RefreshGraphicsStages: an uncovered configuration stops the draw before compiling.
        if (const char* gap = Vulkan::GsEmulationUnsupported(regs)) {
            std::fprintf(stderr, "@@GAP %zu %s\n", job.index, gap);
            return;
        }
        Program(sel, HwStage::Geometry, SwStage::Geometry, code, ud, job.hash);
        if (regs.vgt_strmout_config.raw) {
            // The recompiler's stream-out store proof assumes Gnm's ABI: the stream-out buffer
            // table (PtrSoBufferTable) at s[2:3].
            const bool table_at_s2 = std::ranges::any_of(job.copy_slots, [](u32 slot) {
                return (slot & 0xFF) == 0x18 && ((slot >> 16) & 0xFF) == 2;
            });
            if (!table_at_s2) {
                Setup("stream-out copy shader without PtrSoBufferTable at s[2:3]");
            }
            // The copy shader's stream-out kernel (CopyStreamOut), as RefreshGraphicsStages.
            sel.gs_stream_out_kernel = true;
            Program(sel, HwStage::Vertex, SwStage::Vertex, copy, copy_ud, job.hash);
            sel.gs_stream_out_kernel = false;
        }
        Program(sel, HwStage::Vertex, SwStage::Vertex, copy, copy_ud, job.hash);
    }
};

} // namespace

/// GCN disassembly with the production decoder (no host disassembler handles GFX7): one line
/// per instruction, dword offset, raw words, opcode, then destinations and sources as
/// field:code (s/v registers, constants, literals).
int Disassemble(const char* path, u32 dwords) {
    std::ifstream f(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
    if (bytes.size() < dwords * 4) {
        std::fprintf(stderr, "shader-sweep --disasm: %s is shorter than %u dwords\n", path, dwords);
        return 2;
    }
    std::vector<u32> code(dwords);
    std::memcpy(code.data(), bytes.data(), dwords * 4);
    Shader::Gcn::GcnCodeSlice slice(code.data(), code.data() + code.size());
    Shader::Gcn::GcnDecodeContext decoder;
    const auto operand = [](const Shader::Gcn::InstOperand& op) {
        using F = Shader::Gcn::OperandField;
        switch (op.field) {
        case F::ScalarGPR:
            return "s" + std::to_string(op.code);
        case F::VectorGPR:
            return "v" + std::to_string(op.code);
        case F::LiteralConst: {
            char text[16];
            std::snprintf(text, sizeof(text), "%#x", op.code);
            return std::string(text);
        }
        default:
            return std::string(magic_enum::enum_name(op.field)) + ":" + std::to_string(op.code);
        }
    };
    while (!slice.atEnd()) {
        const u32 at = u32(slice.position() - code.data());
        const auto inst = decoder.decodeInstruction(slice);
        std::string line;
        for (u32 i = 0; i < inst.dst_count; ++i) {
            line += (i ? ", " : " ") + operand(inst.dst[i]);
        }
        line += inst.dst_count ? " <-" : "";
        for (u32 i = 0; i < inst.src_count; ++i) {
            line += (i ? ", " : " ") + operand(inst.src[i]);
        }
        std::printf("%5u  %08x%s  %-28s%s\n", at, code[at],
                    inst.length > 4 ? (" " + [&] {
                        char t[16];
                        std::snprintf(t, sizeof(t), "%08x", code[at + 1]);
                        return std::string(t);
                    }()).c_str()
                                    : "         ",
                    std::string(magic_enum::enum_name(inst.opcode)).c_str(), line.c_str());
        if (inst.opcode == Shader::Gcn::Opcode::S_ENDPGM) {
            break;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--disasm") {
        return Disassemble(argv[2], u32(std::strtoul(argv[3], nullptr, 0)));
    }
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr, "usage: shader-sweep JOBS FIRST END [HULL_JOB]\n");
        return 2;
    }
    setvbuf(stderr, nullptr, _IONBF, 0);
    const auto jobs = ReadJobs(argv[1]);
    const size_t first = std::strtoul(argv[2], nullptr, 0);
    const size_t end = std::min<size_t>(std::strtoul(argv[3], nullptr, 0), jobs.size());
    arena.Init(size_t(4) << 30);
    Vulkan::Instance instance(0, false);
    Vulkan::Scheduler scheduler(instance);
    AmdGpu::Liverpool liverpool;
    Vulkan::PipelineCache cache(instance, scheduler, &liverpool, 14);
    const auto& profile = cache.GetProfile();
    std::fprintf(stderr, "@@PROFILE subgroup %u wave64 %d geometry %d spirv %#x\n",
                 profile.subgroup_size, profile.emulate_wave64, profile.emulate_geometry_stage,
                 profile.supported_spirv);
    Sweep sweep{instance, liverpool, cache};
    if (argc == 5) {
        // An LS reads its tess constants through a compiled hull: compile that HS first.
        const size_t hull = std::strtoul(argv[4], nullptr, 0);
        std::fprintf(stderr, "@@HULLJOB %zu\n", hull);
        sweep.Run(jobs.at(hull));
    }
    for (size_t i = first; i < end; ++i) {
        std::fprintf(stderr, "@@JOB %zu %#llx %s\n", i, (unsigned long long)jobs[i].hash,
                     jobs[i].kind.c_str());
        sweep.Run(jobs[i]);
        std::fprintf(stderr, "@@DONE %zu\n", i);
    }
    std::fprintf(stderr, "@@END\n");
    std::_Exit(0);
}
