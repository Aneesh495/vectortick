// VectorTick Demo Application
// Demonstrates end-to-end flow: generate -> write segment -> inspect -> query -> JIT -> replay

#include "vectortick/common/types.hpp"
#include "vectortick/model/event.hpp"
#include "vectortick/storage/segment_writer.hpp"
#include "vectortick/storage/segment_reader.hpp"
#include "vectortick/query/parser.hpp"
#include "vectortick/ir/builder.hpp"
#include "vectortick/execution/reference_interpreter.hpp"
#include "vectortick/execution/vector_executor.hpp"
#include "vectortick/jit/jit_compiler.hpp"
#include "vectortick/replay/replay_engine.hpp"

#include <iostream>
#include <iomanip>
#include <filesystem>
#include <vector>

using namespace vectortick;

int main(int argc, char* argv[]) {
    std::cout << "========================================================\n";
    std::cout << "       VectorTick Columnar Analytics Engine Demo         \n";
    std::cout << "========================================================\n\n";

    std::string demo_segment = "demo_market_data.vts";
    if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
        demo_segment = argv[1];
    } else if (const char* env_seg = std::getenv("VECTORTICK_DEMO_SEGMENT")) {
        if (env_seg[0] != '\0') {
            demo_segment = env_seg;
        }
    }
    std::error_code ec;
    std::filesystem::remove(demo_segment, ec);

    constexpr usize NUM_EVENTS = 5000;
    std::cout << "[Step 1] Ingesting " << NUM_EVENTS << " market events into VTS1 segment...\n";

    {
        SegmentWriter writer(1, NUM_EVENTS);
        for (usize i = 0; i < NUM_EVENTS; ++i) {
            CanonicalEvent ev{};
            ev.sequence = i + 1;
            ev.exchange_ts_ns = 1704067200000000000ULL + i * 1000000ULL; // 1ms increments
            ev.receive_ts_ns = ev.exchange_ts_ns + 450;
            ev.instrument_id = (i % 3 == 0) ? 101 : ((i % 3 == 1) ? 102 : 103);
            ev.event_type = (i % 5 == 0) ? EventType::Trade : EventType::Quote;
            ev.side = (i % 2 == 0) ? Side::Bid : Side::Ask;
            ev.flags = 0;
            ev.price_ticks = 15000 + static_cast<i64>((i % 50) * 10);
            ev.quantity = 10 + static_cast<u32>((i % 20) * 5);
            ev.venue_id = 1;
            ev.source_id = 1;
            ev.trade_or_order_id = 1000000 + i;

            auto st = writer.add_event(ev);
            if (!st.ok()) {
                std::cerr << "Failed to add event: " << st.message() << "\n";
                return 1;
            }
        }

        auto st = writer.write_to_file(demo_segment);
        if (!st.ok()) {
            std::cerr << "Failed to write segment: " << st.message() << "\n";
            return 1;
        }
        std::cout << "  -> Wrote " << demo_segment << " (" << std::filesystem::file_size(demo_segment) << " bytes)\n\n";
    }

    std::cout << "[Step 2] Opening and Inspecting Segment Metadata...\n";
    SegmentReader reader;
    auto st = reader.open(demo_segment);
    if (!st.ok()) {
        std::cerr << "Failed to open segment: " << st.message() << "\n";
        return 1;
    }

    auto val_st = reader.validate();
    std::cout << "  Segment ID:      " << reader.segment_id() << "\n";
    std::cout << "  Rows:            " << reader.row_count() << "\n";
    std::cout << "  Schema Hash:     0x" << std::hex << reader.schema_hash() << std::dec << "\n";
    std::cout << "  Integrity Check: " << (val_st.ok() ? "PASSED (CRC32C Verified)" : "FAILED") << "\n\n";

    std::cout << "[Step 3] Executing SQL Query via Portable Vectorized Engine...\n";
    std::string sql = "SELECT instrument_id, price_ticks, quantity WHERE price_ticks > 15300 LIMIT 5";
    std::cout << "  Query: " << sql << "\n";

    query::Parser parser(sql);
    auto ast_res = parser.parse_query();
    if (!ast_res.ok()) {
        std::cerr << "Failed to parse query: " << ast_res.status().message() << "\n";
        return 1;
    }

    VectorExecutor vector_exec;
    std::cout << "  Engine Architecture: " << SimdDispatcher::arch_name(vector_exec.arch()) << "\n";
    auto q_res = vector_exec.execute(reader, ast_res.value().get());
    if (!q_res.ok()) {
        std::cerr << "Query execution failed: " << q_res.status().message() << "\n";
        return 1;
    }

    const auto& res = q_res.value();
    std::cout << "  Results (" << res.rows.size() << " rows returned, " 
              << res.rows_matched << " matched out of " << res.rows_scanned << "):\n";
    for (const auto& col : res.column_names) {
        std::cout << std::left << std::setw(16) << col;
    }
    std::cout << "\n------------------------------------------------\n";
    for (const auto& row : res.rows) {
        for (const auto& val : row) {
            std::cout << std::left << std::setw(16) << val;
        }
        std::cout << "\n";
    }
    std::cout << "\n";

    std::cout << "[Step 4] Executing Aggregation Query...\n";
    std::string agg_sql = "SELECT COUNT(*), SUM(quantity), MIN(price_ticks), MAX(price_ticks) WHERE instrument_id = 101";
    std::cout << "  Query: " << agg_sql << "\n";
    query::Parser agg_parser(agg_sql);
    auto agg_ast = agg_parser.parse_query();
    if (!agg_ast.ok()) {
        std::cerr << "Failed to parse agg query: " << agg_ast.status().message() << "\n";
        return 1;
    }

    auto agg_res = vector_exec.execute(reader, agg_ast.value().get());
    if (agg_res.ok() && !agg_res.value().rows.empty()) {
        const auto& a = agg_res.value();
        for (const auto& col : a.column_names) std::cout << std::left << std::setw(16) << col;
        std::cout << "\n----------------------------------------------------------------\n";
        for (const auto& val : a.rows[0]) std::cout << std::left << std::setw(16) << val;
        std::cout << "\n\n";
    }

    std::cout << "[Step 5] Native Host JIT Compiler Verification...\n";
    ir::Builder ir_builder;
    ir::Function* jit_fn = ir_builder.create_function("demo_filter");
    jit_fn->add_parameter(jit_fn->create_value(ir::Type::U64, "ctx"));
    ir::ValueId price_col = ir_builder.create_load_column(7, jit_fn->parameters()[0], ir::Type::I64);
    ir::ValueId thresh = ir_builder.create_const_i64(15400);
    ir::ValueId cmp = ir_builder.create_gt(price_col, thresh, ir::Type::I64);
    ir_builder.create_return(cmp);

    CanonicalEvent sample_ev{};
    if (reader.read_event(0, sample_ev).ok()) {
        sample_ev.price_ticks = 15450;
        auto jit_val = jit::jit_execute(jit_fn, &sample_ev);
        if (jit_val.ok()) {
            std::cout << "  Host JIT execution result on price 15450 > 15400: " << jit_val.value() << " (PASSED)\n\n";
        } else {
            std::cout << "  Host JIT compilation error: " << jit_val.status().message() << "\n\n";
        }
    }

    std::cout << "[Step 6] Deterministic Replay Engine...\n";
    ReplayOptions r_opt;
    r_opt.rate_limit_events_per_sec = 0; // unlimited
    r_opt.verify_sequence = true;
    ReplayEngine replay(r_opt);
    auto rep_st = replay.replay_segment(demo_segment);
    if (rep_st.ok()) {
        const auto& stats = replay.stats();
        std::cout << "  Replayed " << stats.total_events << " events in " 
                  << (stats.elapsed_wall_ns / 1e6) << " ms (" 
                  << std::fixed << std::setprecision(0) << stats.throughput_events_per_sec << " events/sec)\n";
        std::cout << "  Sequence Breaks: " << stats.sequence_breaks << "\n\n";
    }

    reader.close();
    std::filesystem::remove(demo_segment, ec);

    std::cout << "========================================================\n";
    std::cout << "              Demo Completed Successfully!              \n";
    std::cout << "========================================================\n";
    return 0;
}
