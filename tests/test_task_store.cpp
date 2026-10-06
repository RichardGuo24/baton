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

TEST(TaskStore, CreateThenClaimOldestOpen) {
    TaskStore s;
    run(s, req(Op::Create), T0);
    run(s, req(Op::Create), T0);
    json claimed = run(s, req(Op::Claim, "a"), T0);
    EXPECT_EQ(s.find(1)->state, TaskState::Claimed);
    EXPECT_EQ(s.find(1)->holder, "a");
    EXPECT_EQ(s.find(2)->state, TaskState::Open);
    (void)claimed;
}

TEST(TaskStore, SecondClaimIsRefused) {
    TaskStore s;
    run(s, req(Op::Create), T0);
    run(s, req(Op::Claim, "a", 1), T0);
    EXPECT_EQ(error_of(s, req(Op::Claim, "b", 1), T0), ErrorCode::TaskTaken);
}

TEST(TaskStore, PrepareDoesNotChangeState) {
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

TEST(TaskStore, OnlyHolderCanComplete) {
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

// ---- Week 1: create, claim, complete (no leases, notes or locks yet) ----

TEST(TaskStore, CreateAssignsSequentialIdsAndReturnsTheTask) {
    TaskStore s;
    json a = run(s, req(Op::Create, "", std::nullopt, {}, "first"), T0);
    json b = run(s, req(Op::Create, "", std::nullopt, {}, "second"), T0);
    EXPECT_EQ(a["task"]["id"], 1);
    EXPECT_EQ(b["task"]["id"], 2);
    EXPECT_EQ(b["task"]["title"], "second");
    EXPECT_EQ(b["task"]["state"], "open");
    EXPECT_EQ(s.size(), 2u);
}

TEST(TaskStore, PrepareCreateDoesNotConsumeAnId) {
    // Ids are assigned in prepare() but only "used up" by apply(). Two prepares in a row without
    // an apply must propose the same id; this is why the event loop applies before the next prepare.
    TaskStore s;
    auto p1 = s.prepare(req(Op::Create), T0);
    auto p2 = s.prepare(req(Op::Create), T0);
    EXPECT_EQ(std::get<CreateM>(std::get<Mutation>(p1)).id, 1u);
    EXPECT_EQ(std::get<CreateM>(std::get<Mutation>(p2)).id, 1u);
}

TEST(TaskStore, ClaimResponseHasTaskAndLease) {
    TaskStore s;
    run(s, req(Op::Create), T0);
    json r = run(s, req(Op::Claim, "a", 1, {"src/"}), T0);
    EXPECT_EQ(r["task"]["id"], 1);
    EXPECT_EQ(r["task"]["state"], "claimed");
    EXPECT_EQ(r["task"]["holder"], "a");
    EXPECT_EQ(r["task"]["lease_expires_in_ms"], kLeaseMs);
    EXPECT_EQ(r["lease_expiry"], T0 + kLeaseMs);
    EXPECT_EQ(s.find(1)->attempts, 1u);
    EXPECT_EQ(s.find(1)->lease_gen, 1u);
}

TEST(TaskStore, ClaimWithoutIdSkipsClaimedAndDoneTasks) {
    TaskStore s;
    for (int i = 0; i < 3; ++i) run(s, req(Op::Create), T0);
    run(s, req(Op::Claim, "a", 1), T0);
    run(s, req(Op::Complete, "a", 1), T0);
    run(s, req(Op::Claim, "b", 2), T0);
    run(s, req(Op::Claim, "c"), T0);  // oldest open is now 3
    EXPECT_EQ(s.find(3)->holder, "c");
}

TEST(TaskStore, ClaimErrors) {
    TaskStore s;
    EXPECT_EQ(error_of(s, req(Op::Claim, "a"), T0), ErrorCode::NoOpenTasks);
    EXPECT_EQ(error_of(s, req(Op::Claim, "a", 42), T0), ErrorCode::NotFound);
    run(s, req(Op::Create), T0);
    run(s, req(Op::Claim, "a", 1), T0);
    EXPECT_EQ(error_of(s, req(Op::Claim, "a", 1), T0), ErrorCode::TaskTaken);  // already yours
    EXPECT_EQ(error_of(s, req(Op::Claim, "b"), T0), ErrorCode::NoOpenTasks);
    run(s, req(Op::Complete, "a", 1), T0);
    EXPECT_EQ(error_of(s, req(Op::Claim, "b", 1), T0), ErrorCode::TaskTaken);  // done
}

TEST(TaskStore, TakenErrorSuggestsOpenTasks) {
    TaskStore s;
    for (int i = 0; i < 3; ++i) run(s, req(Op::Create), T0);
    run(s, req(Op::Claim, "a", 1), T0);
    auto err = std::get<StoreError>(s.prepare(req(Op::Claim, "b", 1), T0));
    EXPECT_EQ(err.code, ErrorCode::TaskTaken);
    EXPECT_NE(err.message.find("claimed by a"), std::string::npos) << err.message;
    EXPECT_NE(err.message.find("2, 3"), std::string::npos) << err.message;
    EXPECT_EQ(err.extra["held_by"]["agent"], "a");
}

TEST(TaskStore, CompleteErrors) {
    TaskStore s;
    EXPECT_EQ(error_of(s, req(Op::Complete, "a", 1), T0), ErrorCode::NotFound);
    run(s, req(Op::Create), T0);
    EXPECT_EQ(error_of(s, req(Op::Complete, "a", 1), T0), ErrorCode::NotHolder);  // still open
    run(s, req(Op::Claim, "a", 1), T0);
    run(s, req(Op::Complete, "a", 1, {}, "shipped"), T0);
    EXPECT_EQ(s.find(1)->summary, "shipped");
    EXPECT_EQ(s.find(1)->holder, "");
    EXPECT_EQ(error_of(s, req(Op::Complete, "a", 1), T0), ErrorCode::NotHolder);  // already done
}

TEST(TaskStore, UnsupportedOpsAreRefusedNotCrashed) {
    TaskStore s;
    run(s, req(Op::Create), T0);
    run(s, req(Op::Claim, "a", 1), T0);
    EXPECT_EQ(error_of(s, req(Op::Note, "a", 1, {}, "x"), T0), ErrorCode::BadRequest);
    EXPECT_EQ(error_of(s, req(Op::Release, "a", 1), T0), ErrorCode::BadRequest);
    EXPECT_EQ(error_of(s, req(Op::Lock, "a", 1, {"src/"}), T0), ErrorCode::BadRequest);
}

TEST(TaskStore, ReplayingCreateClaimCompleteRebuildsSameState) {
    // Week-1 version of ReplayingMutationsRebuildsSameState (which also needs notes and locks).
    TaskStore live;
    std::vector<Mutation> log;
    auto step = [&](const Request& r, TimeMs now) {
        PrepareResult p = live.prepare(r, now);
        ASSERT_TRUE(std::holds_alternative<Mutation>(p));
        log.push_back(std::get<Mutation>(p));
        live.apply(log.back());
    };
    step(req(Op::Create), T0);
    step(req(Op::Create), T0 + 1);
    step(req(Op::Create), T0 + 2);
    step(req(Op::Claim, "a", 1, {"src/"}), T0 + 3);
    step(req(Op::Complete, "a", 1, {}, "done"), T0 + 4);
    step(req(Op::Claim, "b"), T0 + 5);

    // Replay through the WAL encoding too, the way the server will from week 2.
    TaskStore replayed;
    for (const auto& m : log) replayed.apply(from_record(to_record(m)));
    EXPECT_EQ(replayed.list(std::nullopt, T0 + 10), live.list(std::nullopt, T0 + 10));
    // And the next id continues where the live store left off.
    auto next = replayed.prepare(req(Op::Create), T0);
    EXPECT_EQ(std::get<CreateM>(std::get<Mutation>(next)).id, 4u);
}
