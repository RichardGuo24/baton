// Spec tests for the WAL. Remove DISABLED_ as you implement wal.cpp.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "baton/wal.h"

using namespace baton;
namespace fs = std::filesystem;

namespace {
std::string temp_path(const char* name) {
    return (fs::temp_directory_path() / (std::string("baton_test_") + name + "_" +
                                         std::to_string(::getpid()) + ".wal"))
        .string();
}
std::string read_all(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
}  // namespace

TEST(Wal, DISABLED_Crc32KnownVector) {
    EXPECT_EQ(crc32("123456789"), 0xCBF43926u);
    EXPECT_EQ(crc32(""), 0u);
}

TEST(Wal, DISABLED_EncodeDecodeRoundTrip) {
    Record r{RecordType::Note, R"({"task":1,"text":"hi"})"};
    std::string bytes = encode_record(r);
    EXPECT_EQ(bytes.size(), kRecordHeaderBytes + 1 + r.payload.size());
    DecodeResult d = decode_record(bytes);
    ASSERT_EQ(d.status, DecodeStatus::Ok);
    EXPECT_EQ(d.record, r);
    EXPECT_EQ(d.consumed, bytes.size());
}

TEST(Wal, DISABLED_DecodeDetectsTornAndCorrupt) {
    std::string bytes = encode_record(Record{RecordType::Create, "{}"});
    EXPECT_EQ(decode_record(std::string_view(bytes).substr(0, bytes.size() - 1)).status,
              DecodeStatus::Incomplete);
    EXPECT_EQ(decode_record(std::string_view(bytes).substr(0, 3)).status, DecodeStatus::Incomplete);
    std::string flipped = bytes;
    flipped.back() ^= 0x01;
    EXPECT_EQ(decode_record(flipped).status, DecodeStatus::Corrupt);
}

TEST(Wal, DISABLED_WriterThenReplay) {
    std::string p = temp_path("replay");
    fs::remove(p);
    {
        WalWriter w(p);
        w.append(Record{RecordType::Create, "a"});
        w.append(Record{RecordType::Note, "b"});
        EXPECT_GT(w.pending_bytes(), 0u);
        w.sync();
        EXPECT_EQ(w.pending_bytes(), 0u);
    }
    ReplayResult r = replay_wal(p);
    ASSERT_EQ(r.records.size(), 2u);
    EXPECT_EQ(r.records[1].payload, "b");
    EXPECT_FALSE(r.truncated_tail);
    fs::remove(p);
}

TEST(Wal, DISABLED_MissingFileIsEmptyLog) {
    std::string p = temp_path("missing");
    fs::remove(p);
    ReplayResult r = replay_wal(p);
    EXPECT_TRUE(r.records.empty());
}

TEST(Wal, DISABLED_TornTailIsTruncated) {
    std::string p = temp_path("torn");
    fs::remove(p);
    std::string good = encode_record(Record{RecordType::Create, "ok"});
    std::string torn = encode_record(Record{RecordType::Note, "lost"});
    {
        std::ofstream f(p, std::ios::binary);
        f << good << torn.substr(0, torn.size() - 2);  // simulate a crash mid-write
    }
    ReplayResult r = replay_wal(p);
    ASSERT_EQ(r.records.size(), 1u);
    EXPECT_TRUE(r.truncated_tail);
    EXPECT_EQ(r.valid_bytes, good.size());
    EXPECT_EQ(read_all(p).size(), good.size());  // file was actually cut back
    fs::remove(p);
}

TEST(Wal, DISABLED_CorruptionInTheMiddleThrows) {
    std::string p = temp_path("middle");
    fs::remove(p);
    std::string a = encode_record(Record{RecordType::Create, "one"});
    std::string b = encode_record(Record{RecordType::Create, "two"});
    a.back() ^= 0x01;  // damage the first record, then a valid one follows
    {
        std::ofstream f(p, std::ios::binary);
        f << a << b;
    }
    EXPECT_THROW(replay_wal(p), WalError);
    fs::remove(p);
}
