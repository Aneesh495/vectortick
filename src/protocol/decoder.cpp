#include "vectortick/protocol/decoder.hpp"

namespace vectortick {
namespace vtp1 {

Result<usize> Decoder::decode_frame(const byte *data, usize size, CanonicalEvent &event,
                                    u64 receive_ts_ns) noexcept {
    // Read frame header
    FrameHeader header;
    const byte *payload = nullptr;

    usize consumed = Frame::read_frame(data, size, header, payload);
    if (consumed == 0) {
        return make_error<usize>(StatusCode::TruncatedFrame, "Frame too small");
    }

    // Validate frame
    auto status = Frame::validate(header, payload, size);
    if (!status.ok()) {
        decode_errors_++;
        return make_error<usize>(status.code(), status.message());
    }

    // Check sequence monotonicity
    if (header.sequence <= last_sequence_ &&
        header.message_type != static_cast<u8>(MessageType::StreamMetadata) &&
        header.message_type != static_cast<u8>(MessageType::EndOfStream)) {
        decode_errors_++;
        return make_error<usize>(StatusCode::SequenceNotMonotonic, "Sequence not monotonic");
    }

    // Decode based on message type
    MessageType msg_type = static_cast<MessageType>(header.message_type);

    event.receive_ts_ns = receive_ts_ns > 0 ? receive_ts_ns : header.send_timestamp_ns;
    event.sequence = header.sequence;

    switch (msg_type) {
    case MessageType::Quote: {
        auto s = decode_quote(payload, header.payload_length, event);
        if (!s.ok()) {
            decode_errors_++;
            return make_error<usize>(s.code(), s.message());
        }
        break;
    }
    case MessageType::Trade: {
        auto s = decode_trade(payload, header.payload_length, event);
        if (!s.ok()) {
            decode_errors_++;
            return make_error<usize>(s.code(), s.message());
        }
        break;
    }
    case MessageType::BookDelta: {
        auto s = decode_book_delta(payload, header.payload_length, event);
        if (!s.ok()) {
            decode_errors_++;
            return make_error<usize>(s.code(), s.message());
        }
        break;
    }
    case MessageType::Status: {
        auto s = decode_status(payload, header.payload_length, event);
        if (!s.ok()) {
            decode_errors_++;
            return make_error<usize>(s.code(), s.message());
        }
        break;
    }
    case MessageType::Heartbeat: {
        auto s = decode_heartbeat(payload, header.payload_length, event);
        if (!s.ok()) {
            decode_errors_++;
            return make_error<usize>(s.code(), s.message());
        }
        break;
    }
    case MessageType::StreamMetadata:
    case MessageType::EndOfStream:
        event.event_type = EventType::Invalid;
        last_session_id_ = header.session_id;
        last_sequence_ = header.sequence;
        // These are control messages, not events
        frames_decoded_++;
        return consumed;
    default:
        decode_errors_++;
        return make_error<usize>(StatusCode::InvalidMessageType, "Unknown message type");
    }

    last_session_id_ = header.session_id;
    last_sequence_ = header.sequence;
    frames_decoded_++;
    return consumed;
}

Status Decoder::decode_quote(const byte *payload, u32 len, CanonicalEvent &event) noexcept {
    if (len < QuotePayload::Size) {
        return Status(StatusCode::InvalidPayload, "Quote payload too small");
    }

    QuotePayload quote;
    quote.read_from(payload);

    event.exchange_ts_ns = quote.exchange_ts_ns;
    event.instrument_id = quote.instrument_id;
    event.event_type = EventType::Quote;
    event.flags = 0;
    event.venue_id = 0;
    event.source_id = 0;
    event.trade_or_order_id = 0;

    Side s = static_cast<Side>(quote.side);
    if (s == Side::Ask) {
        event.side = Side::Ask;
        event.price_ticks = quote.ask_price_ticks;
        event.quantity = quote.ask_quantity;
    } else {
        event.side = Side::Bid;
        event.price_ticks = quote.bid_price_ticks;
        event.quantity = quote.bid_quantity;
    }

    return event::validate(event);
}

Status Decoder::decode_trade(const byte *payload, u32 len, CanonicalEvent &event) noexcept {
    if (len < TradePayload::Size) {
        return Status(StatusCode::InvalidPayload, "Trade payload too small");
    }

    TradePayload trade;
    trade.read_from(payload);

    event.exchange_ts_ns = trade.exchange_ts_ns;
    event.instrument_id = trade.instrument_id;
    event.event_type = EventType::Trade;
    event.side = static_cast<Side>(trade.side);
    event.flags = 0;
    event.price_ticks = trade.price_ticks;
    event.quantity = trade.quantity;
    event.venue_id = trade.venue_id;
    event.source_id = trade.source_id;
    event.trade_or_order_id = trade.trade_id;

    return event::validate(event);
}

Status Decoder::decode_book_delta(const byte *payload, u32 len, CanonicalEvent &event) noexcept {
    if (len < BookDeltaPayload::Size) {
        return Status(StatusCode::InvalidPayload, "BookDelta payload too small");
    }

    BookDeltaPayload delta;
    delta.read_from(payload);

    event.exchange_ts_ns = delta.exchange_ts_ns;
    event.instrument_id = delta.instrument_id;
    event.event_type = EventType::BookDelta;
    event.side = static_cast<Side>(delta.side);
    event.flags = delta.flags;
    event.price_ticks = delta.price_ticks;
    event.quantity = delta.quantity;
    event.venue_id = delta.venue_id;
    event.source_id = delta.source_id;
    event.trade_or_order_id = delta.order_id;

    return event::validate(event);
}

Status Decoder::decode_status(const byte *payload, u32 len, CanonicalEvent &event) noexcept {
    if (len < StatusPayload::Size) {
        return Status(StatusCode::InvalidPayload, "Status payload too small");
    }

    StatusPayload status;
    status.read_from(payload);

    event.exchange_ts_ns = status.exchange_ts_ns;
    event.instrument_id = status.instrument_id;
    event.event_type = EventType::Status;
    event.side = Side::Bid; // N/A
    event.flags = (static_cast<u16>(status.status) << 8) | status.reason;
    event.price_ticks = 0;
    event.quantity = 0;
    event.venue_id = 0;
    event.source_id = 0;
    event.trade_or_order_id = 0;

    return event::validate(event);
}

Status Decoder::decode_heartbeat(const byte *payload, u32 len, CanonicalEvent &event) noexcept {
    if (len < HeartbeatPayload::Size) {
        return Status(StatusCode::InvalidPayload, "Heartbeat payload too small");
    }

    HeartbeatPayload hb;
    hb.read_from(payload);

    event.exchange_ts_ns = hb.exchange_ts_ns;
    event.instrument_id = 0;
    event.event_type = EventType::Heartbeat;
    event.side = Side::Bid; // N/A
    event.flags = 0;
    event.price_ticks = 0;
    event.quantity = 0;
    event.venue_id = 0;
    event.source_id = 0;
    event.trade_or_order_id = 0;

    return Status::OK();
}

// Encoder implementation

Result<usize> Encoder::encode_event(const CanonicalEvent &event, byte *buffer, usize buffer_size,
                                    u32 session_id) noexcept {
    MessageType msg_type;
    usize payload_size = 0;

    switch (event.event_type) {
    case EventType::Quote:
        msg_type = MessageType::Quote;
        payload_size = QuotePayload::Size;
        break;
    case EventType::Trade:
        msg_type = MessageType::Trade;
        payload_size = TradePayload::Size;
        break;
    case EventType::BookDelta:
        msg_type = MessageType::BookDelta;
        payload_size = BookDeltaPayload::Size;
        break;
    case EventType::Status:
        msg_type = MessageType::Status;
        payload_size = StatusPayload::Size;
        break;
    case EventType::Heartbeat:
        msg_type = MessageType::Heartbeat;
        payload_size = HeartbeatPayload::Size;
        break;
    default:
        encode_errors_++;
        return make_error<usize>(StatusCode::InvalidMessageType, "Cannot encode event type");
    }

    usize total_size = FrameHeader::Size + payload_size;
    if (buffer_size < total_size) {
        encode_errors_++;
        return make_error<usize>(StatusCode::BufferTooSmall, "Buffer too small for frame");
    }

    // Encode payload
    byte *payload = buffer + FrameHeader::Size;

    switch (event.event_type) {
    case EventType::Quote: {
        QuotePayload quote;
        quote.exchange_ts_ns = event.exchange_ts_ns;
        quote.instrument_id = event.instrument_id;
        quote.side = static_cast<u8>(event.side);
        quote.reserved[0] = quote.reserved[1] = quote.reserved[2] = 0;
        if (event.side == Side::Ask) {
            quote.bid_price_ticks = 0;
            quote.bid_quantity = 0;
            quote.ask_price_ticks = event.price_ticks;
            quote.ask_quantity = event.quantity;
        } else {
            quote.bid_price_ticks = event.price_ticks;
            quote.bid_quantity = event.quantity;
            quote.ask_price_ticks = 0;
            quote.ask_quantity = 0;
        }
        quote.write_to(payload);
        break;
    }
    case EventType::Trade: {
        TradePayload trade;
        trade.exchange_ts_ns = event.exchange_ts_ns;
        trade.instrument_id = event.instrument_id;
        trade.side = static_cast<u8>(event.side);
        trade.reserved[0] = trade.reserved[1] = trade.reserved[2] = 0;
        trade.price_ticks = event.price_ticks;
        trade.quantity = event.quantity;
        trade.trade_id = event.trade_or_order_id;
        trade.venue_id = event.venue_id;
        trade.source_id = event.source_id;
        trade.write_to(payload);
        break;
    }
    case EventType::BookDelta: {
        BookDeltaPayload delta;
        delta.exchange_ts_ns = event.exchange_ts_ns;
        delta.instrument_id = event.instrument_id;
        delta.side = static_cast<u8>(event.side);
        delta.level = 0;
        delta.operation = 1; // modify
        delta.reserved = 0;
        delta.price_ticks = event.price_ticks;
        delta.quantity = event.quantity;
        delta.order_id = event.trade_or_order_id;
        delta.venue_id = event.venue_id;
        delta.source_id = event.source_id;
        delta.flags = event.flags;
        delta.write_to(payload);
        break;
    }
    case EventType::Status: {
        StatusPayload status;
        status.exchange_ts_ns = event.exchange_ts_ns;
        status.instrument_id = event.instrument_id;
        status.status = static_cast<u8>(event.flags >> 8);
        status.reason = static_cast<u8>(event.flags & 0xFF);
        status.reserved = 0;
        status.write_to(payload);
        break;
    }
    case EventType::Heartbeat: {
        HeartbeatPayload hb;
        hb.exchange_ts_ns = event.exchange_ts_ns;
        hb.write_to(payload);
        break;
    }
    default:
        break;
    }

    // Write frame header
    Frame::write_frame(buffer, msg_type, 0, session_id, event.sequence, event.exchange_ts_ns,
                       payload, static_cast<u32>(payload_size));

    frames_encoded_++;
    return total_size;
}

Result<usize> Encoder::encode_metadata(const StreamMetadata &metadata, byte *buffer,
                                       usize buffer_size, u32 session_id, u64 sequence) noexcept {
    usize total_size = FrameHeader::Size + StreamMetadata::Size;
    if (buffer_size < total_size) {
        return make_error<usize>(StatusCode::BufferTooSmall, "Buffer too small");
    }

    byte *payload = buffer + FrameHeader::Size;
    metadata.write_to(payload);

    Frame::write_frame(buffer, MessageType::StreamMetadata, 0, session_id, sequence,
                       metadata.start_time_ns, payload, StreamMetadata::Size);

    frames_encoded_++;
    return total_size;
}

Result<usize> Encoder::encode_end_of_stream(byte *buffer, usize buffer_size, u32 session_id,
                                            u64 sequence, u64 timestamp) noexcept {
    usize total_size = FrameHeader::Size;
    if (buffer_size < total_size) {
        return make_error<usize>(StatusCode::BufferTooSmall, "Buffer too small");
    }

    Frame::write_frame(buffer, MessageType::EndOfStream, 0, session_id, sequence, timestamp,
                       nullptr, 0);

    frames_encoded_++;
    return total_size;
}

} // namespace vtp1
} // namespace vectortick
