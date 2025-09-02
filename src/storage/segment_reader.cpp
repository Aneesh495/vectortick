#include "vectortick/storage/segment_reader.hpp"
#include "vectortick/codec/bitpack.hpp"
#include "vectortick/codec/rle.hpp"
#include "vectortick/codec/varint.hpp"
#include "vectortick/common/checked_math.hpp"
#include <cstring>

namespace vectortick {

Status SegmentReader::open(const std::string &path) noexcept {
    close();
    auto result = MappedFile::open_read(path);
    if (!result.ok()) {
        return result.status();
    }

    mapping_ = std::move(result.value());
    file_size_ = mapping_.size();

    auto status = read_header();
    if (!status.ok()) {
        close();
        return status;
    }

    status = read_descriptors();
    if (status.ok())
        status = validate();
    if (!status.ok()) {
        close();
        return status;
    }

    return Status::OK();
}

void SegmentReader::close() noexcept {
    mapping_.close();
    descriptors_.clear();
    zone_maps_.clear();
    bloom_filter_.clear();
    row_count_ = 0;
    segment_id_ = 0;
}

Status SegmentReader::read_header() noexcept {
    if (!mapping_.is_open()) {
        return Status(StatusCode::InvalidMapping, "No file mapped");
    }

    if (mapping_.size() < vts1::SegmentHeader::Size) {
        return Status(StatusCode::SegmentInvalidHeader, "File too small for header");
    }

    const byte *data = mapping_.data();
    std::memcpy(&header_, data, vts1::SegmentHeader::Size);

    // Validate magic
    if (!header_.is_valid_magic()) {
        return Status(StatusCode::SegmentInvalidHeader, "Invalid VTS1 magic");
    }

    // Validate version
    if (!header_.is_valid_version()) {
        return Status(StatusCode::SegmentInvalidHeader, "Unsupported VTS1 version");
    }

    // Validate CRC
    vts1::SegmentHeader temp = header_;
    temp.header_crc = 0;
    u32 computed_crc = Crc32C::compute(reinterpret_cast<byte *>(&temp), vts1::SegmentHeader::Size);

    if (computed_crc != header_.header_crc) {
        return Status(StatusCode::SegmentChecksumFailed, "Header CRC mismatch");
    }

    // Validate schema hash
    if (header_.schema_hash != vts1::compute_canonical_schema_hash()) {
        return Status(StatusCode::SegmentSchemaMismatch, "VTS1 schema hash mismatch");
    }

    auto within = [&](u64 offset, u64 size) {
        return offset <= file_size_ && size <= file_size_ - offset;
    };
    if (header_.row_count > SegmentConfig::MaxRowsPerSegment ||
        header_.descriptor_offset < vts1::SegmentHeader::Size ||
        header_.descriptor_size != vts1::ColumnCount * vts1::ColumnDescriptor::Size ||
        !within(header_.descriptor_offset, header_.descriptor_size) ||
        !within(header_.zone_map_offset, vts1::ColumnCount * vts1::ZoneMap::Size) ||
        !within(header_.bloom_filter_offset, header_.bloom_filter_size) ||
        !within(header_.footer_offset, vts1::SegmentFooter::Size) ||
        header_.zone_map_offset < header_.descriptor_offset + header_.descriptor_size ||
        header_.bloom_filter_offset <
            header_.zone_map_offset + vts1::ColumnCount * vts1::ZoneMap::Size ||
        header_.footer_offset < header_.bloom_filter_offset + header_.bloom_filter_size ||
        file_size_ - header_.footer_offset != vts1::SegmentFooter::Size)
        return Status(StatusCode::SegmentInvalidHeader,
                      "Invalid segment counts or metadata ranges");
    segment_id_ = header_.segment_id;
    row_count_ = static_cast<usize>(header_.row_count);
    min_timestamp_ = header_.min_timestamp;
    max_timestamp_ = header_.max_timestamp;

    return Status::OK();
}

Status SegmentReader::read_descriptors() noexcept {
    const byte *data = mapping_.data();

    // Read column descriptors
    usize num_columns = vts1::ColumnCount;
    descriptors_.resize(num_columns);

    usize desc_offset = header_.descriptor_offset;
    u64 previous_end = header_.descriptor_offset + header_.descriptor_size;
    for (usize i = 0; i < num_columns; ++i) {
        if (desc_offset + vts1::ColumnDescriptor::Size > file_size_) {
            return Status(StatusCode::SegmentInvalidBlock, "Descriptor out of bounds");
        }

        std::memcpy(&descriptors_[i], data + desc_offset, vts1::ColumnDescriptor::Size);
        desc_offset += vts1::ColumnDescriptor::Size;

        // Validate descriptor CRC
        vts1::ColumnDescriptor temp = descriptors_[i];
        temp.descriptor_crc = 0;
        u32 computed_crc = Crc32C::compute(reinterpret_cast<byte *>(&temp),
                                           vts1::ColumnDescriptor::Size - sizeof(u32));

        if (computed_crc != descriptors_[i].descriptor_crc) {
            return Status(StatusCode::SegmentChecksumFailed, "Descriptor CRC mismatch");
        }
        const auto &d = descriptors_[i];
        auto expected_type = vts1::get_column_type(static_cast<vts1::ColumnID>(i));
        auto width = vts1::column_type_size(expected_type);
        if (d.column_id != i || d.type != expected_type || d.null_count != 0 ||
            d.uncompressed_size != header_.row_count * width || d.offset < previous_end ||
            d.offset > header_.zone_map_offset ||
            d.compressed_size > header_.zone_map_offset - d.offset)
            return Status(StatusCode::SegmentSchemaMismatch,
                          "Inconsistent column metadata or range");
        bool supported = d.encoding == vts1::EncodingType::Raw ||
                         (d.encoding == vts1::EncodingType::Delta &&
                          (d.type == vts1::ColumnType::U64 || d.type == vts1::ColumnType::I64)) ||
                         (d.encoding == vts1::EncodingType::BitPacked &&
                          (d.type == vts1::ColumnType::U32 || d.type == vts1::ColumnType::U16)) ||
                         (d.encoding == vts1::EncodingType::RLE && d.type == vts1::ColumnType::U8);
        if (!supported ||
            (d.encoding == vts1::EncodingType::Raw && d.compressed_size != d.uncompressed_size) ||
            (row_count_ == 0 && d.compressed_size != 0))
            return Status(StatusCode::SegmentInvalidBlock, "Invalid column encoding or size");
        previous_end = d.offset + d.compressed_size;
    }

    // Read zone maps
    usize zone_offset = header_.zone_map_offset;
    zone_maps_.resize(num_columns);

    for (usize i = 0; i < num_columns; ++i) {
        if (zone_offset + vts1::ZoneMap::Size > file_size_) {
            return Status(StatusCode::SegmentInvalidBlock, "Zone map out of bounds");
        }

        std::memcpy(&zone_maps_[i], data + zone_offset, vts1::ZoneMap::Size);
        zone_offset += vts1::ZoneMap::Size;
        if (zone_maps_[i].row_count != row_count_ || zone_maps_[i].null_count != 0)
            return Status(StatusCode::SegmentCorrupted, "Zone map count mismatch");
    }

    // Read bloom filter
    if (header_.bloom_filter_size > 0) {
        usize bf_offset = header_.bloom_filter_offset;
        if (bf_offset + header_.bloom_filter_size > file_size_) {
            return Status(StatusCode::SegmentInvalidBlock, "Bloom filter out of bounds");
        }

        bloom_filter_.resize(header_.bloom_filter_size);
        std::memcpy(bloom_filter_.data(), data + bf_offset, header_.bloom_filter_size);
    }

    return Status::OK();
}

Status SegmentReader::read_all_events(std::vector<CanonicalEvent> &events) const noexcept {
    events.clear();
    if (!is_open())
        return Status(StatusCode::InvalidMapping, "No segment open");
    if (row_count_ == 0)
        return Status::OK();

    std::vector<u64> exchange_ts(row_count_);
    std::vector<u64> receive_ts(row_count_);
    std::vector<u64> seqs(row_count_);
    std::vector<u32> inst_ids(row_count_);
    std::vector<u8> ev_types(row_count_);
    std::vector<u8> sides(row_count_);
    std::vector<u16> flags(row_count_);
    std::vector<i64> prices(row_count_);
    std::vector<u32> qtys(row_count_);
    std::vector<u16> venues(row_count_);
    std::vector<u16> sources(row_count_);
    std::vector<u64> trade_order_ids(row_count_);

    auto s = read_column_u64(vts1::ColumnID::ExchangeTsNs, exchange_ts.data(), row_count_);
    if (!s.ok())
        return s;
    s = read_column_u64(vts1::ColumnID::ReceiveTsNs, receive_ts.data(), row_count_);
    if (!s.ok())
        return s;
    s = read_column_u64(vts1::ColumnID::Sequence, seqs.data(), row_count_);
    if (!s.ok())
        return s;
    s = read_column_u32(vts1::ColumnID::InstrumentId, inst_ids.data(), row_count_);
    if (!s.ok())
        return s;
    s = read_column_u8(vts1::ColumnID::EventType, ev_types.data(), row_count_);
    if (!s.ok())
        return s;
    s = read_column_u8(vts1::ColumnID::Side, sides.data(), row_count_);
    if (!s.ok())
        return s;
    s = read_column_u16(vts1::ColumnID::Flags, flags.data(), row_count_);
    if (!s.ok())
        return s;
    s = read_column_i64(vts1::ColumnID::PriceTicks, prices.data(), row_count_);
    if (!s.ok())
        return s;
    s = read_column_u32(vts1::ColumnID::Quantity, qtys.data(), row_count_);
    if (!s.ok())
        return s;
    s = read_column_u16(vts1::ColumnID::VenueId, venues.data(), row_count_);
    if (!s.ok())
        return s;
    s = read_column_u16(vts1::ColumnID::SourceId, sources.data(), row_count_);
    if (!s.ok())
        return s;
    s = read_column_u64(vts1::ColumnID::TradeOrOrderId, trade_order_ids.data(), row_count_);
    if (!s.ok())
        return s;

    events.resize(row_count_);
    for (usize i = 0; i < row_count_; ++i) {
        events[i].exchange_ts_ns = exchange_ts[i];
        events[i].receive_ts_ns = receive_ts[i];
        events[i].sequence = seqs[i];
        events[i].instrument_id = inst_ids[i];
        events[i].event_type = static_cast<EventType>(ev_types[i]);
        events[i].side = static_cast<Side>(sides[i]);
        events[i].flags = flags[i];
        events[i].price_ticks = prices[i];
        events[i].quantity = qtys[i];
        events[i].venue_id = venues[i];
        events[i].source_id = sources[i];
        events[i].trade_or_order_id = trade_order_ids[i];
    }

    return Status::OK();
}

Status SegmentReader::read_event(usize row_idx, CanonicalEvent &event) const noexcept {
    if (row_idx >= row_count_) {
        return Status(StatusCode::OutOfRange, "Row index out of range");
    }

    std::vector<CanonicalEvent> all;
    auto s = read_all_events(all);
    if (!s.ok())
        return s;
    event = all[row_idx];
    return Status::OK();
}

Status SegmentReader::decode_column(const vts1::ColumnDescriptor &desc, byte *output,
                                    usize num_values) const noexcept {
    const byte *data = mapping_.data();
    if (!is_open() || num_values != row_count_ || (num_values && !output) ||
        desc.offset > file_size_ || desc.compressed_size > file_size_ - desc.offset)
        return Status(StatusCode::SegmentInvalidBlock, "Invalid column decode range or count");
    if (num_values == 0)
        return Status::OK();
    const byte *col_data = data + desc.offset;

    // Validate data CRC
    u32 computed_crc = Crc32C::compute(col_data, desc.compressed_size);
    if (computed_crc != desc.data_crc) {
        return Status(StatusCode::SegmentChecksumFailed, "Column data CRC mismatch");
    }

    // Decode based on encoding type
    if (desc.encoding == vts1::EncodingType::Raw) {
        std::memcpy(output, col_data, desc.compressed_size);
        return Status::OK();
    }

    if (desc.encoding == vts1::EncodingType::Delta) {
        if (desc.compressed_size < 8)
            return Status(StatusCode::SegmentCorrupted, "Truncated delta base");
        if (desc.type == vts1::ColumnType::I64) {
            i64 first_val = 0;
            std::memcpy(&first_val, col_data, sizeof(i64));
            usize offset = sizeof(i64);
            i64 *values = reinterpret_cast<i64 *>(output);
            if (num_values > 0)
                values[0] = first_val;
            for (usize i = 1; i < num_values; ++i) {
                u64 zigzag = 0;
                usize consumed = codec::VarIntU::decode(col_data + offset,
                                                        desc.compressed_size - offset, zigzag);
                if (consumed == 0) {
                    return Status(StatusCode::SegmentCorrupted, "Failed to decode i64 delta");
                }
                offset += consumed;
                i64 delta = codec::ZigZag::decode(zigzag);
                auto next = checked_add(values[i - 1], delta);
                if (!next)
                    return Status(StatusCode::SegmentCorrupted, "Delta overflow");
                values[i] = *next;
            }
            if (offset != desc.compressed_size)
                return Status(StatusCode::SegmentCorrupted, "Trailing delta bytes");
            return Status::OK();
        } else {
            u64 first_val = 0;
            std::memcpy(&first_val, col_data, sizeof(u64));
            usize offset = sizeof(u64);
            u64 *values = reinterpret_cast<u64 *>(output);
            if (num_values > 0)
                values[0] = first_val;
            for (usize i = 1; i < num_values; ++i) {
                u64 delta = 0;
                usize consumed =
                    codec::VarIntU::decode(col_data + offset, desc.compressed_size - offset, delta);
                if (consumed == 0) {
                    return Status(StatusCode::SegmentCorrupted, "Failed to decode u64 delta");
                }
                offset += consumed;
                values[i] = values[i - 1] + delta;
            }
            if (offset != desc.compressed_size)
                return Status(StatusCode::SegmentCorrupted, "Trailing delta bytes");
            return Status::OK();
        }
    }

    if (desc.encoding == vts1::EncodingType::BitPacked) {
        if (desc.compressed_size < 5)
            return Status(StatusCode::SegmentCorrupted, "Truncated packed header");
        // Read reference value
        u32 reference = 0;
        std::memcpy(&reference, col_data, sizeof(u32));
        usize offset = sizeof(u32);

        // Read bits per value
        u32 bits = static_cast<u32>(col_data[offset++]);

        if (bits == 0 || bits > 32)
            return Status(StatusCode::SegmentCorrupted, "Invalid packed bit width");
        // Decode packed values
        std::vector<u32> packed(num_values);
        usize consumed = codec::BitPackU32::decode(col_data + offset, desc.compressed_size - offset,
                                                   bits, packed.data(), num_values);
        if (consumed == 0 || consumed + offset != desc.compressed_size) {
            return Status(StatusCode::SegmentCorrupted, "Failed to decode bit-packed values");
        }

        // Add reference back
        if (desc.type == vts1::ColumnType::U16) {
            u16 *values = reinterpret_cast<u16 *>(output);
            for (usize i = 0; i < num_values; ++i) {
                if (packed[i] > UINT16_MAX || reference > UINT16_MAX - packed[i])
                    return Status(StatusCode::SegmentCorrupted, "Packed u16 overflow");
                values[i] = static_cast<u16>(packed[i] + reference);
            }
        } else {
            u32 *values = reinterpret_cast<u32 *>(output);
            for (usize i = 0; i < num_values; ++i) {
                if (reference > UINT32_MAX - packed[i])
                    return Status(StatusCode::SegmentCorrupted, "Packed u32 overflow");
                values[i] = packed[i] + reference;
            }
        }
        return Status::OK();
    }

    if (desc.encoding == vts1::EncodingType::RLE) {
        if (desc.type == vts1::ColumnType::U8) {
            usize consumed = codec::RLE::decode<u8>(col_data, desc.compressed_size,
                                                    reinterpret_cast<u8 *>(output), num_values);
            if (consumed == 0 || consumed != desc.compressed_size) {
                return Status(StatusCode::SegmentCorrupted, "Failed to decode RLE");
            }
            return Status::OK();
        }
    }

    return Status(StatusCode::SegmentInvalidBlock, "Unsupported column encoding");
}

Status SegmentReader::read_column_u64(vts1::ColumnID col, u64 *values,
                                      usize num_values) const noexcept {
    if (col >= descriptors_.size()) {
        return Status(StatusCode::OutOfRange, "Invalid column ID");
    }

    if (descriptors_[col].type != vts1::ColumnType::U64)
        return Status(StatusCode::InvalidArgument, "Column output type mismatch");
    return decode_column(descriptors_[col], reinterpret_cast<byte *>(values), num_values);
}

Status SegmentReader::read_column_u32(vts1::ColumnID col, u32 *values,
                                      usize num_values) const noexcept {
    if (col >= descriptors_.size()) {
        return Status(StatusCode::OutOfRange, "Invalid column ID");
    }

    if (descriptors_[col].type != vts1::ColumnType::U32)
        return Status(StatusCode::InvalidArgument, "Column output type mismatch");
    return decode_column(descriptors_[col], reinterpret_cast<byte *>(values), num_values);
}

Status SegmentReader::read_column_i64(vts1::ColumnID col, i64 *values,
                                      usize num_values) const noexcept {
    if (col >= descriptors_.size()) {
        return Status(StatusCode::OutOfRange, "Invalid column ID");
    }

    if (descriptors_[col].type != vts1::ColumnType::I64)
        return Status(StatusCode::InvalidArgument, "Column output type mismatch");
    return decode_column(descriptors_[col], reinterpret_cast<byte *>(values), num_values);
}

Status SegmentReader::read_column_u16(vts1::ColumnID col, u16 *values,
                                      usize num_values) const noexcept {
    if (col >= descriptors_.size()) {
        return Status(StatusCode::OutOfRange, "Invalid column ID");
    }

    if (descriptors_[col].type != vts1::ColumnType::U16)
        return Status(StatusCode::InvalidArgument, "Column output type mismatch");
    return decode_column(descriptors_[col], reinterpret_cast<byte *>(values), num_values);
}

Status SegmentReader::read_column_u8(vts1::ColumnID col, u8 *values,
                                     usize num_values) const noexcept {
    if (col >= descriptors_.size()) {
        return Status(StatusCode::OutOfRange, "Invalid column ID");
    }

    if (descriptors_[col].type != vts1::ColumnType::U8)
        return Status(StatusCode::InvalidArgument, "Column output type mismatch");
    return decode_column(descriptors_[col], reinterpret_cast<byte *>(values), num_values);
}

u64 SegmentReader::column_min(vts1::ColumnID col) const noexcept {
    if (col < descriptors_.size()) {
        return descriptors_[col].min_value;
    }
    return 0;
}

u64 SegmentReader::column_max(vts1::ColumnID col) const noexcept {
    if (col < descriptors_.size()) {
        return descriptors_[col].max_value;
    }
    return 0;
}

bool SegmentReader::might_contain_instrument(u32 instrument_id) const noexcept {
    if (bloom_filter_.empty()) {
        return true; // No bloom filter, assume might contain
    }

    // Hash and check bits
    u64 hash = static_cast<u64>(instrument_id) * 14695981039346656037ULL;
    usize num_bits = bloom_filter_.size() * 8;

    for (usize h = 0; h < 7; ++h) { // 7 hash functions
        usize bit_idx = (hash + h * h) % num_bits;
        if ((bloom_filter_[bit_idx / 8] & (1 << (bit_idx % 8))) == 0) {
            return false;
        }
    }

    return true;
}

Status SegmentReader::validate() const noexcept {
    if (!is_open()) {
        return Status(StatusCode::InvalidMapping, "No segment open");
    }

    const byte *data = mapping_.data();

    // Check all column descriptors and data CRCs
    for (const auto &desc : descriptors_) {
        if (desc.offset > file_size_ || desc.compressed_size > file_size_ - desc.offset) {
            return Status(StatusCode::SegmentInvalidBlock, "Column offset exceeds file size");
        }
        u32 data_crc = Crc32C::compute(data + desc.offset, desc.compressed_size);
        if (data_crc != desc.data_crc) {
            return Status(StatusCode::SegmentChecksumFailed, "Column data CRC mismatch");
        }
    }

    // Validate footer if exists
    if (header_.footer_offset + sizeof(vts1::SegmentFooter) <= file_size_) {
        vts1::SegmentFooter footer;
        std::memcpy(&footer, data + header_.footer_offset, sizeof(vts1::SegmentFooter));

        if (footer.magic != Magic::VTS1 || footer.version != Version::VTS1 ||
            footer.total_size != file_size_ || footer.row_count != row_count_ ||
            footer.column_count != vts1::ColumnCount)
            return Status(StatusCode::SegmentCorrupted, "Inconsistent segment footer");
        if (footer.commit_marker != vts1::SegmentFooter::CommittedMarker) {
            return Status(StatusCode::SegmentCorrupted, "Segment not committed");
        }

        // Validate footer CRC
        vts1::SegmentFooter temp = footer;
        temp.footer_crc = 0;
        u32 computed_crc = Crc32C::compute(reinterpret_cast<byte *>(&temp),
                                           sizeof(vts1::SegmentFooter) - sizeof(u32));

        if (computed_crc != footer.footer_crc) {
            return Status(StatusCode::SegmentChecksumFailed, "Footer CRC mismatch");
        }

        // Validate descriptor table hash if present
        if (footer.descriptor_table_hash != 0) {
            u32 desc_hash = Crc32C::compute(data + header_.descriptor_offset,
                                            descriptors_.size() * vts1::ColumnDescriptor::Size);
            if (footer.descriptor_table_hash != desc_hash) {
                return Status(StatusCode::SegmentChecksumFailed, "Descriptor table hash mismatch");
            }
        }

        // Validate segment digest if present
        if (footer.segment_digest != 0) {
            u64 computed_digest = 0;
            for (const auto &desc : descriptors_) {
                computed_digest = hash_combine(computed_digest, desc.data_crc);
            }
            if (footer.segment_digest != computed_digest) {
                return Status(StatusCode::SegmentChecksumFailed, "Segment digest mismatch");
            }
        }
    }

    return Status::OK();
}

double SegmentReader::compression_ratio() const noexcept {
    if (row_count_ == 0)
        return 0.0;

    usize canonical_size = row_count_ * CanonicalEvent::LogicalSize;
    if (file_size_ == 0)
        return 0.0;

    return static_cast<double>(canonical_size) / static_cast<double>(file_size_);
}

} // namespace vectortick
