#pragma once

#include "../common/types.hpp"
#include "../common/status.hpp"
#include "aligned_buffer.hpp"
#include <cstddef>
#include <cstdlib>
#include <new>
#include <utility>

namespace vectortick {

// Memory arena for fast allocation during query compilation
// All allocations are freed when the arena is reset or destroyed
class Arena {
public:
    // Default block size: 64 KB
    static constexpr usize DefaultBlockSize = 64 * 1024;
    static constexpr usize MaxAlignment = alignof(std::max_align_t);
    
    explicit Arena(usize block_size = DefaultBlockSize)
        : block_size_(block_size)
        , current_block_(nullptr)
        , current_offset_(0)
        , total_allocated_(0) {}
    
    ~Arena() {
        reset();
    }
    
    // Non-copyable, non-movable
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;
    
    // Allocate aligned memory
    [[nodiscard]] void* allocate(usize size, usize alignment = alignof(std::max_align_t)) noexcept {
        if (size == 0) return nullptr;
        
        // Align size
        usize aligned_size = align_up(size, alignment);
        
        // Check if we need a new block
        if (!current_block_ || (current_offset_ + aligned_size) > block_size_) {
            if (!allocate_new_block(aligned_size)) {
                return nullptr;
            }
        }
        
        // Allocate from current block
        void* ptr = current_block_->data + current_offset_;
        current_offset_ += aligned_size;
        total_allocated_ += aligned_size;
        
        return ptr;
    }
    
    // Typed allocation
    template <typename T>
    [[nodiscard]] T* allocate(usize count = 1) noexcept {
        return static_cast<T*>(allocate(count * sizeof(T), alignof(T)));
    }
    
    // Reset arena (frees all memory)
    void reset() noexcept {
        // Free all blocks
        Block* block = current_block_;
        while (block) {
            Block* prev = block->prev;
            std::free(block);
            block = prev;
        }
        
        current_block_ = nullptr;
        current_offset_ = 0;
        total_allocated_ = 0;
    }
    
    // Get statistics
    [[nodiscard]] usize total_allocated() const noexcept { return total_allocated_; }
    [[nodiscard]] usize block_count() const noexcept {
        usize count = 0;
        Block* block = current_block_;
        while (block) {
            ++count;
            block = block->prev;
        }
        return count;
    }
    
    // Check if empty
    [[nodiscard]] bool empty() const noexcept {
        return current_block_ == nullptr;
    }

private:
    struct Block {
        Block* prev;
        usize size;
        byte data[1];  // Flexible array member
    };
    
    bool allocate_new_block(usize min_size) noexcept {
        // Allocate block large enough for requested size
        usize alloc_size = block_size_;
        if (min_size > block_size_) {
            alloc_size = min_size;
        }
        
        // Allocate block header + data
        usize total_size = sizeof(Block) - sizeof(byte) + alloc_size;
        void* ptr = std::malloc(total_size);
        if (!ptr) return false;
        
        Block* new_block = static_cast<Block*>(ptr);
        new_block->prev = current_block_;
        new_block->size = alloc_size;
        
        current_block_ = new_block;
        current_offset_ = 0;
        
        return true;
    }
    
    usize block_size_;
    Block* current_block_;
    usize current_offset_;
    usize total_allocated_;
};

// RAII wrapper for arena allocation
template <typename T>
class ArenaPtr {
public:
    ArenaPtr(Arena& arena, T* ptr) : arena_(&arena), ptr_(ptr) {}
    
    ArenaPtr(const ArenaPtr&) = delete;
    ArenaPtr& operator=(const ArenaPtr&) = delete;
    
    ArenaPtr(ArenaPtr&& other) noexcept 
        : arena_(other.arena_), ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }
    
    ArenaPtr& operator=(ArenaPtr&& other) noexcept {
        if (this != &other) {
            ptr_ = other.ptr_;
            arena_ = other.arena_;
            other.ptr_ = nullptr;
        }
        return *this;
    }
    
    ~ArenaPtr() = default;  // Arena owns the memory
    
    [[nodiscard]] T* get() noexcept { return ptr_; }
    [[nodiscard]] const T* get() const noexcept { return ptr_; }
    
    [[nodiscard]] T& operator*() noexcept { return *ptr_; }
    [[nodiscard]] const T& operator*() const noexcept { return *ptr_; }
    [[nodiscard]] T* operator->() noexcept { return ptr_; }
    [[nodiscard]] const T* operator->() const noexcept { return ptr_; }
    
    [[nodiscard]] explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
    Arena* arena_;
    T* ptr_;
};

// Helper to construct objects in arena
template <typename T, typename... Args>
[[nodiscard]] ArenaPtr<T> make_arena_ptr(Arena& arena, Args&&... args) noexcept {
    T* ptr = arena.allocate<T>();
    if (ptr) {
        new (ptr) T(std::forward<Args>(args)...);
    }
    return ArenaPtr<T>(arena, ptr);
}

} // namespace vectortick
