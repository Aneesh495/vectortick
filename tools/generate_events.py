#!/usr/bin/env python3
import argparse
import os
import struct
import sys

def generate_events(count, output_path):
    if count < 0:
        raise ValueError("event count must be non-negative")
    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    is_pcap = output_path.endswith(".pcap")
    
    with open(output_path, "wb") as f:
        if is_pcap:
            # PCAP Global Header (24 bytes)
            # magic (0xa1b2c3d4), v2.4, thiszone=0, sigfigs=0, snaplen=65535, network=1 (Ethernet)
            f.write(struct.pack("<IHHiIII", 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1))

        base_ts_ns = 1704067200000000000
        batch_size = 50000
        generated = 0

        while generated < count:
            batch_count = min(batch_size, count - generated)
            chunks = []
            for i in range(batch_count):
                idx = generated + i
                seq = idx + 1
                ex_ts = base_ts_ns + idx * 1000
                rcv_ts = ex_ts + 50
                trade_id = 1000000 + idx
                price = 10000 + (idx % 1000) * 10
                inst_id = 1001 + (idx % 10)
                qty = 10 + (idx % 100)
                venue = 1
                source = 1
                event_type = 2 if (idx % 3 == 0) else 1 # trade or quote
                side = 0 if (idx % 2 == 0) else 1
                flags = 0
                reserved = 0

                if is_pcap:
                    # Synthetic Ethernet (14) + IP (20) + UDP (8) + VTP1 Frame (40) + Trade/Quote Payload
                    # For PCAP we write valid PCAP packet record
                    # Ethernet: dst(6), src(6), type(2) = 0x0800
                    eth = b"\x00\x11\x22\x33\x44\x55\x66\x77\x88\x99\xaa\xbb\x08\x00"
                    # IP: v4, ihl=5, total_len, udp proto=17
                    # UDP: sport, dport, len, csum
                    # VTP1 Frame Header:
                    magic = b"VTP1"
                    version = 1
                    msg_type = event_type
                    p_flags = 0
                    if msg_type == 2: # Trade payload: 32 bytes
                        payload = struct.pack(">QIIqIBBBB", ex_ts, inst_id, side, price, qty, 0, 0, 0, 0)
                    else: # Quote payload: 40 bytes
                        payload = struct.pack(">QIB3sqIqI", ex_ts, inst_id, side, b"\x00\x00\x00", price, qty, price + 5, qty + 2)
                    payload_len = len(payload)
                    frame_hdr = struct.pack(">4sBBHIIQQII", magic, version, msg_type, p_flags, payload_len, 1, seq, ex_ts, 0, 0)
                    udp_payload = frame_hdr + payload
                    udp_hdr = struct.pack(">HHHH", 12345, 12345, 8 + len(udp_payload), 0)
                    ip_len = 20 + len(udp_hdr) + len(udp_payload)
                    ip_hdr = struct.pack(">BBHHHBBHII", 0x45, 0, ip_len, 0, 0, 64, 17, 0, 0x7f000001, 0x7f000001)
                    packet_data = eth + ip_hdr + udp_hdr + udp_payload
                    
                    # PCAP Record Header (16 bytes)
                    # ts_sec, ts_usec, incl_len, orig_len
                    ts_sec = int(ex_ts // 1000000000)
                    ts_usec = int((ex_ts % 1000000000) // 1000)
                    pcap_rec = struct.pack("<IIII", ts_sec, ts_usec, len(packet_data), len(packet_data))
                    chunks.append(pcap_rec + packet_data)
                else:
                    event_data = struct.pack("<QQQQqIIHHBBBB", seq, ex_ts, rcv_ts, trade_id, price, inst_id, qty, venue, source, event_type, side, flags, reserved)
                    chunks.append(event_data)

            f.write(b"".join(chunks))
            generated += batch_count
            if generated % 1000000 == 0 or generated == count:
                print(f"Generated {generated}/{count} events ({(generated/count)*100:.1f}%)")

def main():
    parser = argparse.ArgumentParser(description="VectorTick Synthetic Event Generator")
    parser.add_argument("--count", type=int, default=10000, help="Number of events to generate")
    parser.add_argument("--output", type=str, required=True, help="Output path (.bin or .pcap)")
    args = parser.parse_args()
    if args.count < 0:
        parser.error("--count must be non-negative")
    generate_events(args.count, args.output)

if __name__ == "__main__":
    main()
