// VectorTick Ingest Application
// Ingests market data from VTP1 PCAP files

#include "vectortick/common/types.hpp"
#include "vectortick/protocol/pcap_reader.hpp"
#include "vectortick/protocol/decoder.hpp"
#include "vectortick/storage/segment_writer.hpp"

#include <iostream>
#include <fstream>
#include <cstring>

using namespace vectortick;

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options] <input.pcap>\n";
    std::cerr << "Options:\n";
    std::cerr << "  -o <output.vts>   Output segment file\n";
    std::cerr << "  -v                Verbose output\n";
    std::cerr << "  -h                Show this help\n";
}

int main(int argc, char* argv[]) {
    std::string input_file;
    std::string output_file = "output.vts";
    bool verbose = false;
    
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            input_file = argv[i];
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }
    
    if (input_file.empty()) {
        std::cerr << "Error: No input file specified\n";
        print_usage(argv[0]);
        return 1;
    }
    
    if (verbose) {
        std::cout << "Input: " << input_file << "\n";
        std::cout << "Output: " << output_file << "\n";
    }
    
    // Open PCAP reader
    pcap::PcapReader reader;
    auto status = reader.open(input_file);
    if (!status.ok()) {
        std::cerr << "Error opening PCAP: " << status.message() << "\n";
        return 1;
    }
    
    if (verbose) {
        std::cout << "PCAP opened successfully\n";
    }
    
    vtp1::Decoder decoder;
    SegmentWriter writer(1);
    
    // Process packets
    u64 packet_count = 0;
    u64 byte_count = 0;
    u64 event_count = 0;
    
    while (reader.is_open()) {
        auto result = reader.read_next();
        if (!result.ok()) {
            std::cerr << "Error reading packet: " << result.status().message() << "\n";
            return 1;
        }
        
        if (result.value() == 0) {
            // EOF
            break;
        }
        
        ++packet_count;
        byte_count += result.value();
        
        const byte* vtp_data = reader.vtp1_payload();
        usize vtp_size = reader.vtp1_payload_size();
        if (vtp_data != nullptr && vtp_size >= vtp1::FrameHeader::Size) {
            usize offset = 0;
            while (offset < vtp_size) {
                CanonicalEvent event{};
                auto dec_res = decoder.decode_frame(vtp_data + offset, vtp_size - offset, event, reader.packet_timestamp_ns());
                if (!dec_res.ok()) {
                    std::cerr << "Decode error at packet " << packet_count << ": " << dec_res.status().message() << "\n";
                    return 1;
                }
                offset += dec_res.value();
                
                // If it was a market event
                if (event.event_type == EventType::Quote ||
                    event.event_type == EventType::Trade ||
                    event.event_type == EventType::BookDelta ||
                    event.event_type == EventType::Status ||
                    event.event_type == EventType::Heartbeat) {
                    auto add_st = writer.add_event(event);
                    if (!add_st.ok()) {
                        std::cerr << "Error adding event to segment: " << add_st.message() << "\n";
                        return 1;
                    }
                    ++event_count;
                }
            }
        }
        
        if (verbose && packet_count % 10000 == 0) {
            std::cout << "Processed " << packet_count << " packets, " << event_count << " events\n";
        }
    }
    
    if (writer.row_count() > 0) {
        auto write_st = writer.write_to_file(output_file);
        if (!write_st.ok()) {
            std::cerr << "Error writing segment: " << write_st.message() << "\n";
            return 1;
        }
    } else {
        std::cerr << "No events found to write\n";
        return 1;
    }
    
    std::cout << "Ingestion complete:\n";
    std::cout << "  Packets: " << packet_count << "\n";
    std::cout << "  Bytes: " << byte_count << "\n";
    std::cout << "  Events: " << event_count << "\n";
    std::cout << "  Output: " << output_file << "\n";
    
    return 0;
}
