// VectorTick Query Application
// Executes queries against stored segment files

#include "vectortick/common/types.hpp"
#include "vectortick/storage/segment_reader.hpp"
#include "vectortick/query/lexer.hpp"
#include "vectortick/query/parser.hpp"
#include "vectortick/execution/query_result.hpp"
#include "vectortick/execution/vector_executor.hpp"
#include "vectortick/execution/reference_executor.hpp"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <cstring>
#include <chrono>
#include <charconv>

using namespace vectortick;

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options] <segment.vts> \"<query>\"\n";
    std::cerr << "Options:\n";
    std::cerr << "  -e <engine>       Execution engine (vector, interpreter, jit; default: vector)\n";
    std::cerr << "  -l <limit>        Limit number of results\n";
    std::cerr << "  --json [file]     Output result as JSON (to file or stdout)\n";
    std::cerr << "  -v                Verbose output\n";
    std::cerr << "  -h                Show this help\n";
}

void print_table(const QueryResult& qres, std::ostream& out) {
    // Print header
    for (usize i = 0; i < qres.column_names.size(); ++i) {
        out << std::left << std::setw(16) << qres.column_names[i];
    }
    out << "\n";
    for (usize i = 0; i < qres.column_names.size(); ++i) {
        out << "----------------";
    }
    out << "\n";

    // Print rows
    for (const auto& row : qres.rows) {
        for (const auto& cell : row) {
            out << std::left << std::setw(16) << cell;
        }
        out << "\n";
    }

    out << "\n(" << qres.rows.size() << " rows, scanned " 
        << qres.rows_scanned << " rows in " 
        << std::fixed << std::setprecision(3) << (qres.execution_time_ns / 1e6) 
        << " ms)\n";
}

void print_json(const QueryResult& qres, const std::string& engine, std::ostream& out) {
    out << "{\n";
    out << "  \"engine\": \"" << engine << "\",\n";
    out << "  \"rows_scanned\": " << qres.rows_scanned << ",\n";
    out << "  \"rows_matched\": " << qres.rows_matched << ",\n";
    out << "  \"execution_time_ns\": " << qres.execution_time_ns << ",\n";
    out << "  \"columns\": [\n";
    for (usize i = 0; i < qres.column_names.size(); ++i) {
        out << "    {\"name\": \"" << qres.column_names[i] << "\", \"type\": \"";
        if (i < qres.column_types.size()) {
            out << column_type_to_string(qres.column_types[i]);
        } else {
            out << "Unknown";
        }
        out << "\"}";
        if (i + 1 < qres.column_names.size()) out << ",";
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"rows\": [\n";
    for (usize r = 0; r < qres.rows.size(); ++r) {
        out << "    [";
        for (usize c = 0; c < qres.rows[r].size(); ++c) {
            out << "\"" << qres.rows[r][c] << "\"";
            if (c + 1 < qres.rows[r].size()) out << ", ";
        }
        out << "]";
        if (r + 1 < qres.rows.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

int main(int argc, char* argv[]) {
    std::string segment_file;
    std::string query_str;
    std::string engine = "vector";
    std::string json_file;
    bool json_mode = false;
    u64 limit_override = 0;
    bool verbose = false;
    
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            engine = argv[++i];
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            const char* str = argv[++i];
            size_t len = strlen(str);
            auto [ptr, ec] = std::from_chars(str, str + len, limit_override);
            if (ec != std::errc{} || ptr != str + len) {
                std::cerr << "Invalid limit value: " << str << "\n";
                return 1;
            }
        } else if (strcmp(argv[i], "--json") == 0) {
            json_mode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                json_file = argv[++i];
            }
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            if (segment_file.empty()) {
                segment_file = argv[i];
            } else {
                query_str = argv[i];
            }
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    // Engine validation
    if (engine != "vector" && engine != "interpreter" && engine != "jit") {
        std::cerr << "Unknown engine: " << engine << " (supported: vector, interpreter, jit)\n";
        return 1;
    }
    
    if (segment_file.empty() || query_str.empty()) {
        std::cerr << "Error: Segment file and query required\n";
        print_usage(argv[0]);
        return 1;
    }
    
    if (verbose) {
        std::cout << "Segment: " << segment_file << "\n";
        std::cout << "Engine:  " << engine << "\n";
        std::cout << "Query:   " << query_str << "\n";
        std::cout << "----------------------------------------\n";
    }
    
    // Parse query
    query::Parser parser(query_str);
    auto ast_result = parser.parse_query();
    
    if (!ast_result.ok()) {
        std::cerr << "Parse Error: " << ast_result.status().message() << "\n";
        return 1;
    }
    
    auto* q_ast = ast_result.value().get();
    if (limit_override > 0) {
        q_ast->limit = limit_override;
    }
    
    // Open segment file
    SegmentReader reader;
    auto open_st = reader.open(segment_file);
    if (!open_st.ok()) {
        std::cerr << "Storage Error: " << open_st.message() << "\n";
        return 1;
    }

    Result<QueryResult> res = make_error<QueryResult>(StatusCode::InternalError, "Not executed");
    if (engine == "vector") {
        VectorExecutor exec;
        res = exec.execute(reader, q_ast);
    } else if (engine == "interpreter") {
        ReferenceExecutor exec;
        res = exec.execute(reader, q_ast);
    } else if (engine == "jit") {
        std::cerr << "Execution Error: JIT engine execution mode integration in progress\n";
        return 1;
    }

    if (!res.ok()) {
        std::cerr << "Execution Error: " << res.status().message() << "\n";
        return 1;
    }

    const auto& qres = res.value();

    if (json_mode) {
        if (!json_file.empty()) {
            std::ofstream out(json_file);
            if (!out.is_open()) {
                std::cerr << "Error: Could not open output file " << json_file << "\n";
                return 1;
            }
            print_json(qres, engine, out);
        } else {
            print_json(qres, engine, std::cout);
        }
    } else {
        print_table(qres, std::cout);
    }
    
    return 0;
}
