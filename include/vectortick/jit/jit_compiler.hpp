#pragma once

#include "x86_assembler.hpp"
#include "../ir/function.hpp"
#include "../ir/instruction.hpp"
#include "../common/status.hpp"
#include "../common/result.hpp"
#include <vector>
#include <unordered_map>
#include <memory>

namespace vectortick {

namespace jit {

// JIT-compiled function pointer type
using JitFunction = u64 (*)(const void* context, u64* error);

// Memory protection flags
enum class MemoryProtection : u32 {
    None = 0,
    Read = 1,
    Write = 2,
    Execute = 4,
    ReadWrite = Read | Write,
    ReadExecute = Read | Execute,
    All = Read | Write | Execute,
};

// Memory manager for JIT code
class JitMemory {
public:
    JitMemory();
    JitMemory(const JitMemory&) = delete;
    JitMemory& operator=(const JitMemory&) = delete;
    JitMemory(JitMemory&& other) noexcept;
    JitMemory& operator=(JitMemory&& other) noexcept;
    ~JitMemory();
    
    // Allocate executable memory
    [[nodiscard]] Status allocate(usize size) noexcept;
    
    // Write code to memory
    [[nodiscard]] Status write(const u8* code, usize size) noexcept;
    
    // Make memory executable
    [[nodiscard]] Status make_executable() noexcept;
    
    // Get the memory pointer
    [[nodiscard]] void* memory() noexcept { return memory_; }
    [[nodiscard]] const void* memory() const noexcept { return memory_; }
    [[nodiscard]] usize size() const noexcept { return size_; }
    
    // Get as function pointer
    [[nodiscard]] JitFunction function() noexcept {
        return reinterpret_cast<JitFunction>(memory_);
    }
    
private:
    void* memory_;
    usize size_;
    bool executable_;
    
    // Platform-specific implementation
    [[nodiscard]] Status protect(int prot) noexcept;
};

// JIT compiler - compiles IR to native code
class JitCompiler {
public:
    JitCompiler() : assembler_(), memory_() {}
    
    // Compile an IR function to native code
    [[nodiscard]] Result<JitFunction> compile(const ir::Function* function) noexcept;
    
    JitFunction function() noexcept { return memory_.function(); }

    // Get the assembler for inspection
    [[nodiscard]] const X86Assembler& assembler() const noexcept { return assembler_; }
    
private:
    X86Assembler assembler_;
    JitMemory memory_;
    
    // Value location tracking
    std::unordered_map<ir::ValueId, X86Reg> value_locations_;
    
    // Available registers for allocation
    std::vector<X86Reg> free_regs_;
    
    // Compile a single basic block
    [[nodiscard]] Status compile_block(const ir::BasicBlock* block) noexcept;
    
    // Compile a single instruction
    [[nodiscard]] Status compile_instruction(const ir::Instruction* instr) noexcept;
    
    // Allocate a register for a value
    [[nodiscard]] X86Reg allocate_register() noexcept;
    
    // Free a register
    void free_register(X86Reg reg) noexcept;
    
    // Get location for a value
    [[nodiscard]] X86Reg get_location(ir::ValueId value) noexcept;
    
    // Set location for a value
    void set_location(ir::ValueId value, X86Reg reg) noexcept;
    
    // Emit function prologue
    void emit_prologue() noexcept;
    
    // Emit function epilogue
    void emit_epilogue() noexcept;
};

// Compiler instrumentation assumes a compiler-generated function prefix. Raw JIT
// mappings have no such prefix, so suppress only that check at this boundary.
[[nodiscard]]
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
inline Result<u64> invoke_native(JitFunction fn, const void* context) noexcept {
    if (!fn || !context) return make_error<u64>(StatusCode::InvalidArgument, "Null native function or context");
    u64 error = 0;
    u64 value = fn(context, &error);
    if (error != 0) return make_error<u64>(StatusCode::InternalError, "Native arithmetic error");
    return value;
}

// Convenience function to compile and execute
[[nodiscard]] inline Result<u64> jit_execute(const ir::Function* function, 
                                              const void* context) noexcept {
    JitCompiler compiler;
    auto result = compiler.compile(function);
    if (!result.ok()) {
        return make_error<u64>(result.status().code(), result.status().message());
    }
    
    JitFunction fn = result.value();
    return invoke_native(fn, context);
}

} // namespace jit

} // namespace vectortick
