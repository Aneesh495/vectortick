#pragma once
#include <vector>
#include "../common/endian.hpp"

#include "../common/types.hpp"
#include "../common/status.hpp"
#include <cstring>
#include <unordered_map>

namespace vectortick {

namespace codec {

// Dictionary encoding for strings or repeated values
// Maps values to small integer indices

template <typename T>
class DictionaryEncoder {
public:
    // Build dictionary from values
    void build(const T* values, usize num_values) noexcept {
        dict_.clear();
        indices_.clear();
        reverse_map_.clear();
        
        for (usize i = 0; i < num_values; ++i) {
            auto it = dict_.find(values[i]);
            if (it == dict_.end()) {
                u32 idx = static_cast<u32>(dict_.size());
                dict_[values[i]] = idx;
                reverse_map_.push_back(values[i]);
            }
            indices_.push_back(dict_[values[i]]);
        }
    }
    
    // Get encoded indices
    [[nodiscard]] const std::vector<u32>& indices() const noexcept { return indices_; }
    
    // Get dictionary values
    [[nodiscard]] const std::vector<T>& values() const noexcept { return reverse_map_; }
    
    // Get dictionary size
    [[nodiscard]] usize size() const noexcept { return dict_.size(); }
    
    // Encode to buffer
    // Format: [num_dict_entries: u32][dict entries...][num_values: u32][indices as varint]
    [[nodiscard]] usize encode(byte* output, usize output_size) const noexcept {
        usize offset = 0;
        
        // Write dictionary size
        if (offset + sizeof(u32) > output_size) return 0;
        write_be_u32(output + offset, static_cast<u32>(reverse_map_.size()));
        offset += sizeof(u32);
        
        // Write dictionary values
        for (const auto& val : reverse_map_) {
            if (offset + sizeof(T) > output_size) return 0;
            std::memcpy(output + offset, &val, sizeof(T));
            offset += sizeof(T);
        }
        
        // Write number of values
        if (offset + sizeof(u32) > output_size) return 0;
        write_be_u32(output + offset, static_cast<u32>(indices_.size()));
        offset += sizeof(u32);
        
        // Write indices as varint (simplified - just raw u32 for now)
        for (u32 idx : indices_) {
            if (offset + sizeof(u32) > output_size) return 0;
            write_be_u32(output + offset, idx);
            offset += sizeof(u32);
        }
        
        return offset;
    }
    
private:
    std::unordered_map<T, u32> dict_;
    std::vector<u32> indices_;
    std::vector<T> reverse_map_;
};

template <typename T>
class DictionaryDecoder {
public:
    // Decode from buffer
    [[nodiscard]] usize decode(const byte* input, usize input_size, usize& num_values) noexcept {
        usize offset = 0;
        
        // Read dictionary size
        if (offset + sizeof(u32) > input_size) return 0;
        u32 dict_size = read_be_u32(input + offset);
        offset += sizeof(u32);
        
        // Read dictionary values
        dict_.resize(dict_size);
        for (u32 i = 0; i < dict_size; ++i) {
            if (offset + sizeof(T) > input_size) return 0;
            std::memcpy(&dict_[i], input + offset, sizeof(T));
            offset += sizeof(T);
        }
        
        // Read number of values
        if (offset + sizeof(u32) > input_size) return 0;
        num_values = read_be_u32(input + offset);
        offset += sizeof(u32);
        
        // Read indices
        indices_.resize(num_values);
        for (usize i = 0; i < num_values; ++i) {
            if (offset + sizeof(u32) > input_size) return 0;
            indices_[i] = read_be_u32(input + offset);
            offset += sizeof(u32);
        }
        
        return offset;
    }
    
    // Get decoded values
    void get_values(T* values) const noexcept {
        for (usize i = 0; i < indices_.size(); ++i) {
            values[i] = dict_[indices_[i]];
        }
    }
    
    [[nodiscard]] const std::vector<T>& dictionary() const noexcept { return dict_; }

private:
    std::vector<T> dict_;
    std::vector<u32> indices_;
};

} // namespace codec

} // namespace vectortick
