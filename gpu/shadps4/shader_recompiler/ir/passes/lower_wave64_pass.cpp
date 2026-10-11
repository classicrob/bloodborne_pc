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
#include <span>
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

const char* StageName(SwStage stage) {
    switch (stage) {
    case SwStage::Vertex: return "vertex";
    case SwStage::TessellationControl: return "tessellation control";
    case SwStage::TessellationEval: return "tessellation evaluation";
    case SwStage::Geometry: return "geometry";
    case SwStage::Fragment: return "fragment";
    default: return "compute";
    }
}

// Graphics stages where the device has no subgroup operations (Apple: Metal vertex functions, for
// vertex and tessellation evaluation shaders).
// A VS wave's lanes are vertices, and the recompiler keeps exec and VOPC/VOP3 compare masks as one
// bit per lane; a ballot of such a predicate is only ever read back by InverseBallot (this lane's
// bit). Ballot(x) becomes the lane's own bit in every position (x ? ~0 : 0), which pure bitwise
// ops, 32-bit halves and phis preserve, and InverseBallot(m) becomes m != 0. That is exact at a
// read when every value that reaches it is ballot-derived, with both halves mirrored. A phi arm
// that is not (an SGPR the guest reuses for other data on a path that skips the compare) is
// allowed only when it is the skip edge of an if whose condition also encloses the read: every
// lane that executes the read went through the ballot. Anything else (another reader of a mask,
// a mask as data, any other subgroup operation) stops compilation naming the shader.
void LowerWave64WithoutSubgroups(IR::Program& program) {
    using Type = IR::AbstractSyntaxNode::Type;
    const auto strip = [](const IR::Inst* c) {
        if (c && c->GetOpcode() == IR::Opcode::ConditionRef) c = c->Arg(0).TryInst();
        while (c && c->GetOpcode() == IR::Opcode::LogicalNot && c->Arg(0).TryInst() &&
               c->Arg(0).TryInst()->GetOpcode() == IR::Opcode::LogicalNot) {
            c = c->Arg(0).TryInst()->Arg(0).TryInst();
        }
        return c;
    };
    // Enclosing if conditions of each block, and for each if merge block: its header and condition.
    std::unordered_map<const IR::Block*, std::vector<const IR::Inst*>> enclosing;
    struct IfInfo {
        const IR::Block* header;
        const IR::Inst* cond;
    };
    std::unordered_map<const IR::Block*, IfInfo> if_by_merge;
    {
        std::vector<const IR::Inst*> conds;
        for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
            if (node.type == Type::If) {
                const IR::Inst* ref = node.data.if_node.cond.TryInst();
                conds.push_back(strip(ref));
                if_by_merge[node.data.if_node.merge] = {ref ? ref->GetParent() : nullptr, strip(ref)};
            } else if (node.type == Type::EndIf) {
                conds.pop_back();
            } else if (node.type == Type::Block) {
                enclosing[node.data.block] = conds;
            }
        }
    }
    const auto is_imm = [](const IR::Value& v, u64 x) {
        if (!v.IsImmediate()) return false;
        if (v.Type() == IR::Type::U64) return v.U64() == x;
        if (v.Type() == IR::Type::U32) return v.U32() == u32(x);
        return false;
    };
    // Whether condition `c` being true implies `d` is true: the same value, d = p || q with p or q
    // implied, or c = p && q with p or q implying d (double negations looked through).
    std::function<bool(const IR::Inst*, const IR::Inst*, int)> implies;
    implies = [&](const IR::Inst* c, const IR::Inst* d, int depth) -> bool {
        c = strip(c);
        d = strip(d);
        if (!c || !d || depth > 16) return false;
        if (c == d) return true;
        if (d->GetOpcode() == IR::Opcode::LogicalOr &&
            (implies(c, d->Arg(0).TryInst(), depth + 1) || implies(c, d->Arg(1).TryInst(), depth + 1))) {
            return true;
        }
        return c->GetOpcode() == IR::Opcode::LogicalAnd &&
               (implies(c->Arg(0).TryInst(), d, depth + 1) || implies(c->Arg(1).TryInst(), d, depth + 1));
    };
    // Whether the phi arm from `pred` into a phi of `merge` cannot be taken by a lane that executes
    // a block enclosed by `conds`: it is the skip edge of if (c) with c implied by one of `conds`.
    const auto arm_dead = [&](const IR::Block* merge, const IR::Block* pred,
                              const std::vector<const IR::Inst*>& conds) {
        const auto it = if_by_merge.find(merge);
        if (it == if_by_merge.end() || it->second.header != pred || !it->second.cond) return false;
        return std::ranges::any_of(conds, [&](const IR::Inst* c) { return implies(c, it->second.cond, 0); });
    };
    // valid64: the value is ~0 or 0 in every lane that executes a block enclosed by `conds`.
    std::function<bool(const IR::Value&, const std::vector<const IR::Inst*>&, int)> valid64;
    std::function<bool(const IR::Value&, const IR::Value&, const std::vector<const IR::Inst*>&, int)> mirror;
    valid64 = [&](const IR::Value& v, const std::vector<const IR::Inst*>& conds, int depth) -> bool {
        if (depth > 64) return false;
        if (is_imm(v, 0) || is_imm(v, ~0ull)) return true;
        const IR::Inst* inst = v.TryInst();
        if (!inst) return false;
        switch (inst->GetOpcode()) {
        case IR::Opcode::Ballot:
            return true;
        case IR::Opcode::PackUint2x32: {
            const IR::Inst* pair = inst->Arg(0).TryInst();
            return pair && pair->GetOpcode() == IR::Opcode::CompositeConstructU32x2 &&
                   mirror(pair->Arg(0), pair->Arg(1), conds, depth + 1);
        }
        case IR::Opcode::BitwiseAnd64:
        case IR::Opcode::BitwiseOr64:
        case IR::Opcode::BitwiseXor64:
            return valid64(inst->Arg(0), conds, depth + 1) && valid64(inst->Arg(1), conds, depth + 1);
        case IR::Opcode::BitwiseNot64:
            return valid64(inst->Arg(0), conds, depth + 1);
        case IR::Opcode::Phi:
            for (size_t i = 0; i < inst->NumArgs(); ++i) {
                if (arm_dead(inst->GetParent(), inst->PhiBlock(i), conds)) continue;
                if (!valid64(inst->Arg(i), conds, depth + 1)) return false;
            }
            return true;
        default:
            return false;
        }
    };
    // mirror: the two 32-bit halves are equal and ~0 or 0 in every such lane.
    mirror = [&](const IR::Value& lo, const IR::Value& hi, const std::vector<const IR::Inst*>& conds,
                 int depth) -> bool {
        if (depth > 64) return false;
        if ((is_imm(lo, 0) && is_imm(hi, 0)) || (is_imm(lo, ~0u) && is_imm(hi, ~0u))) return true;
        const IR::Inst* a = lo.TryInst();
        const IR::Inst* b = hi.TryInst();
        if (!a || !b || a->GetOpcode() != b->GetOpcode()) return false;
        if (a->GetOpcode() == IR::Opcode::CompositeExtractU32x2) {
            const IR::Inst* ua = a->Arg(0).TryInst();
            return ua && ua == b->Arg(0).TryInst() && ua->GetOpcode() == IR::Opcode::UnpackUint2x32 &&
                   is_imm(a->Arg(1), 0) && is_imm(b->Arg(1), 1) && valid64(ua->Arg(0), conds, depth + 1);
        }
        if (a->GetOpcode() == IR::Opcode::Phi && a->GetParent() == b->GetParent() &&
            a->NumArgs() == b->NumArgs()) {
            for (size_t i = 0; i < a->NumArgs(); ++i) {
                if (a->PhiBlock(i) != b->PhiBlock(i)) return false;
                if (arm_dead(a->GetParent(), a->PhiBlock(i), conds)) continue;
                if (!mirror(a->Arg(i), b->Arg(i), conds, depth + 1)) return false;
            }
            return true;
        }
        return false;
    };

    std::vector<IR::Inst*> reads;
    std::vector<IR::Inst*> ballots;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            const auto op = inst.GetOpcode();
            if (op == IR::Opcode::InverseBallot) {
                ASSERT_MSG(valid64(inst.Arg(0), enclosing[block], 0),
                           "Apple wave64: {} shader {:#x} reads a lane bit of a value that is not "
                           "provably this lane's own ballot bit (this stage has no subgroup "
                           "operations on the device)",
                           StageName(program.info.sw_stage), program.info.pgm_hash);
                reads.push_back(&inst);
            } else if (op == IR::Opcode::Ballot) {
                ballots.push_back(&inst);
            } else if (IsCrossLane(op) || op == IR::Opcode::LaneId || op == IR::Opcode::BallotFindLsb ||
                       op == IR::Opcode::WriteLane || op == IR::Opcode::WarpId ||
                       (op == IR::Opcode::GetAttributeU32 &&
                        inst.Arg(0).Attribute() == IR::Attribute::SubgroupLtMask)) {
                UNREACHABLE_MSG("Apple wave64: {} shader {:#x} uses {}, which needs subgroup "
                                "operations; this stage has none on the device",
                                StageName(program.info.sw_stage), program.info.pgm_hash, op);
            }
        }
    }
    // Every ballot must be consumed only on the validated paths: through the mask plumbing above
    // into InverseBallot reads (its own-bit form is visible to nothing else), or as the
    // old-destination operand of a scalar buffer load (an SGPR pair the guest reuses as an
    // s_buffer_load destination), which the shader then reads as 0 for out-of-range dwords: the
    // SMEM_ZERO reading, as for compute (docs/macos/wave64.md).
    bool smem_zero = false;
    for (IR::Inst* ballot : ballots) {
        std::vector<const IR::Inst*> work{ballot};
        std::unordered_set<const IR::Inst*> seen{ballot};
        while (!work.empty()) {
            const IR::Inst* x = work.back();
            work.pop_back();
            for (const IR::Use& use : x->Uses()) {
                const auto op = use.user->GetOpcode();
                const bool plumbing =
                    op == IR::Opcode::Phi || op == IR::Opcode::UnpackUint2x32 ||
                    op == IR::Opcode::CompositeExtractU32x2 || op == IR::Opcode::CompositeConstructU32x2 ||
                    op == IR::Opcode::PackUint2x32 || op == IR::Opcode::BitwiseAnd64 ||
                    op == IR::Opcode::BitwiseOr64 || op == IR::Opcode::BitwiseXor64 ||
                    op == IR::Opcode::BitwiseNot64;
                if (op == IR::Opcode::ReadConstBuffer && use.operand == 2) {
                    smem_zero = true;
                    continue;
                }
                ASSERT_MSG(plumbing || op == IR::Opcode::InverseBallot,
                           "Apple wave64: {} shader {:#x} uses a ballot mask as data ({}); this "
                           "stage has no subgroup operations on the device",
                           StageName(program.info.sw_stage), program.info.pgm_hash, op);
                if (plumbing && seen.insert(use.user).second) work.push_back(use.user);
            }
        }
    }
    if (smem_zero) {
        LOG_WARNING(Render_Recompiler,
                    "Apple wave64: {} shader {:#x} reads out-of-range scalar buffer dwords as 0 "
                    "(SMEM_ZERO reading): a compare mask is also an s_buffer_load destination",
                    StageName(program.info.sw_stage), program.info.pgm_hash);
        for (IR::Block* block : program.blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                if (inst.GetOpcode() == IR::Opcode::ReadConstBuffer) inst.SetArg(2, IR::Value{0u});
            }
        }
    }
    for (IR::Inst* ballot : ballots) {
        IR::IREmitter ir{*ballot->GetParent(), IR::Block::InstructionList::s_iterator_to(*ballot)};
        ballot->ReplaceUsesWithAndRemove(
            ir.Select(IR::U1{ballot->Arg(0)}, ir.Imm64(~0ull), ir.Imm64(u64{0})));
    }
    for (IR::Inst* read : reads) {
        IR::IREmitter ir{*read->GetParent(), IR::Block::InstructionList::s_iterator_to(*read)};
        read->ReplaceUsesWithAndRemove(ir.LogicalNot(ir.IEqual(IR::U64{read->Arg(0)}, ir.Imm64(u64{0}))));
    }
}

// The VkShaderStageFlags bit of the stage this program is emitted as (emit_spirv.cpp
// DefineEntryPoint): the geometry-emulation ES/GS roles are compute kernels.
u32 EmittedStageBit(const IR::Program& program, const RuntimeInfo& runtime_info) {
    if (runtime_info.gs_role == GsEmulationRole::EsCompute ||
        runtime_info.gs_role == GsEmulationRole::GsCompute) {
        return 0x20;
    }
    switch (program.info.sw_stage) {
    case SwStage::Vertex: return 0x1;
    case SwStage::TessellationControl: return 0x2;
    case SwStage::TessellationEval: return 0x4;
    case SwStage::Geometry: return 0x8;
    case SwStage::Fragment: return 0x10;
    default: return 0x20;
    }
}

void LowerWave64Graphics(IR::Program& program, const RuntimeInfo& runtime_info, const Profile& profile) {
    if (!(profile.subgroup_supported_stages & EmittedStageBit(program, runtime_info))) {
        LowerWave64WithoutSubgroups(program);
        return;
    }
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

// Whether the dynamic lane of `read_lane` is the frontend's V_READFIRSTLANE_B32 selector in
// compute, exactly: B = Ballot(p); lane = Select(IEqual64(B, 0), 0, BallotFindLsb(B)), with the
// same B in both places and B, the selector and the ReadLane in one block (one executing set).
// A non-empty B names its lowest invocation, which executes this ReadLane. An empty B names lane
// 0 (v_readfirstlane ignores exec); lane 0 executes the ReadLane only when the block is
// workgroup-uniform, or B cannot be empty: p is true, or p is the condition of an if enclosing
// the block (every invocation that executes the ballot has p set, so B has its own bit; this is
// v_readfirstlane inside an s_cbranch_execz scope). Anything else (another selector, a ballot
// from another block, a bare BallotFindLsb, which is ~0 for an empty mask like s_ff1_i32_b64 and
// so names lane 63) is rejected.
bool IsFirstActiveLane(const IR::Inst& read_lane, bool block_uniform,
                       std::span<const IR::Inst* const> enclosing_conditions) {
    const IR::Block* block = read_lane.GetParent();
    const auto in_block = [block](const IR::Value& v, IR::Opcode op) -> const IR::Inst* {
        const IR::Inst* inst = v.TryInst();
        return inst && inst->GetOpcode() == op && inst->GetParent() == block ? inst : nullptr;
    };
    const auto is_zero = [](const IR::Value& v) {
        return v.IsImmediate() && ((v.Type() == IR::Type::U64 && v.U64() == 0) ||
                                   (v.Type() == IR::Type::U32 && v.U32() == 0));
    };
    const IR::Inst* select = in_block(read_lane.Arg(1), IR::Opcode::SelectU32);
    if (!select || !is_zero(select->Arg(1))) return false;
    const IR::Inst* is_empty = in_block(select->Arg(0), IR::Opcode::IEqual64);
    const IR::Inst* find = in_block(select->Arg(2), IR::Opcode::BallotFindLsb);
    if (!is_empty || !find || !is_zero(is_empty->Arg(1))) return false;
    const IR::Inst* ballot = in_block(find->Arg(0), IR::Opcode::Ballot);
    if (!ballot || in_block(is_empty->Arg(0), IR::Opcode::Ballot) != ballot) return false;
    const IR::Value pred = ballot->Arg(0);
    return block_uniform || (pred.IsImmediate() && pred.Type() == IR::Type::U1 && pred.U1()) ||
           (pred.TryInst() && std::ranges::contains(enclosing_conditions, pred.TryInst()));
}

// Whether every consumer of `inst` (through phis, 32/64-bit packing and pure bitwise ops, e.g. an
// s_and_saveexec mask combined with another) is the old-destination
// operand of a scalar buffer load (ReadConstBuffer arg 2: the SDST value kept for out-of-range
// dwords). Collects the pass-through instructions in `chain`.
bool OnlyFeedsScalarLoadFallback(const IR::Inst& inst, std::vector<IR::Inst*>& chain) {
    std::vector<const IR::Inst*> work{&inst};
    std::unordered_set<const IR::Inst*> seen{&inst};
    while (!work.empty()) {
        const IR::Inst* x = work.back();
        work.pop_back();
        for (const IR::Use& use : x->Uses()) {
            IR::Inst* user = use.user;
            const auto op = user->GetOpcode();
            if (op == IR::Opcode::ReadConstBuffer && use.operand == 2) continue;
            const std::string_view name = IR::NameOf(op);
            const bool pass = op == IR::Opcode::Phi || name.starts_with("CompositeExtract") ||
                              name.starts_with("CompositeConstruct") || op == IR::Opcode::UnpackUint2x32 ||
                              op == IR::Opcode::PackUint2x32 || name.starts_with("BitwiseAnd") ||
                              name.starts_with("BitwiseOr") || name.starts_with("BitwiseXor") ||
                              name.starts_with("BitwiseNot");
            if (!pass) return false;
            if (seen.insert(user).second) {
                chain.push_back(user);
                work.push_back(user);
            }
        }
    }
    return true;
}

void LowerWave64Compute(IR::Program& program, const RuntimeInfo& runtime_info, const Profile& profile) {
    auto& info = program.info;
    const auto [size_x, size_y, size_z] = runtime_info.hw.cs.workgroup_size;
    const u32 num_threads = size_x * size_y * size_z;
    const u32 num_waves = (num_threads + 63) / 64;
    // A workgroup of at most one subgroup is one GCN wave (lanes beyond the workgroup absent) that
    // runs as one subgroup. Its wave operations are built from LocalInvocationIndex and subgroup
    // reductions over the executing invocations (exact in any control flow, and independent of the
    // order the driver numbers subgroup invocations in); no shared memory or barrier.
    // Lane-index exchanges (shuffles) would need the invocation order: such shaders keep the
    // shared-memory exchange (exact in workgroup-uniform flow, loud elsewhere).
    bool lane_exchange = false;
    for (const IR::Block* block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            const auto op = inst.GetOpcode();
            lane_exchange |= op == IR::Opcode::Shuffle || op == IR::Opcode::ShuffleXor || op == IR::Opcode::QuadBroadcast;
        }
    }
    const bool single_subgroup = num_threads <= profile.subgroup_size && !lane_exchange;
    const WorkgroupUniformity uniformity{program, num_waves <= 1};
    const auto uniform_blocks = FindWorkgroupUniformBlocks(program, uniformity);

    // A scalar buffer load leaves an out-of-range destination dword unwritten (§7.4 "not
    // performed"), so ReadConstBuffer carries the destination's old value. Game code reuses the
    // SGPR pair of a VOP3 compare as such a destination; when the compare sits in
    // workgroup-divergent flow, its 64-lane mask cannot be exchanged without a barrier, yet it is
    // observable only if that scalar load is out of range. If that is the only use of every
    // divergent cross-lane value, the shader takes the other accepted reading of §7.4 for all of
    // its scalar buffer loads (out-of-range dwords read 0; tests/gcn_buffer_conformance/SPEC.md
    // SMEM_ZERO) and the masks become dead. Any other divergent use stays a hard error.
    if (!single_subgroup) {
        std::vector<IR::Inst*> divergent_cross_lane, chain;
        bool all_fallback = true;
        for (IR::Block* block : program.blocks) {
            if (std::ranges::contains(uniform_blocks, block)) continue;
            for (IR::Inst& inst : block->Instructions()) {
                if (!IsCrossLane(inst.GetOpcode())) continue;
                divergent_cross_lane.push_back(&inst);
                all_fallback = all_fallback && OnlyFeedsScalarLoadFallback(inst, chain);
            }
        }
        if (!divergent_cross_lane.empty() && all_fallback) {
            LOG_WARNING(Render_Recompiler,
                        "Apple wave64: shader {:#x} reads out-of-range scalar buffer dwords as 0 (SMEM_ZERO "
                        "reading): {} divergent wave masks only fed s_buffer_load destinations",
                        info.pgm_hash, divergent_cross_lane.size());
            for (IR::Block* block : program.blocks) {
                for (IR::Inst& inst : block->Instructions()) {
                    if (inst.GetOpcode() == IR::Opcode::ReadConstBuffer) inst.SetArg(2, IR::Value{0u});
                }
            }
            // The masks and their phis/packing are dead now: OnlyFeedsScalarLoadFallback proved
            // every use of each of them is inside this set or a fallback operand zeroed above, so
            // the whole set goes at once (loop-carried phi cycles included).
            chain.insert(chain.end(), divergent_cross_lane.begin(), divergent_cross_lane.end());
            for (IR::Inst* x : chain) {
                x->Invalidate();
            }
        }
    }

    // The conditions of the ifs around each block, for IsFirstActiveLane. ConditionRef and double
    // negations (an s_cbranch_execz skip is if (!(!exec))) are looked through.
    std::unordered_map<const IR::Block*, std::vector<const IR::Inst*>> enclosing_conditions;
    if (single_subgroup) {
        std::vector<const IR::Inst*> conds;
        for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
            using Type = IR::AbstractSyntaxNode::Type;
            if (node.type == Type::If) {
                const IR::Inst* c = node.data.if_node.cond.TryInst();
                if (c && c->GetOpcode() == IR::Opcode::ConditionRef) c = c->Arg(0).TryInst();
                while (c && c->GetOpcode() == IR::Opcode::LogicalNot && c->Arg(0).TryInst() &&
                       c->Arg(0).TryInst()->GetOpcode() == IR::Opcode::LogicalNot) {
                    c = c->Arg(0).TryInst()->Arg(0).TryInst();
                }
                conds.push_back(c);
            } else if (node.type == Type::EndIf) {
                conds.pop_back();
            } else if (node.type == Type::Block) {
                enclosing_conditions[node.data.block] = conds;
            }
        }
    }

    std::vector<IR::Inst*> cross_lane, per_lane;
    bool needs_lane_values = false;
    for (IR::Block* block : program.blocks) {
        const bool block_uniform = std::ranges::contains(uniform_blocks, block);
        for (IR::Inst& inst : block->Instructions()) {
            const auto op = inst.GetOpcode();
            if (IsCrossLane(op) && single_subgroup) {
                if (op == IR::Opcode::ReadLane) {
                    const IR::Value lane = inst.Arg(1);
                    if (lane.IsImmediate()) {
                        // v_readlane reads the lane whatever exec says: only exact when it executes here
                        ASSERT_MSG(lane.U32() < num_threads && block_uniform,
                                   "Apple wave64: v_readlane of lane {} in a {}-invocation workgroup "
                                   "({}): shader={:#x}",
                                   lane.U32(), num_threads,
                                   lane.U32() < num_threads ? "workgroup-divergent flow" : "never launched",
                                   info.pgm_hash);
                    } else {
                        ASSERT_MSG(IsFirstActiveLane(inst, block_uniform, enclosing_conditions[block]),
                                   "Apple wave64: v_readlane with a dynamic lane in a {}-invocation "
                                   "workgroup: only v_readfirstlane's own selector is accepted, and in "
                                   "workgroup-divergent flow only when its exec cannot be empty (lane 0 "
                                   "might not execute): shader={:#x}",
                                   num_threads, info.pgm_hash);
                    }
                }
                cross_lane.push_back(&inst);
            } else if (IsCrossLane(op)) {
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
    if (!cross_lane.empty() && !single_subgroup) {
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
    // One subgroup: each executing lane adds its own bit (bits are disjoint, so the sum is the
    // mask), or the target lane alone adds its value.
    const auto subgroup_ballot = [&](IR::IREmitter& ir, const Lanes& l, const IR::U1& pred) -> IR::U64 {
        const IR::U32 lo = ir.GroupIAdd(IR::U32{ir.Select(pred, l.bit, ir.Imm32(0))});
        return ir.PackUint2x32(ir.CompositeConstruct(lo, ir.Imm32(0)));
    };
    const auto subgroup_read_lane = [&](IR::IREmitter& ir, const Lanes& l, const IR::U32& value,
                                        const IR::U32& target) -> IR::U32 {
        return ir.GroupIAdd(IR::U32{ir.Select(ir.IEqual(l.lane, target), value, ir.Imm32(0))});
    };
    for (IR::Inst* inst : cross_lane) {
        if (!single_subgroup) continue;
        IR::IREmitter ir{*inst->GetParent(), IR::Block::InstructionList::s_iterator_to(*inst)};
        const Lanes l = lanes(ir);
        switch (inst->GetOpcode()) {
        case IR::Opcode::Ballot:
            inst->ReplaceUsesWithAndRemove(subgroup_ballot(ir, l, IR::U1{inst->Arg(0)}));
            break;
        case IR::Opcode::GroupAny: // already independent of the invocation order
            break;
        case IR::Opcode::ReadLane:
            inst->ReplaceUsesWithAndRemove(subgroup_read_lane(ir, l, IR::U32{inst->Arg(0)}, IR::U32{inst->Arg(1)}));
            break;
        case IR::Opcode::ReadFirstLane: {
            const IR::U64 exec = subgroup_ballot(ir, l, ir.Imm1(true));
            const IR::U32 first{ir.Select(ir.IEqual(exec, ir.Imm64(u64{0})), ir.Imm32(0), find_lsb64(ir, exec))};
            inst->ReplaceUsesWithAndRemove(subgroup_read_lane(ir, l, IR::U32{inst->Arg(0)}, first));
            break;
        }
        default:
            UNREACHABLE();
        }
    }
    for (IR::Inst* inst : cross_lane) {
        if (single_subgroup) break;
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
            LowerWave64Graphics(program, runtime_info, profile);
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
