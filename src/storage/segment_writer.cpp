#include "vectortick/storage/segment_writer.hpp"
#include "vectortick/codec/bitpack.hpp"
#include "vectortick/codec/rle.hpp"
#include "vectortick/codec/varint.hpp"
#include "vectortick/common/checked_math.hpp"
#include "vectortick/memory/mapped_file.hpp"
#include <algorithm>
#include <cstring>

namespace vectortick {

SegmentWriter::SegmentWriter(u64 segment_id, usize max_rows)
    : segment_id_(segment_id), max_rows_(std::min(max_rows, SegmentConfig::MaxRowsPerSegment)),
      row_count_(0), min_timestamp_(UINT64_MAX), max_timestamp_(0), min_sequence_(UINT64_MAX),
      max_sequence_(0), min_instrument_id_(UINT64_MAX), max_instrument_id_(0),
      min_price_(INT64_MAX), max_price_(INT64_MIN), min_quantity_(UINT32_MAX), max_quantity_(0) {

    // Pre-allocate column buffers
    exchange_ts_ns_.reserve(max_rows_);
    receive_ts_ns_.reserve(max_rows_);
    sequences_.reserve(max_rows_);
    instrument_ids_.reserve(max_rows_);
    event_types_.reserve(max_rows_);
    sides_.reserve(max_rows_);
    flags_.reserve(max_rows_);
    price_ticks_.reserve(max_rows_);
    quantities_.reserve(max_rows_);
    venue_ids_.reserve(max_rows_);
    source_ids_.reserve(max_rows_);
    trade_or_order_ids_.reserve(max_rows_);
}

Status SegmentWriter::add_event(const CanonicalEvent &event) noexcept {
    if (is_full()) {
        return Status(StatusCode::LimitExceeded, "Segment is full");
    }

    // Validate event
    auto status = event::validate(event);
    if (!status.ok()) {
        return status;
    }

    // Append to column buffers
    exchange_ts_ns_.push_back(event.exchange_ts_ns);
    receive_ts_ns_.push_back(event.receive_ts_ns);
    sequences_.push_back(event.sequence);
    instrument_ids_.push_back(event.instrument_id);
    event_types_.push_back(static_cast<u8>(event.event_type));
    sides_.push_back(static_cast<u8>(event.side));
    flags_.push_back(event.flags);
    price_ticks_.push_back(event.price_ticks);
    quantities_.push_back(event.quantity);
    venue_ids_.push_back(event.venue_id);
    source_ids_.push_back(event.source_id);
    trade_or_order_ids_.push_back(event.trade_or_order_id);

    // Update statistics
    update_statistics(event);

    ++row_count_;
    return Status::OK();
}

void SegmentWriter::update_statistics(const CanonicalEvent &event) noexcept {
    // Timestamp range
    if (event.exchange_ts_ns < min_timestamp_)
        min_timestamp_ = event.exchange_ts_ns;
    if (event.exchange_ts_ns > max_timestamp_)
        max_timestamp_ = event.exchange_ts_ns;

    // Sequence range
    if (event.sequence < min_sequence_)
        min_sequence_ = event.sequence;
    if (event.sequence > max_sequence_)
        max_sequence_ = event.sequence;

    // Instrument ID range
    if (event.instrument_id < min_instrument_id_)
        min_instrument_id_ = event.instrument_id;
    if (event.instrument_id > max_instrument_id_)
        max_instrument_id_ = event.instrument_id;

    // Price range
    if (event.price_ticks < min_price_)
        min_price_ = event.price_ticks;
    if (event.price_ticks > max_price_)
        max_price_ = event.price_ticks;

    // Quantity range
    if (event.quantity < min_quantity_)
        min_quantity_ = event.quantity;
    if (event.quantity > max_quantity_)
        max_quantity_ = event.quantity;
}

usize SegmentWriter::estimated_size() const noexcept {
    // Header + descriptors + column data + footer
    usize size = vts1::SegmentHeader::Size;
    size += vts1::ColumnDescriptor::Size * vts1::ColumnCount;

    // Estimate compressed column sizes (rough estimate: 50% compression)
    size += row_count_ * 56 / 2; // 56 bytes per row, 50% compression

    size += vts1::ZoneMap::Size * vts1::ColumnCount;
    size += 1024; // Bloom filter estimate
    size += vts1::SegmentFooter::Size;

    return size;
}

Status SegmentWriter::write_to_file(const std::string &path) noexcept {
    if (row_count_ > SegmentConfig::MaxRowsPerSegment)
        return Status(StatusCode::InvalidArgument, "Segment row limit exceeded");

    // Calculate required size
    // Bound the worst-case expansion, including 10-byte delta varints,
    // bitpacking headers, RLE, bloom bits and alignment between columns.
    usize estimated = row_count_ * 128 + 8192;
    AlignedBuffer buffer;
    auto status = buffer.allocate(estimated);
    if (!status.ok()) {
        return status;
    }

    byte *data = buffer.data();
    usize offset = 0;

    // Write segment header
    vts1::SegmentHeader header = {};
    header.magic = Magic::VTS1;
    header.version = Version::VTS1;
    header.schema_hash = vts1::compute_canonical_schema_hash();
    header.segment_id = segment_id_;
    header.row_count = row_count_;
    header.min_timestamp = row_count_ ? min_timestamp_ : 0;
    header.max_timestamp = row_count_ ? max_timestamp_ : 0;

    // Leave space for header, write after we know offsets
    offset = vts1::SegmentHeader::Size;

    // Write column descriptors
    header.descriptor_offset = static_cast<u32>(offset);

    // Initialize column descriptors
    std::vector<vts1::ColumnDescriptor> descriptors(vts1::ColumnCount);

    for (usize i = 0; i < vts1::ColumnCount; ++i) {
        descriptors[i].column_id = static_cast<u32>(i);
        descriptors[i].type = vts1::get_column_type(static_cast<vts1::ColumnID>(i));
        descriptors[i].encoding = vts1::EncodingType::Raw; // Will be set per-column below
        descriptors[i].offset = 0;
        descriptors[i].compressed_size = 0;
        descriptors[i].uncompressed_size = row_count_ * sizeof(u64); // Estimate
        descriptors[i].null_count = 0;
        descriptors[i].data_crc = 0;
    }

    offset += vts1::ColumnDescriptor::Size * vts1::ColumnCount;
    header.descriptor_size = static_cast<u32>(vts1::ColumnDescriptor::Size * vts1::ColumnCount);

    // Align to 64 bytes
    offset = (offset + 63) & ~usize(63);

    // Encode each column
    // Column 0: exchange_ts_ns
    descriptors[0].offset = offset;
    descriptors[0].encoding = vts1::EncodingType::Delta;
    auto enc_status =
        encode_column_u64(exchange_ts_ns_, vts1::EncodingType::Delta, data, offset, estimated);
    if (!enc_status.ok())
        return enc_status;
    descriptors[0].compressed_size = offset - descriptors[0].offset;
    descriptors[0].uncompressed_size = row_count_ * sizeof(u64);
    descriptors[0].min_value = min_timestamp_;
    descriptors[0].max_value = max_timestamp_;

    // Column 1: receive_ts_ns
    descriptors[1].offset = offset;
    descriptors[1].encoding = vts1::EncodingType::Delta;
    enc_status =
        encode_column_u64(receive_ts_ns_, vts1::EncodingType::Delta, data, offset, estimated);
    if (!enc_status.ok())
        return enc_status;
    descriptors[1].compressed_size = offset - descriptors[1].offset;
    descriptors[1].uncompressed_size = row_count_ * sizeof(u64);

    // Column 2: sequence
    descriptors[2].offset = offset;
    descriptors[2].encoding = vts1::EncodingType::Delta;
    enc_status = encode_column_u64(sequences_, vts1::EncodingType::Delta, data, offset, estimated);
    if (!enc_status.ok())
        return enc_status;
    descriptors[2].compressed_size = offset - descriptors[2].offset;
    descriptors[2].uncompressed_size = row_count_ * sizeof(u64);
    descriptors[2].min_value = min_sequence_;
    descriptors[2].max_value = max_sequence_;

    // Column 3: instrument_id
    descriptors[3].offset = offset;
    descriptors[3].encoding = vts1::EncodingType::BitPacked;
    enc_status =
        encode_column_u32(instrument_ids_, vts1::EncodingType::BitPacked, data, offset, estimated);
    if (!enc_status.ok())
        return enc_status;
    descriptors[3].compressed_size = offset - descriptors[3].offset;
    descriptors[3].uncompressed_size = row_count_ * sizeof(u32);
    descriptors[3].min_value = min_instrument_id_;
    descriptors[3].max_value = max_instrument_id_;

    // Column 4: event_type
    descriptors[4].offset = offset;
    descriptors[4].encoding = vts1::EncodingType::RLE;
    enc_status = encode_column_u8(event_types_, vts1::EncodingType::RLE, data, offset, estimated);
    if (!enc_status.ok())
        return enc_status;
    descriptors[4].compressed_size = offset - descriptors[4].offset;
    descriptors[4].uncompressed_size = row_count_ * sizeof(u8);

    // Column 5: side
    descriptors[5].offset = offset;
    descriptors[5].encoding = vts1::EncodingType::RLE;
    enc_status = encode_column_u8(sides_, vts1::EncodingType::RLE, data, offset, estimated);
    if (!enc_status.ok())
        return enc_status;
    descriptors[5].compressed_size = offset - descriptors[5].offset;
    descriptors[5].uncompressed_size = row_count_ * sizeof(u8);

    // Column 6: flags
    descriptors[6].offset = offset;
    descriptors[6].encoding = vts1::EncodingType::BitPacked;
    enc_status = encode_column_u16(flags_, vts1::EncodingType::BitPacked, data, offset, estimated);
    if (!enc_status.ok())
        return enc_status;
    descriptors[6].compressed_size = offset - descriptors[6].offset;
    descriptors[6].uncompressed_size = row_count_ * sizeof(u16);

    // Column 7: price_ticks
    descriptors[7].offset = offset;
    bool safe_delta = true;
    for (usize i = 1; i < price_ticks_.size(); ++i)
        if (!checked_sub(price_ticks_[i], price_ticks_[i - 1]))
            safe_delta = false;
    descriptors[7].encoding = safe_delta ? vts1::EncodingType::Delta : vts1::EncodingType::Raw;
    enc_status = encode_column_i64(price_ticks_, descriptors[7].encoding, data, offset, estimated);
    if (!enc_status.ok())
        return enc_status;
    descriptors[7].compressed_size = offset - descriptors[7].offset;
    descriptors[7].uncompressed_size = row_count_ * sizeof(i64);
    descriptors[7].min_value = static_cast<u64>(min_price_);
    descriptors[7].max_value = static_cast<u64>(max_price_);

    // Column 8: quantity
    descriptors[8].offset = offset;
    descriptors[8].encoding = vts1::EncodingType::BitPacked;
    enc_status =
        encode_column_u32(quantities_, vts1::EncodingType::BitPacked, data, offset, estimated);
    if (!enc_status.ok())
        return enc_status;
    descriptors[8].compressed_size = offset - descriptors[8].offset;
    descriptors[8].uncompressed_size = row_count_ * sizeof(u32);
    descriptors[8].min_value = min_quantity_;
    descriptors[8].max_value = max_quantity_;

    // Column 9: venue_id
    descriptors[9].offset = offset;
    descriptors[9].encoding = vts1::EncodingType::Raw;
    enc_status = encode_column_u16(venue_ids_, vts1::EncodingType::Raw, data, offset, estimated);
    if (!enc_status.ok())
        return enc_status;
    descriptors[9].compressed_size = offset - descriptors[9].offset;
    descriptors[9].uncompressed_size = row_count_ * sizeof(u16);

    // Column 10: source_id
    descriptors[10].offset = offset;
    descriptors[10].encoding = vts1::EncodingType::Raw;
    enc_status = encode_column_u16(source_ids_, vts1::EncodingType::Raw, data, offset, estimated);
    if (!enc_status.ok())
        return enc_status;
    descriptors[10].compressed_size = offset - descriptors[10].offset;
    descriptors[10].uncompressed_size = row_count_ * sizeof(u16);

    // Column 11: trade_or_order_id
    descriptors[11].offset = offset;
    descriptors[11].encoding = vts1::EncodingType::Raw;
    enc_status =
        encode_column_u64(trade_or_order_ids_, vts1::EncodingType::Raw, data, offset, estimated);
    if (!enc_status.ok())
        return enc_status;
    descriptors[11].compressed_size = offset - descriptors[11].offset;
    descriptors[11].uncompressed_size = row_count_ * sizeof(u64);

    // Align for zone maps
    offset = (offset + 63) & ~usize(63);
    header.zone_map_offset = static_cast<u32>(offset);

    // Write zone maps (simplified - just copy descriptor mins/maxes)
    for (usize i = 0; i < vts1::ColumnCount; ++i) {
        vts1::ZoneMap zone = {};
        zone.min_value = descriptors[i].min_value;
        zone.max_value = descriptors[i].max_value;
        zone.row_count = static_cast<u32>(row_count_);
        zone.null_count = 0;
        zone.first_row_idx = 0;
        zone.last_row_idx = row_count_ ? static_cast<u32>(row_count_ - 1) : 0;

        std::memcpy(data + offset, &zone, sizeof(zone));
        offset += vts1::ZoneMap::Size;
    }

    // Align for bloom filter
    offset = (offset + 63) & ~usize(63);
    header.bloom_filter_offset = static_cast<u32>(offset);

    // Write simple bloom filter for instrument_id
    vts1::BloomFilterParams bf_params = vts1::BloomFilterParams::for_items(row_count_);
    usize bf_size = bf_params.size_bytes;

    // Simple bloom filter: just store instrument_ids as a set for now
    std::vector<u8> bloom_filter(bf_size, 0);
    for (u32 inst_id : instrument_ids_) {
        // Hash and set bits
        u64 hash = static_cast<u64>(inst_id) * 14695981039346656037ULL;
        for (usize h = 0; h < bf_params.num_hashes; ++h) {
            usize bit_idx = (hash + h * h) % bf_params.num_bits;
            bloom_filter[bit_idx / 8] |= (1 << (bit_idx % 8));
        }
    }

    if (bf_size)
        std::memcpy(data + offset, bloom_filter.data(), bf_size);
    offset += bf_size;
    header.bloom_filter_size = static_cast<u32>(bf_size);

    // Align for footer
    offset = (offset + 63) & ~usize(63);
    header.footer_offset = static_cast<u32>(offset);

    // Write footer
    vts1::SegmentFooter footer = {};
    footer.magic = Magic::VTS1;
    footer.version = Version::VTS1;
    footer.total_size = offset + vts1::SegmentFooter::Size;
    footer.commit_marker = vts1::SegmentFooter::CommittedMarker;
    footer.row_count = static_cast<u32>(row_count_);
    footer.column_count = vts1::ColumnCount;
    footer.min_sequence = min_sequence_;
    footer.max_sequence = max_sequence_;

    // Compute CRCs for descriptors and overall segment digest
    u64 segment_digest = 0;
    for (auto &desc : descriptors) {
        desc.data_crc = Crc32C::compute(data + desc.offset, desc.compressed_size);
        desc.descriptor_crc = Crc32C::compute(reinterpret_cast<byte *>(&desc),
                                              vts1::ColumnDescriptor::Size - sizeof(u32));
        segment_digest = hash_combine(segment_digest, desc.data_crc);
    }

    // Write descriptors back
    std::memcpy(data + header.descriptor_offset, descriptors.data(),
                descriptors.size() * vts1::ColumnDescriptor::Size);

    // Fill in footer hashes
    footer.descriptor_table_hash = Crc32C::compute(
        data + header.descriptor_offset, descriptors.size() * vts1::ColumnDescriptor::Size);
    footer.segment_digest = segment_digest;

    // Compute header CRC
    header.header_crc = 0;
    header.header_crc =
        Crc32C::compute(reinterpret_cast<byte *>(&header), vts1::SegmentHeader::Size);

    // Write header
    std::memcpy(data, &header, vts1::SegmentHeader::Size);

    // Compute footer CRC
    footer.footer_crc = 0;
    footer.footer_crc =
        Crc32C::compute(reinterpret_cast<byte *>(&footer), vts1::SegmentFooter::Size - sizeof(u32));

    std::memcpy(data + offset, &footer, vts1::SegmentFooter::Size);
    offset += vts1::SegmentFooter::Size;

    // Write to file
    return file::write_all(path, data, offset);
}

void SegmentWriter::reset(u64 new_segment_id) noexcept {
    segment_id_ = new_segment_id;
    row_count_ = 0;
    min_timestamp_ = UINT64_MAX;
    max_timestamp_ = 0;
    min_sequence_ = UINT64_MAX;
    max_sequence_ = 0;
    min_instrument_id_ = UINT64_MAX;
    max_instrument_id_ = 0;
    min_price_ = INT64_MAX;
    max_price_ = INT64_MIN;
    min_quantity_ = UINT32_MAX;
    max_quantity_ = 0;

    exchange_ts_ns_.clear();
    receive_ts_ns_.clear();
    sequences_.clear();
    instrument_ids_.clear();
    event_types_.clear();
    sides_.clear();
    flags_.clear();
    price_ticks_.clear();
    quantities_.clear();
    venue_ids_.clear();
    source_ids_.clear();
    trade_or_order_ids_.clear();
}

Status SegmentWriter::encode_column_u64(const std::vector<u64> &values, vts1::EncodingType encoding,
                                        byte *output, usize &offset, usize output_size) noexcept {
    if (values.empty())
        return Status::OK();

    if (encoding == vts1::EncodingType::Raw) {
        usize bytes = values.size() * sizeof(u64);
        if (offset + bytes > output_size) {
            return Status(StatusCode::BufferTooSmall, "Buffer too small for column");
        }
        std::memcpy(output + offset, values.data(), bytes);
        offset += bytes;
        return Status::OK();
    }

    if (encoding == vts1::EncodingType::Delta) {
        // Simple delta encoding: first value raw, then deltas
        if (offset + sizeof(u64) > output_size) {
            return Status(StatusCode::BufferTooSmall, "Buffer too small");
        }

        // Write first value
        std::memcpy(output + offset, &values[0], sizeof(u64));
        offset += sizeof(u64);

        // Write deltas
        for (usize i = 1; i < values.size(); ++i) {
            u64 delta = values[i] - values[i - 1];
            usize written = codec::VarIntU::encode(delta, output + offset, output_size - offset);
            if (written == 0) {
                return Status(StatusCode::BufferTooSmall, "Buffer too small for delta");
            }
            offset += written;
        }

        return Status::OK();
    }

    // Default to raw
    return encode_column_u64(values, vts1::EncodingType::Raw, output, offset, output_size);
}

Status SegmentWriter::encode_column_u32(const std::vector<u32> &values, vts1::EncodingType encoding,
                                        byte *output, usize &offset, usize output_size) noexcept {
    if (values.empty())
        return Status::OK();

    if (encoding == vts1::EncodingType::Raw) {
        usize bytes = values.size() * sizeof(u32);
        if (offset + bytes > output_size) {
            return Status(StatusCode::BufferTooSmall, "Buffer too small");
        }
        std::memcpy(output + offset, values.data(), bytes);
        offset += bytes;
        return Status::OK();
    }

    if (encoding == vts1::EncodingType::BitPacked) {
        // Find min/max for frame of reference
        u32 min_val = *std::min_element(values.begin(), values.end());
        u32 max_val = *std::max_element(values.begin(), values.end());

        // Write min value as reference
        if (offset + sizeof(u32) > output_size) {
            return Status(StatusCode::BufferTooSmall, "Buffer too small");
        }
        std::memcpy(output + offset, &min_val, sizeof(u32));
        offset += sizeof(u32);

        // Calculate bits needed
        u32 range = max_val - min_val;
        u32 bits = codec::BitPackU32::bits_required(range);

        // Write bits per value
        if (offset + sizeof(u8) > output_size) {
            return Status(StatusCode::BufferTooSmall, "Buffer too small");
        }
        output[offset++] = static_cast<byte>(bits);

        // Pack values relative to min
        std::vector<u32> packed(values.size());
        for (usize i = 0; i < values.size(); ++i) {
            packed[i] = values[i] - min_val;
        }

        usize required = codec::BitPackU32::max_encoded_size(values.size(), bits);
        if (offset + required > output_size) {
            return Status(StatusCode::BufferTooSmall, "Buffer too small");
        }

        usize written = codec::BitPackU32::encode(packed.data(), values.size(), bits,
                                                  output + offset, output_size - offset);
        offset += written;

        return Status::OK();
    }

    return encode_column_u32(values, vts1::EncodingType::Raw, output, offset, output_size);
}

Status SegmentWriter::encode_column_i64(const std::vector<i64> &values, vts1::EncodingType encoding,
                                        byte *output, usize &offset, usize output_size) noexcept {
    if (values.empty())
        return Status::OK();

    if (encoding == vts1::EncodingType::Raw) {
        usize bytes = values.size() * sizeof(i64);
        if (offset + bytes > output_size) {
            return Status(StatusCode::BufferTooSmall, "Buffer too small");
        }
        std::memcpy(output + offset, values.data(), bytes);
        offset += bytes;
        return Status::OK();
    }

    if (encoding == vts1::EncodingType::Delta) {
        // Zigzag encode deltas
        if (offset + sizeof(i64) > output_size) {
            return Status(StatusCode::BufferTooSmall, "Buffer too small");
        }

        // Write first value
        std::memcpy(output + offset, &values[0], sizeof(i64));
        offset += sizeof(i64);

        // Write deltas
        for (usize i = 1; i < values.size(); ++i) {
            auto checked_delta = checked_sub(values[i], values[i - 1]);
            if (!checked_delta)
                return Status(StatusCode::InvalidArgument, "Price delta overflow");
            i64 delta = *checked_delta;
            u64 zigzag = codec::ZigZag::encode(delta);
            usize written = codec::VarIntU::encode(zigzag, output + offset, output_size - offset);
            if (written == 0) {
                return Status(StatusCode::BufferTooSmall, "Buffer too small");
            }
            offset += written;
        }

        return Status::OK();
    }

    return encode_column_i64(values, vts1::EncodingType::Raw, output, offset, output_size);
}

Status SegmentWriter::encode_column_u16(const std::vector<u16> &values, vts1::EncodingType encoding,
                                        byte *output, usize &offset, usize output_size) noexcept {
    if (values.empty())
        return Status::OK();

    if (encoding == vts1::EncodingType::Raw) {
        usize bytes = values.size() * sizeof(u16);
        if (offset + bytes > output_size) {
            return Status(StatusCode::BufferTooSmall, "Buffer too small for u16 raw column");
        }
        std::memcpy(output + offset, values.data(), bytes);
        offset += bytes;
        return Status::OK();
    }

    // Convert to u32 and use u32 encoding
    std::vector<u32> values32(values.begin(), values.end());
    return encode_column_u32(values32, encoding, output, offset, output_size);
}

Status SegmentWriter::encode_column_u8(const std::vector<u8> &values, vts1::EncodingType encoding,
                                       byte *output, usize &offset, usize output_size) noexcept {
    if (values.empty())
        return Status::OK();

    if (encoding == vts1::EncodingType::Raw) {
        usize bytes = values.size();
        if (offset + bytes > output_size) {
            return Status(StatusCode::BufferTooSmall, "Buffer too small");
        }
        std::memcpy(output + offset, values.data(), bytes);
        offset += bytes;
        return Status::OK();
    }

    if (encoding == vts1::EncodingType::RLE) {
        // Use RLE for low-cardinality data
        usize written =
            codec::RLE::encode(values.data(), values.size(), output + offset, output_size - offset);
        if (written == 0) {
            // Fall back to raw
            return encode_column_u8(values, vts1::EncodingType::Raw, output, offset, output_size);
        }
        offset += written;
        return Status::OK();
    }

    return encode_column_u8(values, vts1::EncodingType::Raw, output, offset, output_size);
}

usize SegmentWriter::select_encoding_size(u64 min_val, u64 max_val) const noexcept {
    u64 range = max_val - min_val;
    if (range == 0)
        return 1;
    usize bits = 0;
    while (range > 0) {
        bits++;
        range >>= 1;
    }
    return (bits + 7) / 8;
}

} // namespace vectortick
