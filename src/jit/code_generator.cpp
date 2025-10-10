#include "vectortick/jit/code_generator.hpp"

namespace vectortick {
namespace jit {

// === CodeGenerator base implementation ===

void CodeGenerator::analyze_function(const ir::Function* function) noexcept {
    count_uses(function);
    build_interference(function);
}

void CodeGenerator::count_uses(const ir::Function* function) noexcept {
    reg_info_.clear();
    
    for (usize b = 0; b < function->num_blocks(); ++b) {
        const ir::BasicBlock* block = function->block(b);
        
        for (usize i = 0; i < block->num_instructions(); ++i) {
            const ir::Instruction* instr = block->instruction(i);
            
            // Count uses of operands
            for (usize o = 0; o < instr->num_operands(); ++o) {
                u32 value_id = instr->operand(o);
                reg_info_[value_id].use_count++;
            }
            
            // Count definitions
            if (instr->result() != 0) {
                reg_info_[instr->result()].def_count++;
            }
        }
    }
}

void CodeGenerator::build_interference(const ir::Function* function) noexcept {
    (void)function;
}

// === Factory implementations ===

std::unique_ptr<CodeGenerator> create_generator(TargetArch arch) {
    if (arch == TargetArch::X86_64) {
        return std::make_unique<X86CodeGenerator>();
    }
    if (arch == TargetArch::AArch64) {
        return std::make_unique<A64CodeGenerator>();
    }
    return nullptr;
}

std::unique_ptr<CodeGenerator> create_host_generator() {
    return create_generator(detect_host_arch());
}

} // namespace jit
} // namespace vectortick
