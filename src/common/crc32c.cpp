#include "vectortick/common/crc32c.hpp"

#include <atomic>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC target("+crc")
#endif
#include <arm_acle.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif
#endif

namespace vectortick {

namespace {

// CRC32C polynomial: 0x1EDC6F41 (Castagnoli)
// Reflected polynomial: 0x82F63B78
struct Crc32cTable {
    u32 table[256];
    constexpr Crc32cTable() : table{} {
        constexpr u32 polynomial = 0x82F63B78;
        for (u32 i = 0; i < 256; ++i) {
            u32 crc = i;
            for (int j = 0; j < 8; ++j) {
                if (crc & 1) {
                    crc = (crc >> 1) ^ polynomial;
                } else {
                    crc >>= 1;
                }
            }
            table[i] = crc;
        }
    }
};

// Purely immutable constexpr lookup table constructed at compile-time
inline constexpr Crc32cTable kCrc32cTable{};

// Thread-safe dispatch policy override for testing
std::atomic<u8> s_dispatch_policy{static_cast<u8>(CrcDispatchPolicy::Auto)};

} // namespace

void Crc32C::init() noexcept {
    // Immutable compile-time table is always ready; no-op preserved for backwards compatibility
}

bool Crc32C::is_hw_supported() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    u32 eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        return (ecx & bit_SSE4_2) != 0;
    }
    return false;
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(__APPLE__)
    int val = 0;
    size_t len = sizeof(val);
    if (sysctlbyname("hw.optional.armv8_crc32", &val, &len, nullptr, 0) == 0) {
        return val != 0;
    }
    return false;
#elif defined(__linux__)
    unsigned long hwcap = getauxval(AT_HWCAP);
#ifndef HWCAP_CRC32
#define HWCAP_CRC32 (1 << 7)
#endif
    return (hwcap & HWCAP_CRC32) != 0;
#else
    return false;
#endif
#else
    return false;
#endif
}

Status Crc32C::set_dispatch_override(CrcDispatchPolicy policy) noexcept {
    if (policy == CrcDispatchPolicy::ForceHardware) {
        if (!is_hw_supported()) {
            return Status(StatusCode::NotImplemented, "Hardware CRC32C acceleration not supported on this host");
        }
    }
    s_dispatch_policy.store(static_cast<u8>(policy), std::memory_order_relaxed);
    return Status::OK();
}

CrcDispatchPolicy Crc32C::get_dispatch_override() noexcept {
    return static_cast<CrcDispatchPolicy>(s_dispatch_policy.load(std::memory_order_relaxed));
}

void Crc32C::reset_dispatch_override() noexcept {
    s_dispatch_policy.store(static_cast<u8>(CrcDispatchPolicy::Auto), std::memory_order_relaxed);
}

u32 Crc32C::compute_sw(u32 init_crc, const byte* data, usize length) noexcept {
    if (data == nullptr || length == 0) {
        return init_crc;
    }
    u32 crc = init_crc ^ 0xFFFFFFFF;
    const u8* ptr = reinterpret_cast<const u8*>(data);
    const u8* end = ptr + length;
    while (ptr < end) {
        crc = kCrc32cTable.table[(crc ^ *ptr) & 0xFF] ^ (crc >> 8);
        ++ptr;
    }
    return crc ^ 0xFFFFFFFF;
}

u32 Crc32C::compute_sw(const byte* data, usize length) noexcept {
    return compute_sw(0, data, length);
}

#if defined(__aarch64__) || defined(_M_ARM64)
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("+crc")))
#endif
u32 Crc32C::compute_arm(u32 init_crc, const byte* data, usize length) noexcept {
    u32 crc = init_crc;
    const u8* ptr = reinterpret_cast<const u8*>(data);
    
    while (length >= 8) {
        u64 val;
        std::memcpy(&val, ptr, 8);
        crc = __crc32cd(crc, val);
        ptr += 8;
        length -= 8;
    }
    
    if (length >= 4) {
        u32 val;
        std::memcpy(&val, ptr, 4);
        crc = __crc32cw(crc, val);
        ptr += 4;
        length -= 4;
    }
    
    if (length >= 2) {
        u16 val;
        std::memcpy(&val, ptr, 2);
        crc = __crc32ch(crc, val);
        ptr += 2;
        length -= 2;
    }
    
    if (length >= 1) {
        crc = __crc32cb(crc, *ptr);
    }
    
    return crc;
}
#else
u32 Crc32C::compute_arm(u32 init_crc, const byte*, usize) noexcept {
    return init_crc;
}
#endif

u32 Crc32C::compute_hw(u32 init_crc, const byte* data, usize length) noexcept {
    if (data == nullptr || length == 0) {
        return init_crc;
    }
#if defined(__x86_64__) || defined(_M_X64)
    if (is_hw_supported()) {
        u32 acc = init_crc ^ 0xFFFFFFFF;
        acc = compute_sse42(acc, data, length);
        return acc ^ 0xFFFFFFFF;
    }
#elif defined(__aarch64__) || defined(_M_ARM64)
    if (is_hw_supported()) {
        u32 acc = init_crc ^ 0xFFFFFFFF;
        acc = compute_arm(acc, data, length);
        return acc ^ 0xFFFFFFFF;
    }
#endif
    return compute_sw(init_crc, data, length);
}

u32 Crc32C::compute_hw(const byte* data, usize length) noexcept {
    return compute_hw(0, data, length);
}

u32 Crc32C::compute(u32 init_crc, const byte* data, usize length) noexcept {
    if (data == nullptr || length == 0) {
        return init_crc;
    }
    
    CrcDispatchPolicy policy = static_cast<CrcDispatchPolicy>(s_dispatch_policy.load(std::memory_order_relaxed));
    if (policy == CrcDispatchPolicy::ForceSoftware) {
        return compute_sw(init_crc, data, length);
    }
    if (policy == CrcDispatchPolicy::ForceHardware) {
        return compute_hw(init_crc, data, length);
    }
    // Auto policy
    if (is_hw_supported()) {
        return compute_hw(init_crc, data, length);
    }
    return compute_sw(init_crc, data, length);
}

u32 Crc32C::compute(const byte* data, usize length) noexcept {
    return compute(0, data, length);
}

u32 Crc32C::combine(u32 crc_a, u32 crc_b, usize len_b) noexcept {
    if (len_b == 0) return crc_a;
    u32 result = crc_a;
    for (usize i = 0; i < len_b; ++i) {
        result = (result >> 8) ^ kCrc32cTable.table[result & 0xFF];
    }
    result ^= crc_b;
    return result;
}

} // namespace vectortick
