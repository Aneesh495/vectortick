#pragma once
#include "code_generator.hpp"
#include <map>

namespace vectortick::jit {
struct NativeLocation { int reg = -1; i32 slot = -1; };
struct NativePlan {
    std::map<ir::ValueId, NativeLocation> locations;
    std::vector<const ir::BasicBlock*> blocks;
    usize stack_bytes = 0;
    usize spill_count = 0;
};
// Validate the supported acyclic SSA subset before any executable mapping exists.
Result<NativePlan> plan_native(const ir::Function* fn, usize registers) noexcept;
}
