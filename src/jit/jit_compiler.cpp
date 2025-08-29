#include "vectortick/jit/jit_compiler.hpp"
#include "vectortick/jit/code_generator.hpp"

#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <limits>

#if defined(__APPLE__)
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#endif

namespace vectortick {
namespace jit {

// === JitMemory implementation ===

JitMemory::JitMemory()
    : memory_(nullptr)
    , size_(0)
    , executable_(false) {}

JitMemory::~JitMemory() {
    if (memory_ && size_ > 0) {
        munmap(memory_, size_);
    }
}

JitMemory::JitMemory(JitMemory&& other) noexcept
    : memory_(std::exchange(other.memory_, nullptr)), size_(std::exchange(other.size_, 0)),
      executable_(std::exchange(other.executable_, false)) {}

JitMemory& JitMemory::operator=(JitMemory&& other) noexcept {
    if (this != &other) {
        if (memory_) munmap(memory_, size_);
        memory_ = std::exchange(other.memory_, nullptr);
        size_ = std::exchange(other.size_, 0);
        executable_ = std::exchange(other.executable_, false);
    }
    return *this;
}

Status JitMemory::allocate(usize size) noexcept {
    if (size == 0) {
        return Status(StatusCode::InvalidArgument, "Size must be > 0");
    }
    
    // Round up to page size
    usize page_size = static_cast<usize>(sysconf(_SC_PAGESIZE));
    if (size > std::numeric_limits<usize>::max() - page_size) {
        return Status(StatusCode::InvalidArgument, "Code allocation size overflow");
    }
    usize alloc_size = ((size + page_size - 1) / page_size) * page_size;
    
#if defined(__APPLE__) && defined(__aarch64__)
    // Apple Silicon MAP_JIT requirement
    void* mem = mmap(nullptr, alloc_size, 
                     PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
#else
    // Strict W^X: allocate RW first
    void* mem = mmap(nullptr, alloc_size, 
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    
    if (mem == MAP_FAILED) {
        return Status(StatusCode::AllocationFailed, "mmap failed");
    }
    
    if (memory_ && size_ > 0) {
        munmap(memory_, size_);
    }
    
    memory_ = mem;
    size_ = alloc_size;
    executable_ = false;
    
    return Status::OK();
}

Status JitMemory::write(const u8* code, usize size) noexcept {
    if (!memory_ || !code || size == 0 || size > size_ || executable_) {
        return Status(StatusCode::InvalidArgument, "Invalid memory or size");
    }
    
#if defined(__APPLE__) && defined(__aarch64__)
    pthread_jit_write_protect_np(0); // Allow writes, disable execute
#endif

    std::memcpy(memory_, code, size);

#if defined(__APPLE__) && defined(__aarch64__)
    pthread_jit_write_protect_np(1); // Disable writes, enable execute
    sys_icache_invalidate(memory_, size);
#endif

    return Status::OK();
}

Status JitMemory::make_executable() noexcept {
    if (!memory_ || size_ == 0) {
        return Status(StatusCode::InvalidArgument, "No memory allocated");
    }
    
    if (executable_) {
        return Status::OK();
    }
    
#if defined(__APPLE__) && defined(__aarch64__)
    executable_ = true;
    return Status::OK();
#else
    return protect(PROT_READ | PROT_EXEC);
#endif
}

Status JitMemory::protect(int prot) noexcept {
#if !defined(__APPLE__) || !defined(__aarch64__)
    if (mprotect(memory_, size_, prot) != 0) {
        return Status(StatusCode::MprotectFailed, "mprotect failed");
    }
    __builtin___clear_cache(reinterpret_cast<char*>(memory_),
                            reinterpret_cast<char*>(memory_) + size_);
#else
    (void)prot;
#endif
    executable_ = (prot & PROT_EXEC) != 0;
    return Status::OK();
}

// === JitCompiler implementation ===

Result<JitFunction> JitCompiler::compile(const ir::Function* function) noexcept {
    if (!function) {
        return make_error<JitFunction>(StatusCode::InvalidArgument, "Null function");
    }
    
    if (function->num_blocks() == 0) {
        return make_error<JitFunction>(StatusCode::InvalidArgument, "Function has no blocks");
    }
    
    auto generator = create_host_generator();
    if (!generator) {
        return make_error<JitFunction>(StatusCode::NotImplemented, "Host JIT generator not available");
    }
    
    auto code_res = generator->generate(function);
    if (!code_res.ok()) {
        return make_error<JitFunction>(code_res.status().code(), code_res.status().message());
    }
    
    const auto& code = code_res.value();
    auto status = memory_.allocate(code.size());
    if (!status.ok()) {
        return make_error<JitFunction>(status.code(), status.message());
    }
    
    status = memory_.write(code.data(), code.size());
    if (!status.ok()) {
        return make_error<JitFunction>(status.code(), status.message());
    }
    
    status = memory_.make_executable();
    if (!status.ok()) {
        return make_error<JitFunction>(status.code(), status.message());
    }
    
    return memory_.function();
}

} // namespace jit
} // namespace vectortick
