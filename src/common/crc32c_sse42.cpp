#include "vectortick/common/crc32c.hpp"

#if defined(__x86_64__) || defined(_M_X64)

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC target("sse4.2")
#endif

#include <nmmintrin.h>
#include <cstring>

namespace vectortick {

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("sse4.2")))
#endif
u32 Crc32C::compute_sse42(u32 init_crc, const byte* data, usize length) noexcept {
    u32 crc = init_crc;
    const u8* ptr = reinterpret_cast<const u8*>(data);
    
    // Process 8 bytes at a time
    while (length >= 8) {
        u64 val;
        std::memcpy(&val, ptr, 8);
        crc = static_cast<u32>(_mm_crc32_u64(crc, val));
        ptr += 8;
        length -= 8;
    }
    
    // Process 4 bytes
    if (length >= 4) {
        u32 val;
        std::memcpy(&val, ptr, 4);
        crc = _mm_crc32_u32(crc, val);
        ptr += 4;
        length -= 4;
    }
    
    // Process 2 bytes
    if (length >= 2) {
        u16 val;
        std::memcpy(&val, ptr, 2);
        crc = _mm_crc32_u16(crc, val);
        ptr += 2;
        length -= 2;
    }
    
    if (length >= 1) {
        crc = _mm_crc32_u8(crc, *ptr);
    }
    
    return crc;
}

} // namespace vectortick

#else

namespace vectortick {

u32 Crc32C::compute_sse42(u32 init_crc, const byte*, usize) noexcept {
    return init_crc;
}

} // namespace vectortick

#endif
