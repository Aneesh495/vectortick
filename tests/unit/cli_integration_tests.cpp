#include "../test_framework.hpp"
#include "vectortick/storage/segment_writer.hpp"

#include <cstdlib>
#include <string>
#include <fstream>
#include <filesystem>
#include <array>
#include <memory>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

using namespace vectortick;
using namespace vectortick::test;

namespace {

std::string find_bin(const std::string& name) {
#if defined(__APPLE__)
    char path[1024];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        std::string dir = std::filesystem::path(path).parent_path().string();
        if (std::filesystem::exists(dir + "/" + name)) {
            return dir + "/" + name;
        }
    }
#elif defined(__linux__)
    char path[1024];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len > 0) {
        path[len] = '\0';
        std::string dir = std::filesystem::path(path).parent_path().string();
        if (std::filesystem::exists(dir + "/" + name)) {
            return dir + "/" + name;
        }
    }
#endif
    if (std::filesystem::exists("./bin/" + name)) {
        return "./bin/" + name;
    }
    if (std::filesystem::exists("./build-baseline/bin/" + name)) {
        return "./build-baseline/bin/" + name;
    }
    if (std::filesystem::exists("../bin/" + name)) {
        return "../bin/" + name;
    }
    return name;
}

std::string exec_cmd(const std::string& cmd, int& exit_code) {
    std::array<char, 256> buffer;
    std::string result;
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe) {
        exit_code = -1;
        return "";
    }
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    int status = pclose(pipe);
    exit_code = WEXITSTATUS(status);
    return result;
}

void make_cli_segment(const std::string& path, usize count) {
    SegmentWriter writer(1, count);
    for (usize i = 0; i < count; ++i) {
        CanonicalEvent ev{};
        ev.sequence = i + 1;
        ev.exchange_ts_ns = 1704067200000000000ULL + i * 1000;
        ev.receive_ts_ns = ev.exchange_ts_ns + 50;
        ev.instrument_id = (i % 2 == 0) ? 1001 : 1002;
        ev.event_type = EventType::Trade;
        ev.side = Side::Bid;
        ev.price_ticks = 10000 + static_cast<i64>(i * 50);
        ev.quantity = 10 + static_cast<u32>(i);
        ev.venue_id = 1;
        ev.source_id = 1;
        ev.trade_or_order_id = 500000 + i;
        auto st = writer.add_event(ev);
        VT_ASSERT(st.ok());
    }
    auto st = writer.write_to_file(path);
    VT_ASSERT(st.ok());
}

std::string unique_test_path(const std::string& prefix, const std::string& ext) {
    return (std::filesystem::temp_directory_path() / (prefix + "_" + std::to_string(getpid()) + ext)).string();
}

} // namespace

VT_TEST(cli_integration_tests, inspect_schema_and_metadata) {
    std::string seg_file = unique_test_path("test_cli_inspect", ".vts");
    std::error_code ec;
    std::filesystem::remove(seg_file, ec);

    make_cli_segment(seg_file, 50);

    int code = 0;
    std::string out = exec_cmd(find_bin("vectortick_inspect") + " -s -m -d 3 " + seg_file, code);
    VT_ASSERT_EQ(code, 0);
    VT_ASSERT(out.find("Row Count:       50") != std::string::npos);
    VT_ASSERT(out.find("exchange_ts_ns") != std::string::npos);
    VT_ASSERT(out.find("price_ticks") != std::string::npos);
    VT_ASSERT(out.find("Integrity Check: PASSED") != std::string::npos);
    VT_ASSERT(out.find("--- Dumping first 3 rows ---") != std::string::npos);

    std::filesystem::remove(seg_file, ec);
}

VT_TEST(cli_integration_tests, query_filter_and_aggregates) {
    std::string seg_file = unique_test_path("test_cli_query", ".vts");
    std::error_code ec;
    std::filesystem::remove(seg_file, ec);

    make_cli_segment(seg_file, 100);

    int code = 0;
    std::string out = exec_cmd(find_bin("vectortick_query") + " " + seg_file + " \"SELECT COUNT(*), SUM(quantity) WHERE price_ticks > 12000\"", code);
    VT_ASSERT_EQ(code, 0);
    VT_ASSERT(out.find("COUNT()") != std::string::npos);
    VT_ASSERT(out.find("SUM()") != std::string::npos);

    // Limit test
    out = exec_cmd(find_bin("vectortick_query") + " -l 2 " + seg_file + " \"SELECT instrument_id, price_ticks, quantity\"", code);
    VT_ASSERT_EQ(code, 0);
    VT_ASSERT(out.find("instrument_id") != std::string::npos);

    // Interpreter engine test
    out = exec_cmd(find_bin("vectortick_query") + " -e interpreter " + seg_file + " \"SELECT COUNT(*), SUM(quantity) WHERE price_ticks > 12000\"", code);
    VT_ASSERT_EQ(code, 0);
    VT_ASSERT(out.find("COUNT()") != std::string::npos);
    VT_ASSERT(out.find("SUM()") != std::string::npos);

    // JIT engine test
    std::string jit_out = exec_cmd(find_bin("vectortick_query") + " -e jit " + seg_file + " \"SELECT COUNT(*), SUM(quantity) WHERE price_ticks > 12000\"", code);
    VT_ASSERT_EQ(code, 0);
    VT_ASSERT(jit_out.find("COUNT()") != std::string::npos);
    VT_ASSERT(jit_out.find("SUM()") != std::string::npos);

    // JIT JSON output test
    std::string json_jit_out = unique_test_path("test_query_jit_out", ".json");
    out = exec_cmd(find_bin("vectortick_query") + " -e jit --json " + json_jit_out + " " + seg_file + " \"SELECT instrument_id, price_ticks LIMIT 2\"", code);
    VT_ASSERT_EQ(code, 0);
    VT_ASSERT(std::filesystem::exists(json_jit_out));
    {
        std::ifstream in(json_jit_out);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        VT_ASSERT(content.find("\"engine\": \"jit\"") != std::string::npos);
        VT_ASSERT(content.find("\"name\": \"instrument_id\"") != std::string::npos);
        VT_ASSERT(content.find("\"rows\":") != std::string::npos);
    }
    std::filesystem::remove(json_jit_out, ec);

    // JSON output test
    std::string json_out = unique_test_path("test_query_out", ".json");
    out = exec_cmd(find_bin("vectortick_query") + " --json " + json_out + " " + seg_file + " \"SELECT instrument_id, price_ticks LIMIT 2\"", code);
    VT_ASSERT_EQ(code, 0);
    VT_ASSERT(std::filesystem::exists(json_out));
    {
        std::ifstream in(json_out);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        VT_ASSERT(content.find("\"engine\": \"vector\"") != std::string::npos);
        VT_ASSERT(content.find("\"name\": \"instrument_id\"") != std::string::npos);
        VT_ASSERT(content.find("\"rows\":") != std::string::npos);
    }
    std::filesystem::remove(json_out, ec);

    // Invalid engine test (must exit nonzero)
    out = exec_cmd(find_bin("vectortick_query") + " -e nonexistent " + seg_file + " \"SELECT *\"", code);
    VT_ASSERT(code != 0);

    // Invalid limit test (must exit nonzero)
    out = exec_cmd(find_bin("vectortick_query") + " -l notanumber " + seg_file + " \"SELECT *\"", code);
    VT_ASSERT(code != 0);

    std::filesystem::remove(seg_file, ec);
}

VT_TEST(cli_integration_tests, replay_cli_execution) {
    std::string seg_file = unique_test_path("test_cli_replay", ".vts");
    std::error_code ec;
    std::filesystem::remove(seg_file, ec);

    make_cli_segment(seg_file, 100);

    int code = 0;
    std::string out = exec_cmd(find_bin("vectortick_replay") + " -s " + seg_file, code);
    VT_ASSERT_EQ(code, 0);
    VT_ASSERT(out.find("Events replayed:    100") != std::string::npos);
    VT_ASSERT(out.find("Sequence breaks:    0") != std::string::npos);

    std::filesystem::remove(seg_file, ec);
}

VT_TEST(cli_integration_tests, demo_execution_status) {
    int code = 0;
    std::string out = exec_cmd(find_bin("vectortick_demo"), code);
    VT_ASSERT_EQ(code, 0);
    VT_ASSERT(out.find("Demo Completed Successfully!") != std::string::npos);
}

VT_TEST(cli_integration_tests, bench_json_generation) {
    std::string json_file = unique_test_path("test_bench_out", ".json");
    std::error_code ec;
    std::filesystem::remove(json_file, ec);

    int code = 0;
    std::string out = exec_cmd(find_bin("vectortick_bench") + " --json " + json_file, code);
    VT_ASSERT_EQ(code, 0);
    VT_ASSERT(std::filesystem::exists(json_file));

    std::ifstream in(json_file);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    VT_ASSERT(content.find("crc32c_gbps") != std::string::npos);
    VT_ASSERT(content.find("jit_mops") != std::string::npos);
    VT_ASSERT(content.find("vector_scan_mops") != std::string::npos);

    std::filesystem::remove(json_file, ec);
}
