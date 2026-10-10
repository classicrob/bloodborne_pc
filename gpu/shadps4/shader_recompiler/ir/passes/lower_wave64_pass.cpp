// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/classes.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Shader::Optimization {

static bool IsDivergentCondition(const IR::U1& condition) {
    if (condition.IsImmediate()) {
        return false;
    }
    const IR::Inst* const condition_inst = condition.Inst();
    return IR::BreadthFirstSearch(condition_inst,
                                  [](const IR::Inst* inst) -> std::optional<bool> {
                                      switch (inst->GetOpcode()) {
                                      case IR::Opcode::LaneId:
                                          return true;
                                      case IR::Opcode::GetAttributeU32:
                                          if (inst->Arg(0).Attribute() ==
                                              IR::Attribute::LocalInvocationId) {
                                              return true;
                                          }
                                          break;
                                      default:
                                          break;
                                      }
                                      return std::nullopt;
                                  })
        .value_or(false);
}

static std::vector<IR::Block*> FindUniformBlocks(const IR::Program& program) {
    using Type = IR::AbstractSyntaxNode::Type;

    struct ConditionalScope {
        const IR::Block* merge;
        bool divergent;
    };

    std::vector<IR::Block*> blocks;
    std::vector<ConditionalScope> conditionals;
    std::vector<const IR::Block*> loops;
    u32 divergence_depth{};
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        switch (node.type) {
        case Type::If: {
            const bool divergent = IsDivergentCondition(node.data.if_node.cond);
            conditionals.push_back({node.data.if_node.merge, divergent});
            divergence_depth += static_cast<u32>(divergent);
            break;
        }
        case Type::EndIf:
            ASSERT(!conditionals.empty() && conditionals.back().merge == node.data.end_if.merge);
            divergence_depth -= static_cast<u32>(conditionals.back().divergent);
            conditionals.pop_back();
            break;
        case Type::Loop:
            loops.push_back(node.data.loop.merge);
            break;
        case Type::Repeat:
            if (loops.empty() || loops.back() != node.data.repeat.merge) {
                return {};
            }
            loops.pop_back();
            break;
        case Type::Block:
            if (divergence_depth == 0 && loops.empty()) {
                blocks.push_back(node.data.block);
            }
            break;
        default:
            break;
        }
    }
    if (!conditionals.empty() || !loops.empty()) {
        return {};
    }
    return blocks;
}

// ---------------------------------------------------------------------------------------------
// Apple (Profile::emulate_wave64): exact GCN wave64 semantics on 32-wide subgroups.
//
// A GCN wave is 64 consecutive invocations of the workgroup (lane = LocalInvocationIndex & 63).
// Compute: every wave-level operation is rebuilt from LocalInvocationIndex and workgroup shared
// memory, with no subgroup operations, so the result does not depend on how the driver groups
// invocations into SIMD groups:
//  - per-lane (valid anywhere): LaneId, InverseBallot, BallotFindLsb, WriteLane, SubgroupLtMask;
//  - cross-lane (Ballot, GroupAny, ReadLane, ReadFirstLane, Shuffle, ShuffleXor, QuadBroadcast):
//    shared-memory exchange between workgroup barriers, using only 32-bit shared accesses. These
//    need every invocation of the workgroup to reach the barrier, so they are lowered only in
//    workgroup-uniform control flow; elsewhere compilation stops with a message (correct or loud).
// Graphics: a 32-lane subgroup is a legal partially populated wave (lanes 32-63 inactive), so
// subgroup semantics are kept, except reading a lane that may be >= 32, which stops compilation.
namespace {

// Workgroup uniformity as a fixed point: every value starts uniform, divergence enters at sources
// (per-invocation attributes, memory, lane data, and wave-level results when the workgroup holds
// several waves) and is propagated to users until nothing changes. No result is cached before the
// propagation is complete, so loop-carried phis are handled exactly.
class WorkgroupUniformity {
public:
    WorkgroupUniformity(const IR::Program& program, bool wave_values_uniform)
        : wave_uniform{wave_values_uniform} {
        std::vector<const IR::Inst*> worklist;
        for (const IR::Block* block : program.blocks) {
            for (const IR::Inst& inst : block->Instructions()) {
                if (IsDivergentSource(inst)) {
                    Mark(&inst, worklist);
                }
            }
        }
        Propagate(worklist);
        // Control dependence: a phi merging different values after a divergent branch, and
        // anything defined in (or carried out of) a loop with a divergent exit, differs between
        // invocations even when every input is uniform. Iterate with data propagation until stable.
        using Type = IR::AbstractSyntaxNode::Type;
        const auto& list = program.syntax_list;
        bool changed = true;
        while (changed) {
            changed = false;
            const size_t before = divergent.size();
            for (size_t n = 0; n < list.size(); ++n) {
                const auto& node = list[n];
                if (node.type == Type::If && !IsUniform(node.data.if_node.cond)) {
                    MarkMergePhis(node.data.if_node.merge, worklist);
                } else if (node.type == Type::Loop) {
                    // the loop body runs from this node to its Repeat. A break is divergent when its
                    // condition is, or when it sits under a divergent if inside the loop (the
                    // structurizer can move a break into an if: EliminateAsConditional,
                    // MoveOutwardIf).
                    bool exit_divergent = false;
                    std::vector<const IR::Block*> body;
                    std::vector<bool> ifs; // uniformity of the ifs open inside the body
                    size_t depth = 0, m = n + 1;
                    for (; m < list.size(); ++m) {
                        const auto& inner = list[m];
                        if (inner.type == Type::Loop) ++depth;
                        if (inner.type == Type::Block) body.push_back(inner.data.block);
                        if (inner.type == Type::If) ifs.push_back(IsUniform(inner.data.if_node.cond));
                        if (inner.type == Type::EndIf && !ifs.empty()) ifs.pop_back();
                        if (inner.type == Type::Break &&
                            (!IsUniform(inner.data.break_node.cond) ||
                             !std::ranges::all_of(ifs, std::identity{}))) {
                            exit_divergent = true;
                        }
                        if (inner.type == Type::Repeat) {
                            if (depth == 0) {
                                exit_divergent |= !IsUniform(inner.data.repeat.cond);
                                break;
                            }
                            --depth;
                        }
                    }
                    if (exit_divergent) {
                        for (const IR::Block* block : body) {
                            for (const IR::Inst& inst : block->Instructions()) Mark(&inst, worklist);
                        }
                        MarkMergePhis(node.data.loop.merge, worklist);
                    }
                }
            }
            Propagate(worklist);
            changed = divergent.size() != before;
        }
    }

    bool IsUniform(const IR::Value& value) const {
        const IR::Inst* inst = value.TryInst();
        return !inst || !divergent.contains(inst);
    }

private:
    bool IsDivergentSource(const IR::Inst& inst) const {
        using IR::Opcode;
        switch (inst.GetOpcode()) {
        case Opcode::GetUserData:
        case Opcode::ReadConst:
        case Opcode::ReadConstBuffer:
            return false; // uniform unless an argument is divergent (propagated)
        case Opcode::GetAttributeU32:
            return inst.Arg(0).Attribute() != IR::Attribute::WorkgroupId &&
                   inst.Arg(0).Attribute() != IR::Attribute::WorkgroupIndex;
        case Opcode::Ballot:
        case Opcode::GroupAny:
        case Opcode::ReadFirstLane:
        case Opcode::ReadLane:
            return !wave_uniform; // one value per wave; workgroup-uniform only for one wave
        default:
            break;
        }
        const std::string_view name = IR::NameOf(inst.GetOpcode());
        if (name.starts_with("Undef")) {
            // SSA construction's value for a register on a path that does not define it (e.g.
            // through the structurizer's goto variables). The guest never reads it there: no
            // hardware-defined register is Undef, so a read would be of an uninitialised register.
            return false;
        }
        if (inst.NumArgs() == 0) {
            return inst.GetOpcode() != Opcode::Phi; // a source we do not know to be uniform
        }
        return name.find("Load") != std::string_view::npos || name.find("Atomic") != std::string_view::npos ||
               name.find("Image") != std::string_view::npos || name.find("Attribute") != std::string_view::npos ||
               name.find("Lane") != std::string_view::npos || name.find("Shuffle") != std::string_view::npos ||
               name.find("Quad") != std::string_view::npos || name.find("InverseBallot") != std::string_view::npos ||
               name.find("Exec") != std::string_view::npos || name.find("Data") != std::string_view::npos;
    }

    // Whether a divergent operand makes `user` divergent.
    bool PropagatesFrom(const IR::Inst& user, u32 operand) const {
        using IR::Opcode;
        switch (user.GetOpcode()) {
        case Opcode::Ballot:
        case Opcode::GroupAny:
        case Opcode::ReadFirstLane:
            return false; // the same for every lane of the wave whatever the operand
        case Opcode::ReadLane:
            return operand == 1; // a divergent lane select makes the read divergent
        default:
            return true;
        }
    }

    void Mark(const IR::Inst* inst, std::vector<const IR::Inst*>& worklist) {
        if (divergent.insert(inst).second) worklist.push_back(inst);
    }

    void MarkMergePhis(const IR::Block* merge, std::vector<const IR::Inst*>& worklist) {
        for (const IR::Inst& inst : merge->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::Phi) continue;
            bool same = true;
            for (size_t i = 1; i < inst.NumArgs(); ++i) same &= inst.Arg(i) == inst.Arg(0);
            if (!same) Mark(&inst, worklist);
        }
    }

    void Propagate(std::vector<const IR::Inst*>& worklist) {
        while (!worklist.empty()) {
            const IR::Inst* inst = worklist.back();
            worklist.pop_back();
            for (const IR::Use& use : inst->Uses()) {
                if (!divergent.contains(use.user) && PropagatesFrom(*use.user, use.operand)) {
                    Mark(use.user, worklist);
                }
            }
        }
    }

    bool wave_uniform;
    std::unordered_set<const IR::Inst*> divergent;
};

// Blocks where every invocation of the workgroup arrives together: all enclosing conditions and
// loop exits are workgroup-uniform, and no invocation has returned early.
std::vector<IR::Block*> FindWorkgroupUniformBlocks(const IR::Program& program,
                                                   const WorkgroupUniformity& uniformity) {
    using Type = IR::AbstractSyntaxNode::Type;
    const auto& list = program.syntax_list;
    // A loop is uniform when its continue condition and every break inside it are. A break under
    // a divergent if is divergent whatever its own condition (conservative for ifs outside the
    // loop: their blocks are not uniform anyway).
    std::unordered_map<const IR::Block*, bool> loop_uniform;
    {
        std::vector<std::pair<const IR::Block*, bool>> open;
        std::vector<bool> ifs;
        for (const auto& node : list) {
            if (node.type == Type::If) {
                ifs.push_back(uniformity.IsUniform(node.data.if_node.cond));
            } else if (node.type == Type::EndIf && !ifs.empty()) {
                ifs.pop_back();
            } else if (node.type == Type::Loop) {
                open.push_back({node.data.loop.merge, true});
            } else if (node.type == Type::Break && !open.empty()) {
                const bool uniform_break = uniformity.IsUniform(node.data.break_node.cond) &&
                                           std::ranges::all_of(ifs, std::identity{});
                for (auto& [merge, uniform] : open) {
                    uniform = uniform && uniform_break;
                }
            } else if (node.type == Type::Repeat && !open.empty()) {
                auto [merge, uniform] = open.back();
                open.pop_back();
                loop_uniform[merge] = uniform && uniformity.IsUniform(node.data.repeat.cond);
            }
        }
    }
    std::vector<IR::Block*> blocks;
    std::vector<bool> scopes; // uniformity of each enclosing if/loop
    bool divergent_exit = false;
    auto all_uniform = [&] { return !divergent_exit && std::ranges::all_of(scopes, std::identity{}); };
    for (const auto& node : list) {
        switch (node.type) {
        case Type::If:
            scopes.push_back(uniformity.IsUniform(node.data.if_node.cond));
            break;
        case Type::Loop:
            scopes.push_back(loop_uniform[node.data.loop.merge]);
            break;
        case Type::EndIf:
        case Type::Repeat:
            if (!scopes.empty()) scopes.pop_back();
            break;
        case Type::Return:
        case Type::Unreachable:
            divergent_exit |= !all_uniform();
            break;
        case Type::Block:
            if (all_uniform()) blocks.push_back(node.data.block);
            break;
        default:
            break;
        }
    }
    return blocks;
}

bool IsCrossLane(IR::Opcode op) {
    using IR::Opcode;
    return op == Opcode::Ballot || op == Opcode::GroupAny || op == Opcode::ReadLane ||
           op == Opcode::ReadFirstLane || op == Opcode::Shuffle || op == Opcode::ShuffleXor ||
           op == Opcode::QuadBroadcast;
}

void LowerWave64Graphics(IR::Program& program) {
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::ReadLane) continue;
            const IR::Value lane = inst.Arg(1);
            ASSERT_MSG(lane.IsImmediate() && lane.U32() < 32,
                       "Apple wave64: v_readlane of a lane that may be >= 32 in a {} shader "
                       "(32-wide subgroup): shader={:#x} lane={}",
                       program.info.hw_stage, program.info.pgm_hash,
                       lane.IsImmediate() ? std::to_string(lane.U32()) : std::string("dynamic"));
        }
    }
}

void LowerWave64Compute(IR::Program& program, const RuntimeInfo& runtime_info, const Profile& profile) {
    auto& info = program.info;
    const auto [size_x, size_y, size_z] = runtime_info.hw.cs.workgroup_size;
    const u32 num_threads = size_x * size_y * size_z;
    const u32 num_waves = (num_threads + 63) / 64;
    const WorkgroupUniformity uniformity{program, num_waves <= 1};
    const auto uniform_blocks = FindWorkgroupUniformBlocks(program, uniformity);

    std::vector<IR::Inst*> cross_lane, per_lane;
    bool needs_lane_values = false;
    for (IR::Block* block : program.blocks) {
        const bool block_uniform = std::ranges::contains(uniform_blocks, block);
        for (IR::Inst& inst : block->Instructions()) {
            const auto op = inst.GetOpcode();
            if (IsCrossLane(op)) {
                ASSERT_MSG(block_uniform,
                           "Apple wave64: {} in workgroup-divergent control flow cannot be "
                           "emulated on 32-wide subgroups: shader={:#x} workgroup={}x{}x{}",
                           op, info.pgm_hash, size_x, size_y, size_z);
                cross_lane.push_back(&inst);
                needs_lane_values |= op == IR::Opcode::Shuffle || op == IR::Opcode::ShuffleXor ||
                                     op == IR::Opcode::QuadBroadcast;
            } else if (op == IR::Opcode::LaneId || op == IR::Opcode::InverseBallot ||
                       op == IR::Opcode::BallotFindLsb || op == IR::Opcode::WriteLane ||
                       (op == IR::Opcode::GetAttributeU32 &&
                        inst.Arg(0).Attribute() == IR::Attribute::SubgroupLtMask)) {
                per_lane.push_back(&inst);
            } else if (op == IR::Opcode::WarpId) {
                UNREACHABLE_MSG("Apple wave64: WarpId is not emulated: shader={:#x}", info.pgm_hash);
            }
        }
    }
    if (cross_lane.empty() && per_lane.empty()) {
        return;
    }

    // Scratch after the guest's LDS: per wave two mask words and one value word; one word per
    // invocation for shuffles. 32-bit accesses only (valid SPIR-V without explicit layouts).
    const u32 base = Common::AlignUp(runtime_info.hw.cs.shared_memory_size, sizeof(u32));
    const u32 mask_base = base;
    const u32 value_base = mask_base + num_waves * 2 * 4;
    const u32 lane_base = value_base + num_waves * 4;
    const u32 end = lane_base + (needs_lane_values ? num_waves * 64 * 4 : 0);
    if (!cross_lane.empty()) {
        // The scratch is accessed as 32-bit words. Guest LDS of another single type would make a
        // second workgroup type, which needs VK_KHR_workgroup_memory_explicit_layout.
        bool guest_non_u32 = false;
        for (const IR::Block* block : program.blocks) {
            for (const IR::Inst& inst : block->Instructions()) {
                const std::string_view name = IR::NameOf(inst.GetOpcode());
                const bool shared = name.find("Shared") != std::string_view::npos;
                guest_non_u32 |= shared && (name.ends_with("U16") || name.ends_with("U64") || name.ends_with("64"));
            }
        }
        ASSERT_MSG(!guest_non_u32 || profile.supports_workgroup_explicit_memory_layout,
                   "Apple wave64: guest LDS with 16/64-bit accesses plus 32-bit wave scratch needs "
                   "workgroup explicit layout: shader={:#x}",
                   info.pgm_hash);
        ASSERT_MSG(end <= profile.max_shared_memory_size,
                   "Apple wave64: guest LDS ({} bytes) plus wave scratch exceeds shared memory ({} "
                   "of {} bytes): shader={:#x}",
                   runtime_info.hw.cs.shared_memory_size, end, profile.max_shared_memory_size, info.pgm_hash);
        info.shared_memory_scratch_size = end - runtime_info.hw.cs.shared_memory_size;
    }

    struct Lanes {
        IR::U32 index, lane, wave, half_word, bit;
    };
    const auto lanes = [](IR::IREmitter& ir) {
        const IR::U32 index = ir.GetAttributeU32(IR::Attribute::LocalInvocationIndex);
        return Lanes{index, ir.BitwiseAnd(index, ir.Imm32(63)), ir.ShiftRightLogical(index, ir.Imm32(6)),
                     ir.ShiftRightLogical(index, ir.Imm32(5)),
                     ir.ShiftLeftLogical(ir.Imm32(1), ir.BitwiseAnd(index, ir.Imm32(31)))};
    };
    const auto word = [](IR::IREmitter& ir, u32 base_offset, const IR::U32& slot) {
        return ir.IAdd(ir.Imm32(base_offset), ir.ShiftLeftLogical(slot, ir.Imm32(2)));
    };
    // 64-bit wave mask of a predicate. The mask words start at zero (cleared at entry); each lane
    // ORs its own bit, everyone reads after a barrier, and after a second barrier each lane
    // clears only its own bit again. A lane that runs ahead into the next ballot only touches its
    // own bit, and nobody reads the next mask before everyone has cleared, so two barriers suffice.
    const auto wave_ballot = [&](IR::IREmitter& ir, const Lanes& l, const IR::U1& pred) -> IR::U64 {
        const IR::U32 lo_addr = word(ir, mask_base, ir.ShiftLeftLogical(l.wave, ir.Imm32(1)));
        const IR::U32 hi_addr = ir.IAdd(lo_addr, ir.Imm32(4));
        const IR::U32 own = word(ir, mask_base, l.half_word);
        (void)ir.SharedAtomicOr(own, IR::U32{ir.Select(pred, l.bit, ir.Imm32(0))}, false);
        ir.Barrier();
        const IR::U32 lo{ir.LoadShared(32, false, lo_addr)};
        const IR::U32 hi{ir.LoadShared(32, false, hi_addr)};
        ir.Barrier();
        (void)ir.SharedAtomicAnd(own, ir.BitwiseNot(l.bit), false);
        return ir.PackUint2x32(ir.CompositeConstruct(lo, hi));
    };
    bool uses_mask_words = false;
    // The value of `value` at wave lane `target` (wave-uniform).
    const auto wave_read_lane = [&](IR::IREmitter& ir, const Lanes& l, const IR::U32& value,
                                    const IR::U32& target) -> IR::U32 {
        const IR::U32 addr = word(ir, value_base, l.wave);
        (void)ir.SharedAtomicAnd(addr, ir.Imm32(0), false); // every lane clears: atomic, race-free
        ir.Barrier();
        (void)ir.SharedAtomicOr(addr, IR::U32{ir.Select(ir.IEqual(l.lane, target), value, ir.Imm32(0))},
                                false);
        ir.Barrier();
        const IR::U32 result{ir.LoadShared(32, false, addr)};
        ir.Barrier();
        return result;
    };
    const auto find_lsb64 = [](IR::IREmitter& ir, const IR::U64& mask) -> IR::U32 {
        const auto halves = ir.UnpackUint2x32(mask);
        const IR::U32 lo{ir.CompositeExtract(halves, 0)}, hi{ir.CompositeExtract(halves, 1)};
        const IR::U32 from_hi{ir.Select(ir.INotEqual(hi, ir.Imm32(0)),
                                        ir.IAdd(ir.FindILsb(hi), ir.Imm32(32)), ir.Imm32(~0u))};
        return IR::U32{ir.Select(ir.INotEqual(lo, ir.Imm32(0)), ir.FindILsb(lo), from_hi)};
    };

    for (IR::Inst* inst : per_lane) {
        IR::IREmitter ir{*inst->GetParent(), IR::Block::InstructionList::s_iterator_to(*inst)};
        const Lanes l = lanes(ir);
        switch (inst->GetOpcode()) {
        case IR::Opcode::LaneId:
            inst->ReplaceUsesWithAndRemove(l.lane);
            break;
        case IR::Opcode::InverseBallot: {
            const auto halves = ir.UnpackUint2x32(IR::U64{inst->Arg(0)});
            const IR::U32 lo{ir.CompositeExtract(halves, 0)}, hi{ir.CompositeExtract(halves, 1)};
            const IR::U32 w{ir.Select(ir.ILessThan(l.lane, ir.Imm32(32), false), lo, hi)};
            inst->ReplaceUsesWithAndRemove(ir.INotEqual(ir.BitwiseAnd(w, l.bit), ir.Imm32(0)));
            break;
        }
        case IR::Opcode::BallotFindLsb:
            inst->ReplaceUsesWithAndRemove(find_lsb64(ir, IR::U64{inst->Arg(0)}));
            break;
        case IR::Opcode::WriteLane: // v_writelane: lane `Arg(2)` takes the scalar, others keep theirs
            inst->ReplaceUsesWithAndRemove(ir.Select(
                ir.IEqual(l.lane, ir.BitwiseAnd(IR::U32{inst->Arg(2)}, ir.Imm32(63))),
                IR::U32{inst->Arg(1)}, IR::U32{inst->Arg(0)}));
            break;
        default: { // SubgroupLtMask: lanes below this one in the wave
            const IR::U64 mask =
                ir.ISub(ir.ShiftLeftLogical(ir.Imm64(u64{1}), l.lane), ir.Imm64(u64{1}));
            inst->ReplaceUsesWithAndRemove(
                ir.CompositeExtract(ir.UnpackUint2x32(mask), inst->Arg(1).U32()));
            break;
        }
        }
    }
    for (IR::Inst* inst : cross_lane) {
        IR::IREmitter ir{*inst->GetParent(), IR::Block::InstructionList::s_iterator_to(*inst)};
        const Lanes l = lanes(ir);
        switch (inst->GetOpcode()) {
        case IR::Opcode::Ballot:
            uses_mask_words = true;
            inst->ReplaceUsesWithAndRemove(wave_ballot(ir, l, IR::U1{inst->Arg(0)}));
            break;
        case IR::Opcode::GroupAny:
            uses_mask_words = true;
            inst->ReplaceUsesWithAndRemove(
                ir.INotEqual(wave_ballot(ir, l, IR::U1{inst->Arg(0)}), ir.Imm64(u64{0})));
            break;
        case IR::Opcode::ReadLane:
            inst->ReplaceUsesWithAndRemove(wave_read_lane(
                ir, l, IR::U32{inst->Arg(0)}, ir.BitwiseAnd(IR::U32{inst->Arg(1)}, ir.Imm32(63))));
            break;
        case IR::Opcode::ReadFirstLane: {
            uses_mask_words = true;
            // V_READFIRSTLANE_B32: lane = FindFirst1fromLSB(exec), lane 0 if exec is zero
            const IR::U64 exec = wave_ballot(ir, l, ir.Imm1(true));
            const IR::U32 first{ir.Select(ir.IEqual(exec, ir.Imm64(u64{0})), ir.Imm32(0), find_lsb64(ir, exec))};
            inst->ReplaceUsesWithAndRemove(wave_read_lane(ir, l, IR::U32{inst->Arg(0)}, first));
            break;
        }
        default: { // Shuffle / ShuffleXor / QuadBroadcast: exchange through one word per lane
            const IR::U32 wave_base = ir.ShiftLeftLogical(l.wave, ir.Imm32(6));
            ir.WriteShared(32, IR::U32{inst->Arg(0)}, word(ir, lane_base, l.index));
            ir.Barrier();
            IR::U32 src_lane;
            if (inst->GetOpcode() == IR::Opcode::Shuffle) {
                src_lane = ir.BitwiseAnd(IR::U32{inst->Arg(1)}, ir.Imm32(63));
            } else if (inst->GetOpcode() == IR::Opcode::ShuffleXor) {
                src_lane = ir.BitwiseAnd(ir.BitwiseXor(l.lane, IR::U32{inst->Arg(1)}), ir.Imm32(63));
            } else {
                src_lane = ir.IAdd(ir.BitwiseAnd(l.lane, ir.Imm32(~3u)), ir.BitwiseAnd(IR::U32{inst->Arg(1)}, ir.Imm32(3)));
            }
            const IR::U32 value{ir.LoadShared(32, false, word(ir, lane_base, ir.IAdd(wave_base, src_lane)))};
            ir.Barrier();
            inst->ReplaceUsesWithAndRemove(value);
            break;
        }
        }
    }
    if (uses_mask_words) {
        // Workgroup memory starts undefined: clear the mask words once, before any ballot. Every
        // lane clears both words of its wave (atomic, idempotent): a wave with 32 or fewer
        // invocations (small workgroups, a partial last wave) has no lane in its high half, yet
        // every ballot reads both words.
        IR::Block* entry = program.syntax_list.front().data.block;
        IR::IREmitter ir{*entry, entry->begin()};
        const Lanes l = lanes(ir);
        const IR::U32 lo_addr = word(ir, mask_base, ir.ShiftLeftLogical(l.wave, ir.Imm32(1)));
        (void)ir.SharedAtomicAnd(lo_addr, ir.Imm32(0), false);
        (void)ir.SharedAtomicAnd(ir.IAdd(lo_addr, ir.Imm32(4)), ir.Imm32(0), false);
        ir.Barrier();
    }
}

} // namespace

void LowerWave64BallotPass(IR::Program& program, const RuntimeInfo& runtime_info,
                           const Profile& profile) {
    if (profile.emulate_wave64) {
        if (program.info.hw_stage == HwStage::Compute) {
            LowerWave64Compute(program, runtime_info, profile);
        } else {
            LowerWave64Graphics(program);
        }
        return;
    }
    if (program.info.hw_stage != HwStage::Compute || profile.subgroup_size == 64) {
        return;
    }

    const auto [size_x, size_y, size_z] = runtime_info.hw.cs.workgroup_size;
    const u32 num_threads = size_x * size_y * size_z;
    if (num_threads <= 32) {
        return;
    }

    std::vector<IR::Inst*> worklist;
    const auto uniform_blocks = FindUniformBlocks(program);
    for (IR::Block* block : program.blocks) {
        const bool is_uniform = std::ranges::contains(uniform_blocks, block);
        const auto push_worklist = [&](IR::Inst& inst) {
            if (is_uniform) {
                worklist.push_back(&inst);
            } else {
                LOG_WARNING(Render_Recompiler, "{} instruction in non uniform control flow",
                            inst.GetOpcode());
            }
        };
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::ReadLane && inst.Arg(1).IsImmediate()) {
                push_worklist(inst);
            } else if (inst.GetOpcode() == IR::Opcode::Ballot) {
                const auto is_unpack = [](const IR::Use& use) {
                    return use.user->GetOpcode() == IR::Opcode::UnpackUint2x32;
                };
                if (std::ranges::any_of(inst.Uses(), is_unpack)) {
                    push_worklist(inst);
                }
            } else if (inst.GetOpcode() == IR::Opcode::GetAttributeU32 &&
                       inst.Arg(0).Attribute() == IR::Attribute::SubgroupLtMask) {
                worklist.push_back(&inst);
            }
        }
    }
    if (worklist.empty()) {
        return;
    }

    const u32 scratch_base = Common::AlignUp(runtime_info.hw.cs.shared_memory_size, sizeof(u64));
    const u32 scratch_size =
        (Common::AlignUp(num_threads, 64) / profile.subgroup_size) * sizeof(u32);
    program.info.shared_memory_scratch_size =
        scratch_base + scratch_size - runtime_info.hw.cs.shared_memory_size;

    for (IR::Inst* inst : worklist) {
        LOG_INFO(Render_Recompiler, "Lowering {} instruction for wave64", inst->GetOpcode());
        IR::IREmitter ir{*inst->GetParent(), IR::Block::InstructionList::s_iterator_to(*inst)};
        const IR::U32 invocation_index = ir.GetAttributeU32(IR::Attribute::LocalInvocationIndex);
        const IR::U32 subgroup_id = ir.ShiftRightLogical(invocation_index, ir.Imm32(5));
        if (inst->GetOpcode() == IR::Opcode::Ballot) {
            const IR::U32 mask_low =
                IR::U32{ir.CompositeExtract(ir.UnpackUint2x32(ir.Ballot(IR::U1{inst->Arg(0)})), 0)};
            const IR::U32 offset =
                ir.IAdd(ir.Imm32(scratch_base), ir.ShiftLeftLogical(subgroup_id, ir.Imm32(2u)));
            ir.WriteShared(32, mask_low, offset);
            ir.Barrier();
            const IR::U64 mask = IR::U64{ir.LoadShared(64, false, offset)};
            ir.Barrier();
            inst->ReplaceUsesWithAndRemove(mask);
        } else if (inst->GetOpcode() == IR::Opcode::ReadLane) {
            const IR::U32 lane32 = ir.BitwiseAnd(IR::U32{inst->Arg(1)}, ir.Imm32(31));
            const IR::U32 half = ir.ShiftRightLogical(IR::U32{inst->Arg(1)}, ir.Imm32(5u));
            const IR::U32 offset =
                ir.IAdd(ir.Imm32(scratch_base), ir.ShiftLeftLogical(subgroup_id, ir.Imm32(2u)));
            ir.WriteShared(32, ir.ReadLane(IR::U32{inst->Arg(0)}, lane32), offset);
            ir.Barrier();
            const IR::U32 value =
                IR::U32{ir.LoadShared(32, false,
                                      ir.IAdd(ir.BitwiseAnd(offset, ir.Imm32(~7u)),
                                              ir.ShiftLeftLogical(half, ir.Imm32(2u))))};
            ir.Barrier();
            inst->ReplaceUsesWithAndRemove(value);
        } else if (inst->GetOpcode() == IR::Opcode::GetAttributeU32 &&
                   inst->Arg(0).Attribute() == IR::Attribute::SubgroupLtMask) {
            const IR::U32 subgroup_invocation_id = ir.BitwiseAnd(invocation_index, ir.Imm32(63));
            const IR::U64 mask = ir.ISub(
                ir.ShiftLeftLogical(ir.Imm64(u64{1}), subgroup_invocation_id), ir.Imm64(u64{1}));
            inst->ReplaceUsesWithAndRemove(
                ir.CompositeExtract(ir.UnpackUint2x32(mask), inst->Arg(1).U32()));
        }
    }
}

} // namespace Shader::Optimization
