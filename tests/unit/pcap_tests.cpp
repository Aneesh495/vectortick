#include "test_framework.hpp"
#include "vectortick/protocol/pcap_reader.hpp"
#include "vectortick/protocol/decoder.hpp"
#include "vectortick/protocol/messages.hpp"
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
using namespace vectortick::pcap;

namespace {

void write_u16_le(byte* dst, u16 v) {
    dst[0] = static_cast<byte>(v & 0xFF);
    dst[1] = static_cast<byte>((v >> 8) & 0xFF);
}

void write_u32_le(byte* dst, u32 v) {
    dst[0] = static_cast<byte>(v & 0xFF);
    dst[1] = static_cast<byte>((v >> 8) & 0xFF);
    dst[2] = static_cast<byte>((v >> 16) & 0xFF);
    dst[3] = static_cast<byte>((v >> 24) & 0xFF);
}

void write_u16_be(byte* dst, u16 v) {
    dst[0] = static_cast<byte>((v >> 8) & 0xFF);
    dst[1] = static_cast<byte>(v & 0xFF);
}

void write_u32_be(byte* dst, u32 v) {
    dst[0] = static_cast<byte>((v >> 24) & 0xFF);
    dst[1] = static_cast<byte>((v >> 16) & 0xFF);
    dst[2] = static_cast<byte>((v >> 8) & 0xFF);
    dst[3] = static_cast<byte>(v & 0xFF);
}

std::vector<byte> make_synthetic_pcap(const std::vector<std::vector<byte>>& payloads, bool with_vlan = false) {
    std::vector<byte> pcap;
    // Global Header (24 bytes)
    pcap.resize(GlobalHeader::Size);
    write_u32_le(pcap.data(), GlobalHeader::MagicLE);
    write_u16_le(pcap.data() + 4, 2); // major 2
    write_u16_le(pcap.data() + 6, 4); // minor 4
    write_u32_le(pcap.data() + 8, 0); // thiszone
    write_u32_le(pcap.data() + 12, 0); // sigfigs
    write_u32_le(pcap.data() + 16, 65535); // snaplen
    write_u32_le(pcap.data() + 20, 1); // linktype Ethernet

    for (usize idx = 0; idx < payloads.size(); ++idx) {
        const auto& payload = payloads[idx];
        
        // Ethernet header (14 bytes or 18 with VLAN)
        std::vector<byte> eth;
        eth.resize(with_vlan ? 18 : 14, 0);
        if (with_vlan) {
            write_u16_be(eth.data() + 12, Ethernet::EtherTypeVLAN);
            write_u16_be(eth.data() + 14, 100); // VLAN tag 100
            write_u16_be(eth.data() + 16, Ethernet::EtherTypeIPv4);
        } else {
            write_u16_be(eth.data() + 12, Ethernet::EtherTypeIPv4);
        }
        
        // IPv4 header (20 bytes)
        std::vector<byte> ip;
        ip.resize(20, 0);
        ip[0] = 0x45; // Version 4, IHL 5
        u16 total_ip_len = static_cast<u16>(20 + 8 + payload.size());
        write_u16_be(ip.data() + 2, total_ip_len);
        write_u16_be(ip.data() + 6, 0x4000); // DF
        ip[8] = 64; // TTL
        ip[9] = 17; // Protocol UDP
        write_u32_be(ip.data() + 12, 0x7F000001); // 127.0.0.1
        write_u32_be(ip.data() + 16, 0x7F000001);
        
        // UDP header (8 bytes)
        std::vector<byte> udp;
        udp.resize(8, 0);
        write_u16_be(udp.data() + 0, 12345);
        write_u16_be(udp.data() + 2, 54321);
        write_u16_be(udp.data() + 4, static_cast<u16>(8 + payload.size()));
        
        // Packet data
        std::vector<byte> pkt_data;
        pkt_data.insert(pkt_data.end(), eth.begin(), eth.end());
        pkt_data.insert(pkt_data.end(), ip.begin(), ip.end());
        pkt_data.insert(pkt_data.end(), udp.begin(), udp.end());
        pkt_data.insert(pkt_data.end(), payload.begin(), payload.end());
        
        // Packet Header (16 bytes)
        usize pkt_hdr_offset = pcap.size();
        pcap.resize(pkt_hdr_offset + PacketHeader::Size);
        write_u32_le(pcap.data() + pkt_hdr_offset, 1700000000 + static_cast<u32>(idx));
        write_u32_le(pcap.data() + pkt_hdr_offset + 4, 100000 * static_cast<u32>(idx)); // usec
        write_u32_le(pcap.data() + pkt_hdr_offset + 8, static_cast<u32>(pkt_data.size()));
        write_u32_le(pcap.data() + pkt_hdr_offset + 12, static_cast<u32>(pkt_data.size()));
        
        // Append pkt data
        pcap.insert(pcap.end(), pkt_data.begin(), pkt_data.end());
    }
    
    return pcap;
}

} // namespace

VT_TEST(pcap_tests, parse_synthetic_pcap_packets) {
    // Create 2 VTP1 frames
    vtp1::Encoder encoder;
    
    CanonicalEvent ev1{};
    ev1.exchange_ts_ns = 1700000000100000000ULL;
    ev1.sequence = 1;
    ev1.instrument_id = 10;
    ev1.event_type = EventType::Trade;
    ev1.price_ticks = 42000;
    ev1.quantity = 5;
    
    byte buf1[256];
    auto enc1 = encoder.encode_event(ev1, buf1, sizeof(buf1), 1);
    VT_ASSERT(enc1.ok());
    std::vector<byte> f1(buf1, buf1 + enc1.value());
    
    CanonicalEvent ev2{};
    ev2.exchange_ts_ns = 1700000000200000000ULL;
    ev2.sequence = 2;
    ev2.instrument_id = 10;
    ev2.event_type = EventType::Quote;
    ev2.side = Side::Ask;
    ev2.price_ticks = 42100;
    ev2.quantity = 15;
    
    byte buf2[256];
    auto enc2 = encoder.encode_event(ev2, buf2, sizeof(buf2), 1);
    VT_ASSERT(enc2.ok());
    std::vector<byte> f2(buf2, buf2 + enc2.value());
    
    auto pcap_bytes = make_synthetic_pcap({f1, f2}, false);
    
    std::string tmp_path = "/tmp/test_synth_" + std::to_string(getpid()) + ".pcap";
    FILE* fp = fopen(tmp_path.c_str(), "wb");
    VT_ASSERT(fp != nullptr);
    fwrite(pcap_bytes.data(), 1, pcap_bytes.size(), fp);
    fclose(fp);
    
    PcapReader reader;
    auto st = reader.open(tmp_path);
    VT_ASSERT(st.ok());
    
    // Packet 1
    auto r1 = reader.read_next();
    VT_ASSERT(r1.ok());
    VT_ASSERT(r1.value() > 0);
    VT_ASSERT(reader.vtp1_payload() != nullptr);
    VT_ASSERT(reader.vtp1_payload_size() == f1.size());
    VT_ASSERT(std::memcmp(reader.vtp1_payload(), f1.data(), f1.size()) == 0);
    
    // Packet 2
    auto r2 = reader.read_next();
    VT_ASSERT(r2.ok());
    VT_ASSERT(r2.value() > 0);
    VT_ASSERT(reader.vtp1_payload() != nullptr);
    VT_ASSERT(reader.vtp1_payload_size() == f2.size());
    VT_ASSERT(std::memcmp(reader.vtp1_payload(), f2.data(), f2.size()) == 0);
    
    // Packet 3 (EOF)
    auto r3 = reader.read_next();
    VT_ASSERT(r3.ok());
    VT_ASSERT(r3.value() == 0);
    
    VT_ASSERT(reader.packets_read() == 2);
    VT_ASSERT(reader.vtp1_frames_found() == 2);
    
    remove(tmp_path.c_str());
}

VT_TEST(pcap_tests, parse_vlan_tagged_packet) {
    vtp1::Encoder encoder;
    CanonicalEvent ev{};
    ev.exchange_ts_ns = 1700000000300000000ULL;
    ev.sequence = 10;
    ev.instrument_id = 999;
    ev.event_type = EventType::Trade;
    ev.price_ticks = 1000;
    ev.quantity = 1;
    
    byte buf[256];
    auto enc = encoder.encode_event(ev, buf, sizeof(buf), 1);
    VT_ASSERT(enc.ok());
    std::vector<byte> f(buf, buf + enc.value());
    
    auto pcap_bytes = make_synthetic_pcap({f}, true); // with VLAN tag
    
    std::string tmp_path = "/tmp/test_vlan_" + std::to_string(getpid()) + ".pcap";
    FILE* fp = fopen(tmp_path.c_str(), "wb");
    VT_ASSERT(fp != nullptr);
    fwrite(pcap_bytes.data(), 1, pcap_bytes.size(), fp);
    fclose(fp);
    
    PcapReader reader;
    auto st = reader.open(tmp_path);
    VT_ASSERT(st.ok());
    
    auto r = reader.read_next();
    VT_ASSERT(r.ok());
    VT_ASSERT(r.value() > 0);
    VT_ASSERT(reader.vtp1_payload_size() == f.size());
    
    vtp1::Decoder decoder;
    CanonicalEvent decoded{};
    auto dec_st = decoder.decode_frame(reader.vtp1_payload(), reader.vtp1_payload_size(), decoded);
    VT_ASSERT(dec_st.ok());
    VT_ASSERT(decoded.instrument_id == 999);
    VT_ASSERT(decoded.price_ticks == 1000);
    
    remove(tmp_path.c_str());
}
