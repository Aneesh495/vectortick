#include "vectortick/execution/reference_interpreter.hpp"
#include "vectortick/common/result.hpp"
#include "vectortick/storage/file_format.hpp"
#include <limits>

namespace vectortick {

Result<u64> ReferenceInterpreter::execute(const ir::Function* function,
                                           const CanonicalEvent* event) noexcept {
    if (!function || !event) {
        return make_error<u64>(StatusCode::InvalidArgument, "Null function or event");
    }
    
    // Clear value storage
    values_.clear();
    
    // Set parameters
    for (usize i = 0; i < function->parameters().size(); ++i) {
        values_[function->parameters()[i]] = 0;
    }
    
    // Start at entry block
    const ir::BasicBlock* current = function->entry_block();
    
    while (current) {
        const ir::BasicBlock* next_block = nullptr;
        for (usize i = 0; i < current->num_instructions(); ++i) {
            const ir::Instruction* instr = current->instruction(i);
            
            if (instr->opcode() == ir::Opcode::Return) {
                ++instructions_executed_;
                if (instr->num_operands() > 0) {
                    auto it = values_.find(instr->operand(0));
                    if (it == values_.end()) {
                        return make_error<u64>(StatusCode::InternalError, "Undefined SSA value in return");
                    }
                    return it->second;
                }
                return 0ULL;
            }
            
            if (instr->opcode() == ir::Opcode::Jump) {
                ++instructions_executed_;
                auto jump = static_cast<const ir::JumpOp*>(instr);
                next_block = jump->target();
                break;
            }
            
            if (instr->opcode() == ir::Opcode::Branch) {
                ++instructions_executed_;
                auto branch = static_cast<const ir::BranchOp*>(instr);
                auto it = values_.find(branch->condition());
                if (it == values_.end()) {
                    return make_error<u64>(StatusCode::InternalError, "Undefined SSA condition in branch");
                }
                next_block = (it->second != 0) ? branch->true_block() : branch->false_block();
                break;
            }
            
            auto status = execute_instruction(instr, event);
            if (!status.ok()) {
                return make_error<u64>(status.code(), status.message());
            }
            
            ++instructions_executed_;
        }
        current = next_block;
    }
    
    return make_error<u64>(StatusCode::InternalError, "Function did not return");
}

Result<bool> ReferenceInterpreter::execute_filter(const ir::Function* function,
                                                   const CanonicalEvent* event) noexcept {
    auto result = execute(function, event);
    if (!result.ok()) {
        return make_error<bool>(result.status().code(), result.status().message());
    }
    return result.value() != 0;
}

Result<u64> ReferenceInterpreter::execute_aggregate(const ir::Function* function,
                                                     const std::vector<CanonicalEvent>& events) noexcept {
    if (!function) {
        return make_error<u64>(StatusCode::InvalidArgument, "Null function");
    }
    
    // Check if the function contains an aggregate instruction
    const ir::Instruction* agg_instr = nullptr;
    for (usize b = 0; b < function->num_blocks(); ++b) {
        const auto* block = function->block(b);
        for (usize i = 0; i < block->num_instructions(); ++i) {
            if (block->instruction(i)->is_aggregate()) {
                agg_instr = block->instruction(i);
                break;
            }
        }
        if (agg_instr) break;
    }
    
    if (!agg_instr) {
        // If no explicit aggregate instruction, count rows matching filter
        u64 count = 0;
        for (const auto& event : events) {
            auto r = execute(function, &event);
            if (!r.ok()) return r;
            if (r.value() != 0) {
                ++count;
            }
        }
        return count;
    }
    
    // Aggregate instruction present: accumulate cross-row state
    auto agg_op = static_cast<const ir::AggregateOp*>(agg_instr);
    ir::Opcode op = agg_op->opcode();
    
    u64 count = 0;
    u64 sum_u64 = 0;
    i64 sum_i64 = 0;
    u64 min_u64 = std::numeric_limits<u64>::max();
    u64 max_u64 = 0;
    i64 min_i64 = std::numeric_limits<i64>::max();
    i64 max_i64 = std::numeric_limits<i64>::min();
    
    for (const auto& event : events) {
        values_.clear();
        for (usize i = 0; i < function->parameters().size(); ++i) {
            values_[function->parameters()[i]] = 0;
        }
        
        const ir::BasicBlock* block = function->entry_block();
        u64 input_val = 0;
        
        for (usize i = 0; i < block->num_instructions(); ++i) {
            const ir::Instruction* instr = block->instruction(i);
            if (instr == agg_instr) {
                if (agg_instr->num_operands() > 0) {
                    auto it = values_.find(agg_instr->operand(0));
                    if (it != values_.end()) {
                        input_val = it->second;
                    }
                }
                break;
            }
            auto st = execute_instruction(instr, &event);
            if (!st.ok()) return make_error<u64>(st.code(), st.message());
        }
        
        ++count;
        switch (op) {
            case ir::Opcode::Count:
                break;
            case ir::Opcode::SumU64:
                sum_u64 += input_val;
                break;
            case ir::Opcode::SumI64:
                sum_i64 += static_cast<i64>(input_val);
                break;
            case ir::Opcode::MinU64:
                min_u64 = std::min(min_u64, input_val);
                break;
            case ir::Opcode::MaxU64:
                max_u64 = std::max(max_u64, input_val);
                break;
            case ir::Opcode::MinI64:
                min_i64 = std::min(min_i64, static_cast<i64>(input_val));
                break;
            case ir::Opcode::MaxI64:
                max_i64 = std::max(max_i64, static_cast<i64>(input_val));
                break;
            default:
                break;
        }
    }
    
    switch (op) {
        case ir::Opcode::Count: return count;
        case ir::Opcode::SumU64: return sum_u64;
        case ir::Opcode::SumI64: return static_cast<u64>(sum_i64);
        case ir::Opcode::MinU64: return (count == 0) ? 0 : min_u64;
        case ir::Opcode::MaxU64: return max_u64;
        case ir::Opcode::MinI64: return (count == 0) ? 0 : static_cast<u64>(min_i64);
        case ir::Opcode::MaxI64: return (count == 0) ? 0 : static_cast<u64>(max_i64);
        default: return 0;
    }
}

u64 ReferenceInterpreter::load_column_value(u32 column_id, const CanonicalEvent* event) const noexcept {
    // Column IDs match the storage format
    switch (column_id) {
        case 0:  // ExchangeTsNs
            return event->exchange_ts_ns;
        case 1:  // ReceiveTsNs
            return event->receive_ts_ns;
        case 2:  // Sequence
            return event->sequence;
        case 3:  // InstrumentId
            return event->instrument_id;
        case 4:  // EventType
            return static_cast<u64>(event->event_type);
        case 5:  // Side
            return static_cast<u64>(event->side);
        case 6:  // Flags
            return event->flags;
        case 7:  // PriceTicks
            return static_cast<u64>(event->price_ticks);
        case 8:  // Quantity
            return event->quantity;
        case 9:  // VenueId
            return event->venue_id;
        case 10: // SourceId
            return event->source_id;
        case 11: // TradeOrOrderId
            return event->trade_or_order_id;
        default:
            return 0;
    }
}

Status ReferenceInterpreter::execute_instruction(const ir::Instruction* instr,
                                                   const CanonicalEvent* event) noexcept {
    ir::Opcode op = instr->opcode();
    ir::ValueId result = instr->result();
    
    auto get_val = [this](ir::ValueId id, u64& out) -> Status {
        auto it = values_.find(id);
        if (it == values_.end()) {
            return Status(StatusCode::InternalError, "Undefined SSA value");
        }
        out = it->second;
        return Status::OK();
    };
    
    switch (op) {
        case ir::Opcode::Nop:
        case ir::Opcode::Return:
            break;
            
        case ir::Opcode::ConstI64:
        case ir::Opcode::ConstU64:
        case ir::Opcode::ConstU32:
        case ir::Opcode::ConstBool: {
            auto const_op = static_cast<const ir::ConstOp*>(instr);
            values_[result] = const_op->constant().get_u64();
            break;
        }
        
        case ir::Opcode::LoadColumn: {
            auto load = static_cast<const ir::LoadColumnOp*>(instr);
            u64 value = load_column_value(load->column_id(), event);
            values_[result] = value;
            break;
        }
        
        case ir::Opcode::AddU64: {
            u64 lhs = 0, rhs = 0;
            if (auto st = get_val(instr->operand(0), lhs); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs); !st.ok()) return st;
            auto sum = checked_add_u64(lhs, rhs);
            if (!sum) {
                return Status(StatusCode::InternalError, "Addition overflow");
            }
            values_[result] = *sum;
            break;
        }
        
        case ir::Opcode::SubU64: {
            u64 lhs = 0, rhs = 0;
            if (auto st = get_val(instr->operand(0), lhs); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs); !st.ok()) return st;
            auto diff = checked_sub_u64(lhs, rhs);
            if (!diff) {
                return Status(StatusCode::InternalError, "Subtraction underflow");
            }
            values_[result] = *diff;
            break;
        }
        
        case ir::Opcode::MulU64: {
            u64 lhs = 0, rhs = 0;
            if (auto st = get_val(instr->operand(0), lhs); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs); !st.ok()) return st;
            auto prod = checked_mul_u64(lhs, rhs);
            if (!prod) {
                return Status(StatusCode::InternalError, "Multiplication overflow");
            }
            values_[result] = *prod;
            break;
        }
        
        case ir::Opcode::DivU64: {
            u64 lhs = 0, rhs = 0;
            if (auto st = get_val(instr->operand(0), lhs); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs); !st.ok()) return st;
            if (rhs == 0) {
                return Status(StatusCode::InternalError, "Division by zero");
            }
            values_[result] = lhs / rhs;
            break;
        }
        
        case ir::Opcode::ModU64: {
            u64 lhs = 0, rhs = 0;
            if (auto st = get_val(instr->operand(0), lhs); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs); !st.ok()) return st;
            if (rhs == 0) {
                return Status(StatusCode::InternalError, "Modulo by zero");
            }
            values_[result] = lhs % rhs;
            break;
        }
        
        case ir::Opcode::AddI64: {
            u64 lhs_u = 0, rhs_u = 0;
            if (auto st = get_val(instr->operand(0), lhs_u); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs_u); !st.ok()) return st;
            i64 lhs = static_cast<i64>(lhs_u);
            i64 rhs = static_cast<i64>(rhs_u);
            auto sum = checked_add_i64(lhs, rhs);
            if (!sum) {
                return Status(StatusCode::InternalError, "Addition overflow");
            }
            values_[result] = static_cast<u64>(*sum);
            break;
        }
        
        case ir::Opcode::SubI64: {
            u64 lhs_u = 0, rhs_u = 0;
            if (auto st = get_val(instr->operand(0), lhs_u); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs_u); !st.ok()) return st;
            i64 lhs = static_cast<i64>(lhs_u);
            i64 rhs = static_cast<i64>(rhs_u);
            auto diff = checked_sub_i64(lhs, rhs);
            if (!diff) {
                return Status(StatusCode::InternalError, "Subtraction underflow");
            }
            values_[result] = static_cast<u64>(*diff);
            break;
        }
        
        case ir::Opcode::MulI64: {
            u64 lhs_u = 0, rhs_u = 0;
            if (auto st = get_val(instr->operand(0), lhs_u); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs_u); !st.ok()) return st;
            i64 lhs = static_cast<i64>(lhs_u);
            i64 rhs = static_cast<i64>(rhs_u);
            auto prod = checked_mul_i64(lhs, rhs);
            if (!prod) {
                return Status(StatusCode::InternalError, "Multiplication overflow");
            }
            values_[result] = static_cast<u64>(*prod);
            break;
        }
        
        case ir::Opcode::DivI64: {
            u64 lhs_u = 0, rhs_u = 0;
            if (auto st = get_val(instr->operand(0), lhs_u); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs_u); !st.ok()) return st;
            i64 lhs = static_cast<i64>(lhs_u);
            i64 rhs = static_cast<i64>(rhs_u);
            if (rhs == 0) {
                return Status(StatusCode::InternalError, "Division by zero");
            }
            values_[result] = static_cast<u64>(lhs / rhs);
            break;
        }
        
        case ir::Opcode::ModI64: {
            u64 lhs_u = 0, rhs_u = 0;
            if (auto st = get_val(instr->operand(0), lhs_u); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs_u); !st.ok()) return st;
            i64 lhs = static_cast<i64>(lhs_u);
            i64 rhs = static_cast<i64>(rhs_u);
            if (rhs == 0) {
                return Status(StatusCode::InternalError, "Modulo by zero");
            }
            values_[result] = static_cast<u64>(lhs % rhs);
            break;
        }
        
        case ir::Opcode::NegI64: {
            u64 val_u = 0;
            if (auto st = get_val(instr->operand(0), val_u); !st.ok()) return st;
            i64 val = static_cast<i64>(val_u);
            if (val == std::numeric_limits<i64>::min()) {
                return Status(StatusCode::InternalError, "Negation overflow");
            }
            values_[result] = static_cast<u64>(-val);
            break;
        }
        
        case ir::Opcode::EqU64: {
            u64 lhs = 0, rhs = 0;
            if (auto st = get_val(instr->operand(0), lhs); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs); !st.ok()) return st;
            values_[result] = (lhs == rhs) ? 1 : 0;
            break;
        }
        
        case ir::Opcode::EqI64: {
            u64 lhs_u = 0, rhs_u = 0;
            if (auto st = get_val(instr->operand(0), lhs_u); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs_u); !st.ok()) return st;
            values_[result] = (static_cast<i64>(lhs_u) == static_cast<i64>(rhs_u)) ? 1 : 0;
            break;
        }
        
        case ir::Opcode::NeU64: {
            u64 lhs = 0, rhs = 0;
            if (auto st = get_val(instr->operand(0), lhs); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs); !st.ok()) return st;
            values_[result] = (lhs != rhs) ? 1 : 0;
            break;
        }
        
        case ir::Opcode::NeI64: {
            u64 lhs_u = 0, rhs_u = 0;
            if (auto st = get_val(instr->operand(0), lhs_u); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs_u); !st.ok()) return st;
            values_[result] = (static_cast<i64>(lhs_u) != static_cast<i64>(rhs_u)) ? 1 : 0;
            break;
        }
        
        case ir::Opcode::LtU64: {
            u64 lhs = 0, rhs = 0;
            if (auto st = get_val(instr->operand(0), lhs); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs); !st.ok()) return st;
            values_[result] = (lhs < rhs) ? 1 : 0;
            break;
        }
        
        case ir::Opcode::LeU64: {
            u64 lhs = 0, rhs = 0;
            if (auto st = get_val(instr->operand(0), lhs); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs); !st.ok()) return st;
            values_[result] = (lhs <= rhs) ? 1 : 0;
            break;
        }
        
        case ir::Opcode::GtU64: {
            u64 lhs = 0, rhs = 0;
            if (auto st = get_val(instr->operand(0), lhs); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs); !st.ok()) return st;
            values_[result] = (lhs > rhs) ? 1 : 0;
            break;
        }
        
        case ir::Opcode::GeU64: {
            u64 lhs = 0, rhs = 0;
            if (auto st = get_val(instr->operand(0), lhs); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs); !st.ok()) return st;
            values_[result] = (lhs >= rhs) ? 1 : 0;
            break;
        }
        
        case ir::Opcode::LtI64: {
            u64 lhs_u = 0, rhs_u = 0;
            if (auto st = get_val(instr->operand(0), lhs_u); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs_u); !st.ok()) return st;
            values_[result] = (static_cast<i64>(lhs_u) < static_cast<i64>(rhs_u)) ? 1 : 0;
            break;
        }
        
        case ir::Opcode::LeI64: {
            u64 lhs_u = 0, rhs_u = 0;
            if (auto st = get_val(instr->operand(0), lhs_u); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs_u); !st.ok()) return st;
            values_[result] = (static_cast<i64>(lhs_u) <= static_cast<i64>(rhs_u)) ? 1 : 0;
            break;
        }
        
        case ir::Opcode::GtI64: {
            u64 lhs_u = 0, rhs_u = 0;
            if (auto st = get_val(instr->operand(0), lhs_u); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs_u); !st.ok()) return st;
            values_[result] = (static_cast<i64>(lhs_u) > static_cast<i64>(rhs_u)) ? 1 : 0;
            break;
        }
        
        case ir::Opcode::GeI64: {
            u64 lhs_u = 0, rhs_u = 0;
            if (auto st = get_val(instr->operand(0), lhs_u); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs_u); !st.ok()) return st;
            values_[result] = (static_cast<i64>(lhs_u) >= static_cast<i64>(rhs_u)) ? 1 : 0;
            break;
        }
        
        case ir::Opcode::And: {
            u64 lhs = 0, rhs = 0;
            if (auto st = get_val(instr->operand(0), lhs); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs); !st.ok()) return st;
            values_[result] = (lhs && rhs) ? 1 : 0;
            break;
        }
        
        case ir::Opcode::Or: {
            u64 lhs = 0, rhs = 0;
            if (auto st = get_val(instr->operand(0), lhs); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs); !st.ok()) return st;
            values_[result] = (lhs || rhs) ? 1 : 0;
            break;
        }
        
        case ir::Opcode::Not: {
            u64 val = 0;
            if (auto st = get_val(instr->operand(0), val); !st.ok()) return st;
            values_[result] = val ? 0 : 1;
            break;
        }
        
        case ir::Opcode::BitAnd: {
            u64 lhs = 0, rhs = 0;
            if (auto st = get_val(instr->operand(0), lhs); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs); !st.ok()) return st;
            values_[result] = lhs & rhs;
            break;
        }
        
        case ir::Opcode::BitOr: {
            u64 lhs = 0, rhs = 0;
            if (auto st = get_val(instr->operand(0), lhs); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs); !st.ok()) return st;
            values_[result] = lhs | rhs;
            break;
        }
        
        case ir::Opcode::BitXor: {
            u64 lhs = 0, rhs = 0;
            if (auto st = get_val(instr->operand(0), lhs); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs); !st.ok()) return st;
            values_[result] = lhs ^ rhs;
            break;
        }
        
        case ir::Opcode::BitNot: {
            u64 val = 0;
            if (auto st = get_val(instr->operand(0), val); !st.ok()) return st;
            values_[result] = ~val;
            break;
        }
        
        case ir::Opcode::Shl: {
            u64 lhs = 0, rhs = 0;
            if (auto st = get_val(instr->operand(0), lhs); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs); !st.ok()) return st;
            values_[result] = (rhs < 64) ? (lhs << rhs) : 0;
            break;
        }
        
        case ir::Opcode::Shr: {
            u64 lhs = 0, rhs = 0;
            if (auto st = get_val(instr->operand(0), lhs); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), rhs); !st.ok()) return st;
            values_[result] = (rhs < 64) ? (lhs >> rhs) : 0;
            break;
        }
        
        case ir::Opcode::ExtendI32ToI64: {
            u64 val = 0;
            if (auto st = get_val(instr->operand(0), val); !st.ok()) return st;
            i32 s32 = static_cast<i32>(val);
            values_[result] = static_cast<u64>(static_cast<i64>(s32));
            break;
        }
        
        case ir::Opcode::ExtendU32ToU64: {
            u64 val = 0;
            if (auto st = get_val(instr->operand(0), val); !st.ok()) return st;
            u32 u = static_cast<u32>(val);
            values_[result] = static_cast<u64>(u);
            break;
        }
        
        case ir::Opcode::TruncI64ToI32: {
            u64 val = 0;
            if (auto st = get_val(instr->operand(0), val); !st.ok()) return st;
            values_[result] = static_cast<u64>(static_cast<u32>(val));
            break;
        }
        
        case ir::Opcode::Select: {
            u64 cond = 0, true_val = 0, false_val = 0;
            if (auto st = get_val(instr->operand(0), cond); !st.ok()) return st;
            if (auto st = get_val(instr->operand(1), true_val); !st.ok()) return st;
            if (auto st = get_val(instr->operand(2), false_val); !st.ok()) return st;
            values_[result] = cond ? true_val : false_val;
            break;
        }
        
        default:
            return Status(StatusCode::NotImplemented, "Opcode not implemented");
    }
    
    return Status::OK();
}

std::optional<u64> ReferenceInterpreter::checked_add_u64(u64 a, u64 b) noexcept {
    if (a > std::numeric_limits<u64>::max() - b) {
        return std::nullopt;
    }
    return a + b;
}

std::optional<u64> ReferenceInterpreter::checked_sub_u64(u64 a, u64 b) noexcept {
    if (a < b) {
        return std::nullopt;
    }
    return a - b;
}

std::optional<u64> ReferenceInterpreter::checked_mul_u64(u64 a, u64 b) noexcept {
    if (a > 0 && b > std::numeric_limits<u64>::max() / a) {
        return std::nullopt;
    }
    return a * b;
}

std::optional<i64> ReferenceInterpreter::checked_add_i64(i64 a, i64 b) noexcept {
    if (b > 0 && a > std::numeric_limits<i64>::max() - b) {
        return std::nullopt;
    }
    if (b < 0 && a < std::numeric_limits<i64>::min() - b) {
        return std::nullopt;
    }
    return a + b;
}

std::optional<i64> ReferenceInterpreter::checked_sub_i64(i64 a, i64 b) noexcept {
    if (b > 0 && a < std::numeric_limits<i64>::min() + b) {
        return std::nullopt;
    }
    if (b < 0 && a > std::numeric_limits<i64>::max() + b) {
        return std::nullopt;
    }
    return a - b;
}

std::optional<i64> ReferenceInterpreter::checked_mul_i64(i64 a, i64 b) noexcept {
    if (a == 0 || b == 0) return 0;
    if (a == 1) return b;
    if (b == 1) return a;
    
    if (a > 0) {
        if (b > 0) {
            if (a > std::numeric_limits<i64>::max() / b) return std::nullopt;
        } else {
            if (b < std::numeric_limits<i64>::min() / a) return std::nullopt;
        }
    } else {
        if (b > 0) {
            if (a < std::numeric_limits<i64>::min() / b) return std::nullopt;
        } else {
            if (a < std::numeric_limits<i64>::max() / b) return std::nullopt;
        }
    }
    
    return a * b;
}

} // namespace vectortick
