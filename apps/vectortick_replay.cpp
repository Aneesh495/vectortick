#include <charconv>
// VectorTick Replay Application
// Replays stored market data at specified rates

#include "vectortick/common/types.hpp"
#include "vectortick/replay/replay_engine.hpp"

#include <iostream>
#include <iomanip>
#include <cstring>

using namespace vectortick;

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options] <segment.vts>\n";
    std::cerr << "Options:\n";
    std::cerr << "  -r <rate>         Replay rate limit (events/sec, default: unlimited)\n";
    std::cerr << "  -p                Preserve relative packet timing\n";
    std::cerr << "  -s                Verify sequence continuity\n";
    std::cerr << "  -v                Verbose output (prints each event)\n";
    std::cerr << "  -h                Show this help\n";
}

int main(int argc, char* argv[]) {
    std::string segment_file;
    u64 rate = 0; // 0 = unlimited
    bool verbose = false;
    bool preserve_timing = false;
    bool verify_seq = true;
    
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            const char* value = argv[++i];
            auto end = value + std::strlen(value);
            auto [parsed, error] = std::from_chars(value,end,rate);
            if (error != std::errc{} || parsed != end) { std::cerr << "Invalid rate\n"; return 1; }
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-p") == 0) {
            preserve_timing = true;
        } else if (strcmp(argv[i], "-s") == 0) {
            verify_seq = true;
        } else if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            segment_file = argv[i];
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }
    
    if (segment_file.empty()) {
        std::cerr << "Error: No segment file specified\n";
        print_usage(argv[0]);
        return 1;
    }
    
    ReplayOptions options;
    options.rate_limit_events_per_sec = rate;
    options.preserve_relative_timing = preserve_timing;
    options.verify_sequence = verify_seq;
    
    ReplayEngine engine(options);
    
    std::cout << "Replaying segment: " << segment_file << "\n";
    if (rate > 0) {
        std::cout << "Rate limit: " << rate << " events/sec\n";
    } else {
        std::cout << "Rate limit: Unlimited (max speed)\n";
    }
    std::cout << "----------------------------------------\n";
    
    auto callback = [verbose](const CanonicalEvent& ev) {
        if (verbose) {
            std::cout << "Seq=" << ev.sequence 
                      << " Inst=" << ev.instrument_id
                      << " Price=" << ev.price_ticks
                      << " Qty=" << ev.quantity
                      << " Ts=" << ev.exchange_ts_ns << "\n";
        }
    };
    
    auto status = engine.replay_segment(segment_file, callback);
    if (!status.ok()) {
        std::cerr << "Replay error: " << status.message() << "\n";
        return 1;
    }
    
    const auto& stats = engine.stats();
    std::cout << "\nReplay Summary:\n";
    std::cout << "  Events replayed:    " << stats.total_events << "\n";
    std::cout << "  Min sequence:       " << stats.min_sequence << "\n";
    std::cout << "  Max sequence:       " << stats.max_sequence << "\n";
    std::cout << "  Sequence breaks:    " << stats.sequence_breaks << "\n";
    std::cout << "  Out of order:       " << stats.out_of_order_events << "\n";
    std::cout << "  Elapsed time:       " << std::fixed << std::setprecision(3) 
              << (stats.elapsed_wall_ns / 1e6) << " ms\n";
    std::cout << "  Throughput:         " << std::fixed << std::setprecision(1) 
              << stats.throughput_events_per_sec << " events/sec\n";
    
    return 0;
}
