#pragma once

#include "value.hpp"
#include "opcode.hpp"
#include "../common/types.hpp"
#include <vector>
#include <memory>

namespace vectortick {

namespace ir {

// Forward declarations
class BasicBlock;

// Instruction in SSA form
class Instruction {
public:
    Instruction(Opcode op, std::vector<ValueId> operands, ValueId result, Type result_type)
        : opcode_(op), operands_(std::move(operands)), result_(result), result_type_(result_type)
        , parent_(nullptr) {}
    
    virtual ~Instruction() = default;

    // Opcode
    [[nodiscard]] Opcode opcode() const noexcept { return opcode_; }
    
    // Operands
    [[nodiscard]] const std::vector<ValueId>& operands() const noexcept { return operands_; }
    [[nodiscard]] usize num_operands() const noexcept { return operands_.size(); }
    [[nodiscard]] ValueId operand(usize idx) const noexcept { return operands_[idx]; }
    
    // Result
    [[nodiscard]] ValueId result() const noexcept { return result_; }
    [[nodiscard]] Type result_type() const noexcept { return result_type_; }
    [[nodiscard]] bool has_result() const noexcept { return result_ != 0; }
    
    // Parent block
    [[nodiscard]] BasicBlock* parent() noexcept { return parent_; }
    [[nodiscard]] const BasicBlock* parent() const noexcept { return parent_; }
    void set_parent(BasicBlock* parent) noexcept { parent_ = parent; }
    
    // Check instruction type
    [[nodiscard]] bool is_terminator() const noexcept {
        return opcode_ == Opcode::Return || 
               opcode_ == Opcode::Jump || 
               opcode_ == Opcode::Branch;
    }
    
    [[nodiscard]] bool is_branch() const noexcept {
        return opcode_ == Opcode::Jump || opcode_ == Opcode::Branch;
    }
    
    [[nodiscard]] bool is_load() const noexcept {
        return opcode_ == Opcode::LoadColumn || opcode_ == Opcode::LoadSelection;
    }
    
    [[nodiscard]] bool is_aggregate() const noexcept {
        return ir::is_aggregate(opcode_);
    }
    
    // Instruction ID (for ordering)
    [[nodiscard]] usize id() const noexcept { return id_; }
    void set_id(usize id) noexcept { id_ = id; }

private:
    Opcode opcode_;
    std::vector<ValueId> operands_;
    ValueId result_;
    Type result_type_;
    BasicBlock* parent_;
    usize id_ = 0;
};

// Phi inputs are selected on the incoming CFG edge.
class PhiOp : public Instruction {
public:
    PhiOp(std::vector<BasicBlock*> blocks, std::vector<ValueId> values, ValueId result, Type type)
        : Instruction(Opcode::Phi, std::move(values), result, type), blocks_(std::move(blocks)) {}
    const std::vector<BasicBlock*>& incoming_blocks() const noexcept { return blocks_; }
private:
    std::vector<BasicBlock*> blocks_;
};

// Specific instruction types for convenience

class BinaryOp : public Instruction {
public:
    BinaryOp(Opcode op, ValueId left, ValueId right, ValueId result, Type type)
        : Instruction(op, {left, right}, result, type) {}
    
    [[nodiscard]] ValueId left() const noexcept { return operand(0); }
    [[nodiscard]] ValueId right() const noexcept { return operand(1); }
};

class UnaryOp : public Instruction {
public:
    UnaryOp(Opcode op, ValueId operand, ValueId result, Type type)
        : Instruction(op, {operand}, result, type) {}
    
    [[nodiscard]] ValueId operand() const noexcept { return Instruction::operand(0); }
};

class CompareOp : public Instruction {
public:
    CompareOp(Opcode op, ValueId left, ValueId right, ValueId result)
        : Instruction(op, {left, right}, result, Type::I1) {}
    
    [[nodiscard]] ValueId left() const noexcept { return operand(0); }
    [[nodiscard]] ValueId right() const noexcept { return operand(1); }
};

class LoadColumnOp : public Instruction {
public:
    LoadColumnOp(u32 column_id, ValueId row_idx, ValueId result, Type type)
        : Instruction(Opcode::LoadColumn, {row_idx}, result, type), column_id_(column_id) {}
    
    [[nodiscard]] u32 column_id() const noexcept { return column_id_; }
    [[nodiscard]] ValueId row_index() const noexcept { return operand(0); }

private:
    u32 column_id_;
};

class ConstOp : public Instruction {
public:
    ConstOp(const Constant& constant, ValueId result)
        : Instruction(choose_opcode(constant.type), {}, result, constant.type), constant_(constant) {}
    
    [[nodiscard]] const Constant& constant() const noexcept { return constant_; }

private:
    Constant constant_;
    
    [[nodiscard]] static Opcode choose_opcode(Type t) noexcept {
        switch (t) {
            case Type::I64: return Opcode::ConstI64;
            case Type::U32: return Opcode::ConstU32;
            case Type::I1: return Opcode::ConstBool;
            default: return Opcode::ConstU64;
        }
    }
};

class BranchOp : public Instruction {
public:
    BranchOp(ValueId condition, BasicBlock* true_block, BasicBlock* false_block)
        : Instruction(Opcode::Branch, {condition}, 0, Type::Void)
        , true_block_(true_block), false_block_(false_block) {}
    
    [[nodiscard]] ValueId condition() const noexcept { return operand(0); }
    [[nodiscard]] BasicBlock* true_block() noexcept { return true_block_; }
    [[nodiscard]] BasicBlock* false_block() noexcept { return false_block_; }
    [[nodiscard]] const BasicBlock* true_block() const noexcept { return true_block_; }
    [[nodiscard]] const BasicBlock* false_block() const noexcept { return false_block_; }

private:
    BasicBlock* true_block_;
    BasicBlock* false_block_;
};

class JumpOp : public Instruction {
public:
    explicit JumpOp(BasicBlock* target)
        : Instruction(Opcode::Jump, {}, 0, Type::Void), target_(target) {}
    
    [[nodiscard]] BasicBlock* target() noexcept { return target_; }
    [[nodiscard]] const BasicBlock* target() const noexcept { return target_; }

private:
    BasicBlock* target_;
};

class ReturnOp : public Instruction {
public:
    explicit ReturnOp(ValueId value = 0)
        : Instruction(Opcode::Return, value ? std::vector<ValueId>{value} : std::vector<ValueId>{}, 0, Type::Void) {}
    
    [[nodiscard]] bool has_value() const noexcept { return num_operands() > 0; }
    [[nodiscard]] ValueId value() const noexcept { return has_value() ? operand(0) : 0; }
};

class AggregateOp : public Instruction {
public:
    AggregateOp(Opcode op, ValueId input, ValueId result, Type type)
        : Instruction(op, {input}, result, type) {}
    
    [[nodiscard]] ValueId input() const noexcept { return operand(0); }
};

class SelectOp : public Instruction {
public:
    SelectOp(ValueId condition, ValueId true_val, ValueId false_val, ValueId result, Type type)
        : Instruction(Opcode::Select, {condition, true_val, false_val}, result, type) {}
    
    [[nodiscard]] ValueId condition() const noexcept { return operand(0); }
    [[nodiscard]] ValueId true_value() const noexcept { return operand(1); }
    [[nodiscard]] ValueId false_value() const noexcept { return operand(2); }
};

} // namespace ir

} // namespace vectortick
