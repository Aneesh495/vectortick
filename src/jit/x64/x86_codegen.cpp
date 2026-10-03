#include "vectortick/jit/code_generator.hpp"
#include "vectortick/ir/instruction.hpp"
#include <algorithm>
#include <cstring>

namespace vectortick {
namespace jit {

Result<std::vector<u8>> X86CodeGenerator::generate(const ir::Function* function) noexcept {
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

    // Available general-purpose registers:
    // RAX and RDX are reserved for division/modulo and return.
    // R12 is reserved for CanonicalEvent context pointer.
    // RSP, RBP are stack frame pointers.
    // Registers for variables (11 registers):
    free_regs_ = {
        X86Reg::RSI, X86Reg::RDI, X86Reg::R8, X86Reg::R9, X86Reg::R10,
        X86Reg::R11, X86Reg::RBX, X86Reg::R13, X86Reg::R14, X86Reg::R15,
        X86Reg::RCX
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

    // Map function parameter 0 (row/context pointer) to R12
    if (function->num_parameters() > 0) {
        value_to_reg_[function->parameters()[0]] = X86Reg::R12;
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
    
    // Emit epilogue (in case function doesn't return explicitly)
    emit_epilogue();
    
    // Copy code to vector
    std::vector<u8> code(assembler_.code());
    return code;
}

void X86CodeGenerator::emit_prologue() noexcept {
    // Save callee-saved registers
    assembler_.push_r64(X86Reg::RBP);
    assembler_.mov_r64_r64(X86Reg::RBP, X86Reg::RSP);
    assembler_.push_r64(X86Reg::RBX);
    assembler_.push_r64(X86Reg::R12);
    assembler_.push_r64(X86Reg::R13);
    assembler_.push_r64(X86Reg::R14);
    assembler_.push_r64(X86Reg::R15);
    
    // Save argument 0 (context pointer) into callee-saved R12
#if defined(_WIN32)
    assembler_.mov_r64_r64(X86Reg::R12, X86Reg::RCX);
#else
    assembler_.mov_r64_r64(X86Reg::R12, X86Reg::RDI);
#endif

    // Allocate stack space for spills (2056 bytes preserves 16-byte alignment)
    assembler_.add_r64_imm32(X86Reg::RSP, -2056);
}

void X86CodeGenerator::emit_epilogue() noexcept {
    // Restore stack pointer
    assembler_.add_r64_imm32(X86Reg::RSP, 2056);
    
    // Restore callee-saved registers
    assembler_.pop_r64(X86Reg::R15);
    assembler_.pop_r64(X86Reg::R14);
    assembler_.pop_r64(X86Reg::R13);
    assembler_.pop_r64(X86Reg::R12);
    assembler_.pop_r64(X86Reg::RBX);
    assembler_.pop_r64(X86Reg::RBP);
    assembler_.ret();
}

i32 X86CodeGenerator::allocate_spill_slot() noexcept {
    if (!free_slots_.empty()) {
        i32 slot = free_slots_.back();
        free_slots_.pop_back();
        return slot;
    }
    return next_spill_slot_++;
}

void X86CodeGenerator::free_spill_slot(i32 slot) noexcept {
    free_slots_.push_back(slot);
}

void X86CodeGenerator::spill_reg(X86Reg reg) noexcept {
    auto it = reg_to_value_.find(static_cast<u8>(reg));
    if (it == reg_to_value_.end()) {
        return;
    }
    u32 val = it->second;
    i32 slot = allocate_spill_slot();
    assembler_.mov_mem_r64(X86Reg::RSP, reg, slot * 8);
    value_to_slot_[val] = slot;
    value_to_reg_.erase(val);
    reg_to_value_.erase(it);
    free_regs_.push_back(reg);
}

X86Reg X86CodeGenerator::ensure_reg_for_operand(u32 value_id, const std::vector<X86Reg>& pinned) noexcept {
    auto reg_it = value_to_reg_.find(value_id);
    if (reg_it != value_to_reg_.end()) {
        return reg_it->second;
    }
    
    auto slot_it = value_to_slot_.find(value_id);
    if (slot_it != value_to_slot_.end()) {
        i32 slot = slot_it->second;
        if (free_regs_.empty()) {
            X86Reg victim = X86Reg::RAX;
            for (const auto& [reg_code, assigned_val] : reg_to_value_) {
                X86Reg candidate = static_cast<X86Reg>(reg_code);
                if (std::find(pinned.begin(), pinned.end(), candidate) == pinned.end()) {
                    victim = candidate;
                    break;
                }
            }
            spill_reg(victim);
        }
        X86Reg reg = free_regs_.back();
        free_regs_.pop_back();
        assembler_.mov_r64_mem(reg, X86Reg::RSP, slot * 8);
        value_to_reg_[value_id] = reg;
        reg_to_value_[static_cast<u8>(reg)] = value_id;
        return reg;
    }
    
    // Default fallback (e.g. context pointer parameter 0)
    return X86Reg::R12;
}

X86Reg X86CodeGenerator::allocate_reg_for_result(u32 value_id, const std::vector<X86Reg>& pinned) noexcept {
    if (free_regs_.empty()) {
        X86Reg victim = X86Reg::RAX;
        for (const auto& [reg_code, assigned_val] : reg_to_value_) {
            X86Reg candidate = static_cast<X86Reg>(reg_code);
            if (std::find(pinned.begin(), pinned.end(), candidate) == pinned.end()) {
                victim = candidate;
                break;
            }
        }
        spill_reg(victim);
    }
    X86Reg reg = free_regs_.back();
    free_regs_.pop_back();
    value_to_reg_[value_id] = reg;
    reg_to_value_[static_cast<u8>(reg)] = value_id;
    return reg;
}

void X86CodeGenerator::consume_operand(u32 value_id) noexcept {
    auto uses_it = remaining_uses_.find(value_id);
    if (uses_it != remaining_uses_.end() && uses_it->second > 0) {
        uses_it->second--;
        if (uses_it->second == 0) {
            auto reg_it = value_to_reg_.find(value_id);
            if (reg_it != value_to_reg_.end()) {
                X86Reg reg = reg_it->second;
                if (reg != X86Reg::R12) {
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

Status X86CodeGenerator::emit_instruction(const ir::Instruction* instr) noexcept {
    using namespace ir;
    
    switch (instr->opcode()) {
        case Opcode::Nop:
            assembler_.nop();
            break;
            
        case Opcode::ConstU64:
        case Opcode::ConstI64: {
            auto const_op = static_cast<const ConstOp*>(instr);
            X86Reg dst = allocate_reg_for_result(instr->result(), {});
            assembler_.mov_r64_imm64(dst, const_op->constant().get_u64());
            break;
        }
        
        case Opcode::ConstU32: {
            auto const_op = static_cast<const ConstOp*>(instr);
            X86Reg dst = allocate_reg_for_result(instr->result(), {});
            assembler_.mov_r32_imm32(dst, const_op->constant().get_u32());
            break;
        }

        case Opcode::ConstBool: {
            auto const_op = static_cast<const ConstOp*>(instr);
            X86Reg dst = allocate_reg_for_result(instr->result(), {});
            assembler_.mov_r64_imm64(dst, const_op->constant().get_bool() ? 1ULL : 0ULL);
            break;
        }

        case Opcode::LoadColumn: {
            auto load_op = static_cast<const LoadColumnOp*>(instr);
            X86Reg dst = allocate_reg_for_result(instr->result(), {});
            u32 cid = load_op->column_id();
            switch (cid) {
                case 0:  assembler_.mov_r64_mem(dst, X86Reg::R12, 0); break;     // exchange_ts_ns (8B)
                case 1:  assembler_.mov_r64_mem(dst, X86Reg::R12, 8); break;     // receive_ts_ns (8B)
                case 2:  assembler_.mov_r64_mem(dst, X86Reg::R12, 16); break;    // sequence (8B)
                case 3:  assembler_.mov_r32_mem(dst, X86Reg::R12, 24); break;    // instrument_id (4B, zero-ext)
                case 4:  assembler_.movzx_r64_mem8(dst, X86Reg::R12, 28); break; // event_type (1B, zero-ext)
                case 5:  assembler_.movzx_r64_mem8(dst, X86Reg::R12, 29); break; // side (1B, zero-ext)
                case 6:  assembler_.movzx_r64_mem16(dst, X86Reg::R12, 30); break;// flags (2B, zero-ext)
                case 7:  assembler_.mov_r64_mem(dst, X86Reg::R12, 32); break;    // price_ticks (8B)
                case 8:  assembler_.mov_r32_mem(dst, X86Reg::R12, 40); break;    // quantity (4B, zero-ext)
                case 9:  assembler_.movzx_r64_mem16(dst, X86Reg::R12, 44); break;// venue_id (2B, zero-ext)
                case 10: assembler_.movzx_r64_mem16(dst, X86Reg::R12, 46); break;// source_id (2B, zero-ext)
                case 11: assembler_.mov_r64_mem(dst, X86Reg::R12, 48); break;    // trade_or_order_id (8B)
                default: assembler_.mov_r64_imm64(dst, 0); break;
            }
            if (instr->num_operands() > 0) {
                consume_operand(instr->operand(0));
            }
            break;
        }
        
        case Opcode::AddU64:
        case Opcode::AddI64: {
            X86Reg lhs = ensure_reg_for_operand(instr->operand(0), {});
            X86Reg rhs = ensure_reg_for_operand(instr->operand(1), {lhs});
            X86Reg dst = allocate_reg_for_result(instr->result(), {lhs, rhs});
            assembler_.mov_r64_r64(dst, lhs);
            assembler_.add_r64_r64(dst, rhs);
            consume_operand(instr->operand(0));
            consume_operand(instr->operand(1));
            break;
        }
        
        case Opcode::SubU64:
        case Opcode::SubI64: {
            X86Reg lhs = ensure_reg_for_operand(instr->operand(0), {});
            X86Reg rhs = ensure_reg_for_operand(instr->operand(1), {lhs});
            X86Reg dst = allocate_reg_for_result(instr->result(), {lhs, rhs});
            assembler_.mov_r64_r64(dst, lhs);
            assembler_.sub_r64_r64(dst, rhs);
            consume_operand(instr->operand(0));
            consume_operand(instr->operand(1));
            break;
        }
        
        case Opcode::MulU64:
        case Opcode::MulI64: {
            X86Reg lhs = ensure_reg_for_operand(instr->operand(0), {});
            X86Reg rhs = ensure_reg_for_operand(instr->operand(1), {lhs});
            X86Reg dst = allocate_reg_for_result(instr->result(), {lhs, rhs});
            assembler_.mov_r64_r64(dst, lhs);
            assembler_.imul_r64_r64(dst, rhs);
            consume_operand(instr->operand(0));
            consume_operand(instr->operand(1));
            break;
        }

        case Opcode::DivU64:
        case Opcode::ModU64: {
            bool is_mod = (instr->opcode() == Opcode::ModU64);
            X86Reg lhs = ensure_reg_for_operand(instr->operand(0), {});
            X86Reg rhs = ensure_reg_for_operand(instr->operand(1), {lhs});
            X86Reg dst = allocate_reg_for_result(instr->result(), {lhs, rhs});
            
            // Check divisor == 0
            assembler_.test_r64_r64(rhs, rhs);
            assembler_.jcc_rel32(Condition::E, 0);
            usize jmp_zero = assembler_.current_offset() - 4;

            // Non-zero divisor
            assembler_.mov_r64_r64(X86Reg::RAX, lhs);
            assembler_.xor_r64_r64(X86Reg::RDX, X86Reg::RDX);
            assembler_.div_r64(rhs);
            assembler_.mov_r64_r64(dst, is_mod ? X86Reg::RDX : X86Reg::RAX);
            assembler_.jmp_rel32(0);
            usize jmp_done = assembler_.current_offset() - 4;

            // Zero divisor: return 0 gracefully
            usize zero_label = assembler_.current_offset();
            assembler_.patch32(jmp_zero, static_cast<u32>(zero_label - (jmp_zero + 4)));
            assembler_.xor_r64_r64(dst, dst);

            // Done
            usize done_label = assembler_.current_offset();
            assembler_.patch32(jmp_done, static_cast<u32>(done_label - (jmp_done + 4)));

            consume_operand(instr->operand(0));
            consume_operand(instr->operand(1));
            break;
        }

        case Opcode::DivI64:
        case Opcode::ModI64: {
            bool is_mod = (instr->opcode() == Opcode::ModI64);
            X86Reg lhs = ensure_reg_for_operand(instr->operand(0), {});
            X86Reg rhs = ensure_reg_for_operand(instr->operand(1), {lhs});
            X86Reg dst = allocate_reg_for_result(instr->result(), {lhs, rhs});
            
            // Check divisor == 0
            assembler_.test_r64_r64(rhs, rhs);
            assembler_.jcc_rel32(Condition::E, 0);
            usize jmp_zero = assembler_.current_offset() - 4;

            // Non-zero divisor
            assembler_.mov_r64_r64(X86Reg::RAX, lhs);
            assembler_.cqo();
            assembler_.idiv_r64(rhs);
            assembler_.mov_r64_r64(dst, is_mod ? X86Reg::RDX : X86Reg::RAX);
            assembler_.jmp_rel32(0);
            usize jmp_done = assembler_.current_offset() - 4;

            // Zero divisor: return 0 gracefully
            usize zero_label = assembler_.current_offset();
            assembler_.patch32(jmp_zero, static_cast<u32>(zero_label - (jmp_zero + 4)));
            assembler_.xor_r64_r64(dst, dst);

            // Done
            usize done_label = assembler_.current_offset();
            assembler_.patch32(jmp_done, static_cast<u32>(done_label - (jmp_done + 4)));

            consume_operand(instr->operand(0));
            consume_operand(instr->operand(1));
            break;
        }

        case Opcode::NegI64: {
            X86Reg src = ensure_reg_for_operand(instr->operand(0), {});
            X86Reg dst = allocate_reg_for_result(instr->result(), {src});
            assembler_.mov_r64_r64(dst, src);
            assembler_.neg_r64(dst);
            consume_operand(instr->operand(0));
            break;
        }

        case Opcode::Not: {
            X86Reg src = ensure_reg_for_operand(instr->operand(0), {});
            assembler_.test_r64_r64(src, src);
            consume_operand(instr->operand(0));
            X86Reg dst = allocate_reg_for_result(instr->result(), {});
            assembler_.setcc(Condition::E, dst);
            assembler_.movzx_r64_r8(dst, dst);
            break;
        }

        case Opcode::BitNot: {
            X86Reg src = ensure_reg_for_operand(instr->operand(0), {});
            X86Reg dst = allocate_reg_for_result(instr->result(), {src});
            assembler_.mov_r64_r64(dst, src);
            assembler_.not_r64(dst);
            consume_operand(instr->operand(0));
            break;
        }
        
        case Opcode::And:
        case Opcode::BitAnd: {
            X86Reg lhs = ensure_reg_for_operand(instr->operand(0), {});
            X86Reg rhs = ensure_reg_for_operand(instr->operand(1), {lhs});
            X86Reg dst = allocate_reg_for_result(instr->result(), {lhs, rhs});
            assembler_.mov_r64_r64(dst, lhs);
            assembler_.and_r64_r64(dst, rhs);
            consume_operand(instr->operand(0));
            consume_operand(instr->operand(1));
            break;
        }
        
        case Opcode::Or:
        case Opcode::BitOr: {
            X86Reg lhs = ensure_reg_for_operand(instr->operand(0), {});
            X86Reg rhs = ensure_reg_for_operand(instr->operand(1), {lhs});
            X86Reg dst = allocate_reg_for_result(instr->result(), {lhs, rhs});
            assembler_.mov_r64_r64(dst, lhs);
            assembler_.or_r64_r64(dst, rhs);
            consume_operand(instr->operand(0));
            consume_operand(instr->operand(1));
            break;
        }
        
        case Opcode::BitXor: {
            X86Reg lhs = ensure_reg_for_operand(instr->operand(0), {});
            X86Reg rhs = ensure_reg_for_operand(instr->operand(1), {lhs});
            X86Reg dst = allocate_reg_for_result(instr->result(), {lhs, rhs});
            assembler_.mov_r64_r64(dst, lhs);
            assembler_.xor_r64_r64(dst, rhs);
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
            X86Reg lhs = ensure_reg_for_operand(instr->operand(0), {});
            X86Reg rhs = ensure_reg_for_operand(instr->operand(1), {lhs});
            
            // Compare operands first without clobbering either
            assembler_.cmp_r64_r64(lhs, rhs);
            consume_operand(instr->operand(0));
            consume_operand(instr->operand(1));
            
            // Choose condition code
            Condition cond = Condition::E;
            switch (instr->opcode()) {
                case Opcode::EqU64:
                case Opcode::EqI64: cond = Condition::E; break;
                case Opcode::NeU64:
                case Opcode::NeI64: cond = Condition::NE; break;
                case Opcode::LtU64: cond = Condition::B; break;
                case Opcode::LeU64: cond = Condition::BE; break;
                case Opcode::GtU64: cond = Condition::A; break;
                case Opcode::GeU64: cond = Condition::AE; break;
                case Opcode::LtI64: cond = Condition::L; break;
                case Opcode::LeI64: cond = Condition::LE; break;
                case Opcode::GtI64: cond = Condition::G; break;
                case Opcode::GeI64: cond = Condition::GE; break;
                default: break;
            }
            
            X86Reg dst = allocate_reg_for_result(instr->result(), {});
            assembler_.setcc(cond, dst);
            assembler_.movzx_r64_r8(dst, dst);
            break;
        }

        case Opcode::Select: {
            auto sel_op = static_cast<const SelectOp*>(instr);
            X86Reg r_cond = ensure_reg_for_operand(sel_op->condition(), {});
            X86Reg r_true = ensure_reg_for_operand(sel_op->true_value(), {r_cond});
            X86Reg r_false = ensure_reg_for_operand(sel_op->false_value(), {r_cond, r_true});
            X86Reg r_dst = allocate_reg_for_result(instr->result(), {r_cond, r_true, r_false});
            
            assembler_.test_r64_r64(r_cond, r_cond);
            if (r_dst == r_true) {
                assembler_.cmovcc_r64_r64(Condition::E, r_dst, r_false);
            } else {
                assembler_.mov_r64_r64(r_dst, r_false);
                assembler_.cmovcc_r64_r64(Condition::NE, r_dst, r_true);
            }
            
            consume_operand(sel_op->condition());
            consume_operand(sel_op->true_value());
            consume_operand(sel_op->false_value());
            break;
        }
        
        case Opcode::Return: {
            if (instr->num_operands() > 0) {
                X86Reg ret_val = ensure_reg_for_operand(instr->operand(0), {});
                if (ret_val != X86Reg::RAX) {
                    assembler_.mov_r64_r64(X86Reg::RAX, ret_val);
                }
                consume_operand(instr->operand(0));
            } else {
                assembler_.xor_r64_r64(X86Reg::RAX, X86Reg::RAX);
            }
            emit_epilogue();
            break;
        }
        
        default:
            return Status(StatusCode::NotImplemented, "Instruction not implemented in x86 JIT");
    }
    
    return Status::OK();
}

} // namespace jit
} // namespace vectortick
