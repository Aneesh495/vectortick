#pragma once

#include "../common/types.hpp"

namespace vectortick {

namespace ir {

// SSA IR Opcodes
enum class Opcode : u16 {
    // Control flow
    Nop = 0,
    Jump,           // Unconditional jump
    Branch,         // Conditional branch
    Return,         // Return from function
    
    // Constants
    ConstI64,       // 64-bit signed integer constant
    ConstU64,       // 64-bit unsigned integer constant
    ConstU32,       // 32-bit unsigned integer constant
    ConstBool,      // Boolean constant
    
    // Memory operations
    LoadColumn,     // Load value from column at row index
    LoadSelection,  // Load from selection vector
    
    // Arithmetic operations
    AddI64,         // i64 addition
    SubI64,         // i64 subtraction
    MulI64,         // i64 multiplication
    DivI64,         // i64 division
    ModI64,         // i64 modulo
    
    AddU64,         // u64 addition
    SubU64,         // u64 subtraction
    MulU64,         // u64 multiplication
    DivU64,         // u64 division
    ModU64,         // u64 modulo
    
    NegI64,         // i64 negation
    
    // Comparison operations
    EqI64,          // i64 equal
    NeI64,          // i64 not equal
    LtI64,          // i64 less than
    LeI64,          // i64 less than or equal
    GtI64,          // i64 greater than
    GeI64,          // i64 greater than or equal
    
    EqU64,          // u64 equal
    NeU64,          // u64 not equal
    LtU64,          // u64 less than
    LeU64,          // u64 less than or equal
    GtU64,          // u64 greater than
    GeU64,          // u64 greater than or equal
    
    // Boolean operations
    And,            // Boolean and
    Or,             // Boolean or
    Not,            // Boolean not
    
    // Bitwise operations
    BitAnd,         // Bitwise and
    BitOr,          // Bitwise or
    BitXor,         // Bitwise xor
    BitNot,         // Bitwise not
    Shl,            // Shift left
    Shr,            // Shift right (logical)
    
    // Conditional
    Select,         // Select: cond ? a : b
    
    // Conversions
    ExtendI32ToI64, // Sign extend i32 to i64
    ExtendU32ToU64, // Zero extend u32 to u64
    TruncI64ToI32,  // Truncate i64 to i32
    
    Phi,
    U64ToI64,      // Checked conversion
    NegU64,        // Checked unsigned magnitude to negative signed value

    // Aggregates
    Count,          // Count rows
    SumI64,         // Sum i64 values
    SumU64,         // Sum u64 values
    MinI64,         // Min i64 value
    MaxI64,         // Max i64 value
    MinU64,         // Min u64 value
    MaxU64,         // Max u64 value
    
    // Vector operations
    VectorLoad,     // Load vector from column
    VectorFilter,   // Filter vector by predicate
    VectorProject,  // Project vector elements
    
    // Batch operations
    BatchCount,     // Count in batch
    BatchSum,       // Sum in batch
    BatchMin,       // Min in batch
    BatchMax,       // Max in batch
};

// Get opcode name
[[nodiscard]] const char* opcode_name(Opcode op) noexcept;

// Check if opcode is a comparison
[[nodiscard]] inline bool is_comparison(Opcode op) noexcept {
    return op >= Opcode::EqI64 && op <= Opcode::GeU64;
}

// Check if opcode is arithmetic
[[nodiscard]] inline bool is_arithmetic(Opcode op) noexcept {
    return (op >= Opcode::AddI64 && op <= Opcode::NegI64);
}

// Check if opcode is aggregate
[[nodiscard]] inline bool is_aggregate(Opcode op) noexcept {
    return op >= Opcode::Count && op <= Opcode::MaxU64;
}

// Check if opcode is vector
[[nodiscard]] inline bool is_vector(Opcode op) noexcept {
    return op >= Opcode::VectorLoad && op <= Opcode::VectorProject;
}

} // namespace ir

} // namespace vectortick
