#include "vectortick/storage/catalog.hpp"
#include "vectortick/common/crc32c.hpp"
#include "vectortick/common/endian.hpp"
#include <map>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace vectortick {

// ---------------------------------------------------------------------------
// Manifest implementation
// ---------------------------------------------------------------------------

void Manifest::add_segment(const SegmentMeta &meta) {
    for (auto &seg : segments_) {
        if (seg.segment_id == meta.segment_id) {
            seg = meta;
            return;
        }
    }
    segments_.push_back(meta);
}

bool Manifest::remove_segment(u64 segment_id) {
    for (auto it = segments_.begin(); it != segments_.end(); ++it) {
        if (it->segment_id == segment_id) {
            it->active = false;
            return true;
        }
    }
    return false;
}

const SegmentMeta *Manifest::find_segment(u64 segment_id) const {
    for (const auto &seg : segments_) {
        if (seg.segment_id == segment_id && seg.active) {
            return &seg;
        }
    }
    return nullptr;
}

Status Manifest::serialize(std::vector<u8> &out) const {
    out.clear();
    // Header
    const u32 magic = Magic;
    const u32 ver = version_;
    const u64 next_seg = next_segment_id_;
    const u64 next_tx = next_tx_id_;
    const u64 seg_count = segments_.size();

    auto append_u32 = [&](u32 val) {
        u8 buf[4];
        buf[0] = static_cast<u8>(val);
        buf[1] = static_cast<u8>(val >> 8);
        buf[2] = static_cast<u8>(val >> 16);
        buf[3] = static_cast<u8>(val >> 24);
        out.insert(out.end(), buf, buf + 4);
    };

    auto append_u64 = [&](u64 val) {
        u8 buf[8];
        for (int i = 0; i < 8; ++i) {
            buf[i] = static_cast<u8>(val >> (i * 8));
        }
        out.insert(out.end(), buf, buf + 8);
    };

    append_u32(magic);
    append_u32(ver);
    append_u64(next_seg);
    append_u64(next_tx);
    append_u64(seg_count);

    for (const auto &s : segments_) {
        append_u64(s.segment_id);
        const u32 name_len = static_cast<u32>(s.filename.size());
        append_u32(name_len);
        out.insert(out.end(), s.filename.begin(), s.filename.end());
        append_u64(s.min_seq);
        append_u64(s.max_seq);
        append_u64(s.min_exchange_ts);
        append_u64(s.max_exchange_ts);
        append_u64(s.row_count);
        append_u64(s.file_size_bytes);
        append_u32(s.schema_hash);
        append_u32(s.data_crc);
        out.push_back(s.is_compacted ? 1 : 0);
        out.push_back(s.active ? 1 : 0);
    }

    // CRC32C over all data written so far
    u32 crc = crc32c(out.data(), out.size());
    append_u32(crc);

    return Status::OK();
}

Status Manifest::deserialize(std::span<const u8> data) {
    if (data.size() < 36) { // Header (32) + CRC (4)
        return Status(StatusCode::ManifestInvalid, "Manifest payload too small");
    }

    // Check CRC
    const u32 stored_crc = static_cast<u32>(data[data.size() - 4]) |
                           (static_cast<u32>(data[data.size() - 3]) << 8) |
                           (static_cast<u32>(data[data.size() - 2]) << 16) |
                           (static_cast<u32>(data[data.size() - 1]) << 24);

    u32 computed_crc = crc32c(data.data(), data.size() - 4);
    if (stored_crc != computed_crc) {
        return Status(StatusCode::ManifestCorrupted, "Manifest CRC mismatch");
    }

    size_t offset = 0;
    auto read_u32 = [&]() -> u32 {
        u32 val = static_cast<u32>(data[offset]) | (static_cast<u32>(data[offset + 1]) << 8) |
                  (static_cast<u32>(data[offset + 2]) << 16) |
                  (static_cast<u32>(data[offset + 3]) << 24);
        offset += 4;
        return val;
    };

    auto read_u64 = [&]() -> u64 {
        u64 val = 0;
        for (int i = 0; i < 8; ++i) {
            val |= static_cast<u64>(data[offset + i]) << (i * 8);
        }
        offset += 8;
        return val;
    };

    u32 magic = read_u32();
    if (magic != Magic) {
        return Status(StatusCode::ManifestInvalid, "Invalid manifest magic");
    }

    version_ = read_u32();
    next_segment_id_ = read_u64();
    next_tx_id_ = read_u64();
    u64 count = read_u64();

    segments_.clear();
    if (version_ != 1 || !next_segment_id_ || !next_tx_id_ || next_segment_id_ == UINT64_MAX ||
        next_tx_id_ == UINT64_MAX || count > (data.size() - 36) / 70)
        return Status(StatusCode::ManifestInvalid, "Invalid manifest version or count");
    segments_.reserve(static_cast<usize>(count));

    const size_t end_payload = data.size() - 4;
    for (u64 i = 0; i < count; ++i) {
        if (offset + 12 > end_payload) {
            return Status(StatusCode::ManifestInvalid, "Truncated manifest segment record");
        }
        SegmentMeta s;
        s.segment_id = read_u64();
        u32 name_len = read_u32();
        if (offset + name_len + 58 > end_payload) {
            return Status(StatusCode::ManifestInvalid, "Truncated manifest segment data");
        }
        s.filename = std::string(reinterpret_cast<const char *>(data.data() + offset), name_len);
        offset += name_len;
        if (s.filename.empty() || s.filename == "." || s.filename == ".." ||
            s.filename.find('/') != std::string::npos ||
            s.filename.find('\\') != std::string::npos ||
            s.filename.find('\0') != std::string::npos || !s.segment_id ||
            s.segment_id == UINT64_MAX)
            return Status(StatusCode::ManifestInvalid, "Invalid manifest filename or segment ID");
        s.min_seq = read_u64();
        s.max_seq = read_u64();
        s.min_exchange_ts = read_u64();
        s.max_exchange_ts = read_u64();
        s.row_count = read_u64();
        if (s.row_count > SegmentConfig::MaxRowsPerSegment)
            return Status(StatusCode::ManifestInvalid, "Invalid manifest row count");
        s.file_size_bytes = read_u64();
        s.schema_hash = read_u32();
        s.data_crc = read_u32();
        s.is_compacted = (data[offset++] != 0);
        s.active = (data[offset++] != 0);
        segments_.push_back(std::move(s));
    }

    if (offset != end_payload)
        return Status(StatusCode::ManifestInvalid, "Trailing manifest bytes");
    return Status::OK();
}

Status Manifest::save_atomic(const std::string &path) const {
    std::vector<u8> buffer;
    auto status = serialize(buffer);
    if (!status.ok())
        return status;

    std::string tmp_path = path + ".tmp";
    int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return Status(StatusCode::FileOpenFailed, "Failed to create manifest temp file");
    }

    size_t total_written = 0;
    while (total_written < buffer.size()) {
        ssize_t res = ::write(fd, buffer.data() + total_written, buffer.size() - total_written);
        if (res <= 0) {
            ::close(fd);
            ::unlink(tmp_path.c_str());
            return Status(StatusCode::FileWriteFailed, "Failed writing manifest data");
        }
        total_written += static_cast<size_t>(res);
    }

    if (::fsync(fd) != 0) {
        ::close(fd);
        ::unlink(tmp_path.c_str());
        return Status(StatusCode::FileSyncFailed, "Failed to fsync manifest temp file");
    }
    ::close(fd);

    if (::rename(tmp_path.c_str(), path.c_str()) != 0) {
        ::unlink(tmp_path.c_str());
        return Status(StatusCode::FileRenameFailed, "Failed to atomically rename manifest file");
    }

    auto parent = std::filesystem::path(path).parent_path();
    if (parent.empty())
        parent = ".";
    int directory = ::open(parent.c_str(), O_RDONLY);
    if (directory < 0)
        return Status(StatusCode::FileOpenFailed, "Cannot open manifest directory");
    int sync_result = ::fsync(directory);
    ::close(directory);
    if (sync_result != 0)
        return Status(StatusCode::FileSyncFailed, "Cannot sync manifest directory");
    return Status::OK();
}

Status Manifest::load(const std::string &path, Manifest &out_manifest) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return Status(StatusCode::FileNotFound, "Manifest file not found");
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 64 * 1024 * 1024) {
        ::close(fd);
        return Status(StatusCode::ManifestInvalid, "Empty or invalid manifest file");
    }

    std::vector<u8> buffer(static_cast<size_t>(st.st_size));
    size_t total_read = 0;
    while (total_read < buffer.size()) {
        ssize_t res = ::read(fd, buffer.data() + total_read, buffer.size() - total_read);
        if (res <= 0) {
            ::close(fd);
            return Status(StatusCode::FileReadFailed, "Failed reading manifest file");
        }
        total_read += static_cast<size_t>(res);
    }
    ::close(fd);

    return out_manifest.deserialize(buffer);
}

// ---------------------------------------------------------------------------
// Journal implementation
// ---------------------------------------------------------------------------

Journal::~Journal() { close(); }

Status Journal::open(const std::string &path) {
    close();
    path_ = path;

    bool exists = (::access(path.c_str(), F_OK) == 0);
    usize accepted_size = 0;
    if (exists) {
        std::vector<RecoveredTx> recovered;
        auto st = read_all(path, recovered, &accepted_size);
        if (!st.ok())
            return st;
    }
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_ < 0) {
        return Status(StatusCode::FileOpenFailed, "Failed to open journal file");
    }

    if (exists && ::ftruncate(fd_, static_cast<off_t>(accepted_size)) != 0) {
        close();
        return Status(StatusCode::FileWriteFailed, "Journal tail repair failed");
    }
    if (!exists) {
        // Write journal header
        JournalHeader header;
        header.creation_ts =
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count());

        ssize_t written = ::write(fd_, &header, sizeof(header));
        if (written != sizeof(header) || ::fsync(fd_) != 0) {
            close();
            return Status(StatusCode::FileWriteFailed, "Failed to initialize journal header");
        }
    }

    return Status::OK();
}

void Journal::close() {
    if (fd_ >= 0) {
        ::fsync(fd_);
        ::close(fd_);
        fd_ = -1;
    }
}

Status Journal::write_record(u64 tx_id, JournalOpType op, const void *payload, u32 payload_size) {
    if (fd_ < 0) {
        return Status(StatusCode::InternalError, "Journal not open");
    }

    // Record buffer:
    // tx_id (8) + op_type (4) + payload_size (4) + payload + crc32c (4)
    std::vector<u8> buf;
    buf.reserve(16 + payload_size + 4);

    auto append_u32 = [&](u32 val) {
        u8 b[4];
        b[0] = static_cast<u8>(val);
        b[1] = static_cast<u8>(val >> 8);
        b[2] = static_cast<u8>(val >> 16);
        b[3] = static_cast<u8>(val >> 24);
        buf.insert(buf.end(), b, b + 4);
    };

    auto append_u64 = [&](u64 val) {
        u8 b[8];
        for (int i = 0; i < 8; ++i) {
            b[i] = static_cast<u8>(val >> (i * 8));
        }
        buf.insert(buf.end(), b, b + 8);
    };

    append_u64(tx_id);
    append_u32(static_cast<u32>(op));
    append_u32(payload_size);

    if (payload_size > 0 && payload != nullptr) {
        const u8 *p = static_cast<const u8 *>(payload);
        buf.insert(buf.end(), p, p + payload_size);
    }

    u32 crc = crc32c(buf.data(), buf.size());
    append_u32(crc);

    size_t total = 0;
    while (total < buf.size()) {
        ssize_t res = ::write(fd_, buf.data() + total, buf.size() - total);
        if (res <= 0) {
            return Status(StatusCode::FileWriteFailed, "Journal write failed");
        }
        total += static_cast<size_t>(res);
    }

    return flush_and_sync();
}

Status Journal::flush_and_sync() {
    if (fd_ < 0)
        return Status::OK();
    if (::fsync(fd_) != 0) {
        return Status(StatusCode::FileSyncFailed, "Journal fsync failed");
    }
    return Status::OK();
}

Status Journal::append_begin(u64 tx_id) {
    return write_record(tx_id, JournalOpType::TxBegin, nullptr, 0);
}

Status Journal::append_commit(u64 tx_id) {
    return write_record(tx_id, JournalOpType::TxCommit, nullptr, 0);
}

Status Journal::append_abort(u64 tx_id) {
    return write_record(tx_id, JournalOpType::TxAbort, nullptr, 0);
}

Status Journal::append_add_segment(u64 tx_id, const SegmentMeta &meta) {
    std::vector<u8> payload;
    auto append_u32 = [&](u32 val) {
        u8 b[4];
        b[0] = static_cast<u8>(val);
        b[1] = static_cast<u8>(val >> 8);
        b[2] = static_cast<u8>(val >> 16);
        b[3] = static_cast<u8>(val >> 24);
        payload.insert(payload.end(), b, b + 4);
    };

    auto append_u64 = [&](u64 val) {
        u8 b[8];
        for (int i = 0; i < 8; ++i) {
            b[i] = static_cast<u8>(val >> (i * 8));
        }
        payload.insert(payload.end(), b, b + 8);
    };

    append_u64(meta.segment_id);
    u32 len = static_cast<u32>(meta.filename.size());
    append_u32(len);
    payload.insert(payload.end(), meta.filename.begin(), meta.filename.end());
    append_u64(meta.min_seq);
    append_u64(meta.max_seq);
    append_u64(meta.min_exchange_ts);
    append_u64(meta.max_exchange_ts);
    append_u64(meta.row_count);
    append_u64(meta.file_size_bytes);
    append_u32(meta.schema_hash);
    append_u32(meta.data_crc);
    payload.push_back(meta.is_compacted ? 1 : 0);
    payload.push_back(meta.active ? 1 : 0);

    return write_record(tx_id, JournalOpType::AddSegment, payload.data(),
                        static_cast<u32>(payload.size()));
}

Status Journal::append_remove_segment(u64 tx_id, u64 segment_id) {
    u8 buf[8];
    for (int i = 0; i < 8; ++i) {
        buf[i] = static_cast<u8>(segment_id >> (i * 8));
    }
    return write_record(tx_id, JournalOpType::RemoveSegment, buf, 8);
}

Status Journal::append_compact(u64 tx_id, const std::vector<u64> &old_ids,
                               const std::vector<SegmentMeta> &new_metas) {
    std::vector<u8> payload;
    auto append_u32 = [&](u32 val) {
        u8 b[4];
        b[0] = static_cast<u8>(val);
        b[1] = static_cast<u8>(val >> 8);
        b[2] = static_cast<u8>(val >> 16);
        b[3] = static_cast<u8>(val >> 24);
        payload.insert(payload.end(), b, b + 4);
    };
    auto append_u64 = [&](u64 val) {
        u8 b[8];
        for (int i = 0; i < 8; ++i) {
            b[i] = static_cast<u8>(val >> (i * 8));
        }
        payload.insert(payload.end(), b, b + 8);
    };

    append_u32(static_cast<u32>(old_ids.size()));
    for (u64 oid : old_ids)
        append_u64(oid);

    append_u32(static_cast<u32>(new_metas.size()));
    for (const auto &meta : new_metas) {
        append_u64(meta.segment_id);
        u32 len = static_cast<u32>(meta.filename.size());
        append_u32(len);
        payload.insert(payload.end(), meta.filename.begin(), meta.filename.end());
        append_u64(meta.min_seq);
        append_u64(meta.max_seq);
        append_u64(meta.min_exchange_ts);
        append_u64(meta.max_exchange_ts);
        append_u64(meta.row_count);
        append_u64(meta.file_size_bytes);
        append_u32(meta.schema_hash);
        append_u32(meta.data_crc);
        payload.push_back(meta.is_compacted ? 1 : 0);
        payload.push_back(meta.active ? 1 : 0);
    }

    return write_record(tx_id, JournalOpType::CompactSegments, payload.data(),
                        static_cast<u32>(payload.size()));
}

Status Journal::read_all(const std::string &path, std::vector<RecoveredTx> &recovered_txs,
                         usize *valid_end) {
    recovered_txs.clear();
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return Status(StatusCode::FileNotFound, "Journal not found");
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(JournalHeader)) ||
        st.st_size > 64 * 1024 * 1024) {
        ::close(fd);
        return Status(StatusCode::JournalInvalid, "Truncated or excessive journal");
    }
    std::vector<u8> bytes(static_cast<usize>(st.st_size));
    usize total = 0;
    while (total < bytes.size()) {
        auto n = ::read(fd, bytes.data() + total, bytes.size() - total);
        if (n <= 0) {
            ::close(fd);
            return Status(StatusCode::FileReadFailed, "Journal read failed");
        }
        total += static_cast<usize>(n);
    }
    ::close(fd);
    auto read32 = [&](usize pos) { return read_le_u32(bytes.data() + pos); };
    auto read64 = [&](usize pos) { return read_le_u64(bytes.data() + pos); };
    if (read32(0) != JournalHeader::Magic || read32(4) != 1)
        return Status(StatusCode::JournalInvalid, "Invalid journal header");
    usize offset = sizeof(JournalHeader);
    std::map<u64, bool> open_transactions;
    auto corrupt = []() { return Status(StatusCode::JournalInvalid, "Corrupt journal record"); };
    auto filename_valid = [](const std::string &name) {
        return !name.empty() && name.find('/') == std::string::npos &&
               name.find('\\') == std::string::npos && name != "." && name != ".." &&
               name.find('\0') == std::string::npos;
    };
    while (bytes.size() - offset >= 20) {
        usize record = offset;
        u64 id = read64(offset);
        u32 op_raw = read32(offset + 8), size = read32(offset + 12);
        if (!id || id == UINT64_MAX || op_raw < 1 || op_raw > 6 || size > 16 * 1024 * 1024)
            return corrupt();
        if (size > bytes.size() - offset - 20) {
            // A partial last write is recoverable. A valid later record proves
            // this damaged length was interior corruption, so reject it.
            for (usize probe = offset + 16; bytes.size() - probe >= 20; ++probe) {
                u32 next_op = read32(probe + 8), next_size = read32(probe + 12);
                if (read64(probe) && next_op >= 1 && next_op <= 6 &&
                    next_size <= bytes.size() - probe - 20 &&
                    crc32c(bytes.data() + probe, 16 + next_size) == read32(probe + 16 + next_size))
                    return corrupt();
            }
            break;
        }
        usize payload = offset + 16, end = payload + size;
        if (crc32c(bytes.data() + record, 16 + size) != read32(end))
            return corrupt();
        auto op = static_cast<JournalOpType>(op_raw);
        auto tx_it = std::find_if(recovered_txs.begin(), recovered_txs.end(),
                                  [&](const auto &tx) { return tx.tx_id == id; });
        if (op == JournalOpType::TxBegin) {
            if (size != 0 || tx_it != recovered_txs.end())
                return corrupt();
            recovered_txs.emplace_back();
            recovered_txs.back().tx_id = id;
            open_transactions[id] = true;
            offset = end + 4;
            continue;
        }
        if (tx_it == recovered_txs.end() || !open_transactions[id])
            return corrupt();
        auto &tx = *tx_it;
        auto segment = [&](usize &pos, SegmentMeta &meta) {
            if (pos > end || end - pos < 70)
                return false;
            meta.segment_id = read64(pos);
            u32 name_size = read32(pos + 8);
            pos += 12;
            if (name_size > end - pos - 58)
                return false;
            meta.filename.assign(reinterpret_cast<const char *>(bytes.data() + pos), name_size);
            pos += name_size;
            if (!filename_valid(meta.filename))
                return false;
            meta.min_seq = read64(pos);
            meta.max_seq = read64(pos + 8);
            meta.min_exchange_ts = read64(pos + 16);
            meta.max_exchange_ts = read64(pos + 24);
            meta.row_count = read64(pos + 32);
            meta.file_size_bytes = read64(pos + 40);
            meta.schema_hash = read32(pos + 48);
            meta.data_crc = read32(pos + 52);
            meta.is_compacted = bytes[pos + 56] != 0;
            meta.active = bytes[pos + 57] != 0;
            pos += 58;
            return meta.segment_id != 0 && meta.segment_id != UINT64_MAX &&
                   meta.row_count <= SegmentConfig::MaxRowsPerSegment;
        };
        usize pos = payload;
        if (op == JournalOpType::TxCommit || op == JournalOpType::TxAbort) {
            if (size)
                return corrupt();
            tx.committed = op == JournalOpType::TxCommit;
            open_transactions[id] = false;
        } else if (op == JournalOpType::RemoveSegment) {
            if (size != 8)
                return corrupt();
            tx.removed_segments.push_back(read64(pos));
            pos += 8;
        } else if (op == JournalOpType::AddSegment) {
            SegmentMeta meta;
            if (!segment(pos, meta))
                return corrupt();
            tx.added_segments.push_back(std::move(meta));
        } else if (op == JournalOpType::CompactSegments) {
            if (end - pos < 4)
                return corrupt();
            u32 old_count = read32(pos);
            pos += 4;
            if (old_count > (end - pos) / 8)
                return corrupt();
            for (u32 k = 0; k < old_count; ++k) {
                tx.removed_segments.push_back(read64(pos));
                pos += 8;
            }
            if (end - pos < 4)
                return corrupt();
            u32 new_count = read32(pos);
            pos += 4;
            if (new_count > (end - pos) / 70)
                return corrupt();
            for (u32 k = 0; k < new_count; ++k) {
                SegmentMeta meta;
                if (!segment(pos, meta))
                    return corrupt();
                tx.added_segments.push_back(std::move(meta));
            }
        }
        if (pos != end)
            return corrupt();
        offset = end + 4;
    }
    if (valid_end)
        *valid_end = offset;
    return Status::OK();
}

// ---------------------------------------------------------------------------
// Catalog implementation
// ---------------------------------------------------------------------------

Catalog::Catalog(std::string root_dir) : root_dir_(std::move(root_dir)) {
    manifest_path_ = root_dir_ + "/manifest.vtm";
    journal_path_ = root_dir_ + "/journal.vtwal";
}

Catalog::~Catalog() { journal_.close(); }

Status Catalog::open() {
    std::error_code ec;
    std::filesystem::create_directories(root_dir_, ec);
    if (ec) {
        return Status(StatusCode::DirectoryCreateFailed, "Failed to create catalog directory");
    }

    auto status = recover();
    if (!status.ok()) {
        return status;
    }

    return journal_.open(journal_path_);
}

Status Catalog::recover() {
    std::error_code ec;
    // 1. Clean up any leftover temporary files (.tmp or .tmp_*)
    for (const auto &entry : std::filesystem::directory_iterator(root_dir_, ec)) {
        if (entry.is_regular_file()) {
            std::string filename = entry.path().filename().string();
            if (filename.find(".tmp") != std::string::npos) {
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }

    // 2. Load manifest if it exists
    if (std::filesystem::exists(manifest_path_, ec)) {
        auto st = Manifest::load(manifest_path_, manifest_);
        if (!st.ok())
            return st;
    }

    // 3. Scan journal for committed transactions
    if (std::filesystem::exists(journal_path_, ec)) {
        std::vector<Journal::RecoveredTx> recovered_txs;
        auto jstatus = Journal::read_all(journal_path_, recovered_txs);
        if (!jstatus.ok())
            return jstatus;
        {
            for (const auto &tx : recovered_txs) {
                if (tx.tx_id >= manifest_.next_tx_id())
                    manifest_.set_next_tx_id(tx.tx_id + 1);
                if (!tx.committed)
                    continue;

                // Apply additions
                for (const auto &add : tx.added_segments) {
                    {
                        manifest_.add_segment(add);
                        if (add.segment_id >= manifest_.next_segment_id()) {
                            manifest_.set_next_segment_id(add.segment_id + 1);
                        }
                    }
                }

                // Apply removals
                for (u64 rid : tx.removed_segments) {
                    manifest_.remove_segment(rid);
                }

                if (tx.tx_id >= manifest_.next_tx_id()) {
                    manifest_.set_next_tx_id(tx.tx_id + 1);
                }
            }
        }
    }

    // 4. Validate all active segments; remove invalid ones
    for (auto &seg : manifest_.mutable_segments()) {
        if (!seg.active)
            continue;
        std::string full_path = root_dir_ + "/" + seg.filename;
        if (!std::filesystem::exists(full_path, ec)) {
            return Status(StatusCode::SegmentCorrupted, "Committed catalog segment is missing");
        }

        SegmentReader r;
        auto status = r.open(full_path);
        if (!status.ok())
            return status;
        if (seg.row_count != r.row_count())
            return Status(StatusCode::SegmentCorrupted, "Catalog row count mismatch");
    }

    // 5. Save recovered manifest
    return manifest_.save_atomic(manifest_path_);
}

std::vector<SegmentMeta> Catalog::active_segments() const {
    std::vector<SegmentMeta> result;
    for (const auto &s : manifest_.segments()) {
        if (s.active) {
            result.push_back(s);
        }
    }
    return result;
}

std::vector<SegmentMeta> Catalog::find_segments_by_time(u64 min_ts, u64 max_ts) const {
    std::vector<SegmentMeta> result;
    for (const auto &s : manifest_.segments()) {
        if (!s.active)
            continue;
        if (s.max_exchange_ts >= min_ts && s.min_exchange_ts <= max_ts) {
            result.push_back(s);
        }
    }
    return result;
}

std::vector<SegmentMeta> Catalog::find_segments_by_sequence(u64 min_seq, u64 max_seq) const {
    std::vector<SegmentMeta> result;
    for (const auto &s : manifest_.segments()) {
        if (!s.active)
            continue;
        if (s.max_seq >= min_seq && s.min_seq <= max_seq) {
            result.push_back(s);
        }
    }
    return result;
}

Status Catalog::commit_segment(const std::string &temp_segment_path, SegmentMeta meta,
                               std::string *out_final_path) {
    SegmentReader source;
    auto source_status = source.open(temp_segment_path);
    if (!source_status.ok())
        return source_status;
    if (meta.row_count && meta.row_count != source.row_count())
        return Status(StatusCode::SegmentCorrupted, "Publication row count mismatch");
    meta.row_count = source.row_count();
    meta.file_size_bytes = source.file_size();
    meta.min_seq = source.min_sequence();
    meta.max_seq = source.max_sequence();
    meta.schema_hash = source.schema_hash();
    meta.min_exchange_ts = source.min_timestamp();
    meta.max_exchange_ts = source.max_timestamp();
    source.close();
    u64 tx_id = manifest_.next_tx_id();
    manifest_.set_next_tx_id(tx_id + 1);

    u64 seg_id = manifest_.next_segment_id();
    manifest_.set_next_segment_id(seg_id + 1);

    meta.segment_id = seg_id;
    meta.filename = "segment_" + std::to_string(seg_id) + ".vts";
    meta.active = true;

    std::string final_path = root_dir_ + "/" + meta.filename;

    // Flush temp segment file to ensure durability
    int fd = ::open(temp_segment_path.c_str(), O_RDONLY);
    if (fd < 0)
        return Status(StatusCode::FileOpenFailed, "Cannot open publication file");
    int file_sync = ::fsync(fd);
    ::close(fd);
    if (file_sync != 0)
        return Status(StatusCode::FileSyncFailed, "Cannot sync publication file");

    // Step 1: Journal TxBegin
    auto st = journal_.append_begin(tx_id);
    if (!st.ok())
        return st;

    // Step 2: Journal AddSegment
    st = journal_.append_add_segment(tx_id, meta);
    if (!st.ok()) {
        (void)journal_.append_abort(tx_id);
        return st;
    }

    // Publish the durable file before acknowledging the transaction.
    std::error_code ec;
    std::filesystem::rename(temp_segment_path, final_path, ec);
    if (ec) {
        (void)journal_.append_abort(tx_id);
        return Status(StatusCode::FileRenameFailed, "Failed to move segment into catalog");
    }
    int directory = ::open(root_dir_.c_str(), O_RDONLY);
    if (directory < 0)
        return Status(StatusCode::FileOpenFailed, "Cannot open catalog directory");
    int sync_result = ::fsync(directory);
    ::close(directory);
    if (sync_result != 0)
        return Status(StatusCode::FileSyncFailed, "Cannot sync catalog directory");
    st = journal_.append_commit(tx_id);
    if (!st.ok())
        return st;

    // Step 5: Update manifest and atomically save
    manifest_.add_segment(meta);
    st = manifest_.save_atomic(manifest_path_);
    if (!st.ok())
        return st;

    if (out_final_path) {
        *out_final_path = final_path;
    }
    return Status::OK();
}

Status Catalog::compact_segments(const std::vector<u64> &segment_ids_to_compact,
                                 std::vector<u64> *out_new_segment_ids) {
    if (segment_ids_to_compact.empty()) {
        return Status::OK();
    }

    // Collect all events from input segments
    std::vector<CanonicalEvent> all_events;
    for (u64 sid : segment_ids_to_compact) {
        const SegmentMeta *sm = manifest_.find_segment(sid);
        if (!sm)
            continue;

        std::string full_path = root_dir_ + "/" + sm->filename;
        SegmentReader reader;
        auto st = reader.open(full_path);
        if (!st.ok())
            return st;

        std::vector<CanonicalEvent> events;
        st = reader.read_all_events(events);
        if (!st.ok())
            return st;

        all_events.insert(all_events.end(), events.begin(), events.end());
    }

    if (all_events.empty()) {
        return Status::OK();
    }

    // Sort events by sequence and timestamp
    std::sort(all_events.begin(), all_events.end(),
              [](const CanonicalEvent &a, const CanonicalEvent &b) {
                  if (a.exchange_ts_ns != b.exchange_ts_ns) {
                      return a.exchange_ts_ns < b.exchange_ts_ns;
                  }
                  return a.sequence < b.sequence;
              });

    // Write compacted events into new segment
    u64 new_seg_id = manifest_.next_segment_id();
    manifest_.set_next_segment_id(new_seg_id + 1);

    std::string tmp_compact = root_dir_ + "/.tmp_compact_" + std::to_string(new_seg_id) + ".vts";
    SegmentWriter writer(new_seg_id, all_events.size());
    for (const auto &ev : all_events) {
        auto st = writer.add_event(ev);
        if (!st.ok())
            return st;
    }

    auto st = writer.write_to_file(tmp_compact);
    if (!st.ok()) {
        std::error_code ec;
        std::filesystem::remove(tmp_compact, ec);
        return st;
    }

    // Read back meta from written compacted segment
    SegmentReader reader;
    st = reader.open(tmp_compact);
    if (!st.ok())
        return st;

    SegmentMeta new_meta;
    new_meta.segment_id = new_seg_id;
    new_meta.filename = "segment_" + std::to_string(new_seg_id) + ".vts";
    new_meta.min_seq = all_events.front().sequence;
    new_meta.max_seq = all_events.back().sequence;
    new_meta.min_exchange_ts = all_events.front().exchange_ts_ns;
    new_meta.max_exchange_ts = all_events.back().exchange_ts_ns;
    new_meta.row_count = all_events.size();
    new_meta.file_size_bytes = std::filesystem::file_size(tmp_compact);
    new_meta.schema_hash = reader.schema_hash();
    new_meta.data_crc = 0; // Filled on validation
    new_meta.is_compacted = true;
    new_meta.active = true;
    reader.close();

    // Prepare transaction
    u64 tx_id = manifest_.next_tx_id();
    manifest_.set_next_tx_id(tx_id + 1);

    st = journal_.append_begin(tx_id);
    if (!st.ok())
        return st;

    st = journal_.append_compact(tx_id, segment_ids_to_compact, {new_meta});
    if (!st.ok()) {
        (void)journal_.append_abort(tx_id);
        return st;
    }

    std::string final_path = root_dir_ + "/" + new_meta.filename;
    std::error_code ec;
    std::filesystem::rename(tmp_compact, final_path, ec);
    if (ec) {
        (void)journal_.append_abort(tx_id);
        return Status(StatusCode::FileRenameFailed, "Failed to move compacted segment");
    }
    int directory = ::open(root_dir_.c_str(), O_RDONLY);
    if (directory < 0)
        return Status(StatusCode::FileOpenFailed, "Cannot open catalog directory");
    int sync_result = ::fsync(directory);
    ::close(directory);
    if (sync_result != 0)
        return Status(StatusCode::FileSyncFailed, "Cannot sync catalog directory");
    st = journal_.append_commit(tx_id);
    if (!st.ok())
        return st;

    // Update manifest
    for (u64 sid : segment_ids_to_compact) {
        manifest_.remove_segment(sid);
    }
    manifest_.add_segment(new_meta);
    st = manifest_.save_atomic(manifest_path_);
    if (!st.ok())
        return st;

    // Delete superseded segment files
    for (u64 sid : segment_ids_to_compact) {
        std::string old_file = root_dir_ + "/segment_" + std::to_string(sid) + ".vts";
        std::filesystem::remove(old_file, ec);
    }

    if (out_new_segment_ids) {
        out_new_segment_ids->push_back(new_seg_id);
    }

    return Status::OK();
}

} // namespace vectortick
