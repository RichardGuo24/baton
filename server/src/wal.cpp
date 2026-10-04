#include "baton/wal.h"

// Headers you will likely need: <fcntl.h> (open), <unistd.h> (write, fsync, close, ftruncate),
// <cerrno> and <cstring> (strerror) for error messages.
#include "baton/todo.h"

namespace baton {

uint32_t crc32(std::string_view data) {
    (void)data;
    todo("crc32");
}

std::string encode_record(const Record& r) {
    (void)r;
    todo("encode_record");
}

DecodeResult decode_record(std::string_view buf) {
    (void)buf;
    todo("decode_record");
}

WalWriter::WalWriter(const std::string& path) {
    (void)path;
    todo("WalWriter::WalWriter");
}

WalWriter::~WalWriter() {
    // TODO(richard): close fd_ if it is open.
}

void WalWriter::append(const Record& r) {
    (void)r;
    todo("WalWriter::append");
}

void WalWriter::sync() { todo("WalWriter::sync"); }

ReplayResult replay_wal(const std::string& path) {
    (void)path;
    todo("replay_wal");
}

}  // namespace baton
