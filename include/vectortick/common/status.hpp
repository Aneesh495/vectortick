#pragma once

#include "types.hpp"
#include <system_error>
#include <string_view>
#include <string>

namespace vectortick {

// Error codes for the entire system
enum class StatusCode : i32 {
    // Success
    OK = 0,
    
    // Generic errors (1-99)
    Unknown = 1,
    InvalidArgument = 2,
    OutOfRange = 3,
    BufferTooSmall = 4,
    NotImplemented = 5,
    InternalError = 6,
    
    // Memory errors (100-199)
    AllocationFailed = 100,
    OutOfMemory = 101,
    InvalidAlignment = 102,
    
    // File I/O errors (200-299)
    FileNotFound = 200,
    FileOpenFailed = 201,
    FileReadFailed = 202,
    FileWriteFailed = 203,
    FileSeekFailed = 204,
    FileStatFailed = 205,
    FileSyncFailed = 206,
    FileRenameFailed = 207,
    FileRemoveFailed = 208,
    DirectoryCreateFailed = 209,
    
    // Mmap errors (210-219)
    MmapFailed = 210,
    MunmapFailed = 211,
    MprotectFailed = 212,
    MsyncFailed = 213,
    InvalidMapping = 214,
    
    // Protocol errors (300-399)
    InvalidMagic = 300,
    InvalidVersion = 301,
    InvalidChecksum = 302,
    InvalidLength = 303,
    InvalidMessageType = 304,
    InvalidFlags = 305,
    InvalidPayload = 306,
    TruncatedFrame = 307,
    ReservedFieldNotZero = 308,
    SequenceNotMonotonic = 309,
    InvalidTimestamp = 310,
    
    // PCAP errors (320-339)
    PcapInvalidHeader = 320,
    PcapTruncatedRecord = 321,
    PcapUnsupportedLinkType = 322,
    PcapInvalidIPv4Header = 323,
    PcapInvalidUDPHeader = 324,
    PcapFragmentedPacket = 325,
    PcapInvalidVlan = 326,
    
    // Storage errors (400-499)
    SegmentInvalidHeader = 400,
    SegmentInvalidBlock = 401,
    SegmentInvalidFooter = 402,
    SegmentSchemaMismatch = 403,
    SegmentChecksumFailed = 404,
    SegmentCorrupted = 405,
    
    // Manifest errors (420-439)
    ManifestInvalid = 420,
    ManifestCorrupted = 421,
    ManifestGenerationGap = 422,
    
    // Journal errors (440-449)
    JournalInvalid = 440,
    JournalCorrupted = 441,
    JournalReplayFailed = 442,
    
    // Recovery errors (450-459)
    RecoveryFailed = 450,
    RecoveryInconsistent = 451,
    
    // Query errors (500-599)
    LexerError = 500,
    ParserError = 501,
    BinderError = 502,
    TypeError = 503,
    SemanticError = 504,
    InvalidQuery = 505,
    QueryTooComplex = 506,
    UnknownColumn = 507,
    UnknownTable = 508,
    
    // IR errors (600-699)
    IRVerificationFailed = 600,
    IRInvalidOpcode = 601,
    IRTypeMismatch = 602,
    IRInvalidBlock = 603,
    IRInvalidFunction = 604,
    
    // JIT errors (700-799)
    JITCompilationFailed = 700,
    JITCodeTooLarge = 701,
    JITRelocationFailed = 702,
    JITRegisterAllocationFailed = 703,
    JITMemoryAllocationFailed = 704,
    JITFeatureNotSupported = 705,
    
    // Network errors (800-899)
    SocketCreateFailed = 800,
    SocketBindFailed = 801,
    SocketRecvFailed = 802,
    SocketSendFailed = 803,
    SocketTimeout = 804,
    
    // Concurrency errors (900-999)
    QueueFull = 900,
    QueueEmpty = 901,
    WorkerFailed = 902,
    CancellationRequested = 903,
    
    // Limit errors (1000-1099)
    LimitExceeded = 1000,
    MaxRowsExceeded = 1001,
    MaxSegmentsExceeded = 1002,
    MaxFileSizeExceeded = 1003
};

// Status class for error handling
class [[nodiscard]] Status {
public:
    // Construct OK status
    Status() : code_(StatusCode::OK) {}
    
    // Construct from error code
    explicit Status(StatusCode code) : code_(code) {}
    
    // Construct from error code with message
    Status(StatusCode code, std::string_view message) 
        : code_(code), message_(message) {}
    
    // Check if OK
    [[nodiscard]] bool ok() const noexcept {
        return code_ == StatusCode::OK;
    }
    
    // Get error code
    [[nodiscard]] StatusCode code() const noexcept {
        return code_;
    }
    
    // Get message
    [[nodiscard]] std::string_view message() const noexcept {
        return message_;
    }
    
    // Conversion to bool (true if OK)
    [[nodiscard]] explicit operator bool() const noexcept {
        return ok();
    }
    
    // Static factory for OK
    static Status OK() { return Status(); }
    
    // Static factories for common errors
    static Status invalidArgument(std::string_view msg = "") {
        return Status(StatusCode::InvalidArgument, msg);
    }
    
    static Status outOfRange(std::string_view msg = "") {
        return Status(StatusCode::OutOfRange, msg);
    }
    
    static Status bufferTooSmall(std::string_view msg = "") {
        return Status(StatusCode::BufferTooSmall, msg);
    }

private:
    StatusCode code_;
    std::string message_;
};

// Macro for checking status and returning on error
#define VT_RETURN_IF_ERROR(expr) \
    do { \
        const auto _status = (expr); \
        if (!_status.ok()) { \
            return _status; \
        } \
    } while (0)

} // namespace vectortick
