#pragma once

#include "vectortick/common/types.hpp"
#include "vectortick/storage/file_format.hpp"
#include "vectortick/storage/segment_reader.hpp"
#include "vectortick/storage/segment_writer.hpp"

#include <string>
#include <vector>
#include <span>
#include <memory>
#include <filesystem>

namespace vectortick {

struct SegmentMeta {
    u64 segment_id{0};
    std::string filename;
    u64 min_seq{0};
    u64 max_seq{0};
    u64 min_exchange_ts{0};
    u64 max_exchange_ts{0};
    u64 row_count{0};
    u64 file_size_bytes{0};
    u32 schema_hash{0};
    u32 data_crc{0};
    bool is_compacted{false};
    bool active{true};
};

enum class JournalOpType : u32 {
    TxBegin = 1,
    AddSegment = 2,
    RemoveSegment = 3,
    CompactSegments = 4,
    TxCommit = 5,
    TxAbort = 6
};

struct JournalHeader {
    static constexpr u32 Magic = 0x57414C31; // 'WAL1'
    u32 magic{Magic};
    u32 version{1};
    u64 creation_ts{0};
};

struct JournalRecord {
    u64 tx_id{0};
    JournalOpType op_type{JournalOpType::TxBegin};
    u32 payload_size{0};
    // Followed by payload bytes, then 4-byte CRC32C
};

class Manifest {
public:
    static constexpr u32 Magic = 0x314D5456; // 'VTM1'
    static constexpr u32 CurrentVersion = 1;

    Manifest() = default;

    u32 version() const { return version_; }
    u64 next_segment_id() const { return next_segment_id_; }
    void set_next_segment_id(u64 id) { next_segment_id_ = id; }
    u64 next_tx_id() const { return next_tx_id_; }
    void set_next_tx_id(u64 id) { next_tx_id_ = id; }

    const std::vector<SegmentMeta>& segments() const { return segments_; }
    std::vector<SegmentMeta>& mutable_segments() { return segments_; }

    void add_segment(const SegmentMeta& meta);
    bool remove_segment(u64 segment_id);
    const SegmentMeta* find_segment(u64 segment_id) const;

    Status serialize(std::vector<u8>& out) const;
    Status deserialize(std::span<const u8> data);

    Status save_atomic(const std::string& path) const;
    static Status load(const std::string& path, Manifest& out_manifest);

private:
    u32 version_{CurrentVersion};
    u64 next_segment_id_{1};
    u64 next_tx_id_{1};
    std::vector<SegmentMeta> segments_;
};

class Journal {
public:
    Journal() = default;
    ~Journal();

    Status open(const std::string& path);
    void close();

    Status append_begin(u64 tx_id);
    Status append_add_segment(u64 tx_id, const SegmentMeta& meta);
    Status append_remove_segment(u64 tx_id, u64 segment_id);
    Status append_compact(u64 tx_id, const std::vector<u64>& old_ids, const std::vector<SegmentMeta>& new_metas);
    Status append_commit(u64 tx_id);
    Status append_abort(u64 tx_id);

    struct RecoveredTx {
        u64 tx_id{0};
        bool committed{false};
        std::vector<SegmentMeta> added_segments;
        std::vector<u64> removed_segments;
    };

    static Status read_all(const std::string& path, std::vector<RecoveredTx>& recovered_txs, usize* valid_end = nullptr);

private:
    Status write_record(u64 tx_id, JournalOpType op, const void* payload, u32 payload_size);
    Status flush_and_sync();

    std::string path_;
    int fd_{-1};
};

class Catalog {
public:
    explicit Catalog(std::string root_dir);
    ~Catalog();

    Status open();
    Status recover();

    const std::string& root_dir() const { return root_dir_; }
    const Manifest& manifest() const { return manifest_; }

    std::vector<SegmentMeta> active_segments() const;
    std::vector<SegmentMeta> find_segments_by_time(u64 min_ts, u64 max_ts) const;
    std::vector<SegmentMeta> find_segments_by_sequence(u64 min_seq, u64 max_seq) const;

    Status commit_segment(const std::string& temp_segment_path,
                          SegmentMeta meta,
                          std::string* out_final_path = nullptr);

    Status compact_segments(const std::vector<u64>& segment_ids_to_compact,
                            std::vector<u64>* out_new_segment_ids = nullptr);

private:
    std::string root_dir_;
    std::string manifest_path_;
    std::string journal_path_;
    Manifest manifest_;
    Journal journal_;
};

} // namespace vectortick
