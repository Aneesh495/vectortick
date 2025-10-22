#include "vectortick/memory/mapped_file.hpp"
#include "vectortick/memory/mapped_file.hpp"
#include "vectortick/memory/aligned_buffer.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace vectortick {

// MappedFile implementation

MappedFile::~MappedFile() {
    close();
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : path_(std::move(other.path_))
    , data_(other.data_)
    , size_(other.size_)
    , writable_(other.writable_)
    , fd_(other.fd_) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.writable_ = false;
    other.fd_ = -1;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        close();
        path_ = std::move(other.path_);
        data_ = other.data_;
        size_ = other.size_;
        writable_ = other.writable_;
        fd_ = other.fd_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.writable_ = false;
        other.fd_ = -1;
    }
    return *this;
}

Result<MappedFile> MappedFile::open_read(const std::string& path) noexcept {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return make_error<MappedFile>(StatusCode::FileOpenFailed, 
                                       "Failed to open file for reading");
    }
    
    struct stat st;
    if (fstat(fd, &st) < 0) {
        ::close(fd);
        return make_error<MappedFile>(StatusCode::FileStatFailed, 
                                       "Failed to stat file");
    }
    
    usize size = static_cast<usize>(st.st_size);
    if (size == 0) {
        ::close(fd);
        return make_error<MappedFile>(StatusCode::InvalidArgument,
                                       "Cannot map empty file");
    }
    
    void* addr = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) {
        ::close(fd);
        return make_error<MappedFile>(StatusCode::MmapFailed, 
                                       "Failed to map file");
    }
    
    // Advise sequential access
    madvise(addr, size, MADV_SEQUENTIAL);
    
    return MappedFile(path, static_cast<byte*>(addr), size, false, fd);
}

Result<MappedFile> MappedFile::open_read_write(const std::string& path, usize size) noexcept {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return make_error<MappedFile>(StatusCode::FileOpenFailed, 
                                       "Failed to open file for writing");
    }
    
    // Set file size
    if (ftruncate(fd, static_cast<off_t>(size)) < 0) {
        ::close(fd);
        return make_error<MappedFile>(StatusCode::FileWriteFailed, 
                                       "Failed to set file size");
    }
    
    void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        ::close(fd);
        return make_error<MappedFile>(StatusCode::MmapFailed, 
                                       "Failed to map file");
    }
    
    return MappedFile(path, static_cast<byte*>(addr), size, true, fd);
}

void MappedFile::close() noexcept {
    if (data_) {
        munmap(data_, size_);
        data_ = nullptr;
        size_ = 0;
    }
    
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    
    writable_ = false;
}

Status MappedFile::sync() noexcept {
    if (!data_ || !writable_) {
        return Status::OK();
    }
    
    if (msync(data_, size_, MS_SYNC) < 0) {
        return Status(StatusCode::MsyncFailed, "msync failed");
    }
    
    return Status::OK();
}

void MappedFile::advise(Advice advice) noexcept {
    if (!data_) return;
    
    int madv = MADV_NORMAL;
    switch (advice) {
        case Advice::Sequential: madv = MADV_SEQUENTIAL; break;
        case Advice::Random: madv = MADV_RANDOM; break;
        case Advice::WillNeed: madv = MADV_WILLNEED; break;
        case Advice::DontNeed: madv = MADV_DONTNEED; break;
        default: madv = MADV_NORMAL; break;
    }
    
    madvise(data_, size_, madv);
}

// ScopedMapping implementation

ScopedMapping::ScopedMapping(const std::string& path, bool writable) {
    if (writable) {
        auto result = MappedFile::open_read_write(path, 0);
        if (result.ok()) {
            mapped_ = std::move(result.value());
        }
    } else {
        auto result = MappedFile::open_read(path);
        if (result.ok()) {
            mapped_ = std::move(result.value());
        }
    }
}

ScopedMapping::~ScopedMapping() = default;

ScopedMapping::ScopedMapping(ScopedMapping&& other) noexcept
    : mapped_(std::move(other.mapped_)) {}

ScopedMapping& ScopedMapping::operator=(ScopedMapping&& other) noexcept {
    if (this != &other) {
        mapped_ = std::move(other.mapped_);
    }
    return *this;
}

// file namespace utilities

namespace file {

bool exists(const std::string& path) noexcept {
    return access(path.c_str(), F_OK) == 0;
}

Result<usize> size(const std::string& path) noexcept {
    struct stat st;
    if (stat(path.c_str(), &st) < 0) {
        return make_error<usize>(StatusCode::FileStatFailed, "Failed to stat file");
    }
    return static_cast<usize>(st.st_size);
}

Status remove(const std::string& path) noexcept {
    if (::remove(path.c_str()) < 0) {
        return Status(StatusCode::FileRemoveFailed, "Failed to remove file");
    }
    return Status::OK();
}

Status rename(const std::string& old_path, const std::string& new_path) noexcept {
    if (::rename(old_path.c_str(), new_path.c_str()) < 0) {
        return Status(StatusCode::FileRenameFailed, "Failed to rename file");
    }
    return Status::OK();
}

Status mkdir(const std::string& path) noexcept {
    // Create with parents
    std::string current;
    for (usize i = 0; i < path.size(); ++i) {
        if (path[i] == '/' && i > 0) {
            current = path.substr(0, i);
            if (!exists(current)) {
                if (::mkdir(current.c_str(), 0755) < 0 && errno != EEXIST) {
                    return Status(StatusCode::DirectoryCreateFailed, 
                                 "Failed to create directory");
                }
            }
        }
    }
    // Create final directory
    if (::mkdir(path.c_str(), 0755) < 0 && errno != EEXIST) {
        return Status(StatusCode::DirectoryCreateFailed, "Failed to create directory");
    }
    return Status::OK();
}

Status sync_dir(const std::string& path) noexcept {
    int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return Status(StatusCode::FileOpenFailed, "Failed to open directory");
    }
    
    if (fsync(fd) < 0) {
        ::close(fd);
        return Status(StatusCode::FileSyncFailed, "Failed to sync directory");
    }
    
    ::close(fd);
    return Status::OK();
}

Result<AlignedBuffer> read_all(const std::string& path) noexcept {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return make_error<AlignedBuffer>(StatusCode::FileOpenFailed, 
                                          "Failed to open file");
    }
    
    struct stat st;
    if (fstat(fd, &st) < 0) {
        ::close(fd);
        return make_error<AlignedBuffer>(StatusCode::FileStatFailed, 
                                          "Failed to stat file");
    }
    
    usize size = static_cast<usize>(st.st_size);
    AlignedBuffer buffer;
    auto status = buffer.allocate(size);
    if (!status.ok()) {
        ::close(fd);
        return make_error<AlignedBuffer>(status.code(), status.message());
    }
    
    usize offset = 0;
    while (offset < size) {
        ssize_t n = ::read(fd, buffer.data() + offset, size - offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return make_error<AlignedBuffer>(StatusCode::FileReadFailed, 
                                              "Failed to read file");
        }
        if (n == 0) break;  // EOF
        offset += static_cast<usize>(n);
    }
    
    ::close(fd);
    return buffer;
}

Status write_all(const std::string& path, const byte* data, usize size) noexcept {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return Status(StatusCode::FileOpenFailed, "Failed to open file for writing");
    }
    
    usize offset = 0;
    while (offset < size) {
        ssize_t n = ::write(fd, data + offset, size - offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return Status(StatusCode::FileWriteFailed, "Failed to write file");
        }
        offset += static_cast<usize>(n);
    }
    
    if (fsync(fd) < 0) {
        ::close(fd);
        return Status(StatusCode::FileSyncFailed, "Failed to sync file");
    }
    
    ::close(fd);
    return Status::OK();
}

Status append(const std::string& path, const byte* data, usize size) noexcept {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return Status(StatusCode::FileOpenFailed, "Failed to open file for append");
    }
    
    usize offset = 0;
    while (offset < size) {
        ssize_t n = ::write(fd, data + offset, size - offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return Status(StatusCode::FileWriteFailed, "Failed to append to file");
        }
        offset += static_cast<usize>(n);
    }
    
    ::close(fd);
    return Status::OK();
}

Status sync(const std::string& path) noexcept {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return Status(StatusCode::FileOpenFailed, "Failed to open file for sync");
    }
    
    if (fsync(fd) < 0) {
        ::close(fd);
        return Status(StatusCode::FileSyncFailed, "Failed to sync file");
    }
    
    ::close(fd);
    return Status::OK();
}

std::string temp_dir() noexcept {
    const char* tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    return std::string(tmp);
}

Result<std::string> temp_file(const std::string& prefix) noexcept {
    std::string path = temp_dir() + "/" + prefix + "XXXXXX";
    std::vector<char> buf(path.begin(), path.end());
    buf.push_back('\0');
    
    int fd = mkstemp(buf.data());
    if (fd < 0) {
        return make_error<std::string>(StatusCode::FileOpenFailed, 
                                        "Failed to create temp file");
    }
    
    ::close(fd);
    return std::string(buf.data());
}

} // namespace file

} // namespace vectortick
