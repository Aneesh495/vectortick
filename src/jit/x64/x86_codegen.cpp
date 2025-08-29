#include "vectortick/jit/native_plan.hpp"
#include "vectortick/model/event.hpp"
#include <limits>

namespace vectortick::jit {
Result<std::vector<u8>> X86CodeGenerator::generate(const ir::Function *fn) noexcept {
    using namespace ir;
#if defined(_WIN32)
    return make_error<std::vector<u8>>(StatusCode::NotImplemented,
                                       "Windows native ABI is unsupported");
#endif
    auto planned = plan_native(fn, 6);
    if (!planned.ok())
        return make_error<std::vector<u8>>(planned.status().code(), planned.status().message());
    const auto &plan = planned.value();
    auto &a = assembler_;
    a.clear();
    constexpr X86Reg regs[] = {X86Reg::RSI, X86Reg::R8,  X86Reg::R9,
                               X86Reg::RBX, X86Reg::R14, X86Reg::R15};
    constexpr auto x = X86Reg::RAX, y = X86Reg::R10, z = X86Reg::R11;
    i32 frame = static_cast<i32>(plan.stack_bytes + 8);
    auto load = [&](ValueId v, X86Reg dst) {
        if (fn->num_parameters() && v == fn->parameters()[0]) {
            a.mov_r64_r64(dst, X86Reg::R12);
            return;
        }
        auto loc = plan.locations.at(v);
        if (loc.reg >= 0)
            a.mov_r64_r64(dst, regs[loc.reg]);
        else
            a.mov_r64_mem(dst, X86Reg::RSP, loc.slot * 8);
    };
    auto save = [&](ValueId v, X86Reg src) {
        auto loc = plan.locations.at(v);
        if (loc.reg >= 0)
            a.mov_r64_r64(regs[loc.reg], src);
        else
            a.mov_mem_r64(X86Reg::RSP, src, loc.slot * 8);
    };
    std::vector<usize> errors;
    auto fail = [&](Condition c) {
        a.jcc_rel32(c, 0);
        errors.push_back(a.current_offset() - 4);
    };
    auto patch = [&](usize pos, usize target) {
        a.patch32(pos, static_cast<u32>(static_cast<i64>(target) - static_cast<i64>(pos + 4)));
    };
    auto epilogue = [&]() {
        a.add_r64_imm32(X86Reg::RSP, frame);
        for (auto r :
             {X86Reg::R15, X86Reg::R14, X86Reg::R13, X86Reg::R12, X86Reg::RBX, X86Reg::RBP})
            a.pop_r64(r);
        a.ret();
    };
    for (auto r : {X86Reg::RBP, X86Reg::RBX, X86Reg::R12, X86Reg::R13, X86Reg::R14, X86Reg::R15})
        a.push_r64(r);
    a.mov_r64_r64(X86Reg::R12, X86Reg::RDI);
    a.mov_r64_r64(X86Reg::R13, X86Reg::RSI);
    a.add_r64_imm32(X86Reg::RSP, -frame);
    a.mov_r64_imm64(x, 0);
    a.mov_mem_r64(X86Reg::R13, x, 0);
    std::map<const BasicBlock *, usize> labels;
    std::vector<std::pair<usize, const BasicBlock *>> jumps;
    auto edge = [&](const BasicBlock *from, const BasicBlock *target) {
        for (const auto &ip : target->instructions())
            if (ip->opcode() == Opcode::Phi) {
                auto phi = static_cast<const PhiOp *>(ip.get());
                for (usize k = 0; k < phi->num_operands(); ++k)
                    if (phi->incoming_blocks()[k] == from) {
                        load(phi->operand(k), x);
                        save(phi->result(), x);
                    }
            }
        a.jmp_rel32(0);
        jumps.emplace_back(a.current_offset() - 4, target);
    };
    constexpr i32 offsets[] = {0, 8, 16, 24, 28, 29, 30, 32, 40, 44, 46, 48};
    constexpr int widths[] = {8, 8, 8, 4, 1, 1, 2, 8, 4, 2, 2, 8};
    for (auto block : plan.blocks) {
        labels[block] = a.current_offset();
        for (const auto &ip : block->instructions()) {
            auto in = ip.get();
            auto op = in->opcode();
            if (op == Opcode::Phi || op == Opcode::Nop)
                continue;
            if (op == Opcode::Jump) {
                edge(block, static_cast<const JumpOp *>(in)->target());
                continue;
            }
            if (op == Opcode::Branch) {
                auto br = static_cast<const BranchOp *>(in);
                load(br->condition(), x);
                a.test_r64_r64(x, x);
                a.jcc_rel32(Condition::E, 0);
                auto pos = a.current_offset() - 4;
                edge(block, br->true_block());
                patch(pos, a.current_offset());
                edge(block, br->false_block());
                continue;
            }
            if (op == Opcode::Return) {
                if (in->num_operands())
                    load(in->operand(0), x);
                else
                    a.mov_r64_imm64(x, 0);
                epilogue();
                continue;
            }
            if (op >= Opcode::ConstI64 && op <= Opcode::ConstBool) {
                a.mov_r64_imm64(x, static_cast<const ConstOp *>(in)->constant().as_u64());
            } else if (op == Opcode::LoadColumn) {
                auto col = static_cast<const LoadColumnOp *>(in)->column_id();
                if (widths[col] == 8)
                    a.mov_r64_mem(x, X86Reg::R12, offsets[col]);
                if (widths[col] == 4)
                    a.mov_r32_mem(x, X86Reg::R12, offsets[col]);
                if (widths[col] == 2)
                    a.movzx_r64_mem16(x, X86Reg::R12, offsets[col]);
                if (widths[col] == 1)
                    a.movzx_r64_mem8(x, X86Reg::R12, offsets[col]);
            } else {
                load(in->operand(0), x);
                if (in->num_operands() > 1)
                    load(in->operand(1), y);
                switch (op) {
                case Opcode::AddI64:
                case Opcode::AddU64:
                    a.add_r64_r64(x, y);
                    fail(op == Opcode::AddI64 ? Condition::O : Condition::B);
                    break;
                case Opcode::SubI64:
                case Opcode::SubU64:
                    a.sub_r64_r64(x, y);
                    fail(op == Opcode::SubI64 ? Condition::O : Condition::B);
                    break;
                case Opcode::MulI64:
                    a.imul_r64_r64(x, y);
                    fail(Condition::O);
                    break;
                case Opcode::MulU64:
                    a.emit_rex64_b(y);
                    a.emit(0xF7);
                    a.emit(a.modrm(3, static_cast<u8>(4), y));
                    a.test_r64_r64(X86Reg::RDX, X86Reg::RDX);
                    fail(Condition::NE);
                    break;
                case Opcode::DivI64:
                case Opcode::ModI64:
                case Opcode::DivU64:
                case Opcode::ModU64: {
                    bool signed_op = op == Opcode::DivI64 || op == Opcode::ModI64;
                    bool mod = op == Opcode::ModI64 || op == Opcode::ModU64;
                    a.test_r64_r64(y, y);
                    fail(Condition::E);
                    usize special = 0, done = 0;
                    if (signed_op) {
                        a.mov_r64_imm64(z, 0x8000000000000000ULL);
                        a.cmp_r64_r64(x, z);
                        a.jcc_rel32(Condition::NE, 0);
                        auto normal1 = a.current_offset() - 4;
                        a.mov_r64_imm64(z, ~u64{0});
                        a.cmp_r64_r64(y, z);
                        if (!mod)
                            fail(Condition::E);
                        else {
                            a.jcc_rel32(Condition::E, 0);
                            special = a.current_offset() - 4;
                        }
                        patch(normal1, a.current_offset());
                        a.cqo();
                        a.idiv_r64(y);
                    } else {
                        a.xor_r64_r64(X86Reg::RDX, X86Reg::RDX);
                        a.div_r64(y);
                    }
                    if (mod)
                        a.mov_r64_r64(x, X86Reg::RDX);
                    if (special) {
                        a.jmp_rel32(0);
                        done = a.current_offset() - 4;
                        patch(special, a.current_offset());
                        a.mov_r64_imm64(x, 0);
                        patch(done, a.current_offset());
                    }
                    break;
                }
                case Opcode::NegI64:
                    a.neg_r64(x);
                    fail(Condition::O);
                    break;
                case Opcode::U64ToI64:
                    a.test_r64_r64(x, x);
                    fail(Condition::S);
                    break;
                case Opcode::NegU64:
                    a.mov_r64_imm64(y, 0x8000000000000000ULL);
                    a.cmp_r64_r64(x, y);
                    fail(Condition::A);
                    a.neg_r64(x);
                    break;
                case Opcode::And:
                case Opcode::Or:
                    a.test_r64_r64(x, x);
                    a.setcc(Condition::NE, x);
                    a.movzx_r64_r8(x, x);
                    a.test_r64_r64(y, y);
                    a.setcc(Condition::NE, y);
                    a.movzx_r64_r8(y, y);
                    if (op == Opcode::And)
                        a.and_r64_r64(x, y);
                    else
                        a.or_r64_r64(x, y);
                    break;
                case Opcode::Not:
                    a.test_r64_r64(x, x);
                    a.setcc(Condition::E, x);
                    a.movzx_r64_r8(x, x);
                    break;
                case Opcode::BitAnd:
                    a.and_r64_r64(x, y);
                    break;
                case Opcode::BitOr:
                    a.or_r64_r64(x, y);
                    break;
                case Opcode::BitXor:
                    a.xor_r64_r64(x, y);
                    break;
                case Opcode::BitNot:
                    a.not_r64(x);
                    break;
                case Opcode::Select:
                    load(in->operand(2), z);
                    a.test_r64_r64(x, x);
                    a.mov_r64_r64(x, z);
                    a.cmovcc_r64_r64(Condition::NE, x, y);
                    break;
                case Opcode::Shl:
                case Opcode::Shr:
                    a.mov_r64_imm64(z, 64);
                    a.cmp_r64_r64(y, z);
                    fail(Condition::AE);
                    a.mov_r64_r64(X86Reg::RCX, y);
                    a.rex_w();
                    a.emit(0xD3);
                    a.emit(op == Opcode::Shl ? 0xE0 : 0xE8);
                    break;
                case Opcode::ExtendI32ToI64:
                    a.rex_w();
                    a.emit(0x63);
                    a.emit(0xC0);
                    break;
                case Opcode::ExtendU32ToU64:
                case Opcode::TruncI64ToI32:
                    a.emit(0x89);
                    a.emit(0xC0);
                    break;
                default: {
                    constexpr Condition conditions[] = {
                        Condition::E, Condition::NE, Condition::L, Condition::LE,
                        Condition::G, Condition::GE, Condition::E, Condition::NE,
                        Condition::B, Condition::BE, Condition::A, Condition::AE};
                    if (!is_comparison(op))
                        return make_error<std::vector<u8>>(StatusCode::NotImplemented,
                                                           "Unsupported x86 opcode");
                    a.cmp_r64_r64(x, y);
                    a.setcc(conditions[static_cast<int>(op) - static_cast<int>(Opcode::EqI64)], x);
                    a.movzx_r64_r8(x, x);
                    break;
                }
                }
            }
            save(in->result(), x);
        }
    }
    auto error_label = a.current_offset();
    a.mov_r64_imm64(x, 1);
    a.mov_mem_r64(X86Reg::R13, x, 0);
    a.mov_r64_imm64(x, 0);
    epilogue();
    for (auto p : errors)
        patch(p, error_label);
    for (auto [p, b] : jumps)
        patch(p, labels.at(b));
    return std::vector<u8>(a.code());
}
} // namespace vectortick::jit
