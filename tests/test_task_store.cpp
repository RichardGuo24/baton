// Spec tests for TaskStore. Remove DISABLED_ as you implement task_store.cpp.
// These run prepare() + apply() directly, the same way the server does (minus the WAL).

#include <gtest/gtest.h>

#include "baton/task_store.h"

using namespace baton;
using nlohmann::json;

namespace {

Request req(Op op, std::string agent = "", std::optional<TaskId> task = std::nullopt,
            std::vector<std::string> paths = {}, std::string text = "") {
    Request r;
    r.op = op;
    r.agent = std::move(agent);
    r.task = task;
    r.paths = std::move(paths);
    r.text = std::move(text);
    if (op == Op::Create) r.title = r.text.empty() ? "task" : r.text;
    return r;
}

// prepare + apply; fails the test if prepare returns an error.
json run(TaskStore& s, const Request& r, TimeMs now) {
    PrepareResult p = s.prepare(r, now);
    if (auto* err = std::get_if<StoreError>(&p)) {
        ADD_FAILURE() << "unexpected error: " << err->message;
        return {};
    }
    return s.apply(std::get<Mutation>(p));
}

ErrorCode error_of(TaskStore& s, const Request& r, TimeMs now) {
    PrepareResult p = s.prepare(r, now);
    auto* err = std::get_if<StoreError>(&p);
    EXPECT_NE(err, nullptr) << "expected an error";
    return err ? err->code : ErrorCode::Internal;
}

constexpr TimeMs T0 = 1'000'000;

}  // namespace

TEST(TaskStore, DISABLED_CreateThenClaimOldestOpen) {
    TaskStore s;
    run(s, req(Op::Create), T0);
    run(s, req(Op::Create), T0);
    json claimed = run(s, req(Op::Claim, "a"), T0);
    EXPECT_EQ(s.find(1)->state, TaskState::Claimed);
    EXPECT_EQ(s.find(1)->holder, "a");
    EXPECT_EQ(s.find(2)->state, TaskState::Open);
    (void)claimed;
}

TEST(TaskStore, DISABLED_SecondClaimIsRefused) {
    TaskStore s;
    run(s, req(Op::Create), T0);
    run(s, req(Op::Claim, "a", 1), T0);
    EXPECT_EQ(error_of(s, req(Op::Claim, "b", 1), T0), ErrorCode::TaskTaken);
}

TEST(TaskStore, DISABLED_PrepareDoesNotChangeState) {
    TaskStore s;
    run(s, req(Op::Create), T0);
    (void)s.prepare(req(Op::Claim, "a", 1), T0);
    EXPECT_EQ(s.find(1)->state, TaskState::Open);
}

TEST(TaskStore, DISABLED_ClaimLocksPathsAndConflictsAreReported) {
    TaskStore s;
    run(s, req(Op::Create), T0);
    run(s, req(Op::Create), T0);
    run(s, req(Op::Claim, "a", 1, {"models/loader.py"}), T0);
    EXPECT_EQ(error_of(s, req(Op::Claim, "b", 2, {"models/"}), T0), ErrorCode::PathLocked);
}

TEST(TaskStore, DISABLED_OnlyHolderCanComplete) {
    TaskStore s;
    run(s, req(Op::Create), T0);
    run(s, req(Op::Claim, "a", 1), T0);
    EXPECT_EQ(error_of(s, req(Op::Complete, "b", 1), T0), ErrorCode::NotHolder);
    run(s, req(Op::Complete, "a", 1), T0);
    EXPECT_EQ(s.find(1)->state, TaskState::Done);
}

TEST(TaskStore, DISABLED_CompleteReleasesLocks) {
    TaskStore s;
    run(s, req(Op::Create), T0);
    run(s, req(Op::Create), T0);
    run(s, req(Op::Claim, "a", 1, {"src/"}), T0);
    run(s, req(Op::Complete, "a", 1), T0);
    run(s, req(Op::Claim, "b", 2, {"src/parser/"}), T0);  // would fail if src/ were still held
}

TEST(TaskStore, DISABLED_LeaseExpiresWithoutHeartbeat) {
    TaskStore s;
    run(s, req(Op::Create), T0);
    run(s, req(Op::Claim, "a", 1, {"api/"}), T0);
    EXPECT_TRUE(s.collect_expired(T0 + kLeaseMs - 1).empty());
    auto expired = s.collect_expired(T0 + kLeaseMs);
    ASSERT_EQ(expired.size(), 1u);
    s.apply(Mutation{expired[0]});
    EXPECT_EQ(s.find(1)->state, TaskState::Open);
    EXPECT_TRUE(s.find(1)->paths.empty());
}

TEST(TaskStore, DISABLED_HeartbeatExtendsLeaseAndOldEntryIsStale) {
    TaskStore s;
    run(s, req(Op::Create), T0);
    run(s, req(Op::Claim, "a", 1), T0);
    auto hb = s.heartbeat("a", 1, T0 + 30'000);
    ASSERT_TRUE(std::holds_alternative<TimeMs>(hb));
    EXPECT_TRUE(s.collect_expired(T0 + kLeaseMs).empty());  // original entry is stale now
    EXPECT_EQ(s.collect_expired(T0 + 30'000 + kLeaseMs).size(), 1u);
}

TEST(TaskStore, DISABLED_NextClaimerSeesNotes) {
    TaskStore s;
    run(s, req(Op::Create), T0);
    run(s, req(Op::Claim, "c", 1), T0);
    run(s, req(Op::Note, "c", 1, {}, "loader refactored; tests failing on empty payload"), T0);
    for (const auto& e : s.collect_expired(T0 + kLeaseMs)) s.apply(Mutation{e});
    json claimed = run(s, req(Op::Claim, "a", 1), T0 + kLeaseMs);
    EXPECT_EQ(s.find(1)->notes.size(), 1u);
    EXPECT_EQ(s.find(1)->attempts, 2u);
    (void)claimed;
}

TEST(TaskStore, DISABLED_ReplayingMutationsRebuildsSameState) {
    TaskStore live;
    std::vector<Mutation> log;
    auto step = [&](const Request& r, TimeMs now) {
        PrepareResult p = live.prepare(r, now);
        ASSERT_TRUE(std::holds_alternative<Mutation>(p));
        log.push_back(std::get<Mutation>(p));
        live.apply(log.back());
    };
    step(req(Op::Create), T0);
    step(req(Op::Create), T0);
    step(req(Op::Claim, "a", 1, {"src/"}), T0);
    step(req(Op::Note, "a", 1, {}, "progress"), T0);
    step(req(Op::Complete, "a", 1), T0);

    TaskStore replayed;
    for (const auto& m : log) replayed.apply(m);
    EXPECT_EQ(replayed.list(std::nullopt, T0), live.list(std::nullopt, T0));
}
