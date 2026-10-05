#include "vectortick/jit/native_plan.hpp"
#include <cstring>

namespace vectortick::jit {
Result<std::vector<u8>> A64CodeGenerator::generate(const ir::Function *fn) noexcept {
    using namespace ir;
    auto planned = plan_native(fn, 13);
    if (!planned.ok())
        return make_error<std::vector<u8>>(planned.status().code(), planned.status().message());
    const auto &plan = planned.value();
    auto &a = assembler_;
    a.clear();
    constexpr A64Reg regs[] = {A64Reg::X1,  A64Reg::X2,  A64Reg::X3, A64Reg::X4,  A64Reg::X5,
                               A64Reg::X6,  A64Reg::X7,  A64Reg::X8, A64Reg::X13, A64Reg::X14,
                               A64Reg::X15, A64Reg::X16, A64Reg::X17};
    constexpr auto x = A64Reg::X9, y = A64Reg::X10, z = A64Reg::X11, w = A64Reg::X12;
    auto r = [](A64Reg v) { return A64Assembler::r_idx(v); };
    auto load = [&](ValueId v, A64Reg dst) {
        if (fn->num_parameters() && v == fn->parameters()[0]) {
            a.mov_x64_x64(dst, A64Reg::X19);
            return;
        }
        auto loc = plan.locations.at(v);
        if (loc.reg >= 0)
            a.mov_x64_x64(dst, regs[loc.reg]);
        else
            a.ldr_x64(dst, A64Reg::SP, loc.slot * 8);
    };
    auto save = [&](ValueId v, A64Reg src) {
        auto loc = plan.locations.at(v);
        if (loc.reg >= 0)
            a.mov_x64_x64(regs[loc.reg], src);
        else
            a.str_x64(src, A64Reg::SP, loc.slot * 8);
    };
    std::vector<usize> errors;
    auto fail = [&](A64Condition c) {
        errors.push_back(a.current_offset());
        a.b_cond(c, 0);
    };
    auto patch = [&](usize p, usize target) {
        a.patch_branch(p, static_cast<i32>(static_cast<i64>(target) - static_cast<i64>(p)));
    };
    auto stack = [&](bool subtract) {
        usize left = plan.stack_bytes;
        while (left) {
            auto n = static_cast<u16>(std::min(left, usize{4080}));
            if (subtract)
                a.sub_x64_imm12(A64Reg::SP, A64Reg::SP, n);
            else
                a.add_x64_imm12(A64Reg::SP, A64Reg::SP, n);
            left -= n;
        }
    };
    auto epilogue = [&]() {
        stack(false);
        a.ldp_post(A64Reg::X19, A64Reg::X20, A64Reg::SP, 16);
        a.ldp_post(A64Reg::X29, A64Reg::X30, A64Reg::SP, 16);
        a.ret();
    };
    a.stp_pre(A64Reg::X29, A64Reg::X30, A64Reg::SP, -16);
    a.stp_pre(A64Reg::X19, A64Reg::X20, A64Reg::SP, -16);
    a.mov_x64_x64(A64Reg::X19, A64Reg::X0);
    a.mov_x64_x64(A64Reg::X20, A64Reg::X1);
    stack(true);
    a.str_x64(A64Reg::XZR, A64Reg::X20, 0);
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
        jumps.emplace_back(a.current_offset(), target);
        a.b(0);
    };
    constexpr i32 offsets[] = {0, 8, 16, 24, 28, 29, 30, 32, 40, 44, 46, 48};
    constexpr int widths[] = {8, 8, 8, 4, 1, 1, 2, 8, 4, 2, 2, 8};
    for (auto block : plan.blocks) {
        labels[block] = a.current_offset();
        for (const auto &ip : block->instructions()) {
            auto in = ip.get();
            auto op = in->opcode();
            if (op == Opcode::Nop || op == Opcode::Phi)
                continue;
            if (op == Opcode::Jump) {
                edge(block, static_cast<const JumpOp *>(in)->target());
                continue;
            }
            if (op == Opcode::Branch) {
                auto br = static_cast<const BranchOp *>(in);
                load(br->condition(), x);
                a.cmp_x64_x64(x, A64Reg::XZR);
                auto p = a.current_offset();
                a.b_cond(A64Condition::EQ, 0);
                edge(block, br->true_block());
                patch(p, a.current_offset());
                edge(block, br->false_block());
                continue;
            }
            if (op == Opcode::Return) {
                if (in->num_operands())
                    load(in->operand(0), A64Reg::X0);
                else
                    a.mov_x64_imm64(A64Reg::X0, 0);
                epilogue();
                continue;
            }
            if (op >= Opcode::ConstI64 && op <= Opcode::ConstBool)
                a.mov_x64_imm64(x, static_cast<const ConstOp *>(in)->constant().as_u64());
            else if (op == Opcode::LoadColumn) {
                auto col = static_cast<const LoadColumnOp *>(in)->column_id();
                if (widths[col] == 8)
                    a.ldr_x64(x, A64Reg::X19, offsets[col]);
                if (widths[col] == 4)
                    a.ldr_w32(x, A64Reg::X19, offsets[col]);
                if (widths[col] == 2)
                    a.ldrh(x, A64Reg::X19, offsets[col]);
                if (widths[col] == 1)
                    a.ldrb(x, A64Reg::X19, offsets[col]);
            } else {
                load(in->operand(0), x);
                if (in->num_operands() > 1)
                    load(in->operand(1), y);
                switch (op) {
                case Opcode::AddI64:
                case Opcode::AddU64:
                    a.emit(0xAB000000 | (r(y) << 16) | (r(x) << 5) | r(x));
                    fail(op == Opcode::AddI64 ? A64Condition::VS : A64Condition::CS);
                    break;
                case Opcode::SubI64:
                case Opcode::SubU64:
                    a.emit(0xEB000000 | (r(y) << 16) | (r(x) << 5) | r(x));
                    fail(op == Opcode::SubI64 ? A64Condition::VS : A64Condition::CC);
                    break;
                case Opcode::MulI64:
                case Opcode::MulU64:
                    a.emit((op == Opcode::MulI64 ? 0x9B407C00 : 0x9BC07C00) | (r(y) << 16) |
                           (r(x) << 5) | r(z));
                    a.mul_x64_x64_x64(x, x, y);
                    if (op == Opcode::MulI64) {
                        a.emit(0x937FFC00 | (r(x) << 5) | r(w));
                        a.cmp_x64_x64(z, w);
                    } else
                        a.cmp_x64_x64(z, A64Reg::XZR);
                    fail(A64Condition::NE);
                    break;
                case Opcode::DivI64:
                case Opcode::ModI64:
                case Opcode::DivU64:
                case Opcode::ModU64: {
                    bool signed_op = op == Opcode::DivI64 || op == Opcode::ModI64;
                    bool mod = op == Opcode::ModI64 || op == Opcode::ModU64;
                    a.cmp_x64_x64(y, A64Reg::XZR);
                    fail(A64Condition::EQ);
                    if (op == Opcode::DivI64) {
                        a.mov_x64_imm64(z, 0x8000000000000000ULL);
                        a.cmp_x64_x64(x, z);
                        auto p = a.current_offset();
                        a.b_cond(A64Condition::NE, 0);
                        a.mov_x64_imm64(z, ~u64{0});
                        a.cmp_x64_x64(y, z);
                        fail(A64Condition::EQ);
                        patch(p, a.current_offset());
                    }
                    if (signed_op)
                        a.sdiv_x64_x64_x64(z, x, y);
                    else
                        a.udiv_x64_x64_x64(z, x, y);
                    if (mod)
                        a.msub_x64(x, z, y, x);
                    else
                        a.mov_x64_x64(x, z);
                    break;
                }
                case Opcode::NegI64:
                    a.emit(0xEB0003E0 | (r(x) << 16) | r(x));
                    fail(A64Condition::VS);
                    break;
                case Opcode::U64ToI64:
                    a.cmp_x64_x64(x, A64Reg::XZR);
                    fail(A64Condition::LT);
                    break;
                case Opcode::NegU64:
                    a.mov_x64_imm64(y, 0x8000000000000000ULL);
                    a.cmp_x64_x64(x, y);
                    fail(A64Condition::HI);
                    a.sub_x64_x64_x64(x, A64Reg::XZR, x);
                    break;
                case Opcode::And:
                case Opcode::Or:
                    a.cmp_x64_x64(x, A64Reg::XZR);
                    a.cset_x64(x, A64Condition::NE);
                    a.cmp_x64_x64(y, A64Reg::XZR);
                    a.cset_x64(y, A64Condition::NE);
                    if (op == Opcode::And)
                        a.and_x64_x64_x64(x, x, y);
                    else
                        a.orr_x64_x64_x64(x, x, y);
                    break;
                case Opcode::Not:
                    a.cmp_x64_x64(x, A64Reg::XZR);
                    a.cset_x64(x, A64Condition::EQ);
                    break;
                case Opcode::BitAnd:
                    a.and_x64_x64_x64(x, x, y);
                    break;
                case Opcode::BitOr:
                    a.orr_x64_x64_x64(x, x, y);
                    break;
                case Opcode::BitXor:
                    a.eor_x64_x64_x64(x, x, y);
                    break;
                case Opcode::BitNot:
                    a.mvn_x64(x, x);
                    break;
                case Opcode::Select:
                    load(in->operand(2), z);
                    a.cmp_x64_x64(x, A64Reg::XZR);
                    a.csel_x64(x, y, z, A64Condition::NE);
                    break;
                case Opcode::Shl:
                case Opcode::Shr:
                    a.mov_x64_imm64(z, 64);
                    a.cmp_x64_x64(y, z);
                    fail(A64Condition::HS);
                    a.emit((op == Opcode::Shl ? 0x9AC02000 : 0x9AC02400) | (r(y) << 16) |
                           (r(x) << 5) | r(x));
                    break;
                case Opcode::ExtendI32ToI64:
                    a.emit(0x93407C00 | (r(x) << 5) | r(x));
                    break;
                case Opcode::ExtendU32ToU64:
                case Opcode::TruncI64ToI32:
                    a.mov_w32_w32(x, x);
                    break;
                default: {
                    constexpr A64Condition conditions[] = {
                        A64Condition::EQ, A64Condition::NE, A64Condition::LT, A64Condition::LE,
                        A64Condition::GT, A64Condition::GE, A64Condition::EQ, A64Condition::NE,
                        A64Condition::LO, A64Condition::LS, A64Condition::HI, A64Condition::HS};
                    if (!is_comparison(op))
                        return make_error<std::vector<u8>>(StatusCode::NotImplemented,
                                                           "Unsupported AArch64 opcode");
                    a.cmp_x64_x64(x, y);
                    a.cset_x64(x,
                               conditions[static_cast<int>(op) - static_cast<int>(Opcode::EqI64)]);
                    break;
                }
                }
            }
            save(in->result(), x);
        }
    }
    auto error_label = a.current_offset();
    a.mov_x64_imm64(x, 1);
    a.str_x64(x, A64Reg::X20, 0);
    a.mov_x64_imm64(A64Reg::X0, 0);
    epilogue();
    for (auto p : errors) {
        auto delta = static_cast<i64>(error_label) - static_cast<i64>(p);
        if (delta < -(i64{1} << 18) || delta >= (i64{1} << 18))
            return make_error<std::vector<u8>>(StatusCode::InvalidArgument,
                                               "Native error branch is too far");
        patch(p, error_label);
    }
    for (auto [p, b] : jumps) {
        auto delta = static_cast<i64>(labels.at(b)) - static_cast<i64>(p);
        if (delta < -(i64{1} << 25) || delta >= (i64{1} << 25))
            return make_error<std::vector<u8>>(StatusCode::InvalidArgument,
                                               "Native branch is too far");
        patch(p, labels.at(b));
    }
    std::vector<u8> code(a.size_bytes());
    std::memcpy(code.data(), a.bytes(), code.size());
    return code;
}
} // namespace vectortick::jit
