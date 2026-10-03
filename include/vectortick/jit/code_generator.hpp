#pragma once

#include "x86_assembler.hpp"
#include "a64_assembler.hpp"
#include "../ir/function.hpp"
#include "../common/status.hpp"
#include "../common/result.hpp"
#include <unordered_map>
#include <unordered_set>
#include <memory>

namespace vectortick {

namespace jit {

// Target architecture
enum class TargetArch {
    X86_64,
    AArch64,
    Unknown
};

// Detect host architecture
[[nodiscard]] inline TargetArch detect_host_arch() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return TargetArch::X86_64;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return TargetArch::AArch64;
#else
    return TargetArch::Unknown;
#endif
}

// Register allocation info
struct RegInfo {
    u32 value_id;
    bool is_allocated;
    u32 use_count;
    u32 def_count;
    i32 spill_slot;  // -1 if not spilled
    
    RegInfo() : value_id(0), is_allocated(false), use_count(0), def_count(0), spill_slot(-1) {}
};

// Abstract code generator interface
class CodeGenerator {
public:
    virtual ~CodeGenerator() = default;
    
    // Generate code for a function
    [[nodiscard]] virtual Result<std::vector<u8>> generate(const ir::Function* function) noexcept = 0;
    
    // Get the target architecture
    [[nodiscard]] virtual TargetArch target() const noexcept = 0;
    
protected:
    // Register allocation
    std::unordered_map<u32, RegInfo> reg_info_;
    
    // Analyze function for register allocation
    void analyze_function(const ir::Function* function) noexcept;
    
    // Count uses of each value
    void count_uses(const ir::Function* function) noexcept;
    
    // Build interference graph (simple version)
    void build_interference(const ir::Function* function) noexcept;
};

// X86-64 code generator
class X86CodeGenerator : public CodeGenerator {
public:
    X86CodeGenerator() : assembler_() {}
    
    [[nodiscard]] Result<std::vector<u8>> generate(const ir::Function* function) noexcept override;
    [[nodiscard]] TargetArch target() const noexcept override { return TargetArch::X86_64; }
    
private:
    X86Assembler assembler_;
    std::unordered_map<u32, X86Reg> value_to_reg_;
    std::unordered_map<u32, i32> value_to_slot_;
    std::unordered_map<u8, u32> reg_to_value_;
    std::unordered_map<u32, u32> remaining_uses_;
    std::vector<X86Reg> free_regs_;
    std::vector<i32> free_slots_;
    i32 next_spill_slot_ = 0;
    
    // Generate prologue
    void emit_prologue() noexcept;
    
    // Generate epilogue
    void emit_epilogue() noexcept;
    
    // Generate instruction
    [[nodiscard]] Status emit_instruction(const ir::Instruction* instr) noexcept;
    
    // Register allocation and spilling
    [[nodiscard]] X86Reg ensure_reg_for_operand(u32 value_id, const std::vector<X86Reg>& pinned = {}) noexcept;
    [[nodiscard]] X86Reg allocate_reg_for_result(u32 value_id, const std::vector<X86Reg>& pinned = {}) noexcept;
    void consume_operand(u32 value_id) noexcept;
    [[nodiscard]] i32 allocate_spill_slot() noexcept;
    void free_spill_slot(i32 slot) noexcept;
    void spill_reg(X86Reg reg) noexcept;
};

// AArch64 code generator
class A64CodeGenerator : public CodeGenerator {
public:
    A64CodeGenerator() : assembler_() {}
    
    [[nodiscard]] Result<std::vector<u8>> generate(const ir::Function* function) noexcept override;
    [[nodiscard]] TargetArch target() const noexcept override { return TargetArch::AArch64; }
    
private:
    A64Assembler assembler_;
    std::unordered_map<u32, A64Reg> value_to_reg_;
    std::unordered_map<u32, i32> value_to_slot_;
    std::unordered_map<u8, u32> reg_to_value_;
    std::unordered_map<u32, u32> remaining_uses_;
    std::vector<A64Reg> free_regs_;
    std::vector<i32> free_slots_;
    i32 next_spill_slot_ = 0;
    
    // Generate prologue
    void emit_prologue() noexcept;
    
    // Generate epilogue
    void emit_epilogue() noexcept;
    
    // Generate instruction
    [[nodiscard]] Status emit_instruction(const ir::Instruction* instr) noexcept;
    
    // Register allocation and spilling
    [[nodiscard]] A64Reg ensure_reg_for_operand(u32 value_id, const std::vector<A64Reg>& pinned = {}) noexcept;
    [[nodiscard]] A64Reg allocate_reg_for_result(u32 value_id, const std::vector<A64Reg>& pinned = {}) noexcept;
    void consume_operand(u32 value_id) noexcept;
    [[nodiscard]] i32 allocate_spill_slot() noexcept;
    void free_spill_slot(i32 slot) noexcept;
    void spill_reg(A64Reg reg) noexcept;
};

// Create appropriate code generator for target
[[nodiscard]] std::unique_ptr<CodeGenerator> create_generator(TargetArch arch);

// Create code generator for current host
[[nodiscard]] std::unique_ptr<CodeGenerator> create_host_generator();

} // namespace jit

} // namespace vectortick
