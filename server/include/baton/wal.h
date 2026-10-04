#pragma once

// Write-ahead log. Every Mutation is appended here and fsync'd BEFORE it is applied in memory and
// before the client gets "ok". On startup the whole file is replayed.
//
// On-disk record layout (little-endian):
//   [ len   : u32 ]  length of (type + payload)
//   [ crc32 : u32 ]  CRC-32 (IEEE, same as zlib) of (type + payload)
//   [ type  : u8  ]  RecordType
//   [ payload ... ]  len - 1 bytes

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace baton {

enum class RecordType : uint8_t {
    Create = 1,
    Claim = 2,
    Lock = 3,
    Note = 4,
    Complete = 5,
    Release = 6,
    Expire = 7,
};

struct Record {
    RecordType type = RecordType::Create;
    std::string payload;
    bool operator==(const Record&) const = default;
};

inline constexpr size_t kRecordHeaderBytes = 4 + 4;  // len + crc32

// TODO(richard): standard CRC-32 (polynomial 0xEDB88320, reflected). crc32("123456789") == 0xCBF43926.
uint32_t crc32(std::string_view data);

// TODO(richard): serialize one record into the layout above.
std::string encode_record(const Record& r);

enum class DecodeStatus {
    Ok,          // a full, valid record was decoded
    Incomplete,  // buf ends partway through a record (torn write at the tail)
    Corrupt,     // the length was readable but the CRC does not match
};

struct DecodeResult {
    DecodeStatus status = DecodeStatus::Incomplete;
    Record record;
    size_t consumed = 0;  // bytes used from the front of buf (only meaningful when Ok)
};

// TODO(richard): decode one record from the front of buf.
DecodeResult decode_record(std::string_view buf);

class WalError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

class WalWriter {
   public:
    // TODO(richard): open with O_WRONLY | O_CREAT | O_APPEND. Throw WalError on failure.
    explicit WalWriter(const std::string& path);
    ~WalWriter();
    WalWriter(const WalWriter&) = delete;
    WalWriter& operator=(const WalWriter&) = delete;

    // Buffers the encoded record in memory. Nothing touches disk yet.
    void append(const Record& r);

    // Writes every buffered byte (handle short writes!), then fsync()s. Throws WalError on any
    // failure. This is the group-commit point: the event loop calls it once per loop iteration.
    void sync();

    size_t pending_bytes() const { return buffer_.size(); }

   private:
    int fd_ = -1;
    std::string buffer_;
};

struct ReplayResult {
    std::vector<Record> records;
    uint64_t valid_bytes = 0;     // file offset just past the last good record
    bool truncated_tail = false;  // true if a torn/corrupt final record was cut off
};

// TODO(richard): read the whole log (a missing file means an empty log).
//  - Incomplete or Corrupt record at the very end of the file: truncate the file to valid_bytes
//    (ftruncate) and return what was read. This is the normal crash case.
//  - Corrupt record followed by more data: real damage. Throw WalError rather than guess.
ReplayResult replay_wal(const std::string& path);

}  // namespace baton
