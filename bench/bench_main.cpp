// VectorTick Benchmark Suite
// Performance benchmarks for core operations

#include "vectortick/common/types.hpp"
#include "vectortick/common/crc32c.hpp"
#include "vectortick/model/event.hpp"
#include "vectortick/ir/builder.hpp"
#include "vectortick/query/parser.hpp"
#include "vectortick/execution/reference_interpreter.hpp"
#include "vectortick/execution/vector_executor.hpp"
#include "vectortick/jit/jit_compiler.hpp"
#include "vectortick/storage/segment_writer.hpp"
#include "vectortick/storage/segment_reader.hpp"
#include "vectortick/replay/replay_engine.hpp"

#include <iostream>
#include <iomanip>
#include <chrono>
#include <random>
#include <memory>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <unistd.h>

using namespace vectortick;

class Timer {
public:
    Timer() : start_(std::chrono::high_resolution_clock::now()) {}
    
    double elapsed_ms() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(now - start_).count();
    }
    
    double elapsed_sec() const {
        return elapsed_ms() / 1000.0;
    }

private:
    std::chrono::high_resolution_clock::time_point start_;
};

struct BenchmarkMetrics {
    double crc32c_gbps{0.0};
    double interpreter_mops{0.0};
    double jit_mops{0.0};
    double vector_scan_mops{0.0};
    double vector_agg_mops{0.0};
    double storage_write_mops{0.0};
    double storage_read_mops{0.0};
    double replay_mops{0.0};
};

BenchmarkMetrics g_metrics;

void bench_crc32c() {
    const size_t data_size = 1024 * 1024; // 1MB
    const size_t iterations = 200;
    
    std::vector<u8> data(data_size);
    for (size_t i = 0; i < data_size; ++i) {
        data[i] = static_cast<u8>((i * 31 + 17) & 0xFF);
    }
    
    std::cout << "[1] CRC32C Hardware / Table Benchmark\n";
    
    Timer timer;
    u64 total_bytes = 0;
    u32 result = 0;
    
    for (size_t i = 0; i < iterations; ++i) {
        result = crc32c(data.data(), data_size);
        total_bytes += data_size;
    }
    
    double elapsed = timer.elapsed_sec();
    double throughput_gbps = (total_bytes / elapsed) / (1024.0 * 1024.0 * 1024.0);
    g_metrics.crc32c_gbps = throughput_gbps;
    
    std::cout << "  Throughput: " << std::fixed << std::setprecision(2) 
              << throughput_gbps << " GB/s (" << total_bytes / (1024 * 1024) << " MB in " 
              << elapsed * 1000.0 << " ms, crc=0x" << std::hex << result << std::dec << ")\n\n";
}

void bench_interpreter_and_jit() {
    const size_t iterations = 500000;
    
    ir::Builder builder;
    ir::Function* func = builder.create_function("bench_math");
    ir::ValueId c1 = builder.create_const_u64(100);
    ir::ValueId c2 = builder.create_const_u64(200);
    ir::ValueId sum = builder.create_add(c1, c2, ir::Type::U64);
    builder.create_return(sum);
    
    CanonicalEvent event{};
    
    // Interpreter
    std::cout << "[2] Reference Interpreter (Scalar Oracle)\n";
    ReferenceInterpreter interp;
    Timer timer_interp;
    u64 interp_res = 0;
    for (size_t i = 0; i < iterations; ++i) {
        auto r = interp.execute(func, &event);
        if (r.ok()) interp_res = r.value();
    }
    double elapsed_interp = timer_interp.elapsed_sec();
    double mops_interp = (iterations / elapsed_interp) / 1e6;
    g_metrics.interpreter_mops = mops_interp;
    std::cout << "  Result:     " << interp_res << "\n";
    std::cout << "  Throughput: " << std::fixed << std::setprecision(2) << mops_interp << " M op/sec\n\n";

    // Host JIT
    std::cout << "[3] Native Host JIT Execution\n";
    jit::JitCompiler compiler;
    auto comp_res = compiler.compile(func);
    if (!comp_res.ok()) {
        std::cerr << "  JIT compile failed: " << comp_res.status().message() << "\n\n";
        return;
    }

    auto jit_fn = comp_res.value();
    Timer timer_jit;
    u64 jit_res = 0;
    for (size_t i = 0; i < iterations; ++i) {
        jit_res = jit_fn(&event);
    }
    double elapsed_jit = timer_jit.elapsed_sec();
    double mops_jit = (iterations / elapsed_jit) / 1e6;
    g_metrics.jit_mops = mops_jit;
    std::cout << "  Result:     " << jit_res << "\n";
    std::cout << "  Throughput: " << std::fixed << std::setprecision(2) << mops_jit << " M op/sec ("
              << (mops_jit / mops_interp) << "x speedup over interpreter)\n\n";
}

void bench_vector_execution() {
    constexpr usize N = 100000;
    std::cout << "[4] Portable Vectorized Engine (Batch Columnar + SIMD)\n";
    std::cout << "  Rows: " << N << "\n";

    std::vector<CanonicalEvent> events(N);
    for (usize i = 0; i < N; ++i) {
        events[i].sequence = i + 1;
        events[i].exchange_ts_ns = 1704067200000000000ULL + i * 100;
        events[i].instrument_id = 100 + static_cast<u32>(i % 10);
        events[i].price_ticks = 20000 + static_cast<i64>(i % 500);
        events[i].quantity = 10 + static_cast<u32>(i % 50);
    }

    VectorExecutor exec;
    std::cout << "  SIMD Dispatch: " << SimdDispatcher::arch_name(exec.arch()) << "\n";

    // Scan + Filter query
    query::Parser p_filter("SELECT instrument_id, price_ticks, quantity WHERE price_ticks > 20250");
    auto ast_filter = p_filter.parse_query();
    
    constexpr usize iters = 20;
    Timer timer_scan;
    for (usize i = 0; i < iters; ++i) {
        auto res = exec.execute_events(events, ast_filter.value().get());
        (void)res;
    }
    double elapsed_scan = timer_scan.elapsed_sec();
    double scan_mops = (N * iters / elapsed_scan) / 1e6;
    g_metrics.vector_scan_mops = scan_mops;
    std::cout << "  Filter Scan:    " << std::fixed << std::setprecision(2) << scan_mops << " M rows/sec\n";

    // Aggregations query
    query::Parser p_agg("SELECT COUNT(*), SUM(quantity), MIN(price_ticks), MAX(price_ticks)");
    auto ast_agg = p_agg.parse_query();
    Timer timer_agg;
    for (usize i = 0; i < iters; ++i) {
        auto res = exec.execute_events(events, ast_agg.value().get());
        (void)res;
    }
    double elapsed_agg = timer_agg.elapsed_sec();
    double agg_mops = (N * iters / elapsed_agg) / 1e6;
    g_metrics.vector_agg_mops = agg_mops;
    std::cout << "  Aggregations:   " << std::fixed << std::setprecision(2) << agg_mops << " M rows/sec\n\n";
}

void bench_storage_and_replay() {
    constexpr usize N = 50000;
    std::string seg_file = (std::filesystem::temp_directory_path() / ("bench_temp_" + std::to_string(getpid()) + ".vts")).string();
    std::error_code ec;
    std::filesystem::remove(seg_file, ec);

    std::cout << "[5] VTS1 Storage Serialization & Columnar Read\n";
    std::cout << "  Rows: " << N << "\n";

    Timer timer_write;
    {
        SegmentWriter writer(1, N);
        for (usize i = 0; i < N; ++i) {
            CanonicalEvent ev{};
            ev.sequence = i + 1;
            ev.exchange_ts_ns = 1704067200000000000ULL + i * 50;
            ev.receive_ts_ns = ev.exchange_ts_ns + 20;
            ev.instrument_id = 101;
            ev.event_type = EventType::Trade;
            ev.side = Side::Bid;
            ev.price_ticks = 50000;
            ev.quantity = 100;
            (void)writer.add_event(ev);
        }
        auto w_st = writer.write_to_file(seg_file);
        if (!w_st.ok()) {
            std::cerr << "Failed to write bench segment: " << w_st.message() << "\n";
            return;
        }
    }
    double elapsed_write = timer_write.elapsed_sec();
    double write_mops = (N / elapsed_write) / 1e6;
    g_metrics.storage_write_mops = write_mops;
    std::cout << "  Columnar Write: " << std::fixed << std::setprecision(2) << write_mops << " M events/sec\n";

    Timer timer_read;
    {
        SegmentReader reader;
        auto o_st = reader.open(seg_file);
        if (o_st.ok()) {
            std::vector<CanonicalEvent> read_events;
            (void)reader.read_all_events(read_events);
        }
    }
    double elapsed_read = timer_read.elapsed_sec();
    double read_mops = (N / elapsed_read) / 1e6;
    g_metrics.storage_read_mops = read_mops;
    std::cout << "  Columnar Read:  " << std::fixed << std::setprecision(2) << read_mops << " M events/sec\n\n";

    std::cout << "[6] Deterministic Replay Throughput\n";
    ReplayOptions r_opt;
    r_opt.rate_limit_events_per_sec = 0;
    r_opt.verify_sequence = true;
    ReplayEngine replay(r_opt);
    (void)replay.replay_segment(seg_file);
    double replay_mops = replay.stats().throughput_events_per_sec / 1e6;
    g_metrics.replay_mops = replay_mops;
    std::cout << "  Replay Speed:   " << std::fixed << std::setprecision(2) << replay_mops << " M events/sec\n\n";

    std::filesystem::remove(seg_file, ec);
}

void write_json_results(const std::string& path) {
    std::ofstream out(path);
    out << "{\n";
    out << "  \"crc32c_gbps\": " << g_metrics.crc32c_gbps << ",\n";
    out << "  \"interpreter_mops\": " << g_metrics.interpreter_mops << ",\n";
    out << "  \"jit_mops\": " << g_metrics.jit_mops << ",\n";
    out << "  \"vector_scan_mops\": " << g_metrics.vector_scan_mops << ",\n";
    out << "  \"vector_agg_mops\": " << g_metrics.vector_agg_mops << ",\n";
    out << "  \"storage_write_mops\": " << g_metrics.storage_write_mops << ",\n";
    out << "  \"storage_read_mops\": " << g_metrics.storage_read_mops << ",\n";
    out << "  \"replay_mops\": " << g_metrics.replay_mops << "\n";
    out << "}\n";
    std::cout << "Benchmark results written to " << path << "\n";
}

int main(int argc, char* argv[]) {
    std::string json_output = "";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
            json_output = argv[++i];
        }
    }

    std::cout << "========================================================\n";
    std::cout << "               VectorTick Benchmark Suite               \n";
    std::cout << "========================================================\n\n";
    
    bench_crc32c();
    bench_interpreter_and_jit();
    bench_vector_execution();
    bench_storage_and_replay();

    if (!json_output.empty()) {
        write_json_results(json_output);
    }
    
    std::cout << "========================================================\n";
    std::cout << "                  Benchmarks Complete!                  \n";
    std::cout << "========================================================\n";
    return 0;
}
