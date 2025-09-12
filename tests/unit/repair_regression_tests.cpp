#include "../test_framework.hpp"
#include "vectortick/codec/varint.hpp"
#include "vectortick/execution/query_engine.hpp"
#include "vectortick/ir/builder.hpp"
#include "vectortick/jit/code_generator.hpp"
#include "vectortick/protocol/decoder.hpp"
#include "vectortick/query/parser.hpp"
#include "vectortick/replay/replay_engine.hpp"
#include "vectortick/storage/catalog.hpp"
#include "vectortick/storage/segment_reader.hpp"
#include "vectortick/storage/segment_writer.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
using namespace vectortick;
namespace {
std::vector<u8> read(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
void write(const std::string &p, const std::vector<u8> &b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char *>(b.data()), b.size());
}
std::vector<CanonicalEvent> fixture() {
    std::vector<CanonicalEvent> e(8);
    for (usize i = 0; i < e.size(); ++i) {
        e[i].sequence = i + 1;
        e[i].exchange_ts_ns = 100 + i;
        e[i].receive_ts_ns = 200 + i;
        e[i].price_ticks = i == 0   ? -Limits::MaxPrice
                           : i == 1 ? Limits::MaxPrice
                                    : static_cast<i64>(i);
        e[i].quantity = i;
        e[i].instrument_id = 1;
        e[i].event_type = EventType::Trade;
    }
    return e;
}
Status segment(const std::string &p, const std::vector<CanonicalEvent> &e) {
    SegmentWriter w(1, e.size());
    for (auto v : e) {
        auto s = w.add_event(v);
        if (!s.ok())
            return s;
    }
    return w.write_to_file(p);
}
void repair_crcs(std::vector<u8> &b) {
    vts1::SegmentHeader h;
    std::memcpy(&h, b.data(), sizeof(h));
    h.header_crc = 0;
    h.header_crc = crc32c(reinterpret_cast<const byte *>(&h), sizeof(h));
    std::memcpy(b.data(), &h, sizeof(h));
    u64 digest = 0;
    for (u32 i = 0; i < 12; ++i) {
        vts1::ColumnDescriptor d;
        usize p = h.descriptor_offset + i * sizeof(d);
        std::memcpy(&d, b.data() + p, sizeof(d));
        d.data_crc = crc32c(b.data() + d.offset, d.compressed_size);
        d.descriptor_crc = 0;
        d.descriptor_crc = crc32c(reinterpret_cast<const byte *>(&d), sizeof(d) - 4);
        digest = hash_combine(digest, d.data_crc);
        std::memcpy(b.data() + p, &d, sizeof(d));
    }
    vts1::SegmentFooter f;
    std::memcpy(&f, b.data() + h.footer_offset, sizeof(f));
    f.descriptor_table_hash = crc32c(b.data() + h.descriptor_offset, h.descriptor_size);
    f.segment_digest = digest;
    f.footer_crc = 0;
    f.footer_crc = crc32c(reinterpret_cast<const byte *>(&f), sizeof(f) - 4);
    std::memcpy(b.data() + h.footer_offset, &f, sizeof(f));
}
} // namespace
VT_TEST(repair_regression_tests, empty_extreme_and_typed_storage) {
    auto e = fixture();
    VT_ASSERT(segment("repair-extreme.vts", e).ok());
    SegmentReader r;
    VT_ASSERT(r.open("repair-extreme.vts").ok());
    std::vector<CanonicalEvent> decoded;
    VT_ASSERT(r.read_all_events(decoded).ok());
    VT_ASSERT_EQ(decoded.size(), e.size());
    for (usize i = 0; i < e.size(); ++i) {
        VT_ASSERT_EQ(decoded[i].price_ticks, e[i].price_ticks);
        VT_ASSERT_EQ(decoded[i].sequence, e[i].sequence);
    }
    u8 small[8];
    VT_ASSERT(!r.read_column_u8(vts1::ColumnID::Sequence, small, 8).ok());
    u64 large[8];
    VT_ASSERT(!r.read_column_u64(vts1::ColumnID::Sequence, large, 7).ok());
    VT_ASSERT(segment("repair-empty.vts", {}).ok());
    VT_ASSERT(r.open("repair-empty.vts").ok());
    VT_ASSERT(r.read_all_events(decoded).ok());
    VT_ASSERT(decoded.empty());
    query::Parser p("SELECT COUNT(*), MIN(price_ticks), SUM(quantity) FROM events");
    auto q = p.parse_query();
    VT_ASSERT(q.ok());
    for (auto engine : {QueryEngine::Reference, QueryEngine::Vector, QueryEngine::Native}) {
        auto out = execute_query(decoded, q.value().get(), engine);
        VT_ASSERT(out.ok());
        VT_ASSERT_EQ(out.value().rows.size(), 1ULL);
        VT_ASSERT_EQ(out.value().rows[0][0], "0");
    }
}
VT_TEST(repair_regression_tests, truncated_corrupt_schema_lengths_and_decode) {
    VT_ASSERT(segment("repair-source.vts", fixture()).ok());
    auto b = read("repair-source.vts");
    SegmentReader r;
    for (usize n : {usize(0), usize(1), usize(127), b.size() / 2, b.size() - 1}) {
        auto t = b;
        t.resize(n);
        write("repair-bad.vts", t);
        VT_ASSERT(!r.open("repair-bad.vts").ok());
        VT_ASSERT(!r.is_open());
    }
    for (usize p : {usize(0), usize(10), usize(180), b.size() - 8}) {
        auto t = b;
        t[p] ^= 128;
        write("repair-bad.vts", t);
        VT_ASSERT(!r.open("repair-bad.vts").ok());
    }
    auto t = b;
    vts1::SegmentHeader h;
    std::memcpy(&h, t.data(), sizeof(h));
    h.row_count = UINT64_MAX;
    h.header_crc = 0;
    h.header_crc = crc32c(reinterpret_cast<const byte *>(&h), sizeof(h));
    std::memcpy(t.data(), &h, sizeof(h));
    write("repair-bad.vts", t);
    VT_ASSERT(!r.open("repair-bad.vts").ok());
    for (int mode = 0; mode < 4; ++mode) {
        t = b;
        vts1::ColumnDescriptor d;
        std::memcpy(&d, t.data() + 128, sizeof(d));
        if (mode == 0)
            d.encoding = static_cast<vts1::EncodingType>(255);
        if (mode == 1)
            d.uncompressed_size = UINT64_MAX;
        if (mode == 2)
            d.offset = UINT64_MAX;
        if (mode == 3)
            d.type = vts1::ColumnType::U8;
        d.descriptor_crc = 0;
        d.descriptor_crc = crc32c(reinterpret_cast<const byte *>(&d), sizeof(d) - 4);
        std::memcpy(t.data() + 128, &d, sizeof(d));
        write("repair-bad.vts", t);
        VT_ASSERT(!r.open("repair-bad.vts").ok());
    }
    // A checksum-valid malformed delta payload must fail decoding, not become empty.
    t = b;
    vts1::ColumnDescriptor d;
    std::memcpy(&d, t.data() + 128, sizeof(d));
    VT_ASSERT(d.encoding == vts1::EncodingType::Delta);
    for (usize i = 8; i < d.compressed_size; ++i)
        t[d.offset + i] = 255;
    repair_crcs(t);
    write("repair-bad.vts", t);
    VT_ASSERT(r.open("repair-bad.vts").ok());
    std::vector<CanonicalEvent> out;
    VT_ASSERT(!r.read_all_events(out).ok());
}
VT_TEST(repair_regression_tests, wal_interior_corruption_and_torn_tail_append) {
    std::filesystem::remove("repair.wal");
    Journal j;
    VT_ASSERT(j.open("repair.wal").ok());
    VT_ASSERT(j.append_begin(1).ok());
    VT_ASSERT(j.append_commit(1).ok());
    VT_ASSERT(j.append_begin(2).ok());
    VT_ASSERT(j.append_commit(2).ok());
    j.close();
    auto b = read("repair.wal");
    auto t = b;
    t[34] ^= 1;
    write("repair-corrupt.wal", t);
    std::vector<Journal::RecoveredTx> tx;
    VT_ASSERT(!Journal::read_all("repair-corrupt.wal", tx).ok());
    VT_ASSERT(!j.open("repair-corrupt.wal").ok());
    t = b;
    t.insert(t.end(), {1, 2, 3, 4, 5});
    write("repair.wal", t);
    VT_ASSERT(j.open("repair.wal").ok());
    VT_ASSERT(j.append_begin(3).ok());
    VT_ASSERT(j.append_commit(3).ok());
    j.close();
    VT_ASSERT(Journal::read_all("repair.wal", tx).ok());
    VT_ASSERT_EQ(tx.size(), 3ULL);
    VT_ASSERT(tx.back().committed);
    for (usize n : {usize(0), usize(1), usize(15)}) {
        t = b;
        t.resize(n);
        write("repair-corrupt.wal", t);
        VT_ASSERT(!Journal::read_all("repair-corrupt.wal", tx).ok());
    }
}
VT_TEST(repair_regression_tests, recovery_publication_then_append_query) {
    std::filesystem::remove_all("repair-catalog");
    {
        Catalog c("repair-catalog");
        VT_ASSERT(c.open().ok());
        VT_ASSERT(segment("repair-catalog/.tmp_a.vts", fixture()).ok());
        SegmentMeta m;
        VT_ASSERT(c.commit_segment("repair-catalog/.tmp_a.vts", m).ok());
    }
    // An uncommitted rename is an orphan and cannot enter the acknowledged catalog.
    VT_ASSERT(segment("repair-catalog/orphan.vts", fixture()).ok());
    {
        Journal j;
        VT_ASSERT(j.open("repair-catalog/journal.wal").ok());
        VT_ASSERT(j.append_begin(50).ok());
        SegmentMeta m;
        m.segment_id = 50;
        m.filename = "orphan.vts";
        m.row_count = 8;
        VT_ASSERT(j.append_add_segment(50, m).ok());
    }
    {
        Catalog c("repair-catalog");
        VT_ASSERT(c.open().ok());
        VT_ASSERT_EQ(c.active_segments().size(), 1ULL);
        VT_ASSERT(segment("repair-catalog/.tmp_b.vts", fixture()).ok());
        SegmentMeta m;
        VT_ASSERT(c.commit_segment("repair-catalog/.tmp_b.vts", m).ok());
        VT_ASSERT_EQ(c.active_segments().size(), 2ULL);
    }
    Catalog c("repair-catalog");
    VT_ASSERT(c.open().ok());
    usize delivered = 0;
    ReplayEngine replay;
    VT_ASSERT(replay.replay_catalog(c, [&](auto &) { ++delivered; }).ok());
    VT_ASSERT_EQ(delivered, 16ULL);
    VT_ASSERT_EQ(replay.stats().total_events, 16ULL);
    auto s = c.active_segments();
    SegmentReader r;
    VT_ASSERT(r.open("repair-catalog/" + s.back().filename).ok());
    std::vector<CanonicalEvent> e;
    VT_ASSERT(r.read_all_events(e).ok());
    query::Parser p("SELECT COUNT(*) FROM events");
    auto q = p.parse_query();
    VT_ASSERT(q.ok());
    auto out = execute_query(e, q.value().get(), QueryEngine::Native);
    VT_ASSERT(out.ok());
    VT_ASSERT_EQ(out.value().rows[0][0], "8");
    std::filesystem::remove("repair-catalog/" + s.front().filename);
    VT_ASSERT(!c.open().ok());
}
VT_TEST(repair_regression_tests, replay_sequence_boundaries_and_exact_delivery) {
    std::vector<CanonicalEvent> e(6);
    u64 seq[] = {UINT64_MAX - 1, UINT64_MAX, 0, 0, 3, 2};
    for (usize i = 0; i < 6; ++i) {
        e[i].sequence = seq[i];
        e[i].exchange_ts_ns = 90 - i;
        e[i].receive_ts_ns = 100 + i;
    }
    std::vector<CanonicalEvent> delivered;
    ReplayEngine r;
    VT_ASSERT(r.replay_events(e, [&](auto &v) { delivered.push_back(v); }).ok());
    VT_ASSERT_EQ(delivered.size(), 6ULL);
    VT_ASSERT_EQ(r.stats().total_events, 6ULL);
    VT_ASSERT_EQ(r.stats().out_of_order_events, 3ULL);
    VT_ASSERT_EQ(r.stats().sequence_breaks, 1ULL);
    for (usize i = 0; i < 6; ++i) {
        VT_ASSERT_EQ(delivered[i].sequence, seq[i]);
        VT_ASSERT_EQ(delivered[i].exchange_ts_ns, e[i].exchange_ts_ns);
        VT_ASSERT_EQ(delivered[i].receive_ts_ns, e[i].receive_ts_ns);
    }
    r.reset_stats();
    delivered.clear();
    VT_ASSERT(r.replay_events({}, [&](auto &v) { delivered.push_back(v); }).ok());
    VT_ASSERT_EQ(r.stats().total_events, 0ULL);
    VT_ASSERT(delivered.empty());
}
VT_TEST(repair_regression_tests, vtp1_truncation_checksum_and_excessive_length) {
    auto e = fixture()[2];
    u8 bytes[256];
    vtp1::Encoder encoder;
    auto encoded = encoder.encode_event(e, bytes, sizeof(bytes), 1);
    VT_ASSERT(encoded.ok());
    vtp1::Decoder decoder;
    CanonicalEvent out{};
    for (usize n = 0; n < encoded.value(); ++n)
        VT_ASSERT(!decoder.decode_frame(bytes, n, out).ok());
    bytes[encoded.value() - 1] ^= 1;
    VT_ASSERT(!decoder.decode_frame(bytes, encoded.value(), out).ok());
    bytes[encoded.value() - 1] ^= 1;
    write_be_u32(bytes + 8, UINT32_MAX);
    VT_ASSERT(!decoder.decode_frame(bytes, encoded.value(), out).ok());
}
VT_TEST(repair_regression_tests, native_rejects_unsupported_and_undefined_ir) {
    ir::Builder b;
    auto f = b.create_function("missing");
    b.create_return(9999);
    for (auto arch : {jit::TargetArch::X86_64, jit::TargetArch::AArch64})
        VT_ASSERT(!jit::create_generator(arch)->generate(f).ok());
    ir::Builder c;
    f = c.create_function("unsupported");
    auto value = f->create_value(ir::Type::U64);
    f->entry_block()->append(std::make_unique<ir::Instruction>(
        ir::Opcode::LoadSelection, std::vector<ir::ValueId>{}, value, ir::Type::U64));
    c.create_return(value);
    for (auto arch : {jit::TargetArch::X86_64, jit::TargetArch::AArch64})
        VT_ASSERT(!jit::create_generator(arch)->generate(f).ok());
}
VT_TEST(repair_regression_tests, native_lifetime_moves_eviction_and_failed_compile) {
    CanonicalEvent event{};
    for (u64 i = 0; i < 300; ++i) {
        ir::Builder b;
        auto f = b.create_function("lifetime");
        auto v = b.create_const_u64(i);
        b.create_return(v);
        jit::JitCompiler compiler;
        auto compiled = compiler.compile(f);
        VT_ASSERT(compiled.ok());
        jit::JitCompiler moved = std::move(compiler);
        auto evaluated = jit::invoke_native(moved.function(), &event);
        VT_ASSERT(evaluated.ok());
        VT_ASSERT_EQ(evaluated.value(), i);
    }
    jit::JitMemory memory;
    VT_ASSERT(!memory.allocate(0).ok());
    VT_ASSERT(memory.allocate(1).ok());
    u8 code = 0;
    VT_ASSERT(memory.write(&code, 1).ok());
    VT_ASSERT(memory.make_executable().ok());
    VT_ASSERT(!memory.write(&code, 1).ok());
    jit::JitMemory moved = std::move(memory);
    VT_ASSERT(memory.memory() == nullptr);
    VT_ASSERT(moved.memory() != nullptr);
    NativeCache cache;
    for (u64 i = 0; i < 300; ++i) {
        std::string sql = "SELECT " + std::to_string(i) + " FROM events";
        query::Parser p(sql);
        auto q = p.parse_query();
        VT_ASSERT(q.ok());
        auto r = execute_query({event}, q.value().get(), QueryEngine::Native, &cache);
        VT_ASSERT(r.ok());
        VT_ASSERT_EQ(r.value().rows[0][0], std::to_string(i));
        VT_ASSERT(r.value().native_invocations > 0);
        VT_ASSERT(cache.size() <= 128);
    }
}

VT_TEST(repair_regression_tests, incompressible_segment_expansion_and_manifest_limits) {
    std::mt19937_64 random(49520261008ULL);
    std::vector<CanonicalEvent> events(10000);
    for (auto &e : events) {
        e.exchange_ts_ns = random() % Limits::MaxTimestamp;
        e.receive_ts_ns = random() % Limits::MaxTimestamp;
        e.sequence = random();
        e.instrument_id = random() % Limits::MaxInstruments;
        e.event_type = static_cast<EventType>(random() % 5);
        e.side = static_cast<Side>(random() % 2);
        e.flags = static_cast<u16>(random());
        e.price_ticks = static_cast<i64>(random() % Limits::MaxPrice);
        e.quantity = random() % Limits::MaxQuantity;
        e.venue_id = random() % Limits::MaxVenues;
        e.source_id = random() % Limits::MaxSources;
        e.trade_or_order_id = random();
    }
    VT_ASSERT(segment("repair-incompressible.vts", events).ok());
    SegmentReader reader;
    VT_ASSERT(reader.open("repair-incompressible.vts").ok());
    std::vector<CanonicalEvent> decoded;
    VT_ASSERT(reader.read_all_events(decoded).ok());
    VT_ASSERT_EQ(decoded.size(), events.size());
    for (usize i = 0; i < events.size(); ++i)
        VT_ASSERT_BYTES_EQ(&events[i], &decoded[i], sizeof(CanonicalEvent));
    std::ofstream file("repair-huge.manifest", std::ios::binary);
    file.seekp(64 * 1024 * 1024);
    file.put('x');
    file.close();
    Manifest manifest;
    VT_ASSERT(!Manifest::load("repair-huge.manifest", manifest).ok());
}

VT_TEST(repair_regression_tests, native_rejects_vector_types_and_context_arithmetic) {
    ir::Builder b;
    auto f = b.create_function("unsupported_type");
    auto v = f->create_value(ir::Type::Vector);
    auto constant = ir::Constant::u64_const(1);
    constant.type = ir::Type::Vector;
    f->entry_block()->append(std::make_unique<ir::ConstOp>(constant, v));
    b.create_return(v);
    for (auto arch : {jit::TargetArch::X86_64, jit::TargetArch::AArch64})
        VT_ASSERT(!jit::create_generator(arch)->generate(f).ok());
    ir::Builder c;
    f = c.create_function("context_return");
    v = f->create_value(ir::Type::U64);
    f->add_parameter(v);
    c.create_return(v);
    for (auto arch : {jit::TargetArch::X86_64, jit::TargetArch::AArch64})
        VT_ASSERT(!jit::create_generator(arch)->generate(f).ok());
}
