#pragma once

#include "types.hpp"
#include "status.hpp"

namespace vectortick {

// CRC32C dispatch policy for runtime dispatch and test-only overrides
enum class CrcDispatchPolicy : u8 {
    Auto = 0,          // Hardware acceleration if supported, otherwise software
    ForceSoftware = 1, // Always use software table implementation
    ForceHardware = 2  // Force hardware acceleration (fails if unsupported)
};

// CRC32C implementation with hardware acceleration
// Castagnoli polynomial (0x1EDC6F41) used by iSCSI, Ceph, and modern columnar storage
class Crc32C {
public:
    // Initialize CRC table (immutable constexpr table in current implementation; no-op preserved for API compatibility)
    static void init() noexcept;
    
    // Compute CRC32C of a buffer (dispatches according to policy)
    [[nodiscard]] static u32 compute(const byte* data, usize length) noexcept;
    [[nodiscard]] static u32 compute(u32 init_crc, const byte* data, usize length) noexcept;
    
    // Hardware-accelerated version if available, otherwise falls back to software
    [[nodiscard]] static u32 compute_hw(const byte* data, usize length) noexcept;
    [[nodiscard]] static u32 compute_hw(u32 init_crc, const byte* data, usize length) noexcept;

    // Direct software table-based computation
    [[nodiscard]] static u32 compute_sw(const byte* data, usize length) noexcept;
    [[nodiscard]] static u32 compute_sw(u32 init_crc, const byte* data, usize length) noexcept;
    
    // Check if hardware acceleration is available on this host
    [[nodiscard]] static bool is_hw_supported() noexcept;

    // Test-only dispatch override
    [[nodiscard]] static Status set_dispatch_override(CrcDispatchPolicy policy) noexcept;
    [[nodiscard]] static CrcDispatchPolicy get_dispatch_override() noexcept;
    static void reset_dispatch_override() noexcept;
    
    // Combine two CRCs (CRC(A || B) = combine(CRC(A), CRC(B), len(B)))
    [[nodiscard]] static u32 combine(u32 crc_a, u32 crc_b, usize len_b) noexcept;
    
private:
    // SSE4.2 hardware implementation (x86-64)
    [[nodiscard]] static u32 compute_sse42(u32 init_crc, const byte* data, usize length) noexcept;
    
    // ARM CRC hardware implementation (AArch64)
    [[nodiscard]] static u32 compute_arm(u32 init_crc, const byte* data, usize length) noexcept;
};

// Convenience functions
inline u32 crc32c(const byte* data, usize length) noexcept {
    return Crc32C::compute(data, length);
}

inline u32 crc32c(u32 init_crc, const byte* data, usize length) noexcept {
    return Crc32C::compute(init_crc, data, length);
}

} // namespace vectortick
