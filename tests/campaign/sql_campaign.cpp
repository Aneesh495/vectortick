#include "../oracle/query_oracle.hpp"
#include "vectortick/execution/query_engine.hpp"
#include "vectortick/query/parser.hpp"
#include <charconv>
#include <fstream>
#include <iostream>
#include <random>

using namespace vectortick;
static bool same(const QueryResult &a, const QueryResult &b) {
    return a.column_names == b.column_names && a.column_types == b.column_types &&
           a.rows == b.rows && a.rows_scanned == b.rows_scanned && a.rows_matched == b.rows_matched;
}
int main(int argc, char **argv) {
    u64 seed = 0x49520261006ULL, queries = 2000;
    std::string output = "sql-campaign.json";
    for (int i = 1; i < argc; ++i) {
        if (i + 1 == argc)
            return 2;
        std::string key = argv[i], value = argv[++i];
        if (key == "--json")
            output = value;
        else {
            u64 n = 0;
            auto p = std::from_chars(value.data(), value.data() + value.size(), n);
            if (p.ec != std::errc{} || p.ptr != value.data() + value.size() || !n)
                return 2;
            if (key == "--seed")
                seed = n;
            else if (key == "--queries")
                queries = n;
            else
                return 2;
        }
    }
    std::mt19937_64 rng(seed);
    std::vector<CanonicalEvent> events(128);
    for (usize i = 0; i < events.size(); ++i) {
        auto &e = events[i];
        e.exchange_ts_ns = rng();
        e.receive_ts_ns = rng();
        e.sequence = rng();
        e.instrument_id = static_cast<u32>(rng() % 7);
        e.event_type = static_cast<EventType>(rng() & 255);
        e.side = static_cast<Side>(rng() & 255);
        e.flags = static_cast<u16>(rng());
        e.price_ticks = static_cast<i64>(rng());
        e.quantity = static_cast<u32>(rng());
        e.venue_id = static_cast<u16>(rng());
        e.source_id = static_cast<u16>(rng());
        e.trade_or_order_id = rng();
    }
    events[0].price_ticks = INT64_MIN;
    events[1].price_ticks = INT64_MAX;
    events[2].price_ticks = -1;
    events[0].sequence = UINT64_MAX;
    events[1].sequence = 0;
    events[2].sequence = 1ULL << 63;
    events[0].quantity = UINT32_MAX;
    events[1].quantity = 0;
    std::vector<std::string> fixed = {
        "SELECT * FROM events",
        "FROM events",
        "SELECT exchange_ts_ns, receive_ts_ns, sequence, instrument_id, event_type, side, flags, "
        "price_ticks, quantity, venue_id, source_id, trade_or_order_id FROM events",
        "SELECT sequence, price_ticks FROM events WHERE price_ticks < sequence ORDER BY sequence "
        "DESC LIMIT 9",
        "SELECT price_ticks < sequence AS c, sequence > price_ticks AS d FROM events",
        "SELECT price_ticks FROM events WHERE 0 AND (1 / 0)",
        "SELECT price_ticks FROM events WHERE 1 OR (1 / 0)",
        "SELECT price_ticks / -1 FROM events",
        "SELECT price_ticks % -1 FROM events",
        "SELECT -sequence FROM events",
        "SELECT price_ticks + sequence FROM events",
        "SELECT sequence + 1 FROM events",
        "SELECT sequence - 1 FROM events",
        "SELECT sequence * sequence FROM events",
        "SELECT quantity / 0 FROM events",
        "SELECT MIN(price_ticks), MAX(price_ticks), COUNT(*) FROM events",
        "SELECT SUM(price_ticks), AVG(price_ticks) FROM events",
        "SELECT COUNT(quantity / 0) FROM events",
        "SELECT instrument_id, COUNT(*), MIN(price_ticks), MAX(price_ticks), SUM(quantity), "
        "AVG(quantity) FROM events GROUP BY instrument_id ORDER BY instrument_id",
        "SELECT COUNT(*), SUM(quantity), MIN(flags), MAX(venue_id) FROM events WHERE 0",
        "SELECT (quantity & 3) | (flags ^ 5) AS b FROM events ORDER BY b DESC LIMIT 11"};
    const std::vector<std::string> cols = {"exchange_ts_ns", "receive_ts_ns", "sequence",
                                           "instrument_id",  "event_type",    "side",
                                           "flags",          "price_ticks",   "quantity",
                                           "venue_id",       "source_id",     "trade_or_order_id"};
    const std::vector<std::string> cmp = {"<", "<=", ">", ">=", "=", "!="};
    u64 success = 0, errors = 0, calls = 0, functions = 0, empty = 0, cache_hits = 0;
    std::string failed;
    for (u64 i = 0; i < queries; ++i) {
        std::string sql;
        if (i < fixed.size())
            sql = fixed[i];
        else {
            usize col_index = rng() % cols.size();
            std::string col = cols[col_index],
                        other = cols[(col_index + 1 + rng() % (cols.size() - 1)) % cols.size()];
            std::string threshold = std::to_string(rng());
            std::string condition = col + " " + cmp[rng() % cmp.size()] + " " + threshold;
            switch (i % 8) {
            case 0:
                sql = "SELECT " + col + ", " + other + " FROM events WHERE " + condition +
                      " ORDER BY " + col + " DESC LIMIT " + std::to_string(1 + rng() % 32);
                break;
            case 1:
                sql = "SELECT " + col + " " + cmp[rng() % 6] + " " + other +
                      " AS c FROM events WHERE " + condition;
                break;
            case 2:
                sql = "SELECT " + col + " + " + other + " AS v FROM events WHERE " + condition;
                break;
            case 3:
                sql = "SELECT instrument_id, COUNT(*), SUM(quantity), MIN(price_ticks), "
                      "MAX(flags), AVG(venue_id) FROM events WHERE " +
                      condition + " GROUP BY instrument_id ORDER BY instrument_id DESC LIMIT 4";
                break;
            case 4:
                sql = "SELECT COUNT(*), SUM(quantity), MIN(price_ticks), MAX(" + col +
                      "), AVG(source_id) FROM events WHERE " + condition;
                break;
            case 5:
                sql = "SELECT " + col + ", " + other + " FROM events WHERE (" + condition +
                      ") AND (" + other + " != 0 OR (1 / 0)) LIMIT 7";
                break;
            case 6:
                sql = "SELECT (" + col + " & 255) ^ (" + other +
                      " | 7) AS v FROM events ORDER BY v LIMIT 16";
                break;
            default:
                sql = "SELECT -" + col + " AS n FROM events WHERE " + condition;
                break;
            }
        }
        query::Parser parser(sql);
        auto parsed = parser.parse_query();
        if (!parsed.ok()) {
            failed = sql;
            break;
        }
        const auto &input = (i % 31 == 30) ? std::vector<CanonicalEvent>{} : events;
        if (input.empty())
            ++empty;
        NativeCache cache;
        auto oracle = test::QueryOracle::evaluate(input, parsed.value().get());
        auto ref = execute_query(input, parsed.value().get(), QueryEngine::Reference);
        auto vec = execute_query(input, parsed.value().get(), QueryEngine::Vector);
        auto native = execute_query(input, parsed.value().get(), QueryEngine::Native, &cache);
        bool ok = oracle.ok() == ref.ok() && ref.ok() == vec.ok() && ref.ok() == native.ok();
        if (ref.ok() && ok) {
            ok = same(oracle.value(), ref.value()) && same(ref.value(), vec.value()) &&
                 same(ref.value(), native.value()) && native.value().fallback_count == 0 &&
                 (input.empty() || native.value().native_invocations >= input.size());
            auto warm = execute_query(input, parsed.value().get(), QueryEngine::Native, &cache);
            ok = ok && warm.ok() && same(native.value(), warm.value()) &&
                 warm.value().functions_compiled == 0;
            if (ok) {
                ++success;
                ++cache_hits;
                calls += native.value().native_invocations + warm.value().native_invocations;
                functions += native.value().functions_compiled;
            }
        } else if (ok) {
            ok = ref.status().code() == vec.status().code() &&
                 ref.status().code() == native.status().code();
            if (ok)
                ++errors;
        }
        if (!ok) {
            std::cerr << "SQL mismatch seed=" << seed << " query=" << i << "\n"
                      << sql << "\noracle=" << oracle.ok() << " reference=" << ref.ok()
                      << " vector=" << vec.ok() << " native=" << native.ok() << "\n";
            if (ref.ok() && oracle.ok()) {
                std::cerr << "rows " << oracle.value().rows.size() << " / "
                          << ref.value().rows.size() << "\n";
            }
            failed = sql;
            break;
        }
    }
    std::ofstream out(output);
    out << "{\"schema\":1,\"seed\":" << seed << ",\"queries_requested\":" << queries
        << ",\"queries_checked\":" << (success + errors) << ",\"successful_queries\":" << success
        << ",\"runtime_errors_checked\":" << errors << ",\"empty_inputs\":" << empty
        << ",\"warm_cache_checks\":" << cache_hits << ",\"native_invocations\":" << calls
        << ",\"functions_compiled\":" << functions << ",\"failures\":" << (!failed.empty())
        << ",\"failing_sql\":\"" << failed << "\"}\n";
    std::cout << "SQL queries=" << (success + errors) << " native_invocations=" << calls
              << " errors=" << errors << " failures=" << (!failed.empty()) << "\n";
    return !out || !failed.empty() || success + errors != queries ? 1 : 0;
}
