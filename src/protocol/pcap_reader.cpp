#include "vectortick/protocol/pcap_reader.hpp"
#include <cstring>

namespace vectortick {
namespace pcap {

Status PcapReader::open(const std::string &path) noexcept {
    close();
    reset();
    auto result = MappedFile::open_read(path);
    if (!result.ok()) {
        return result.status();
    }

    mapping_ = std::move(result.value());
    offset_ = 0;

    auto st = parse_global_header();
    if (!st.ok())
        close();
    return st;
}

Status PcapReader::parse_global_header() noexcept {
    if (mapping_.size() < GlobalHeader::Size) {
        return Status(StatusCode::PcapInvalidHeader, "File too small for PCAP header");
    }

    const byte *data = mapping_.data();

    // Read magic number to determine endianness
    u32 magic = read_le_u32(data);

    switch (magic) {
    case GlobalHeader::MagicLE:
        is_big_endian_ = false;
        is_nanosecond_ts_ = false;
        break;
    case GlobalHeader::MagicLE_NS:
        is_big_endian_ = false;
        is_nanosecond_ts_ = true;
        break;
    case GlobalHeader::MagicBE:
        is_big_endian_ = true;
        is_nanosecond_ts_ = false;
        break;
    case GlobalHeader::MagicBE_NS:
        is_big_endian_ = true;
        is_nanosecond_ts_ = true;
        break;
    default:
        return Status(StatusCode::PcapInvalidHeader, "Invalid PCAP magic number");
    }

    u16 version_major = is_big_endian_ ? read_be_u16(data + 4) : read_le_u16(data + 4);
    // version_minor is also at offset 6, but we only check major version

    // Check version (2.4 is most common)
    if (version_major != 2) {
        return Status(StatusCode::PcapInvalidHeader, "Unsupported PCAP version");
    }

    snaplen_ = is_big_endian_ ? read_be_u32(data + 16) : read_le_u32(data + 16);
    u32 network = is_big_endian_ ? read_be_u32(data + 20) : read_le_u32(data + 20);

    if (snaplen_ == 0 || snaplen_ > 16 * 1024 * 1024)
        return Status(StatusCode::PcapInvalidHeader, "Invalid PCAP snaplen");
    link_type_ = static_cast<LinkType>(network);

    // Only support Ethernet for now
    if (link_type_ != LinkType::Ethernet && link_type_ != LinkType::Null &&
        link_type_ != LinkType::Loopback) {
        return Status(StatusCode::PcapUnsupportedLinkType, "Unsupported link type");
    }

    offset_ = GlobalHeader::Size;
    return Status::OK();
}

Result<usize> PcapReader::read_next() noexcept {
    if (!mapping_.is_open()) {
        return make_error<usize>(StatusCode::FileNotFound, "No file open");
    }

    // Check if at EOF
    if (offset_ >= mapping_.size()) {
        return 0; // EOF
    }

    // Check if enough bytes for packet header
    if (offset_ + PacketHeader::Size > mapping_.size()) {
        return make_error<usize>(StatusCode::PcapTruncatedRecord, "Truncated packet header");
    }

    const byte *data = mapping_.data() + offset_;

    // Read packet header
    PacketHeader pkt_hdr;
    pkt_hdr.ts_sec = is_big_endian_ ? read_be_u32(data) : read_le_u32(data);
    pkt_hdr.ts_usec = is_big_endian_ ? read_be_u32(data + 4) : read_le_u32(data + 4);
    pkt_hdr.incl_len = is_big_endian_ ? read_be_u32(data + 8) : read_le_u32(data + 8);
    pkt_hdr.orig_len = is_big_endian_ ? read_be_u32(data + 12) : read_le_u32(data + 12);

    // Validate lengths
    if (pkt_hdr.incl_len > pkt_hdr.orig_len ||
        pkt_hdr.ts_usec >= (is_nanosecond_ts_ ? 1000000000U : 1000000U) ||
        pkt_hdr.incl_len > snaplen_ ||
        pkt_hdr.incl_len > mapping_.size() - offset_ - PacketHeader::Size) {
        return make_error<usize>(StatusCode::PcapTruncatedRecord,
                                 "Packet length exceeds snaplen or file size");
    }

    if (pkt_hdr.incl_len == 0) {
        return make_error<usize>(StatusCode::PcapTruncatedRecord, "Zero-length packet");
    }

    // Convert timestamp to nanoseconds
    packet_ts_ns_ = static_cast<u64>(pkt_hdr.ts_sec) * 1000000000ULL;
    if (is_nanosecond_ts_) {
        packet_ts_ns_ += pkt_hdr.ts_usec;
    } else {
        packet_ts_ns_ += static_cast<u64>(pkt_hdr.ts_usec) * 1000ULL;
    }

    packet_size_ = pkt_hdr.incl_len;
    packet_data_ = data + PacketHeader::Size;

    // Reset VTP1 payload
    vtp1_payload_ = nullptr;
    vtp1_payload_size_ = 0;

    // Parse packet layers
    Status parse_status = parse_packet();
    if (!parse_status.ok()) {
        parse_errors_++;
        vtp1_payload_ = nullptr;
        vtp1_payload_size_ = 0;
        return make_error<usize>(parse_status.code(), parse_status.message());
    }

    usize consumed = PacketHeader::Size + pkt_hdr.incl_len;
    offset_ += consumed;
    packets_read_++;

    return consumed;
}

Status PcapReader::parse_packet() noexcept {
    usize offset = 0;
    const byte *data = packet_data_;
    usize size = packet_size_;

    // Handle null/loopback link type
    if (link_type_ == LinkType::Null || link_type_ == LinkType::Loopback) {
        // Skip 4-byte null header
        if (size < 4) {
            return Status(StatusCode::PcapTruncatedRecord, "Truncated null/loopback header");
        }
        offset = 4;
    } else {
        // Parse Ethernet header
        auto status = parse_ethernet(data, size, offset);
        if (!status.ok())
            return status;
        if (offset == 0)
            return Status::OK();
    }

    // Parse IPv4
    auto status = parse_ipv4(data, size, offset);
    if (!status.ok())
        return status;
    if (offset == size)
        return Status::OK();
    size = packet_size_;

    // Parse UDP
    status = parse_udp(data, size, offset);
    if (!status.ok())
        return status;

    // Parse VTP1
    return parse_vtp1(data + offset, vtp1_payload_size_);
}

Status PcapReader::parse_ethernet(const byte *data, usize size, usize &offset) noexcept {
    if (offset + Ethernet::HeaderSize > size) {
        return Status(StatusCode::PcapTruncatedRecord, "Truncated Ethernet header");
    }

    // Check for VLAN tags
    u16 ether_type = read_be_u16(data + offset + 12);

    // Handle 802.1Q VLAN tags
    if (ether_type == Ethernet::EtherTypeVLAN || ether_type == Ethernet::EtherTypeVLANOuter) {
        // Skip VLAN tag (4 bytes) and check for double-tagged
        offset += 4;

        if (offset + Ethernet::HeaderSize > size) {
            return Status(StatusCode::PcapTruncatedRecord, "Truncated VLAN header");
        }

        ether_type = read_be_u16(data + offset + 12);

        // Check for inner VLAN tag
        if (ether_type == Ethernet::EtherTypeVLAN) {
            offset += 4;

            if (offset + Ethernet::HeaderSize > size) {
                return Status(StatusCode::PcapTruncatedRecord, "Truncated inner VLAN header");
            }

            ether_type = read_be_u16(data + offset + 12);
        }
    }

    if (ether_type != Ethernet::EtherTypeIPv4) {
        // Not IPv4, skip
        return Status(StatusCode::OK);
    }

    offset += Ethernet::HeaderSize;
    return Status::OK();
}

Status PcapReader::parse_ipv4(const byte *data, usize size, usize &offset) noexcept {
    if (offset + IPv4Header::MinSize > size) {
        return Status(StatusCode::PcapInvalidIPv4Header, "Truncated IPv4 header");
    }

    IPv4Header ip_header;
    std::memcpy(&ip_header, data + offset, sizeof(ip_header));
    const IPv4Header *ip = &ip_header;

    // Check version
    if (ip->version() != 4) {
        return Status(StatusCode::PcapInvalidIPv4Header, "Not IPv4");
    }

    u8 ihl = ip->ihl();
    if (ihl < IPv4Header::MinSize) {
        return Status(StatusCode::PcapInvalidIPv4Header, "Invalid IPv4 IHL");
    }

    if (offset + ihl > size) {
        return Status(StatusCode::PcapInvalidIPv4Header, "Truncated IPv4 options");
    }

    // Check if fragmented
    if (ip->is_fragmented()) {
        return Status(StatusCode::PcapFragmentedPacket, "Fragmented IPv4 packet");
    }

    // Check protocol
    if (ip->protocol != IPv4Header::ProtocolUDP) {
        // Not UDP, skip
        offset = size;
        return Status::OK();
    }

    // Validate total length
    u16 total_len = read_be_u16(reinterpret_cast<const byte *>(&ip->total_length));
    if (total_len < ihl + UDPHeader::Size || total_len > size - offset) {
        return Status(StatusCode::PcapInvalidIPv4Header, "IPv4 total length too small");
    }

    packet_size_ = offset + total_len;
    offset += ihl;
    return Status::OK();
}

Status PcapReader::parse_udp(const byte *data, usize size, usize &offset) noexcept {
    if (offset + UDPHeader::Size > size) {
        return Status(StatusCode::PcapInvalidUDPHeader, "Truncated UDP header");
    }

    UDPHeader udp_header;
    std::memcpy(&udp_header, data + offset, sizeof(udp_header));
    const UDPHeader *udp = &udp_header;

    u16 udp_len = read_be_u16(reinterpret_cast<const byte *>(&udp->length));
    if (udp_len < UDPHeader::Size) {
        return Status(StatusCode::PcapInvalidUDPHeader, "Invalid UDP length");
    }

    if (offset + udp_len > size) {
        return Status(StatusCode::PcapInvalidUDPHeader, "Truncated UDP payload");
    }

    offset += UDPHeader::Size;
    vtp1_payload_size_ = udp_len - UDPHeader::Size;

    return Status::OK();
}

Status PcapReader::parse_vtp1(const byte *data, usize size) noexcept {
    if (size < 4 || read_be_u32(data) != Magic::VTP1) {
        vtp1_payload_size_ = 0;
        return Status::OK();
    }
    if (size < 40)
        return Status(StatusCode::TruncatedFrame, "Truncated VTP1 payload");
    vtp1_payload_ = data;
    vtp1_payload_size_ = size;
    vtp1_frames_found_++;

    return Status::OK();
}

} // namespace pcap
} // namespace vectortick
