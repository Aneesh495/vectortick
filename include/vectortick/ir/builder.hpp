#pragma once

#include "function.hpp"
#include "instruction.hpp"
#include "../query/ast.hpp"
#include "../common/types.hpp"
#include <memory>

namespace vectortick {

namespace ir {

// IR Builder - constructs SSA IR from AST
class Builder {
public:
    Builder() : function_(nullptr), current_block_(nullptr), next_temp_(1) {}
    
    // Create a new function
    Function* create_function(const std::string& name);
    
    // Set current insertion block
    void set_insertion_point(BasicBlock* block) {
        current_block_ = block;
    }
    
    // Get current insertion block
    [[nodiscard]] BasicBlock* insertion_point() noexcept { return current_block_; }
    
    // Create basic block
    BasicBlock* create_block(const std::string& name = "");
    
    // Create constants
    ValueId create_const_i64(i64 value);
    ValueId create_const_u64(u64 value);
    ValueId create_const_u32(u32 value);
    ValueId create_const_bool(bool value);
    
    // Create arithmetic operations
    ValueId create_add(ValueId lhs, ValueId rhs, Type type);
    ValueId create_sub(ValueId lhs, ValueId rhs, Type type);
    ValueId create_mul(ValueId lhs, ValueId rhs, Type type);
    ValueId create_div(ValueId lhs, ValueId rhs, Type type);
    ValueId create_mod(ValueId lhs, ValueId rhs, Type type);
    ValueId create_neg(ValueId operand, Type type);
    
    // Create comparisons
    ValueId create_eq(ValueId lhs, ValueId rhs, Type type);
    ValueId create_ne(ValueId lhs, ValueId rhs, Type type);
    ValueId create_lt(ValueId lhs, ValueId rhs, Type type);
    ValueId create_le(ValueId lhs, ValueId rhs, Type type);
    ValueId create_gt(ValueId lhs, ValueId rhs, Type type);
    ValueId create_ge(ValueId lhs, ValueId rhs, Type type);
    
    // Create boolean operations
    ValueId create_and(ValueId lhs, ValueId rhs);
    ValueId create_or(ValueId lhs, ValueId rhs);
    ValueId create_not(ValueId operand);
    
    // Create bitwise operations
    ValueId create_bitand(ValueId lhs, ValueId rhs);
    ValueId create_bitor(ValueId lhs, ValueId rhs);
    ValueId create_bitxor(ValueId lhs, ValueId rhs);
    ValueId create_bitnot(ValueId operand);
    
    // Create select
    ValueId create_select(ValueId cond, ValueId true_val, ValueId false_val, Type type);
    
    // Create load from column
    ValueId create_load_column(u32 column_id, ValueId row_idx, Type type);
    
    // Create aggregates
    ValueId create_count(ValueId input);
    ValueId create_sum(ValueId input, Type type);
    ValueId create_min(ValueId input, Type type);
    ValueId create_max(ValueId input, Type type);
    
    // Create terminators
    void create_return(ValueId value = 0);
    void create_jump(BasicBlock* target);
    void create_branch(ValueId cond, BasicBlock* true_block, BasicBlock* false_block);
    
    // Lower AST expression to IR
    ValueId lower_expression(const query::Expression* expr);
    
    // Build IR from query statement
    std::unique_ptr<Function> build_from_query(const query::QueryStmt* stmt);
    
    // Get current function
    [[nodiscard]] Function* function() noexcept { return function_; }
    [[nodiscard]] const Function* function() const noexcept { return function_; }
    
private:
    Type infer_type(const query::Expression* expr) const;
    Opcode get_comparison_opcode(query::TokenType op, Type type) const;
    Opcode get_arithmetic_opcode(query::TokenType op, Type type) const;
    
    std::unique_ptr<Function> owned_function_;
    Function* function_;
    BasicBlock* current_block_;
    u32 next_temp_;
};

} // namespace ir

} // namespace vectortick
