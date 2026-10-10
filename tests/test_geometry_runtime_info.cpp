// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <vector>
#include "shader_recompiler/specialization.h"

int main() {
    Shader::Info info{};
    Shader::StageSpecialization base;
    base.info = &info;
    base.runtime_info.Initialize(Shader::HwStage::Geometry, Shader::SwStage::Geometry);
    auto& gs = base.runtime_info.hw.gs;
    gs.num_outputs = 1;
    gs.num_invocations = 1;
    gs.output_vertices = 3;
    gs.in_vertex_data_size = 16;
    gs.out_vertex_data_size = 32;
    gs.mode = AmdGpu::GsScenario::ScenarioA;
    assert(base == base);

    std::vector<Shader::StageSpecialization> permutations{base};
    auto check_variant = [&](auto change) {
        auto variant = base;
        change(variant.runtime_info.hw.gs);
        assert(!(base == variant) && !(variant == base));
        for (const auto& previous : permutations) {
            assert(!(previous == variant) && !(variant == previous));
        }
        permutations.push_back(variant);
        assert(permutations.back() == variant);
    };
    check_variant([](auto& value) { value.num_invocations = 2; });
    check_variant([](auto& value) { value.in_vertex_data_size = 32; });
    check_variant([](auto& value) { value.out_vertex_data_size = 64; });
    check_variant([](auto& value) { value.mode = AmdGpu::GsScenario::ScenarioB; });

    auto zero = base;
    zero.runtime_info.hw.gs.num_invocations = 0;
    assert(zero == zero);
    assert(!(zero == base) && !(base == zero));

    const std::array<u32, 2> first_copy{1, 2}, second_copy{1, 2};
    auto copy = base;
    base.runtime_info.hw.gs.vs_copy = first_copy;
    copy.runtime_info.hw.gs.vs_copy = second_copy;
    assert(base == copy);
    copy.runtime_info.hw.gs.vs_copy_hash = 1;
    assert(!(base == copy));
    std::puts("Geometry runtime specialization: instance count, ring strides, mode, zero count and copy identity PASS");
}
