#include "../test_framework.hpp"
#include "vectortick/common/types.hpp"
#include "vectortick/common/endian.hpp"
#include "vectortick/common/crc32c.hpp"
#include "vectortick/model/event.hpp"

#include <cstring>
#include <thread>
#include <vector>

using namespace vectortick;

VT_TEST(types_tests, integer_sizes) {
    VT_ASSERT_EQ(sizeof(u8), 1ULL);
    VT_ASSERT_EQ(sizeof(u16), 2ULL);
    VT_ASSERT_EQ(sizeof(u32), 4ULL);
    VT_ASSERT_EQ(sizeof(u64), 8ULL);
    VT_ASSERT_EQ(sizeof(i8), 1ULL);
    VT_ASSERT_EQ(sizeof(i16), 2ULL);
    VT_ASSERT_EQ(sizeof(i32), 4ULL);
    VT_ASSERT_EQ(sizeof(i64), 8ULL);
    VT_ASSERT_EQ(sizeof(f32), 4ULL);
    VT_ASSERT_EQ(sizeof(f64), 8ULL);
}

VT_TEST(types_tests, canonical_event_size) {
    VT_ASSERT_EQ(sizeof(CanonicalEvent), 56ULL);
}

VT_TEST(types_tests, endian_roundtrip) {
    u16 val16 = 0x1234;
    u32 val32 = 0x12345678;
    u64 val64 = 0x123456789ABCDEF0ULL;
    
    u16 net16 = host_to_network(val16);
    u32 net32 = host_to_network(val32);
    u64 net64 = host_to_network(val64);
    
    VT_ASSERT_EQ(network_to_host(net16), val16);
    VT_ASSERT_EQ(network_to_host(net32), val32);
    VT_ASSERT_EQ(network_to_host(net64), val64);
}

VT_TEST(types_tests, crc32c_known_vectors) {
    // Known-answer test vectors for CRC32C (Castagnoli 0x1EDC6F41):
    // "123456789" -> 0xE3069283
    const char* str = "123456789";
    u32 crc = crc32c(reinterpret_cast<const u8*>(str), 9);
    VT_ASSERT_EQ(crc, 0xE3069283U);

    // Empty buffer has CRC 0
    VT_ASSERT_EQ(crc32c(nullptr, 0), 0U);
    VT_ASSERT_EQ(crc32c(reinterpret_cast<const u8*>(""), 0), 0U);

    // 32 zero bytes
    u8 zeros[32] = {0};
    u32 crc_zeros = crc32c(zeros, sizeof(zeros));
    VT_ASSERT_EQ(crc_zeros, 0x8A9136AAU);

    // 32 0xFF bytes
    u8 ones[32];
    std::memset(ones, 0xFF, sizeof(ones));
    u32 crc_ones = crc32c(ones, sizeof(ones));
    VT_ASSERT_EQ(crc_ones, 0x62A8AB43U);
}

VT_TEST(types_tests, crc32c_dispatch_override_and_agreement) {
    const char* str = "123456789";
    const byte* ptr = reinterpret_cast<const byte*>(str);

    // Test forced software dispatch
    auto st_sw = Crc32C::set_dispatch_override(CrcDispatchPolicy::ForceSoftware);
    VT_ASSERT(st_sw.ok());
    VT_ASSERT_EQ(static_cast<int>(Crc32C::get_dispatch_override()), static_cast<int>(CrcDispatchPolicy::ForceSoftware));
    VT_ASSERT_EQ(Crc32C::compute(ptr, 9), 0xE3069283U);

    // Test hardware dispatch if supported, or clean rejection if not
    bool hw_supported = Crc32C::is_hw_supported();
    auto st_hw = Crc32C::set_dispatch_override(CrcDispatchPolicy::ForceHardware);
    if (hw_supported) {
        VT_ASSERT(st_hw.ok());
        VT_ASSERT_EQ(Crc32C::compute(ptr, 9), 0xE3069283U);
    } else {
        VT_ASSERT(!st_hw.ok());
        VT_ASSERT_EQ(static_cast<int>(st_hw.code()), static_cast<int>(StatusCode::NotImplemented));
    }

    // Reset override
    Crc32C::reset_dispatch_override();
    VT_ASSERT_EQ(static_cast<int>(Crc32C::get_dispatch_override()), static_cast<int>(CrcDispatchPolicy::Auto));
    VT_ASSERT_EQ(Crc32C::compute(ptr, 9), 0xE3069283U);
}

VT_TEST(types_tests, crc32c_unaligned_and_tail_lengths) {
    std::vector<u8> buffer(1024);
    for (size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = static_cast<u8>((i * 37 + 13) & 0xFF);
    }

    // Test across unaligned offsets 0..15 and lengths 0..128
    for (size_t offset = 0; offset <= 15; ++offset) {
        for (size_t len = 0; len <= 128; ++len) {
            const byte* data = reinterpret_cast<const byte*>(buffer.data() + offset);

            // Compute via direct software
            u32 sw_crc = Crc32C::compute_sw(0, data, len);

            // Compute via Auto/hardware
            u32 auto_crc = Crc32C::compute(data, len);
            VT_ASSERT_EQ(auto_crc, sw_crc);

            // Test seeded incremental updates: split into len/2 and len - len/2
            if (len > 1) {
                size_t p1 = len / 2;
                size_t p2 = len - p1;
                u32 split_crc = Crc32C::compute(Crc32C::compute(data, p1), data + p1, p2);
                VT_ASSERT_EQ(split_crc, sw_crc);
            }
        }
    }
}

VT_TEST(types_tests, crc32c_concurrent_first_use) {
    constexpr int kThreads = 16;
    constexpr int kIters = 500;
    std::vector<std::thread> threads;
    std::vector<bool> thread_ok(kThreads, false);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t, &thread_ok]() {
            bool all_ok = true;
            for (int i = 0; i < kIters; ++i) {
                char buf[64];
                int len = std::snprintf(buf, sizeof(buf), "thread_%d_iter_%d", t, i);
                u32 crc1 = crc32c(reinterpret_cast<const byte*>(buf), static_cast<size_t>(len));
                u32 crc2 = Crc32C::compute_sw(0, reinterpret_cast<const byte*>(buf), static_cast<size_t>(len));
                if (crc1 != crc2) {
                    all_ok = false;
                    break;
                }
            }
            thread_ok[t] = all_ok;
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    for (int t = 0; t < kThreads; ++t) {
        VT_ASSERT(thread_ok[t]);
    }
}

