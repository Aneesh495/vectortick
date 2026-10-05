#pragma once

#include "../common/types.hpp"
#include "../common/status.hpp"
#include "../common/result.hpp"
#include "../memory/mapped_file.hpp"
#include "../model/event.hpp"
#include "file_format.hpp"
#include <string>
#include <memory>

namespace vectortick {

// Segment reader - reads VTS1 columnar format via mmap
class SegmentReader {
public:
    SegmentReader() = default;
    ~SegmentReader() = default;
    
    // Non-copyable
    SegmentReader(const SegmentReader&) = delete;
    SegmentReader& operator=(const SegmentReader&) = delete;
    
    // Movable
    SegmentReader(SegmentReader&& other) noexcept = default;
    SegmentReader& operator=(SegmentReader&& other) noexcept = default;
    
    // Open segment file
    [[nodiscard]] Status open(const std::string& path) noexcept;
    
    // Close segment
    void close() noexcept;
    
    // Get segment info
    [[nodiscard]] bool is_open() const noexcept { return mapping_.is_open(); }
    [[nodiscard]] usize row_count() const noexcept { return row_count_; }
    [[nodiscard]] usize file_size() const noexcept { return file_size_; }
    [[nodiscard]] u64 segment_id() const noexcept { return segment_id_; }
    [[nodiscard]] u64 min_timestamp() const noexcept { return min_timestamp_; }
    [[nodiscard]] u64 max_timestamp() const noexcept { return max_timestamp_; }
    
    // Read event at row index
    [[nodiscard]] Status read_event(usize row_idx, CanonicalEvent& event) const noexcept;
    
    // Read all events in segment
    [[nodiscard]] Status read_all_events(std::vector<CanonicalEvent>& events) const noexcept;
    
    // Read column data
    [[nodiscard]] Status read_column_u64(vts1::ColumnID col, u64* values, usize num_values) const noexcept;
    [[nodiscard]] Status read_column_u32(vts1::ColumnID col, u32* values, usize num_values) const noexcept;
    [[nodiscard]] Status read_column_i64(vts1::ColumnID col, i64* values, usize num_values) const noexcept;
    [[nodiscard]] Status read_column_u16(vts1::ColumnID col, u16* values, usize num_values) const noexcept;
    [[nodiscard]] Status read_column_u8(vts1::ColumnID col, u8* values, usize num_values) const noexcept;
    
    // Get column statistics
    [[nodiscard]] u64 column_min(vts1::ColumnID col) const noexcept;
    [[nodiscard]] u64 column_max(vts1::ColumnID col) const noexcept;
    
    // Check if segment might contain instrument (bloom filter)
    [[nodiscard]] bool might_contain_instrument(u32 instrument_id) const noexcept;
    
    // Validate segment integrity
    [[nodiscard]] Status validate() const noexcept;
    
    // Get compression ratio
    [[nodiscard]] double compression_ratio() const noexcept;

    // Metadata accessors
    [[nodiscard]] u32 schema_hash() const noexcept { return header_.schema_hash; }
    [[nodiscard]] const vts1::SegmentHeader& header() const noexcept { return header_; }
    [[nodiscard]] const std::vector<vts1::ColumnDescriptor>& descriptors() const noexcept { return descriptors_; }
    [[nodiscard]] const std::vector<vts1::ZoneMap>& zone_maps() const noexcept { return zone_maps_; }
    [[nodiscard]] const std::vector<u8>& bloom_filter() const noexcept { return bloom_filter_; }
    [[nodiscard]] u64 min_sequence() const noexcept {
        return column_min(vts1::ColumnID::Sequence);
    }
    [[nodiscard]] u64 max_sequence() const noexcept {
        return column_max(vts1::ColumnID::Sequence);
    }

private:
    [[nodiscard]] Status read_header() noexcept;
    [[nodiscard]] Status read_descriptors() noexcept;
    [[nodiscard]] Status decode_column(const vts1::ColumnDescriptor& desc,
                                        byte* output,
                                        usize num_values) const noexcept;
    
    MappedFile mapping_;
    vts1::SegmentHeader header_;
    std::vector<vts1::ColumnDescriptor> descriptors_;
    std::vector<vts1::ZoneMap> zone_maps_;
    std::vector<u8> bloom_filter_;
    
    u64 segment_id_ = 0;
    usize row_count_ = 0;
    u64 min_timestamp_ = 0;
    u64 max_timestamp_ = 0;
    
    usize file_size_ = 0;
};

} // namespace vectortick
