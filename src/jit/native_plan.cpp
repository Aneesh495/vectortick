#include "vectortick/jit/native_plan.hpp"
#include <algorithm>
#include <set>

namespace vectortick::jit {
Result<NativePlan> plan_native(const ir::Function *fn, usize registers) noexcept {
    using namespace ir;
    auto error = [](const char *msg) {
        return make_error<NativePlan>(StatusCode::InvalidArgument, msg);
    };
    if (!fn || fn->num_blocks() == 0 || fn->num_parameters() > 1 || fn->num_instructions() > 100000)
        return error("Invalid native function size or parameters");
    NativePlan plan;
    std::set<const BasicBlock *> all;
    std::map<const BasicBlock *, std::vector<const BasicBlock *>> preds, succs;
    for (const auto &b : fn->blocks())
        all.insert(b.get());
    for (const auto &b : fn->blocks()) {
        for (const auto &in : b->instructions())
            if ((in->opcode() == Opcode::Nop || in->is_terminator()) && in->has_result())
                return error("Native control instructions cannot define SSA results");
        if (!b->has_terminator())
            return error("Native block has no terminator");
        auto t = b->terminator();
        if (t->opcode() == Opcode::Jump)
            succs[b.get()] = {static_cast<const JumpOp *>(t)->target()};
        if (t->opcode() == Opcode::Branch) {
            auto br = static_cast<const BranchOp *>(t);
            succs[b.get()] = {br->true_block(), br->false_block()};
        }
        for (auto s : succs[b.get()]) {
            if (!all.count(s))
                return error("Branch target is outside function");
            preds[s].push_back(b.get());
        }
    }
    if (!preds[fn->entry_block()].empty())
        return error("Native cyclic entry is unsupported");
    std::set<const BasicBlock *> done;
    while (done.size() < all.size()) {
        bool progress = false;
        for (const auto &b : fn->blocks()) {
            if (done.count(b.get()))
                continue;
            if (std::all_of(preds[b.get()].begin(), preds[b.get()].end(),
                            [&](auto p) { return done.count(p) != 0; })) {
                if (b.get() != fn->entry_block() && preds[b.get()].empty())
                    return error("Unreachable native block");
                plan.blocks.push_back(b.get());
                done.insert(b.get());
                progress = true;
            }
        }
        if (!progress)
            return error("Native loops are unsupported");
    }
    std::map<const BasicBlock *, std::set<ValueId>> available;
    std::set<ValueId> definitions;
    for (auto p : fn->parameters()) {
        if (!fn->value(p) || !definitions.insert(p).second)
            return error("Invalid native parameter");
    }
    std::map<ValueId, usize> first, last;
    usize pc = 0;
    for (auto b : plan.blocks) {
        std::set<ValueId> defs;
        if (b == fn->entry_block())
            defs = definitions;
        else {
            defs = available[preds[b][0]];
            for (auto p : preds[b]) {
                std::set<ValueId> common;
                std::set_intersection(defs.begin(), defs.end(), available[p].begin(),
                                      available[p].end(), std::inserter(common, common.end()));
                defs = std::move(common);
            }
        }
        for (usize i = 0; i < b->num_instructions(); ++i, ++pc) {
            auto in = b->instruction(i);
            auto op = in->opcode();
            if (in->is_terminator() && i + 1 != b->num_instructions())
                return error("Instruction after terminator");
            usize arity = 0;
            if (op == Opcode::Nop || op == Opcode::Jump ||
                (op >= Opcode::ConstI64 && op <= Opcode::ConstBool))
                arity = 0;
            else if (op == Opcode::Return)
                arity = in->num_operands();
            else if (op == Opcode::Branch || op == Opcode::LoadColumn || op == Opcode::NegI64 ||
                     op == Opcode::Not || op == Opcode::BitNot ||
                     (op >= Opcode::ExtendI32ToI64 && op <= Opcode::TruncI64ToI32) ||
                     op == Opcode::U64ToI64 || op == Opcode::NegU64)
                arity = 1;
            else if ((op >= Opcode::AddI64 && op <= Opcode::ModU64) || is_comparison(op) ||
                     op == Opcode::And || op == Opcode::Or ||
                     (op >= Opcode::BitAnd && op <= Opcode::Shr))
                arity = 2;
            else if (op == Opcode::Select)
                arity = 3;
            else if (op == Opcode::Phi)
                arity = in->num_operands();
            else
                return make_error<NativePlan>(StatusCode::NotImplemented,
                                              "Unsupported native opcode");
            if (in->num_operands() != arity || (op == Opcode::Return && arity > 1))
                return error("Native operand arity mismatch");
            if (op == Opcode::Phi) {
                const auto *phi = static_cast<const PhiOp *>(in);
                if (arity == 0 || phi->incoming_blocks().size() != arity ||
                    arity != preds[b].size())
                    return error("Invalid phi inputs");
                std::set<const BasicBlock *> incoming;
                for (usize k = 0; k < arity; ++k) {
                    auto p = phi->incoming_blocks()[k];
                    if (std::find(preds[b].begin(), preds[b].end(), p) == preds[b].end() ||
                        !incoming.insert(p).second || !available[p].count(in->operand(k)))
                        return error("Undefined phi input");
                }
            } else {
                for (auto operand : in->operands())
                    if (!defs.count(operand))
                        return error("Undefined or non-dominating native value");
            }
            for (auto operand : in->operands()) {
                if (!fn->parameters().empty() && operand == fn->parameters()[0] &&
                    op != Opcode::LoadColumn)
                    return error("Native context is only valid as a field-load base");
                last[operand] = pc;
            }
            if (op == Opcode::LoadColumn) {
                auto load = static_cast<const LoadColumnOp *>(in);
                if (load->column_id() >= 12 || fn->parameters().empty() ||
                    load->row_index() != fn->parameters()[0])
                    return error("Invalid native column or context");
            }
            if (in->has_result()) {
                if (!fn->value(in->result()) || !definitions.insert(in->result()).second)
                    return error("Duplicate or invalid SSA result");
                if ((in->result_type() < Type::I1 || in->result_type() > Type::U64) ||
                    fn->value(in->result())->type() != in->result_type())
                    return error("Unsupported or inconsistent native result type");
                defs.insert(in->result());
                first[in->result()] = pc;
                last.try_emplace(in->result(), pc);
            } else if (op != Opcode::Return && op != Opcode::Branch && op != Opcode::Jump &&
                       op != Opcode::Nop)
                return error("Native instruction is missing a result");
        }
        available[b] = std::move(defs);
    }
    // Linear scan for straight-line code. CFG values have stable stack homes so
    // predecessor register state cannot leak across edges and phi copies are safe.
    std::vector<std::pair<usize, ValueId>> intervals;
    for (auto [v, start] : first)
        intervals.emplace_back(start, v);
    std::sort(intervals.begin(), intervals.end());
    std::vector<ValueId> active(registers, 0);
    i32 slots = 0;
    for (auto [start, v] : intervals) {
        NativeLocation loc;
        if (plan.blocks.size() == 1) {
            for (usize r = 0; r < registers; ++r) {
                if (active[r] == 0 || last[active[r]] < start) {
                    active[r] = v;
                    loc.reg = static_cast<int>(r);
                    break;
                }
            }
        }
        if (loc.reg < 0) {
            loc.slot = slots++;
            ++plan.spill_count;
        }
        plan.locations.emplace(v, loc);
    }
    plan.stack_bytes = (static_cast<usize>(slots) * 8 + 15) & ~usize{15};
    if (plan.stack_bytes > 32752)
        return error("Native spill frame is too large");
    return plan;
}
} // namespace vectortick::jit
