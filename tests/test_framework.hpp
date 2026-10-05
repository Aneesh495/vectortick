#pragma once

#include "vectortick/common/types.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <iomanip>
#include <sstream>

namespace vectortick {
namespace test {

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> func;
};

class TestRegistry {
public:
    static TestRegistry& instance() {
        static TestRegistry reg;
        return reg;
    }
    
    void add(std::string suite, std::string name, std::function<void()> func) {
        tests_.push_back({std::move(suite), std::move(name), std::move(func)});
    }
    
    const std::vector<TestCase>& tests() const { return tests_; }
    
    int run(const std::string& suite_filter = "") {
        int run_count = 0;
        int pass_count = 0;
        int fail_count = 0;
        
        std::cout << "Running tests" << (suite_filter.empty() ? "" : (" (filter: " + suite_filter + ")")) << "...\n";
        
        for (const auto& tc : tests_) {
            if (!suite_filter.empty() && tc.suite != suite_filter) {
                continue;
            }
            
            ++run_count;
            current_failed = false;
            current_failure_msg.clear();
            
            std::cout << "  [" << tc.suite << "] " << tc.name << "... ";
            std::cout.flush();
            
            tc.func();
            
            if (!current_failed) {
                ++pass_count;
                std::cout << "PASSED\n";
            } else {
                ++fail_count;
                std::cout << "FAILED\n";
                if (!current_failure_msg.empty()) {
                    std::cout << "    " << current_failure_msg << "\n";
                }
            }
        }
        
        std::cout << "\nTest Summary: " << pass_count << " passed, " << fail_count << " failed, "
                  << run_count << " total run.\n";
        
        return fail_count == 0 && run_count > 0 ? 0 : 1;
    }
    
    static bool current_failed;
    static std::string current_failure_msg;

private:
    std::vector<TestCase> tests_;
};

inline bool TestRegistry::current_failed = false;
inline std::string TestRegistry::current_failure_msg = "";

struct AutoTestReg {
    AutoTestReg(const char* suite, const char* name, void (*fn)()) {
        TestRegistry::instance().add(suite, name, fn);
    }
};

} // namespace test
} // namespace vectortick

#define VT_TEST(suite, name) \
    static void vt_test_##suite##_##name(); \
    static ::vectortick::test::AutoTestReg vt_reg_##suite##_##name(#suite, #name, &vt_test_##suite##_##name); \
    static void vt_test_##suite##_##name()

#define VT_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            ::vectortick::test::TestRegistry::current_failed = true; \
            ::vectortick::test::TestRegistry::current_failure_msg = \
                std::string(__FILE__) + ":" + std::to_string(__LINE__) + " Assertion failed: " #cond; \
            return; \
        } \
    } while (0)

#define VT_ASSERT_EQ(a, b) \
    do { \
        auto val_a = (a); \
        auto val_b = (b); \
        if (val_a != val_b) { \
            ::vectortick::test::TestRegistry::current_failed = true; \
            std::ostringstream ss; \
            ss << __FILE__ << ":" << __LINE__ << " Assertion failed: " #a " == " #b \
               << " (got " << val_a << " vs " << val_b << ")"; \
            ::vectortick::test::TestRegistry::current_failure_msg = ss.str(); \
            return; \
        } \
    } while (0)

#define VT_ASSERT_BYTES_EQ(data, expected, len) \
    do { \
        const ::vectortick::u8* d = reinterpret_cast<const ::vectortick::u8*>(data); \
        const ::vectortick::u8* e = reinterpret_cast<const ::vectortick::u8*>(expected); \
        for (::vectortick::usize i = 0; i < (len); ++i) { \
            if (d[i] != e[i]) { \
                ::vectortick::test::TestRegistry::current_failed = true; \
                std::ostringstream ss; \
                ss << __FILE__ << ":" << __LINE__ << " Byte mismatch at index " << i \
                   << " expected 0x" << std::hex << std::setw(2) << std::setfill('0') << (int)e[i] \
                   << " got 0x" << std::setw(2) << std::setfill('0') << (int)d[i]; \
                ::vectortick::test::TestRegistry::current_failure_msg = ss.str(); \
                return; \
            } \
        } \
    } while (0)
