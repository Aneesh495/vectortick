#pragma once

#include "../common/types.hpp"
#include "../common/status.hpp"
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

namespace vectortick {

// Aligned buffer for SIMD operations
// Guarantees 64-byte alignment for AVX-512 compatibility
class AlignedBuffer {
public:
    AlignedBuffer() : data_(nullptr), size_(0), capacity_(0) {}
    
    explicit AlignedBuffer(usize size) 
        : data_(nullptr), size_(0), capacity_(0) {
        (void)allocate(size);
    }
    
    ~AlignedBuffer() {
        deallocate();
    }
    
    // Non-copyable
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    
    // Movable
    AlignedBuffer(AlignedBuffer&& other) noexcept
        : data_(other.data_), size_(other.size_), capacity_(other.capacity_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }
    
    AlignedBuffer& operator=(AlignedBuffer&& other) noexcept {
        if (this != &other) {
            deallocate();
            data_ = other.data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.data_ = nullptr;
            other.size_ = 0;
            other.capacity_ = 0;
        }
        return *this;
    }
    
    // Allocate buffer
    [[nodiscard]] Status allocate(usize size) noexcept {
        if (size == 0) {
            deallocate();
            return Status::OK();
        }
        
        // Align to 64 bytes
        constexpr usize alignment = 64;
        usize alloc_size = ((size + alignment - 1) / alignment) * alignment;
        
        // Use aligned_alloc (C++17)
        void* ptr = std::aligned_alloc(alignment, alloc_size);
        if (!ptr) {
            return Status(StatusCode::AllocationFailed, "aligned_alloc failed");
        }
        
        deallocate();
        data_ = static_cast<byte*>(ptr);
        size_ = size;
        capacity_ = alloc_size;
        return Status::OK();
    }
    
    // Resize buffer (preserves existing data if growing)
    [[nodiscard]] Status resize(usize new_size) noexcept {
        if (new_size <= capacity_) {
            size_ = new_size;
            return Status::OK();
        }
        
        // Need to reallocate
        AlignedBuffer new_buf;
        auto status = new_buf.allocate(new_size);
        if (!status.ok()) {
            return status;
        }
        
        // Copy existing data
        if (data_ && size_ > 0) {
            std::memcpy(new_buf.data_, data_, size_);
            new_buf.size_ = size_;
        }
        
        *this = std::move(new_buf);
        size_ = new_size;
        return Status::OK();
    }
    
    // Deallocate buffer
    void deallocate() noexcept {
        if (data_) {
            std::free(data_);
            data_ = nullptr;
            size_ = 0;
            capacity_ = 0;
        }
    }
    
    // Access
    [[nodiscard]] byte* data() noexcept { return data_; }
    [[nodiscard]] const byte* data() const noexcept { return data_; }
    [[nodiscard]] usize size() const noexcept { return size_; }
    [[nodiscard]] usize capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    
    // Element access
    [[nodiscard]] byte& operator[](usize index) noexcept {
        return data_[index];
    }
    
    [[nodiscard]] const byte& operator[](usize index) const noexcept {
        return data_[index];
    }
    
    // Iterator support
    [[nodiscard]] byte* begin() noexcept { return data_; }
    [[nodiscard]] byte* end() noexcept { return data_ + size_; }
    [[nodiscard]] const byte* begin() const noexcept { return data_; }
    [[nodiscard]] const byte* end() const noexcept { return data_ + size_; }
    
    // Clear without deallocating
    void clear() noexcept {
        size_ = 0;
    }
    
    // Check alignment
    [[nodiscard]] bool is_aligned(usize alignment = 64) const noexcept {
        return (reinterpret_cast<uintptr_t>(data_) % alignment) == 0;
    }

private:
    byte* data_;
    usize size_;
    usize capacity_;
};

// Typed aligned buffer
template <typename T>
class TypedAlignedBuffer {
public:
    TypedAlignedBuffer() = default;
    
    explicit TypedAlignedBuffer(usize count) : buffer_(count * sizeof(T)), count_(count) {}
    
    [[nodiscard]] Status allocate(usize count) noexcept {
        auto status = buffer_.allocate(count * sizeof(T));
        if (status.ok()) {
            count_ = count;
        }
        return status;
    }
    
    [[nodiscard]] T* data() noexcept { 
        return reinterpret_cast<T*>(buffer_.data()); 
    }
    
    [[nodiscard]] const T* data() const noexcept { 
        return reinterpret_cast<const T*>(buffer_.data()); 
    }
    
    [[nodiscard]] usize size() const noexcept { return count_; }
    [[nodiscard]] usize byte_size() const noexcept { return count_ * sizeof(T); }
    
    [[nodiscard]] T& operator[](usize index) noexcept {
        return data()[index];
    }
    
    [[nodiscard]] const T& operator[](usize index) const noexcept {
        return data()[index];
    }
    
    [[nodiscard]] T* begin() noexcept { return data(); }
    [[nodiscard]] T* end() noexcept { return data() + count_; }
    [[nodiscard]] const T* begin() const noexcept { return data(); }
    [[nodiscard]] const T* end() const noexcept { return data() + count_; }

private:
    AlignedBuffer buffer_;
    usize count_ = 0;
};

// Scratch buffer for temporary computations
class ScratchBuffer {
public:
    static constexpr usize DefaultSize = 64 * 1024;  // 64 KB default
    
    ScratchBuffer() : buffer_(DefaultSize) {}
    explicit ScratchBuffer(usize size) : buffer_(size) {}
    
    [[nodiscard]] byte* data() noexcept { return buffer_.data(); }
    [[nodiscard]] const byte* data() const noexcept { return buffer_.data(); }
    [[nodiscard]] usize size() const noexcept { return buffer_.size(); }
    
    template <typename T>
    [[nodiscard]] T* as() noexcept {
        return reinterpret_cast<T*>(buffer_.data());
    }
    
    template <typename T>
    [[nodiscard]] usize capacity() const noexcept {
        return buffer_.size() / sizeof(T);
    }

private:
    AlignedBuffer buffer_;
};

} // namespace vectortick
