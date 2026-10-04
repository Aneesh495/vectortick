#include "test_framework.hpp"
#include "vectortick/protocol/decoder.hpp"
#include "vectortick/protocol/frame.hpp"
#include "vectortick/protocol/messages.hpp"
#include "vectortick/model/event.hpp"

using namespace vectortick;
using namespace vectortick::vtp1;

VT_TEST(protocol_tests, quote_bid_and_ask_preservation) {
    // Test that bid quote preserves bid price and qty
    CanonicalEvent bid_ev{};
    bid_ev.exchange_ts_ns = 1700000000123456789ULL;
    bid_ev.receive_ts_ns = 1700000000123456999ULL;
    bid_ev.sequence = 101;
    bid_ev.instrument_id = 42;
    bid_ev.event_type = EventType::Quote;
    bid_ev.side = Side::Bid;
    bid_ev.price_ticks = 550000;
    bid_ev.quantity = 100;
    
    Encoder encoder;
    byte buffer[512];
    auto enc_res = encoder.encode_event(bid_ev, buffer, sizeof(buffer), 1);
    VT_ASSERT(enc_res.ok());
    VT_ASSERT(enc_res.value() > 0);
    
    Decoder decoder;
    CanonicalEvent decoded_bid{};
    auto dec_res = decoder.decode_frame(buffer, enc_res.value(), decoded_bid, 1700000000123456999ULL);
    VT_ASSERT(dec_res.ok());
    VT_ASSERT(decoded_bid.event_type == EventType::Quote);
    VT_ASSERT(decoded_bid.side == Side::Bid);
    VT_ASSERT(decoded_bid.price_ticks == 550000);
    VT_ASSERT(decoded_bid.quantity == 100);
    VT_ASSERT(decoded_bid.instrument_id == 42);
    VT_ASSERT(decoded_bid.sequence == 101);
    
    // Test that ask quote preserves ask price and qty (Defect #9 regression test)
    CanonicalEvent ask_ev{};
    ask_ev.exchange_ts_ns = 1700000000223456789ULL;
    ask_ev.receive_ts_ns = 1700000000223456999ULL;
    ask_ev.sequence = 102;
    ask_ev.instrument_id = 42;
    ask_ev.event_type = EventType::Quote;
    ask_ev.side = Side::Ask;
    ask_ev.price_ticks = 551000;
    ask_ev.quantity = 250;
    
    enc_res = encoder.encode_event(ask_ev, buffer, sizeof(buffer), 1);
    VT_ASSERT(enc_res.ok());
    
    CanonicalEvent decoded_ask{};
    dec_res = decoder.decode_frame(buffer, enc_res.value(), decoded_ask, 1700000000223456999ULL);
    VT_ASSERT(dec_res.ok());
    VT_ASSERT(decoded_ask.event_type == EventType::Quote);
    VT_ASSERT(decoded_ask.side == Side::Ask);
    VT_ASSERT(decoded_ask.price_ticks == 551000);
    VT_ASSERT(decoded_ask.quantity == 250);
}

VT_TEST(protocol_tests, trade_roundtrip) {
    CanonicalEvent trade_ev{};
    trade_ev.exchange_ts_ns = 1700000000500000000ULL;
    trade_ev.receive_ts_ns = 1700000000500000100ULL;
    trade_ev.sequence = 200;
    trade_ev.instrument_id = 99;
    trade_ev.event_type = EventType::Trade;
    trade_ev.side = Side::Ask;
    trade_ev.price_ticks = -1200; // Negative price test
    trade_ev.quantity = 50;
    trade_ev.venue_id = 7;
    trade_ev.source_id = 3;
    trade_ev.trade_or_order_id = 987654321ULL;
    
    Encoder encoder;
    byte buffer[512];
    auto enc_res = encoder.encode_event(trade_ev, buffer, sizeof(buffer), 2);
    VT_ASSERT(enc_res.ok());
    
    Decoder decoder;
    CanonicalEvent decoded{};
    auto dec_res = decoder.decode_frame(buffer, enc_res.value(), decoded, trade_ev.receive_ts_ns);
    VT_ASSERT(dec_res.ok());
    VT_ASSERT(decoded.event_type == EventType::Trade);
    VT_ASSERT(decoded.side == Side::Ask);
    VT_ASSERT(decoded.price_ticks == -1200);
    VT_ASSERT(decoded.quantity == 50);
    VT_ASSERT(decoded.venue_id == 7);
    VT_ASSERT(decoded.source_id == 3);
    VT_ASSERT(decoded.trade_or_order_id == 987654321ULL);
}

VT_TEST(protocol_tests, crc_corruption_rejected) {
    CanonicalEvent ev{};
    ev.exchange_ts_ns = 1700000000000000000ULL;
    ev.sequence = 300;
    ev.instrument_id = 1;
    ev.event_type = EventType::Trade;
    ev.side = Side::Bid;
    ev.price_ticks = 100;
    ev.quantity = 10;
    
    Encoder encoder;
    byte buffer[512];
    auto enc_res = encoder.encode_event(ev, buffer, sizeof(buffer), 1);
    VT_ASSERT(enc_res.ok());
    
    // Corrupt one payload byte
    buffer[FrameHeader::Size + 2] ^= 0xFF;
    
    Decoder decoder;
    CanonicalEvent decoded{};
    auto dec_res = decoder.decode_frame(buffer, enc_res.value(), decoded);
    VT_ASSERT(!dec_res.ok());
    VT_ASSERT(dec_res.status().code() == StatusCode::InvalidChecksum);
}

VT_TEST(protocol_tests, sequence_non_monotonic_rejected) {
    CanonicalEvent ev1{};
    ev1.exchange_ts_ns = 1700000000000000000ULL;
    ev1.sequence = 500;
    ev1.instrument_id = 1;
    ev1.event_type = EventType::Trade;
    ev1.side = Side::Bid;
    ev1.price_ticks = 100;
    ev1.quantity = 10;
    
    Encoder encoder;
    byte buf1[512];
    auto enc1 = encoder.encode_event(ev1, buf1, sizeof(buf1), 1);
    VT_ASSERT(enc1.ok());
    
    Decoder decoder;
    CanonicalEvent dec1{};
    auto res1 = decoder.decode_frame(buf1, enc1.value(), dec1);
    VT_ASSERT(res1.ok());
    
    // Second event with sequence 400 <= 500
    CanonicalEvent ev2 = ev1;
    ev2.sequence = 400;
    byte buf2[512];
    auto enc2 = encoder.encode_event(ev2, buf2, sizeof(buf2), 1);
    VT_ASSERT(enc2.ok());
    
    CanonicalEvent dec2{};
    auto res2 = decoder.decode_frame(buf2, enc2.value(), dec2);
    VT_ASSERT(!res2.ok());
    VT_ASSERT(res2.status().code() == StatusCode::SequenceNotMonotonic);
}
