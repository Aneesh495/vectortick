#pragma once
#include "bitpack.hpp"

#include "../common/types.hpp"
#include "../common/status.hpp"

namespace vectortick {

namespace codec {

// Variable-length integer encoding
// Uses continuation bits for efficient encoding of small values

// Unsigned varint encoding
// Each byte uses 7 bits for data, 1 bit as continuation flag
class VarIntU {
public:
    // Maximum encoded size for u64
    static constexpr usize MaxSize = 10;
    
    // Encode unsigned value
    // Returns number of bytes written
    [[nodiscard]] static usize encode(u64 value, byte* output, usize output_size) noexcept {
        usize written = 0;
        
        do {
            if (written >= output_size) return 0;
            
            byte b = static_cast<byte>(value & 0x7F);
            value >>= 7;
            
            if (value != 0) {
                b |= 0x80;  // Continuation bit
            }
            
            output[written++] = b;
        } while (value != 0);
        
        return written;
    }
    
    // Decode unsigned value
    // Returns number of bytes consumed, or 0 on error
    [[nodiscard]] static usize decode(const byte* input, usize input_size, u64& value) noexcept {
        value = 0;
        usize shift = 0;
        usize consumed = 0;
        
        for (usize i = 0; i < MaxSize && i < input_size; ++i) {
            byte b = input[i];
            if (i == 9 && (b & 0xFE)) return 0;
            ++consumed;
            
            value |= static_cast<u64>(b & 0x7F) << shift;
            shift += 7;
            
            if ((b & 0x80) == 0) {
                return consumed;
            }
        }
        
        return 0;  // Too long or no terminator
    }
};

// Signed varint using zigzag encoding
class VarIntI {
public:
    // Maximum encoded size for i64
    static constexpr usize MaxSize = 10;
    
    // Encode signed value
    [[nodiscard]] static usize encode(i64 value, byte* output, usize output_size) noexcept {
        // Zigzag encode
        u64 zigzag = ZigZag::encode(value);
        return VarIntU::encode(zigzag, output, output_size);
    }
    
    // Decode signed value
    [[nodiscard]] static usize decode(const byte* input, usize input_size, i64& value) noexcept {
        u64 zigzag;
        usize consumed = VarIntU::decode(input, input_size, zigzag);
        if (consumed == 0) return 0;
        
        value = ZigZag::decode(zigzag);
        return consumed;
    }
};

// Group varint encoding (4 values at once)
// More efficient for sequences of small values
class GroupVarIntU32 {
public:
    // Each group encodes 4 u32 values
    // First byte contains 2-bit lengths for each value
    // Then up to 4*4 = 16 bytes of data
    
    // Encode 4 values
    // Returns bytes written, or 0 on error
    [[nodiscard]] static usize encode(const u32 values[4], byte* output, usize output_size) noexcept {
        // Calculate lengths
        u8 lengths[4];
        usize total_data_bytes = 0;
        
        for (usize i = 0; i < 4; ++i) {
            if (values[i] < (1U << 8)) {
                lengths[i] = 1;
            } else if (values[i] < (1U << 16)) {
                lengths[i] = 2;
            } else if (values[i] < (1U << 24)) {
                lengths[i] = 3;
            } else {
                lengths[i] = 4;
            }
            total_data_bytes += lengths[i];
        }
        
        // Need 1 byte for lengths + data bytes
        usize total_bytes = 1 + total_data_bytes;
        if (output_size < total_bytes) return 0;
        
        // Write lengths byte
        output[0] = static_cast<byte>(
            (lengths[0] - 1) | 
            ((lengths[1] - 1) << 2) | 
            ((lengths[2] - 1) << 4) | 
            ((lengths[3] - 1) << 6)
        );
        
        // Write data
        usize offset = 1;
        for (usize i = 0; i < 4; ++i) {
            u32 v = values[i];
            for (u8 j = 0; j < lengths[i]; ++j) {
                output[offset++] = static_cast<byte>(v);
                v >>= 8;
            }
        }
        
        return total_bytes;
    }
    
    // Decode 4 values
    // Returns bytes consumed, or 0 on error
    [[nodiscard]] static usize decode(const byte* input, usize input_size, u32 values[4]) noexcept {
        if (input_size < 1) return 0;
        
        // Read lengths
        u8 lengths_byte = input[0];
        u8 lengths[4];
        usize total_data_bytes = 0;
        
        for (usize i = 0; i < 4; ++i) {
            lengths[i] = ((lengths_byte >> (i * 2)) & 0x03) + 1;
            total_data_bytes += lengths[i];
        }
        
        if (input_size < 1 + total_data_bytes) return 0;
        
        // Read data
        usize offset = 1;
        for (usize i = 0; i < 4; ++i) {
            u32 v = 0;
            for (u8 j = 0; j < lengths[i]; ++j) {
                v |= static_cast<u32>(input[offset++]) << (j * 8);
            }
            values[i] = v;
        }
        
        return 1 + total_data_bytes;
    }
};

} // namespace codec

} // namespace vectortick
