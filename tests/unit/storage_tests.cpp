#include "test_framework.hpp"
#include "vectortick/storage/segment_writer.hpp"
#include "vectortick/storage/segment_reader.hpp"
#include "vectortick/storage/file_format.hpp"
#include <cstdio>
#include <vector>
#include <string>
#if defined(_WIN32)
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

using namespace vectortick;

VT_TEST(storage_tests, segment_roundtrip_all_columns) {
    std::string path = "/tmp/test_roundtrip_" + std::to_string(getpid()) + ".vts";
    remove(path.c_str());
    
    SegmentWriter writer(42, 1000);
    
    std::vector<CanonicalEvent> original_events;
    for (usize i = 0; i < 50; ++i) {
        CanonicalEvent ev;
        ev.exchange_ts_ns = 1700000000000000000ULL + i * 1000000ULL;
        ev.receive_ts_ns = ev.exchange_ts_ns + 500;
        ev.sequence = 1000 + i;
        ev.instrument_id = static_cast<u32>(100 + (i % 5));
        ev.event_type = (i % 2 == 0) ? EventType::Quote : EventType::Trade;
        ev.side = (i % 3 == 0) ? Side::Ask : Side::Bid;
        ev.flags = static_cast<u16>(i * 3);
        ev.price_ticks = (i % 2 == 0) ? static_cast<i64>(50000 + i * 10) : static_cast<i64>(-1000 - i * 5);
        ev.quantity = static_cast<u32>(10 + i * 2);
        ev.venue_id = static_cast<u16>(1 + (i % 4));
        ev.source_id = static_cast<u16>(10 + (i % 2));
        ev.trade_or_order_id = 999000ULL + i;
        
        original_events.push_back(ev);
        auto st = writer.add_event(ev);
        VT_ASSERT(st.ok());
    }
    
    VT_ASSERT(writer.row_count() == 50);
    auto write_st = writer.write_to_file(path);
    VT_ASSERT(write_st.ok());
    
    SegmentReader reader;
    auto open_st = reader.open(path);
    VT_ASSERT(open_st.ok());
    VT_ASSERT(reader.is_open());
    VT_ASSERT(reader.row_count() == 50);
    VT_ASSERT(reader.segment_id() == 42);
    VT_ASSERT(reader.min_timestamp() == original_events.front().exchange_ts_ns);
    VT_ASSERT(reader.max_timestamp() == original_events.back().exchange_ts_ns);
    
    // Check bloom filter
    VT_ASSERT(reader.might_contain_instrument(100));
    VT_ASSERT(reader.might_contain_instrument(101));
    VT_ASSERT(reader.might_contain_instrument(102));
    VT_ASSERT(reader.might_contain_instrument(103));
    VT_ASSERT(reader.might_contain_instrument(104));
    
    // Check integrity validation
    auto val_st = reader.validate();
    VT_ASSERT(val_st.ok());
    
    // Read all events
    std::vector<CanonicalEvent> read_events;
    auto read_all_st = reader.read_all_events(read_events);
    VT_ASSERT(read_all_st.ok());
    VT_ASSERT(read_events.size() == 50);
    
    for (usize i = 0; i < 50; ++i) {
        const auto& exp = original_events[i];
        const auto& act = read_events[i];
        VT_ASSERT(act.exchange_ts_ns == exp.exchange_ts_ns);
        VT_ASSERT(act.receive_ts_ns == exp.receive_ts_ns);
        VT_ASSERT(act.sequence == exp.sequence);
        VT_ASSERT(act.instrument_id == exp.instrument_id);
        VT_ASSERT(act.event_type == exp.event_type);
        VT_ASSERT(act.side == exp.side);
        VT_ASSERT(act.flags == exp.flags);
        VT_ASSERT(act.price_ticks == exp.price_ticks);
        VT_ASSERT(act.quantity == exp.quantity);
        VT_ASSERT(act.venue_id == exp.venue_id);
        VT_ASSERT(act.source_id == exp.source_id);
        VT_ASSERT(act.trade_or_order_id == exp.trade_or_order_id);
    }
    
    // Test random single row access
    CanonicalEvent single_ev;
    auto s1 = reader.read_event(0, single_ev);
    VT_ASSERT(s1.ok());
    VT_ASSERT(single_ev.sequence == original_events[0].sequence);
    
    auto s2 = reader.read_event(25, single_ev);
    VT_ASSERT(s2.ok());
    VT_ASSERT(single_ev.sequence == original_events[25].sequence);
    
    auto s3 = reader.read_event(49, single_ev);
    VT_ASSERT(s3.ok());
    VT_ASSERT(single_ev.sequence == original_events[49].sequence);
    
    // Out of range row
    auto s_err = reader.read_event(50, single_ev);
    VT_ASSERT(!s_err.ok());
    VT_ASSERT(s_err.code() == StatusCode::OutOfRange);
    
    reader.close();
    remove(path.c_str());
}

VT_TEST(storage_tests, corruption_detection) {
    std::string orig_path = "/tmp/test_corrupt_orig_" + std::to_string(getpid()) + ".vts";
    std::string test_path = "/tmp/test_corrupt_copy_" + std::to_string(getpid()) + ".vts";
    remove(orig_path.c_str());
    remove(test_path.c_str());
    
    SegmentWriter writer(1, 100);
    CanonicalEvent ev;
    ev.exchange_ts_ns = 1000;
    ev.receive_ts_ns = 1001;
    ev.sequence = 1;
    ev.instrument_id = 1;
    ev.event_type = EventType::Trade;
    ev.price_ticks = 100;
    ev.quantity = 10;
    VT_ASSERT(writer.add_event(ev).ok());
    VT_ASSERT(writer.write_to_file(orig_path.c_str()).ok());
    
    // Read file bytes into memory
    FILE* fp = fopen(orig_path.c_str(), "rb");
    VT_ASSERT(fp != nullptr);
    fseek(fp, 0, SEEK_END);
    usize sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::vector<byte> buf(sz);
    fread(buf.data(), 1, sz, fp);
    fclose(fp);
    
    // 1. Corrupt magic
    {
        std::vector<byte> bad_magic = buf;
        bad_magic[0] = 'B';
        bad_magic[1] = 'A';
        bad_magic[2] = 'D';
        bad_magic[3] = '!';
        FILE* out = fopen(test_path.c_str(), "wb");
        fwrite(bad_magic.data(), 1, bad_magic.size(), out);
        fclose(out);
        
        SegmentReader r;
        auto st = r.open(test_path);
        VT_ASSERT(!st.ok());
        VT_ASSERT(st.code() == StatusCode::SegmentInvalidHeader);
        remove(test_path.c_str());
    }
    
    // 2. Corrupt schema hash
    {
        std::vector<byte> bad_schema = buf;
        // Offset 8 is schema_hash
        bad_schema[8] ^= 0xFF;
        // Recompute header CRC so header CRC passes but schema hash fails
        vts1::SegmentHeader* hdr = reinterpret_cast<vts1::SegmentHeader*>(bad_schema.data());
        hdr->header_crc = 0;
        hdr->header_crc = Crc32C::compute(bad_schema.data(), vts1::SegmentHeader::Size);
        
        FILE* out = fopen(test_path.c_str(), "wb");
        fwrite(bad_schema.data(), 1, bad_schema.size(), out);
        fclose(out);
        
        SegmentReader r;
        auto st = r.open(test_path);
        VT_ASSERT(!st.ok());
        VT_ASSERT(st.code() == StatusCode::SegmentSchemaMismatch);
        remove(test_path.c_str());
    }
    
    // 3. Corrupt column data CRC
    {
        std::vector<byte> bad_col = buf;
        vts1::SegmentHeader* hdr = reinterpret_cast<vts1::SegmentHeader*>(bad_col.data());
        vts1::ColumnDescriptor* desc = reinterpret_cast<vts1::ColumnDescriptor*>(bad_col.data() + hdr->descriptor_offset);
        // Corrupt first byte of column 0 data
        bad_col[desc[0].offset] ^= 0xFF;
        
        FILE* out = fopen(test_path.c_str(), "wb");
        fwrite(bad_col.data(), 1, bad_col.size(), out);
        fclose(out);
        
        SegmentReader r;
        auto st = r.open(test_path);
        VT_ASSERT(st.ok()); // open succeeds, validate checks column data CRC
        auto val_st = r.validate();
        VT_ASSERT(!val_st.ok());
        VT_ASSERT(val_st.code() == StatusCode::SegmentChecksumFailed);
        r.close();
        remove(test_path.c_str());
    }
    
    remove(orig_path.c_str());
}
