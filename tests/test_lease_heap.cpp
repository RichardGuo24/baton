// Spec tests for LeaseHeap. Remove DISABLED_ as you implement lease_heap.cpp.

#include <gtest/gtest.h>

#include "baton/lease_heap.h"

using namespace baton;

TEST(LeaseHeap, DISABLED_EmptyHasNoExpiry) {
    LeaseHeap h;
    EXPECT_FALSE(h.next_expiry().has_value());
    EXPECT_TRUE(h.pop_due(1'000'000).empty());
}

TEST(LeaseHeap, DISABLED_NextExpiryIsEarliest) {
    LeaseHeap h;
    h.push(300, 1, 1);
    h.push(100, 2, 1);
    h.push(200, 3, 1);
    EXPECT_EQ(h.next_expiry(), std::optional<TimeMs>(100));
}

TEST(LeaseHeap, DISABLED_PopDueReturnsOnlyExpiredInOrder) {
    LeaseHeap h;
    h.push(300, 1, 1);
    h.push(100, 2, 1);
    h.push(200, 3, 1);
    auto due = h.pop_due(200);
    ASSERT_EQ(due.size(), 2u);
    EXPECT_EQ(due[0].task, 2u);
    EXPECT_EQ(due[1].task, 3u);
    EXPECT_EQ(h.size(), 1u);
    EXPECT_EQ(h.next_expiry(), std::optional<TimeMs>(300));
}

TEST(LeaseHeap, DISABLED_KeepsStaleEntriesForCallerToSkip) {
    // A heartbeat pushes a newer entry for the same task; the old one stays until popped.
    LeaseHeap h;
    h.push(100, 1, 1);
    h.push(160, 1, 1);
    auto due = h.pop_due(100);
    ASSERT_EQ(due.size(), 1u);
    EXPECT_EQ(due[0].expiry, 100u);  // caller sees task 1's lease is now 160 and skips this
}
