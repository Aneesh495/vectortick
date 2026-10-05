#include "vectortick/execution/query_engine.hpp"
#include "vectortick/protocol/decoder.hpp"
#include "vectortick/query/parser.hpp"
#include "vectortick/replay/replay_engine.hpp"
#include "vectortick/storage/segment_reader.hpp"
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sys/resource.h>
using namespace vectortick;
static u64 memory() {
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
#ifdef __APPLE__
    return r.ru_maxrss;
#else
    return static_cast<u64>(r.ru_maxrss) * 1024;
#endif
}
static void result(std::ostream &o, const QueryResult &r) {
    o << "{\"column_names\":[";
    for (usize i = 0; i < r.column_names.size(); ++i) {
        if (i)
            o << ",";
        o << "\"" << r.column_names[i] << "\"";
    }
    o << "],\"column_types\":[";
    for (usize i = 0; i < r.column_types.size(); ++i) {
        if (i)
            o << ",";
        o << "\"" << column_type_to_string(r.column_types[i]) << "\"";
    }
    o << "],\"rows\":[";
    for (usize i = 0; i < r.rows.size(); ++i) {
        if (i)
            o << ",";
        o << "[";
        for (usize j = 0; j < r.rows[i].size(); ++j) {
            if (j)
                o << ",";
            o << "\"" << r.rows[i][j] << "\"";
        }
        o << "]";
    }
    o << "],\"rows_scanned\":" << r.rows_scanned << ",\"rows_matched\":" << r.rows_matched << "}";
}
static void le16(std::ostream &o, u16 n) {
    for (u32 i = 0; i < 2; ++i)
        o.put(static_cast<char>(n >> (i * 8)));
}
static void le32(std::ostream &o, u32 n) {
    for (u32 i = 0; i < 4; ++i)
        o.put(static_cast<char>(n >> (i * 8)));
}
int main(int argc, char **argv) {
    if (argc != 5)
        return 2;
    std::string mode = argv[1], input = argv[2], output = argv[3];
    u64 count = 0;
    std::string number = argv[4];
    auto parsed = std::from_chars(number.data(), number.data() + number.size(), count);
    if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || !count)
        return 2;
    if (mode == "--pcap") {
        std::ofstream o(input, std::ios::binary);
        le32(o, 0xa1b2c3d4);
        le16(o, 2);
        le16(o, 4);
        le32(o, 0);
        le32(o, 0);
        le32(o, 65535);
        le32(o, 1);
        vtp1::Encoder encoder;
        for (u64 i = 0; i < count; ++i) {
            CanonicalEvent e{};
            e.sequence = i + 1;
            e.exchange_ts_ns = 1000000000 + i * 1000;
            e.instrument_id = 1 + i % 13;
            e.event_type = EventType::Trade;
            e.side = i % 2 ? Side::Bid : Side::Ask;
            e.price_ticks = 10000 + i % 2000;
            e.quantity = 10 + i % 100;
            e.venue_id = 3;
            e.trade_or_order_id = 100000 + i;
            u8 frame[256];
            auto encoded = encoder.encode_event(e, frame, sizeof(frame), 1);
            if (!encoded.ok())
                return 1;
            u32 length = 42 + encoded.value();
            le32(o, 1);
            le32(o, static_cast<u32>(i % 1000000));
            le32(o, length);
            le32(o, length);
            u8 header[42]{};
            write_be_u16(header + 12, 0x0800);
            header[14] = 0x45;
            write_be_u16(header + 16, static_cast<u16>(length - 14));
            header[22] = 64;
            header[23] = 17;
            write_be_u16(header + 34, 1000);
            write_be_u16(header + 36, 2000);
            write_be_u16(header + 38, static_cast<u16>(8 + encoded.value()));
            o.write(reinterpret_cast<const char *>(header), 42);
            o.write(reinterpret_cast<const char *>(frame), encoded.value());
        }
        std::ofstream meta(output);
        meta << "{\"rows_generated\":" << count << ",\"seed\":49520261007}\n";
        return !o || !meta;
    }
    if (mode != "--query")
        return 2;
    SegmentReader reader;
    auto status = reader.open(input);
    if (!status.ok()) {
        std::cerr << status.message();
        return 1;
    }
    std::vector<CanonicalEvent> events;
    status = reader.read_all_events(events);
    if (!status.ok())
        return 1;
    std::string sql = "SELECT instrument_id, COUNT(*), SUM(quantity), MIN(price_ticks), "
                      "MAX(price_ticks), AVG(quantity) FROM events WHERE sequence % 3 != 0 AND "
                      "price_ticks >= 10000 GROUP BY instrument_id ORDER BY instrument_id";
    query::Parser p(sql);
    auto q = p.parse_query();
    if (!q.ok())
        return 1;
    std::ofstream o(output);
    o << "{\"compiler\":\"" << __VERSION__ << "\",\"query\":\"" << sql
      << "\",\"input_rows\":" << events.size() << ",\"repetitions\":[";
    bool first = true;
    NativeCache warm_cache;
    auto warmup = execute_query(events, q.value().get(), QueryEngine::Native, &warm_cache);
    if (!warmup.ok())
        return 1;
    for (auto engine : {QueryEngine::Reference, QueryEngine::Vector, QueryEngine::Native})
        for (int state = 0; state < (engine == QueryEngine::Native ? 2 : 1); ++state)
            for (u64 rep = 0; rep < count; ++rep) {
                NativeCache cold_cache;
                auto start = std::chrono::steady_clock::now();
                auto r = execute_query(events, q.value().get(), engine,
                                       state ? &warm_cache : &cold_cache);
                u64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
                if (!r.ok()) {
                    std::cerr << r.status().message();
                    return 1;
                }
                if (!first)
                    o << ",";
                first = false;
                o << "{\"engine\":\""
                  << (engine == QueryEngine::Reference ? "interpreter"
                      : engine == QueryEngine::Vector  ? "vector"
                                                       : "jit")
                  << "\",\"cache\":\"" << (state ? "warm" : "cold") << "\",\"repetition\":" << rep
                  << ",\"wall_ns\":" << ns << ",\"peak_rss_bytes\":" << memory()
                  << ",\"backend\":\"" << r.value().backend
                  << "\",\"native_invocations\":" << r.value().native_invocations
                  << ",\"functions_compiled\":" << r.value().functions_compiled
                  << ",\"fallback_count\":" << r.value().fallback_count
                  << ",\"rows_evaluated\":" << r.value().rows_evaluated << ",\"result\":";
                result(o, r.value());
                o << "}";
            }
    o << "],\"replay_repetitions\":[";
    for (u64 rep = 0; rep < count; ++rep) {
        ReplayEngine replay;
        u64 delivered = 0, digest = 0;
        auto start = std::chrono::steady_clock::now();
        status = replay.replay_events(events, [&](const CanonicalEvent &e) {
            ++delivered;
            digest = hash_combine(digest, crc32c(reinterpret_cast<const u8 *>(&e), sizeof(e)));
        });
        u64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count();
        if (!status.ok() || delivered != events.size())
            return 1;
        if (rep)
            o << ",";
        o << "{\"repetition\":" << rep << ",\"wall_ns\":" << ns
          << ",\"peak_rss_bytes\":" << memory() << ",\"delivered\":" << delivered
          << ",\"rows_scanned\":" << replay.stats().total_events << ",\"output_digest\":\""
          << digest << "\"}";
    }
    o << "]}\n";
    return !o;
}
