// SPDX-License-Identifier: GPL-2.0-or-later
// Shader corpus sweep, MSL stage: converts SPIR-V modules to MSL with the SPIRV-Cross revision
// MoltenVK 1.4.2 vendors and the options MoltenVK sets for each stage on an Apple GPU under
// macOS 26+ (MVKPipeline.mm, MVKDevice.mm at the v1.4.2 tag): MSL 4.0, Metal argument buffers
// (tier 2, padded), native texture buffers and swizzle, the AGX cube-gradient fixup, 32-wide
// fixed subgroups, and the stage setup MoltenVK uses for the LS (vertex feeding tessellation),
// HS (a compute kernel writing patches) and DS (tessellation evaluation). Resource bindings carry
// base types as MoltenVK's pipeline layout supplies them; the Metal indices are only numbering.
//
//   shader-msl [--emit DIR] < paths
//       one SPIR-V path per line; prints "OK <path>" or "ERR <path>\t<message>"; --emit also
//       writes the MSL of each converted module as DIR/<name>.metal
// The stage comes from the BB_DUMP_SHADERS name prefix (vs_, fs_, cs_, gs_, es_, hs_, ls_).
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "spirv_msl.hpp"

namespace {

std::vector<uint32_t> Read(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), words.size() * 4);
    return words;
}

/// What MoltenVK's pipeline layout supplies (MVKPipeline.mm addResourceBindingToShaderConfig):
/// each descriptor's base type (buffers Void, images Image, samplers Sampler; a combined image
/// sampler both) and Metal indices, numbered per kind in binding order.
void AddResourceBindings(spirv_cross::CompilerMSL& msl, spv::ExecutionModel model) {
    using spirv_cross::SPIRType;
    const auto resources = msl.get_shader_resources();
    uint32_t buffers = 0, textures = 0, samplers = 0;
    const auto add = [&](const spirv_cross::Resource& r, std::initializer_list<SPIRType::BaseType> types) {
        const auto& type = msl.get_type(r.type_id);
        uint32_t count = 1;
        for (const uint32_t size : type.array) {
            count *= size == 0 ? 1 : size;
        }
        for (const auto basetype : types) {
            spirv_cross::MSLResourceBinding b;
            b.stage = model;
            b.basetype = basetype;
            b.desc_set = msl.get_decoration(r.id, spv::DecorationDescriptorSet);
            b.binding = msl.get_decoration(r.id, spv::DecorationBinding);
            b.count = count;
            b.msl_buffer = buffers;
            b.msl_texture = textures;
            b.msl_sampler = samplers;
            msl.add_msl_resource_binding(b);
            (basetype == SPIRType::Image ? textures : basetype == SPIRType::Sampler ? samplers
                                                                                     : buffers) += count;
        }
    };
    for (const auto& r : resources.uniform_buffers) add(r, {SPIRType::Void});
    for (const auto& r : resources.storage_buffers) add(r, {SPIRType::Void});
    for (const auto& r : resources.sampled_images) add(r, {SPIRType::Image, SPIRType::Sampler});
    for (const auto& r : resources.separate_images) add(r, {SPIRType::Image});
    for (const auto& r : resources.storage_images) add(r, {SPIRType::Image});
    for (const auto& r : resources.separate_samplers) add(r, {SPIRType::Sampler});
}

std::string Convert(const std::string& path) {
    const auto slash = path.find_last_of('/');
    const std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    const std::string prefix = name.substr(0, 3);
    spirv_cross::CompilerMSL msl(Read(path));
    auto o = msl.get_msl_options();
    using Options = spirv_cross::CompilerMSL::Options;
    o.platform = Options::macOS;
    o.msl_version = Options::make_msl_version(4, 0);
    o.texel_buffer_texture_width = 16384;
    o.r32ui_linear_texture_alignment = 4;
    o.swizzle_texture_samples = false;
    o.texture_buffer_native = true;
    o.argument_buffers = true;
    o.force_active_argument_buffer_resources = false;
    o.pad_argument_buffer_resources = true;
    o.argument_buffers_tier = Options::ArgumentBuffersTier::Tier2;
    o.agx_manual_cube_grad_fixup = true;
    o.replace_recursive_inputs = true;
    o.multi_patch_workgroup = true;
    o.ios_support_base_vertex_instance = true;
    o.emulate_subgroups = false;
    o.ios_use_simdgroup_functions = true;
    const auto model = msl.get_execution_model();
    if (model == spv::ExecutionModelGLCompute) {
        o.fixed_subgroup_size = 32;
    } else if (prefix == "ls_") {
        // A vertex shader feeding tessellation runs as a kernel that captures its outputs.
        o.capture_output_to_buffer = true;
        o.vertex_for_tessellation = true;
        o.disable_rasterization = true;
    } else if (model == spv::ExecutionModelTessellationControl) {
        o.capture_output_to_buffer = true;
        o.multi_patch_workgroup = true;
        o.fixed_subgroup_size = 32;
    } else if (model == spv::ExecutionModelTessellationEvaluation) {
        o.raw_buffer_tese_input = true;
        o.capture_output_to_buffer = false;
    } else if (model == spv::ExecutionModelFragment) {
        o.fixed_subgroup_size = 32;
        o.check_discarded_frag_stores = true;
        o.enable_frag_depth_builtin = true;
        o.enable_frag_stencil_ref_builtin = true;
    } else if (model == spv::ExecutionModelVertex) {
        o.enable_point_size_builtin = false;
    }
    msl.set_msl_options(o);
    AddResourceBindings(msl, model);
    return msl.compile();
}

} // namespace

int main(int argc, char** argv) {
    // --emit DIR: also write each module's MSL as DIR/<module name>.metal (the Metal compile stage).
    const std::string emit = argc == 3 && std::string(argv[1]) == "--emit" ? argv[2] : "";
    for (std::string path; std::getline(std::cin, path);) {
        try {
            const std::string source = Convert(path);
            if (!emit.empty()) {
                const auto slash = path.find_last_of('/');
                std::ofstream(emit + "/" + path.substr(slash + 1) + ".metal") << source;
            }
            std::printf("OK %s\n", path.c_str());
        } catch (const std::exception& e) {
            std::string message = e.what();
            for (auto& c : message) {
                c = c == '\n' || c == '\t' ? ' ' : c;
            }
            std::printf("ERR %s\t%s\n", path.c_str(), message.c_str());
        }
        std::fflush(stdout);
    }
    return 0;
}
