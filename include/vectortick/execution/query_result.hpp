#pragma once

#include "vectortick/common/types.hpp"
#include "vectortick/storage/file_format.hpp"

#include <vector>
#include <string>

namespace vectortick {

// Unified typed query result contract across all engines (Vector, Reference, JIT)
struct QueryResult {
    std::vector<std::string> column_names;
    std::vector<vts1::ColumnType> column_types;
    std::vector<std::vector<std::string>> rows;
    u64 rows_scanned{0};
    u64 rows_matched{0};
    u64 execution_time_ns{0};
};

[[nodiscard]] inline const char* column_type_to_string(vts1::ColumnType type) noexcept {
    switch (type) {
        case vts1::ColumnType::U64: return "U64";
        case vts1::ColumnType::I64: return "I64";
        case vts1::ColumnType::U32: return "U32";
        case vts1::ColumnType::U16: return "U16";
        case vts1::ColumnType::U8: return "U8";
        case vts1::ColumnType::I32: return "I32";
        case vts1::ColumnType::I16: return "I16";
        case vts1::ColumnType::I8: return "I8";
        case vts1::ColumnType::Bool: return "Bool";
        default: return "Unknown";
    }
}

} // namespace vectortick
