#include <gtest/gtest.h>

#include "baton/mutation.h"

using namespace baton;

namespace {
template <class T>
T round_trip(const T& m) {
    Mutation back = from_record(to_record(Mutation{m}));
    return std::get<T>(back);
}
}  // namespace

TEST(Mutation, RoundTripsEveryType) {
    auto c = round_trip(CreateM{7, "Add route", "details", 123});
    EXPECT_EQ(c.id, 7u);
    EXPECT_EQ(c.title, "Add route");

    auto cl = round_trip(ClaimM{7, "agent-a", {"src/", "README.md"}, 60123, 123});
    EXPECT_EQ(cl.agent, "agent-a");
    EXPECT_EQ(cl.paths.size(), 2u);
    EXPECT_EQ(cl.expiry, 60123u);

    EXPECT_EQ(round_trip(LockM{7, {"docs/"}}).paths.at(0), "docs/");
    EXPECT_EQ(round_trip(NoteM{7, "a", "half done", 5}).text, "half done");
    EXPECT_EQ(round_trip(CompleteM{7, "a", "shipped", 6}).summary, "shipped");
    EXPECT_EQ(round_trip(ReleaseM{7, "a", "stuck", 7}).reason, "stuck");
    EXPECT_EQ(round_trip(ExpireM{7, 3, 8}).lease_gen, 3u);
}

TEST(Mutation, RecordTypeMatchesVariant) {
    EXPECT_EQ(to_record(Mutation{NoteM{1, "a", "x", 0}}).type, RecordType::Note);
    EXPECT_EQ(to_record(Mutation{ExpireM{1, 1, 0}}).type, RecordType::Expire);
}

TEST(Mutation, BadPayloadThrows) {
    EXPECT_THROW(from_record(Record{RecordType::Claim, "{}"}), std::runtime_error);
    EXPECT_THROW(from_record(Record{RecordType::Note, "garbage"}), std::runtime_error);
}
