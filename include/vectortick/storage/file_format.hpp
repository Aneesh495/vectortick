#pragma once

#include "../common/types.hpp"
#include "../common/endian.hpp"
#include "../common/crc32c.hpp"
#include "../common/hash.hpp"
#include "../model/event.hpp"
#include <cstring>

namespace vectortick {

// VTS1 Storage Format
// Versioned, checksummed, mmap-readable columnar format
//
// Segment Layout:
//   Segment Header (fixed)
//   Column Descriptor Table
//   Column Blocks (one per column)
//   Zone Maps
//   Bloom Filter (instrument_id)
//   Segment Footer

namespace vts1 {

// Segment header (128 bytes)
struct SegmentHeader {
    u32 magic;              // VTS1 magic
    u32 version;            // Format version
    u64 schema_hash;        // Hash of schema definition
    u64 segment_id;         // Unique segment ID
    u64 row_count;          // Number of rows in segment
    u64 min_timestamp;      // Min exchange_ts_ns
    u64 max_timestamp;      // Max exchange_ts_ns
    u32 descriptor_offset;  // Offset to column descriptors
    u32 descriptor_size;    // Size of descriptor table
    u32 zone_map_offset;    // Offset to zone maps
    u32 bloom_filter_offset;// Offset to bloom filter
    u32 bloom_filter_size;  // Size of bloom filter
    u32 footer_offset;      // Offset to footer
    u32 reserved[13];       // Reserved for future use (52 bytes)
    u32 header_crc;         // CRC32C of header (with this field = 0)
    
    static constexpr usize Size = 128;
    
    [[nodiscard]] bool is_valid_magic() const noexcept { return magic == Magic::VTS1; }
    [[nodiscard]] bool is_valid_version() const noexcept { return version == Version::VTS1; }
};
static_assert(sizeof(SegmentHeader) == SegmentHeader::Size, "SegmentHeader must be 128 bytes");

// Column types
enum class ColumnType : u8 {
    U64 = 0,
    I64 = 1,
    U32 = 2,
    U16 = 3,
    U8 = 4,
    I32 = 5,
    I16 = 6,
    I8 = 7,
    Bool = 8
};

[[nodiscard]] inline usize column_type_size(ColumnType type) noexcept {
    switch (type) {
        case ColumnType::U64: case ColumnType::I64:return 8;
        case ColumnType::U32: case ColumnType::I32:return 4;
        case ColumnType::U16: case ColumnType::I16:return 2;
        default:return 1;
    }
}

// Encoding types
enum class EncodingType : u8 {
    Raw = 0,            // Uncompressed fixed-width
    VarInt = 1,         // Variable-length integer
    BitPacked = 2,      // Frame-of-reference bit packing
    Delta = 3,          // Delta encoding
    DeltaOfDelta = 4,   // Delta-of-delta for timestamps
    RLE = 5,            // Run-length encoding
    Dictionary = 6      // Dictionary encoding
};

// Column descriptor (64 bytes)
struct ColumnDescriptor {
    u32 column_id;          // Column index (4)
    ColumnType type;        // Data type (1)
    EncodingType encoding;  // Encoding used (1)
    u8 bits_per_value;      // For bit-packed encoding (1)
    u8 reserved1;           // Alignment byte (1)
    u64 offset;             // Offset to column data (8)
    u64 compressed_size;    // Compressed size in bytes (8)
    u64 uncompressed_size;  // Uncompressed size in bytes (logical) (8)
    u64 min_value;          // Minimum value (as u64) (8)
    u64 max_value;          // Maximum value (as u64) (8)
    u32 null_count;         // Number of nulls (4)
    u32 data_crc;           // CRC32C of column data (4)
    u32 reserved2;          // Reserved for future use (4)
    u32 descriptor_crc;     // CRC32C of descriptor (with this field = 0) (4)
    
    static constexpr usize Size = 64;
};
static_assert(sizeof(ColumnDescriptor) == ColumnDescriptor::Size, "ColumnDescriptor must be 64 bytes");

// Zone map for a column (64 bytes)
struct ZoneMap {
    u64 min_value;
    u64 max_value;
    u64 sum_value;      // For approximate aggregates
    u32 row_count;
    u32 null_count;
    u32 first_row_idx;  // Index of first row in zone
    u32 last_row_idx;   // Index of last row in zone
    u32 reserved[6];    // 24 bytes
    
    static constexpr usize Size = 64;
};
static_assert(sizeof(ZoneMap) == ZoneMap::Size, "ZoneMap must be 64 bytes");

// Segment footer (128 bytes)
struct SegmentFooter {
    u32 magic;                  // VTS1 magic
    u32 version;                // Format version
    u64 total_size;             // Total file size
    u64 descriptor_table_hash;  // Hash of descriptor table
    u64 segment_digest;         // Hash of all column data
    u64 commit_marker;          // Non-zero if committed
    u64 create_timestamp;       // Creation timestamp
    u64 min_sequence;           // Min sequence number
    u64 max_sequence;           // Max sequence number
    u32 row_count;              // Number of rows
    u32 column_count;           // Number of columns
    u32 reserved[13];           // 52 bytes
    u32 footer_crc;             // CRC32C of footer (with this field = 0)
    
    static constexpr usize Size = 128;
    static constexpr u64 CommittedMarker = 0xDEADBEEFCAFEBABEULL;
};
static_assert(sizeof(SegmentFooter) == SegmentFooter::Size, "SegmentFooter must be 128 bytes");

// Bloom filter parameters
struct BloomFilterParams {
    static constexpr usize DefaultBitsPerItem = 10;
    static constexpr usize DefaultNumHashes = 7;
    
    usize num_bits;
    usize num_hashes;
    usize size_bytes;
    
    static BloomFilterParams for_items(usize num_items, usize bits_per_item = DefaultBitsPerItem) noexcept {
        BloomFilterParams params;
        params.num_bits = num_items * bits_per_item;
        // Round up to power of 2
        usize bits = 1;
        while (bits < params.num_bits) bits <<= 1;
        params.num_bits = bits;
        params.num_hashes = DefaultNumHashes;
        params.size_bytes = params.num_bits / 8;
        return params;
    }
};

// Column ID enumeration for canonical schema
enum ColumnID : u32 {
    ExchangeTsNs = 0,
    ReceiveTsNs = 1,
    Sequence = 2,
    InstrumentId = 3,
    EventType = 4,
    Side = 5,
    Flags = 6,
    PriceTicks = 7,
    Quantity = 8,
    VenueId = 9,
    SourceId = 10,
    TradeOrOrderId = 11,
    ColumnCount = 12
};

// Get column type for each column
[[nodiscard]] inline ColumnType get_column_type(ColumnID id) noexcept {
    switch (id) {
        case ExchangeTsNs:
        case ReceiveTsNs:
        case Sequence:
        case TradeOrOrderId:
            return ColumnType::U64;
        case InstrumentId:
        case Quantity:
            return ColumnType::U32;
        case EventType:
        case Side:
            return ColumnType::U8;
        case Flags:
        case VenueId:
        case SourceId:
            return ColumnType::U16;
        case PriceTicks:
            return ColumnType::I64;
        default:
            return ColumnType::U64;
    }
}

// Get column name
[[nodiscard]] inline const char* get_column_name(ColumnID id) noexcept {
    switch (id) {
        case ExchangeTsNs: return "exchange_ts_ns";
        case ReceiveTsNs: return "receive_ts_ns";
        case Sequence: return "sequence";
        case InstrumentId: return "instrument_id";
        case EventType: return "event_type";
        case Side: return "side";
        case Flags: return "flags";
        case PriceTicks: return "price_ticks";
        case Quantity: return "quantity";
        case VenueId: return "venue_id";
        case SourceId: return "source_id";
        case TradeOrOrderId: return "trade_or_order_id";
        default: return "unknown";
    }
}

// Compute deterministic non-zero schema hash for canonical 12 columns
[[nodiscard]] inline u64 compute_canonical_schema_hash() noexcept {
    FnvHash hasher;
    for (u32 i = 0; i < ColumnCount; ++i) {
        auto col_id = static_cast<ColumnID>(i);
        const char* name = get_column_name(col_id);
        auto type = static_cast<u8>(get_column_type(col_id));
        hasher.update(reinterpret_cast<const u8*>(&i), sizeof(i));
        hasher.update(reinterpret_cast<const u8*>(name), std::strlen(name));
        hasher.update(&type, sizeof(type));
    }
    return hasher.get();
}

} // namespace vts1

} // namespace vectortick
