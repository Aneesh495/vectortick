#include "vectortick/jit/code_generator.hpp"
#include "vectortick/ir/instruction.hpp"
#include <algorithm>
#include <cstring>

namespace vectortick {
namespace jit {

Result<std::vector<u8>> A64CodeGenerator::generate(const ir::Function* function) noexcept {
    if (!function) {
        return make_error<std::vector<u8>>(StatusCode::InvalidArgument, "Null function");
    }
    
    // Reset state
    assembler_.clear();
    value_to_reg_.clear();
    value_to_slot_.clear();
    reg_to_value_.clear();
    remaining_uses_.clear();
    free_slots_.clear();
    next_spill_slot_ = 0;

    // Available general-purpose registers (24 registers):
    // X0 is return value / scratch.
    // X16, X17 are reserved for assembler scratch.
    // X19 is reserved for CanonicalEvent context pointer.
    // X29, X30 are FP, LR.
    free_regs_ = {
        A64Reg::X1,  A64Reg::X2,  A64Reg::X3,  A64Reg::X4,
        A64Reg::X5,  A64Reg::X6,  A64Reg::X7,  A64Reg::X8,
        A64Reg::X9,  A64Reg::X10, A64Reg::X11, A64Reg::X12,
        A64Reg::X13, A64Reg::X14, A64Reg::X15,
        A64Reg::X20, A64Reg::X21, A64Reg::X22, A64Reg::X23,
        A64Reg::X24, A64Reg::X25, A64Reg::X26, A64Reg::X27,
        A64Reg::X28
    };
    
    // Count uses of every operand across all blocks
    for (usize b = 0; b < function->num_blocks(); ++b) {
        const ir::BasicBlock* block = function->block(b);
        for (usize i = 0; i < block->num_instructions(); ++i) {
            const ir::Instruction* instr = block->instruction(i);
            for (usize o = 0; o < instr->num_operands(); ++o) {
                remaining_uses_[instr->operand(o)]++;
            }
        }
    }

    // Map function parameter 0 (row/context pointer) to X19
    if (function->num_parameters() > 0) {
        value_to_reg_[function->parameters()[0]] = A64Reg::X19;
    }
    
    // Emit prologue
    emit_prologue();
    
    // Generate code for each block
    for (usize b = 0; b < function->num_blocks(); ++b) {
        const ir::BasicBlock* block = function->block(b);
        
        for (usize i = 0; i < block->num_instructions(); ++i) {
            const ir::Instruction* instr = block->instruction(i);
            auto status = emit_instruction(instr);
            if (!status.ok()) {
                return make_error<std::vector<u8>>(status.code(), status.message());
            }
        }
    }
    
    // Emit epilogue
    emit_epilogue();
    
    // Copy code to vector of u8
    const u32* code32 = assembler_.data();
    usize num_bytes = assembler_.size_bytes();
    std::vector<u8> code(num_bytes);
    std::memcpy(code.data(), code32, num_bytes);
    
    return code;
}

void A64CodeGenerator::emit_prologue() noexcept {
    // Save frame pointer and link register
    assembler_.stp_pre(A64Reg::X29, A64Reg::X30, A64Reg::SP, -16);
    assembler_.mov_x64_x64(A64Reg::X29, A64Reg::SP);
    
    // Save callee-saved registers
    assembler_.stp_pre(A64Reg::X19, A64Reg::X20, A64Reg::SP, -16);
    assembler_.stp_pre(A64Reg::X21, A64Reg::X22, A64Reg::SP, -16);
    assembler_.stp_pre(A64Reg::X23, A64Reg::X24, A64Reg::SP, -16);
    assembler_.stp_pre(A64Reg::X25, A64Reg::X26, A64Reg::SP, -16);
    assembler_.stp_pre(A64Reg::X27, A64Reg::X28, A64Reg::SP, -16);

    // Save context pointer (X0) into callee-saved register X19
    assembler_.mov_x64_x64(A64Reg::X19, A64Reg::X0);

    // Allocate stack space for spills (2048 bytes, 16-byte aligned)
    assembler_.sub_x64_imm12(A64Reg::SP, A64Reg::SP, 2048);
}

void A64CodeGenerator::emit_epilogue() noexcept {
    // Restore stack pointer
    assembler_.add_x64_imm12(A64Reg::SP, A64Reg::SP, 2048);

    // Restore callee-saved registers
    assembler_.ldp_post(A64Reg::X27, A64Reg::X28, A64Reg::SP, 16);
    assembler_.ldp_post(A64Reg::X25, A64Reg::X26, A64Reg::SP, 16);
    assembler_.ldp_post(A64Reg::X23, A64Reg::X24, A64Reg::SP, 16);
    assembler_.ldp_post(A64Reg::X21, A64Reg::X22, A64Reg::SP, 16);
    assembler_.ldp_post(A64Reg::X19, A64Reg::X20, A64Reg::SP, 16);
    
    // Restore frame pointer and return
    assembler_.ldp_post(A64Reg::X29, A64Reg::X30, A64Reg::SP, 16);
    assembler_.ret();
}

i32 A64CodeGenerator::allocate_spill_slot() noexcept {
    if (!free_slots_.empty()) {
        i32 slot = free_slots_.back();
        free_slots_.pop_back();
        return slot;
    }
    return next_spill_slot_++;
}

void A64CodeGenerator::free_spill_slot(i32 slot) noexcept {
    free_slots_.push_back(slot);
}

void A64CodeGenerator::spill_reg(A64Reg reg) noexcept {
    auto it = reg_to_value_.find(static_cast<u8>(reg));
    if (it == reg_to_value_.end()) {
        return;
    }
    u32 val = it->second;
    i32 slot = allocate_spill_slot();
    assembler_.str_x64(reg, A64Reg::SP, slot * 8);
    value_to_slot_[val] = slot;
    value_to_reg_.erase(val);
    reg_to_value_.erase(it);
    free_regs_.push_back(reg);
}

A64Reg A64CodeGenerator::ensure_reg_for_operand(u32 value_id, const std::vector<A64Reg>& pinned) noexcept {
    auto reg_it = value_to_reg_.find(value_id);
    if (reg_it != value_to_reg_.end()) {
        return reg_it->second;
    }
    
    auto slot_it = value_to_slot_.find(value_id);
    if (slot_it != value_to_slot_.end()) {
        i32 slot = slot_it->second;
        if (free_regs_.empty()) {
            A64Reg victim = A64Reg::X1;
            for (const auto& [reg_code, assigned_val] : reg_to_value_) {
                A64Reg candidate = static_cast<A64Reg>(reg_code);
                if (std::find(pinned.begin(), pinned.end(), candidate) == pinned.end()) {
                    victim = candidate;
                    break;
                }
            }
            spill_reg(victim);
        }
        A64Reg reg = free_regs_.back();
        free_regs_.pop_back();
        assembler_.ldr_x64(reg, A64Reg::SP, slot * 8);
        value_to_reg_[value_id] = reg;
        reg_to_value_[static_cast<u8>(reg)] = value_id;
        return reg;
    }
    
    // Default fallback (e.g. context pointer parameter 0)
    return A64Reg::X19;
}

A64Reg A64CodeGenerator::allocate_reg_for_result(u32 value_id, const std::vector<A64Reg>& pinned) noexcept {
    if (free_regs_.empty()) {
        A64Reg victim = A64Reg::X1;
        for (const auto& [reg_code, assigned_val] : reg_to_value_) {
            A64Reg candidate = static_cast<A64Reg>(reg_code);
            if (std::find(pinned.begin(), pinned.end(), candidate) == pinned.end()) {
                victim = candidate;
                break;
            }
        }
        spill_reg(victim);
    }
    A64Reg reg = free_regs_.back();
    free_regs_.pop_back();
    value_to_reg_[value_id] = reg;
    reg_to_value_[static_cast<u8>(reg)] = value_id;
    return reg;
}

void A64CodeGenerator::consume_operand(u32 value_id) noexcept {
    auto uses_it = remaining_uses_.find(value_id);
    if (uses_it != remaining_uses_.end() && uses_it->second > 0) {
        uses_it->second--;
        if (uses_it->second == 0) {
            auto reg_it = value_to_reg_.find(value_id);
            if (reg_it != value_to_reg_.end()) {
                A64Reg reg = reg_it->second;
                if (reg != A64Reg::X19) {
                    reg_to_value_.erase(static_cast<u8>(reg));
                    value_to_reg_.erase(reg_it);
                    free_regs_.push_back(reg);
                }
            }
            auto slot_it = value_to_slot_.find(value_id);
            if (slot_it != value_to_slot_.end()) {
                free_spill_slot(slot_it->second);
                value_to_slot_.erase(slot_it);
            }
        }
    }
}

Status A64CodeGenerator::emit_instruction(const ir::Instruction* instr) noexcept {
    using namespace ir;
    
    switch (instr->opcode()) {
        case Opcode::Nop:
            assembler_.nop();
            break;
            
        case Opcode::ConstU64:
        case Opcode::ConstI64: {
            auto const_op = static_cast<const ConstOp*>(instr);
            A64Reg dst = allocate_reg_for_result(instr->result(), {});
            assembler_.mov_x64_imm64(dst, const_op->constant().get_u64());
            break;
        }
        
        case Opcode::ConstU32: {
            auto const_op = static_cast<const ConstOp*>(instr);
            A64Reg dst = allocate_reg_for_result(instr->result(), {});
            assembler_.movz_x64(dst, static_cast<u16>(const_op->constant().get_u32() & 0xFFFF), 0);
            if (const_op->constant().get_u32() > 0xFFFF) {
                assembler_.movk_x64(dst, static_cast<u16>(const_op->constant().get_u32() >> 16), 16);
            }
            break;
        }

        case Opcode::ConstBool: {
            auto const_op = static_cast<const ConstOp*>(instr);
            A64Reg dst = allocate_reg_for_result(instr->result(), {});
            assembler_.mov_x64_imm64(dst, const_op->constant().get_bool() ? 1ULL : 0ULL);
            break;
        }

        case Opcode::LoadColumn: {
            auto load_op = static_cast<const LoadColumnOp*>(instr);
            A64Reg dst = allocate_reg_for_result(instr->result(), {});
            u32 cid = load_op->column_id();
            switch (cid) {
                case 0:  assembler_.ldr_x64(dst, A64Reg::X19, 0); break;   // exchange_ts_ns (8B)
                case 1:  assembler_.ldr_x64(dst, A64Reg::X19, 8); break;   // receive_ts_ns (8B)
                case 2:  assembler_.ldr_x64(dst, A64Reg::X19, 16); break;  // sequence (8B)
                case 3:  assembler_.ldr_w32(dst, A64Reg::X19, 24); break;  // instrument_id (4B, zero-ext)
                case 4:  assembler_.ldrb(dst, A64Reg::X19, 28); break;     // event_type (1B, zero-ext)
                case 5:  assembler_.ldrb(dst, A64Reg::X19, 29); break;     // side (1B, zero-ext)
                case 6:  assembler_.ldrh(dst, A64Reg::X19, 30); break;     // flags (2B, zero-ext)
                case 7:  assembler_.ldr_x64(dst, A64Reg::X19, 32); break;  // price_ticks (8B)
                case 8:  assembler_.ldr_w32(dst, A64Reg::X19, 40); break;  // quantity (4B, zero-ext)
                case 9:  assembler_.ldrh(dst, A64Reg::X19, 44); break;     // venue_id (2B, zero-ext)
                case 10: assembler_.ldrh(dst, A64Reg::X19, 46); break;     // source_id (2B, zero-ext)
                case 11: assembler_.ldr_x64(dst, A64Reg::X19, 48); break;  // trade_or_order_id (8B)
                default: assembler_.mov_x64_imm64(dst, 0); break;
            }
            if (instr->num_operands() > 0) {
                consume_operand(instr->operand(0));
            }
            break;
        }
        
        case Opcode::AddU64:
        case Opcode::AddI64: {
            A64Reg lhs = ensure_reg_for_operand(instr->operand(0), {});
            A64Reg rhs = ensure_reg_for_operand(instr->operand(1), {lhs});
            A64Reg dst = allocate_reg_for_result(instr->result(), {lhs, rhs});
            assembler_.add_x64_x64_x64(dst, lhs, rhs);
            consume_operand(instr->operand(0));
            consume_operand(instr->operand(1));
            break;
        }
        
        case Opcode::SubU64:
        case Opcode::SubI64: {
            A64Reg lhs = ensure_reg_for_operand(instr->operand(0), {});
            A64Reg rhs = ensure_reg_for_operand(instr->operand(1), {lhs});
            A64Reg dst = allocate_reg_for_result(instr->result(), {lhs, rhs});
            assembler_.sub_x64_x64_x64(dst, lhs, rhs);
            consume_operand(instr->operand(0));
            consume_operand(instr->operand(1));
            break;
        }
        
        case Opcode::MulU64:
        case Opcode::MulI64: {
            A64Reg lhs = ensure_reg_for_operand(instr->operand(0), {});
            A64Reg rhs = ensure_reg_for_operand(instr->operand(1), {lhs});
            A64Reg dst = allocate_reg_for_result(instr->result(), {lhs, rhs});
            assembler_.mul_x64_x64_x64(dst, lhs, rhs);
            consume_operand(instr->operand(0));
            consume_operand(instr->operand(1));
            break;
        }

        case Opcode::DivU64: {
            A64Reg lhs = ensure_reg_for_operand(instr->operand(0), {});
            A64Reg rhs = ensure_reg_for_operand(instr->operand(1), {lhs});
            A64Reg dst = allocate_reg_for_result(instr->result(), {lhs, rhs});
            assembler_.udiv_x64_x64_x64(dst, lhs, rhs);
            assembler_.cmp_x64_x64(rhs, A64Reg::XZR);
            assembler_.csel_x64(dst, dst, A64Reg::XZR, A64Condition::NE);
            consume_operand(instr->operand(0));
            consume_operand(instr->operand(1));
            break;
        }

        case Opcode::ModU64: {
            A64Reg lhs = ensure_reg_for_operand(instr->operand(0), {});
            A64Reg rhs = ensure_reg_for_operand(instr->operand(1), {lhs});
            A64Reg dst = allocate_reg_for_result(instr->result(), {lhs, rhs});
            // Quotient in X16
            assembler_.udiv_x64_x64_x64(A64Reg::X16, lhs, rhs);
            // Remainder = lhs - (X16 * rhs)
            assembler_.msub_x64(dst, A64Reg::X16, rhs, lhs);
            assembler_.cmp_x64_x64(rhs, A64Reg::XZR);
            assembler_.csel_x64(dst, dst, A64Reg::XZR, A64Condition::NE);
            consume_operand(instr->operand(0));
            consume_operand(instr->operand(1));
            break;
        }

        case Opcode::DivI64: {
            A64Reg lhs = ensure_reg_for_operand(instr->operand(0), {});
            A64Reg rhs = ensure_reg_for_operand(instr->operand(1), {lhs});
            A64Reg dst = allocate_reg_for_result(instr->result(), {lhs, rhs});
            assembler_.sdiv_x64_x64_x64(dst, lhs, rhs);
            assembler_.cmp_x64_x64(rhs, A64Reg::XZR);
            assembler_.csel_x64(dst, dst, A64Reg::XZR, A64Condition::NE);
            consume_operand(instr->operand(0));
            consume_operand(instr->operand(1));
            break;
        }

        case Opcode::ModI64: {
            A64Reg lhs = ensure_reg_for_operand(instr->operand(0), {});
            A64Reg rhs = ensure_reg_for_operand(instr->operand(1), {lhs});
            A64Reg dst = allocate_reg_for_result(instr->result(), {lhs, rhs});
            // Quotient in X16
            assembler_.sdiv_x64_x64_x64(A64Reg::X16, lhs, rhs);
            // Remainder = lhs - (X16 * rhs)
            assembler_.msub_x64(dst, A64Reg::X16, rhs, lhs);
            assembler_.cmp_x64_x64(rhs, A64Reg::XZR);
            assembler_.csel_x64(dst, dst, A64Reg::XZR, A64Condition::NE);
            consume_operand(instr->operand(0));
            consume_operand(instr->operand(1));
            break;
        }

        case Opcode::NegI64: {
            A64Reg src = ensure_reg_for_operand(instr->operand(0), {});
            A64Reg dst = allocate_reg_for_result(instr->result(), {src});
            assembler_.sub_x64_x64_x64(dst, A64Reg::XZR, src);
            consume_operand(instr->operand(0));
            break;
        }

        case Opcode::Not: {
            A64Reg src = ensure_reg_for_operand(instr->operand(0), {});
            assembler_.cmp_x64_x64(src, A64Reg::XZR);
            consume_operand(instr->operand(0));
            A64Reg dst = allocate_reg_for_result(instr->result(), {});
            assembler_.cset_x64(dst, A64Condition::EQ);
            break;
        }

        case Opcode::BitNot: {
            A64Reg src = ensure_reg_for_operand(instr->operand(0), {});
            A64Reg dst = allocate_reg_for_result(instr->result(), {src});
            assembler_.mvn_x64(dst, src);
            consume_operand(instr->operand(0));
            break;
        }
        
        case Opcode::And:
        case Opcode::BitAnd: {
            A64Reg lhs = ensure_reg_for_operand(instr->operand(0), {});
            A64Reg rhs = ensure_reg_for_operand(instr->operand(1), {lhs});
            A64Reg dst = allocate_reg_for_result(instr->result(), {lhs, rhs});
            assembler_.and_x64_x64_x64(dst, lhs, rhs);
            consume_operand(instr->operand(0));
            consume_operand(instr->operand(1));
            break;
        }
        
        case Opcode::Or:
        case Opcode::BitOr: {
            A64Reg lhs = ensure_reg_for_operand(instr->operand(0), {});
            A64Reg rhs = ensure_reg_for_operand(instr->operand(1), {lhs});
            A64Reg dst = allocate_reg_for_result(instr->result(), {lhs, rhs});
            assembler_.orr_x64_x64_x64(dst, lhs, rhs);
            consume_operand(instr->operand(0));
            consume_operand(instr->operand(1));
            break;
        }
        
        case Opcode::BitXor: {
            A64Reg lhs = ensure_reg_for_operand(instr->operand(0), {});
            A64Reg rhs = ensure_reg_for_operand(instr->operand(1), {lhs});
            A64Reg dst = allocate_reg_for_result(instr->result(), {lhs, rhs});
            assembler_.eor_x64_x64_x64(dst, lhs, rhs);
            consume_operand(instr->operand(0));
            consume_operand(instr->operand(1));
            break;
        }
        
        // Unsigned comparisons
        case Opcode::EqU64:
        case Opcode::NeU64:
        case Opcode::LtU64:
        case Opcode::LeU64:
        case Opcode::GtU64:
        case Opcode::GeU64:
        // Signed comparisons
        case Opcode::EqI64:
        case Opcode::NeI64:
        case Opcode::LtI64:
        case Opcode::LeI64:
        case Opcode::GtI64:
        case Opcode::GeI64: {
            A64Reg lhs = ensure_reg_for_operand(instr->operand(0), {});
            A64Reg rhs = ensure_reg_for_operand(instr->operand(1), {lhs});
            
            assembler_.cmp_x64_x64(lhs, rhs);
            consume_operand(instr->operand(0));
            consume_operand(instr->operand(1));
            
            A64Condition cond = A64Condition::EQ;
            switch (instr->opcode()) {
                case Opcode::EqU64:
                case Opcode::EqI64: cond = A64Condition::EQ; break;
                case Opcode::NeU64:
                case Opcode::NeI64: cond = A64Condition::NE; break;
                case Opcode::LtU64: cond = A64Condition::LO; break;
                case Opcode::LeU64: cond = A64Condition::LS; break;
                case Opcode::GtU64: cond = A64Condition::HI; break;
                case Opcode::GeU64: cond = A64Condition::HS; break;
                case Opcode::LtI64: cond = A64Condition::LT; break;
                case Opcode::LeI64: cond = A64Condition::LE; break;
                case Opcode::GtI64: cond = A64Condition::GT; break;
                case Opcode::GeI64: cond = A64Condition::GE; break;
                default: break;
            }
            
            A64Reg dst = allocate_reg_for_result(instr->result(), {});
            assembler_.cset_x64(dst, cond);
            break;
        }

        case Opcode::Select: {
            auto sel_op = static_cast<const SelectOp*>(instr);
            A64Reg r_cond = ensure_reg_for_operand(sel_op->condition(), {});
            A64Reg r_true = ensure_reg_for_operand(sel_op->true_value(), {r_cond});
            A64Reg r_false = ensure_reg_for_operand(sel_op->false_value(), {r_cond, r_true});
            A64Reg r_dst = allocate_reg_for_result(instr->result(), {r_cond, r_true, r_false});
            
            assembler_.cmp_x64_x64(r_cond, A64Reg::XZR);
            assembler_.csel_x64(r_dst, r_true, r_false, A64Condition::NE);
            
            consume_operand(sel_op->condition());
            consume_operand(sel_op->true_value());
            consume_operand(sel_op->false_value());
            break;
        }
        
        case Opcode::Return: {
            if (instr->num_operands() > 0) {
                A64Reg ret_val = ensure_reg_for_operand(instr->operand(0), {});
                if (ret_val != A64Reg::X0) {
                    assembler_.mov_x64_x64(A64Reg::X0, ret_val);
                }
                consume_operand(instr->operand(0));
            } else {
                assembler_.mov_x64_imm64(A64Reg::X0, 0);
            }
            emit_epilogue();
            break;
        }
        
        default:
            return Status(StatusCode::NotImplemented, "Instruction not implemented in AArch64 JIT");
    }
    
    return Status::OK();
}

} // namespace jit
} // namespace vectortick
