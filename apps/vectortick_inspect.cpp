#include <charconv>
// VectorTick Inspect Application
// Inspects and dumps segment file metadata, schema, zone maps, and rows

#include "vectortick/common/types.hpp"
#include "vectortick/storage/segment_reader.hpp"
#include "vectortick/storage/file_format.hpp"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <cstring>
#include <vector>

using namespace vectortick;

namespace {

const char* type_name_str(vts1::ColumnType type_id) {
    switch (type_id) {
        case vts1::ColumnType::U64: return "U64";
        case vts1::ColumnType::I64: return "I64";
        case vts1::ColumnType::U32: return "U32";
        case vts1::ColumnType::U16: return "U16";
        case vts1::ColumnType::U8: return "U8";
        default: return "Unknown";
    }
}

const char* codec_name(vts1::EncodingType codec_id) {
    switch (codec_id) {
        case vts1::EncodingType::Raw: return "Raw";
        case vts1::EncodingType::BitPacked: return "BitPacked";
        case vts1::EncodingType::RLE: return "RLE";
        case vts1::EncodingType::Dictionary: return "Dictionary";
        case vts1::EncodingType::Delta: return "Delta";
        case vts1::EncodingType::DeltaOfDelta: return "DeltaOfDelta";
        case vts1::EncodingType::VarInt: return "VarInt";
        default: return "Custom";
    }
}

} // namespace

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options] <segment.vts>\n";
    std::cerr << "Options:\n";
    std::cerr << "  -s                Show schema details\n";
    std::cerr << "  -m                Show metadata, headers, zone maps, and bloom filter\n";
    std::cerr << "  -d <n>            Dump first N rows\n";
    std::cerr << "  -v                Verbose mode (show schema, metadata, and first 5 rows)\n";
    std::cerr << "  -c                Run checksum and integrity validation\n";
    std::cerr << "  -h                Show this help\n";
}

int main(int argc, char* argv[]) {
    std::string segment_file;
    bool show_schema = false;
    bool show_metadata = false;
    bool validate_integrity = false;
    usize dump_rows = 0;
    
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-s") == 0) {
            show_schema = true;
        } else if (strcmp(argv[i], "-m") == 0) {
            show_metadata = true;
        } else if (strcmp(argv[i], "-c") == 0) {
            validate_integrity = true;
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            const char* value = argv[++i];
            auto end = value + std::strlen(value);
            auto [parsed, error] = std::from_chars(value,end,dump_rows);
            if (error != std::errc{} || parsed != end) { std::cerr << "Invalid row count\n"; return 1; }
        } else if (strcmp(argv[i], "-v") == 0) {
            show_schema = true;
            show_metadata = true;
            if (dump_rows == 0) dump_rows = 5;
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
    
    if (!show_schema && !show_metadata && dump_rows == 0) {
        show_schema = true;
        show_metadata = true;
    }
    
    SegmentReader reader;
    auto status = reader.open(segment_file);
    if (!status.ok()) {
        std::cerr << "Error opening segment: " << status.message() << "\n";
        return 1;
    }
    
    std::cout << "Segment File: " << segment_file << "\n";
    std::cout << "============================================================\n";
    std::cout << "Segment ID:      " << reader.segment_id() << "\n";
    std::cout << "Row Count:       " << reader.row_count() << "\n";
    std::cout << "Schema Hash:     0x" << std::hex << std::setw(8) << std::setfill('0') 
              << reader.schema_hash() << std::dec << "\n";
    std::cout << "Time Range (ns): [" << reader.min_timestamp() << " .. " << reader.max_timestamp() << "]\n";
    std::cout << "Seq Range:       [" << reader.min_sequence() << " .. " << reader.max_sequence() << "]\n";

    if (validate_integrity || true) {
        auto val_st = reader.validate();
        std::cout << "Integrity Check: " << (val_st.ok() ? "PASSED (CRC32C valid)" : "FAILED: " + std::string(val_st.message())) << "\n";
    }

    if (show_schema) {
        std::cout << "\n--- Column Descriptors & Schema ---\n";
        std::cout << std::left << std::setw(4) << "ID"
                  << std::setw(20) << "Column Name"
                  << std::setw(8)  << "Type"
                  << std::setw(14) << "Codec"
                  << std::setw(12) << "Comp (B)"
                  << std::setw(12) << "Uncomp (B)"
                  << "\n";
        std::cout << "--------------------------------------------------------------------\n";
        for (const auto& desc : reader.descriptors()) {
            auto cid = static_cast<vts1::ColumnID>(desc.column_id);
            std::cout << std::left << std::setw(4) << desc.column_id
                      << std::setw(20) << vts1::get_column_name(cid)
                      << std::setw(8)  << type_name_str(desc.type)
                      << std::setw(14) << codec_name(desc.encoding)
                      << std::setw(12) << desc.compressed_size
                      << std::setw(12) << desc.uncompressed_size
                      << "\n";
        }
    }

    if (show_metadata) {
        std::cout << "\n--- Column Zone Maps ---\n";
        std::cout << std::left << std::setw(4) << "ID"
                  << std::setw(20) << "Column Name"
                  << std::setw(22) << "Min Value"
                  << std::setw(22) << "Max Value"
                  << "\n";
        std::cout << "--------------------------------------------------------------------\n";
        const auto& zmaps = reader.zone_maps();
        for (usize i = 0; i < zmaps.size(); ++i) {
            auto cid = static_cast<vts1::ColumnID>(i);
            std::cout << std::left << std::setw(4) << i
                      << std::setw(20) << vts1::get_column_name(cid)
                      << std::setw(22) << zmaps[i].min_value
                      << std::setw(22) << zmaps[i].max_value
                      << "\n";
        }

        std::cout << "\n--- Bloom Filter ---\n";
        std::cout << "Filter Size: " << reader.bloom_filter().size() << " bytes\n";
    }

    if (dump_rows > 0) {
        usize count = std::min(dump_rows, reader.row_count());
        std::cout << "\n--- Dumping first " << count << " rows ---\n";
        std::cout << std::left 
                  << std::setw(8)  << "Seq"
                  << std::setw(10) << "Inst"
                  << std::setw(8)  << "Type"
                  << std::setw(6)  << "Side"
                  << std::setw(12) << "Price"
                  << std::setw(10) << "Qty"
                  << std::setw(22) << "ExchTs"
                  << "\n";
        std::cout << "--------------------------------------------------------------------------------\n";

        for (usize i = 0; i < count; ++i) {
            CanonicalEvent ev{};
            if (reader.read_event(i, ev).ok()) {
                std::cout << std::left
                          << std::setw(8)  << ev.sequence
                          << std::setw(10) << ev.instrument_id
                          << std::setw(8)  << (ev.event_type == EventType::Trade ? "Trade" : "Quote")
                          << std::setw(6)  << (ev.side == Side::Bid ? "Bid" : "Ask")
                          << std::setw(12) << ev.price_ticks
                          << std::setw(10) << ev.quantity
                          << std::setw(22) << ev.exchange_ts_ns
                          << "\n";
            }
        }
    }

    return 0;
}
