#include "../test_framework.hpp"
#include "vectortick/common/crc32c.hpp"
#include <array>
#include <barrier>
#include <thread>
using namespace vectortick;
namespace {
u32 independent_crc(u32 seed, const u8 *data, usize length) {
    u32 crc = ~seed;
    for (usize k = 0; k < length; ++k) {
        crc ^= data[k];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0x82F63B78U : 0U);
    }
    return ~crc;
}
} // namespace
VT_TEST(crc_concurrency_tests, simultaneous_first_dispatch) {
    // This suite runs in a fresh process, so all threads enter the first CRC call together.
    std::barrier ready(16);
    std::array<u8, 16> ok{};
    std::vector<std::thread> threads;
    for (usize t = 0; t < 16; ++t)
        threads.emplace_back([&, t]() {
            std::array<u8, 129> bytes{};
            for (usize k = 0; k < bytes.size(); ++k)
                bytes[k] = static_cast<u8>(t * 31 + k);
            ready.arrive_and_wait();
            ok[t] = 1;
            for (u32 seed = 0; seed < 100; ++seed)
                if (Crc32C::compute(seed, reinterpret_cast<const byte *>(bytes.data() + 1), 128) !=
                    independent_crc(seed, bytes.data() + 1, 128))
                    ok[t] = 0;
        });
    for (auto &t : threads)
        t.join();
    for (auto value : ok)
        VT_ASSERT(value == 1);
}
VT_TEST(types_tests, independent_crc_lengths_seeds_and_combination) {
    std::vector<u8> bytes(1050);
    for (usize k = 0; k < bytes.size(); ++k)
        bytes[k] = static_cast<u8>(k * 71 + 19);
    for (usize offset = 0; offset < 16; ++offset)
        for (usize length = 0; length <= 1024; ++length)
            for (u32 seed : {0U, 1U, 0x80000000U, 0xffffffffU}) {
                auto expected = independent_crc(seed, bytes.data() + offset, length);
                const auto *ptr = reinterpret_cast<const byte *>(bytes.data() + offset);
                VT_ASSERT_EQ(Crc32C::compute_sw(seed, ptr, length), expected);
                VT_ASSERT_EQ(Crc32C::compute_hw(seed, ptr, length), expected);
                if (seed == 0 && offset == 0) {
                    usize split = length / 2;
                    auto a = Crc32C::compute(ptr,split), b = Crc32C::compute(ptr+split,length-split);
                    VT_ASSERT_EQ(Crc32C::combine(a,b,length-split),expected);
                }
            }
}
