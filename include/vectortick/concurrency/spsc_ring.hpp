#pragma once
#include <memory>

#include "../common/types.hpp"
#include <atomic>
#include <cstddef>

namespace vectortick {

// Single-producer, single-consumer lock-free ring buffer
// Exactly one producer thread and one consumer thread
// Bounded capacity, no overwrite of unread data

template <typename T>
class SpscRing {
public:
    explicit SpscRing(usize capacity = 1024)
        : capacity_(capacity + 1)  // +1 for full/empty distinction
        , buffer_(new std::atomic<T>[capacity_])
        , head_(0)
        , tail_(0) {
        // Ensure capacity is power of 2 for fast modulo
        if (capacity_ & (capacity_ - 1)) {
            usize new_cap = 1;
            while (new_cap < capacity_) new_cap <<= 1;
            capacity_ = new_cap;
            buffer_.reset(new std::atomic<T>[capacity_]);
        }
        mask_ = capacity_ - 1;
    }
    
    // Non-copyable, non-movable
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&) = delete;
    SpscRing& operator=(SpscRing&&) = delete;
    
    // Try to push an element (producer only)
    // Returns true if successful, false if full
    [[nodiscard]] bool try_push(const T& value) noexcept {
        usize current_tail = tail_.load(std::memory_order_relaxed);
        usize next_tail = (current_tail + 1) & mask_;
        
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;  // Full
        }
        
        buffer_[current_tail].store(value, std::memory_order_relaxed);
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }
    
    // Try to push an element (producer only) - move version
    [[nodiscard]] bool try_push(T&& value) noexcept {
        usize current_tail = tail_.load(std::memory_order_relaxed);
        usize next_tail = (current_tail + 1) & mask_;
        
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;  // Full
        }
        
        buffer_[current_tail].store(std::move(value), std::memory_order_relaxed);
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }
    
    // Try to pop an element (consumer only)
    // Returns true if successful, false if empty
    [[nodiscard]] bool try_pop(T& value) noexcept {
        usize current_head = head_.load(std::memory_order_relaxed);
        
        if (current_head == tail_.load(std::memory_order_acquire)) {
            return false;  // Empty
        }
        
        value = buffer_[current_head].load(std::memory_order_relaxed);
        head_.store((current_head + 1) & mask_, std::memory_order_release);
        return true;
    }
    
    // Check if empty (approximate, may race with producer)
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == 
               tail_.load(std::memory_order_acquire);
    }
    
    // Check if full (approximate, may race with consumer)
    [[nodiscard]] bool full() const noexcept {
        return ((tail_.load(std::memory_order_acquire) + 1) & mask_) ==
               head_.load(std::memory_order_acquire);
    }
    
    // Get capacity
    [[nodiscard]] usize capacity() const noexcept {
        return capacity_ - 1;
    }
    
    // Get approximate size (may race)
    [[nodiscard]] usize size() const noexcept {
        usize tail = tail_.load(std::memory_order_acquire);
        usize head = head_.load(std::memory_order_acquire);
        return (tail - head + capacity_) & mask_;
    }
    
    // Reset the ring (not thread-safe, only use when stopped)
    void reset() noexcept {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

private:
    usize capacity_;
    usize mask_;
    std::unique_ptr<std::atomic<T>[]> buffer_;
    
    // Cache-line padded head and tail to avoid false sharing
    alignas(64) std::atomic<usize> head_;
    alignas(64) std::atomic<usize> tail_;
};

// Specialization for simple command structures
struct WorkerCommand {
    enum Type : u32 {
        Stop = 0,
        ProcessSegment = 1,
        ProcessQuery = 2,
        Wait = 3
    };
    
    Type type;
    u32 segment_id;
    u64 arg1;
    u64 arg2;
};

struct WorkerResult {
    enum Status : u32 {
        Success = 0,
        Error = 1,
        Cancelled = 2
    };
    
    Status status;
    u32 worker_id;
    u64 rows_processed;
    u64 result_hash;
};

// Type aliases for common uses
using CommandQueue = SpscRing<WorkerCommand>;
using ResultQueue = SpscRing<WorkerResult>;

} // namespace vectortick
