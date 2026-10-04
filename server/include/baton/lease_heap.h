#pragma once

// Min-heap of lease expiries with lazy deletion. A heartbeat pushes a new entry instead of
// updating the old one; stale entries are skipped by the caller when they reach the top
// (the task's lease_gen or lease_expiry no longer match the entry).

#include <cstddef>
#include <functional>
#include <optional>
#include <queue>
#include <vector>

#include "baton/types.h"

namespace baton {

struct LeaseEntry {
    TimeMs expiry = 0;
    TaskId task = 0;
    uint64_t gen = 0;
    bool operator>(const LeaseEntry& o) const { return expiry > o.expiry; }
};

class LeaseHeap {
   public:
    // TODO(richard)
    void push(TimeMs expiry, TaskId task, uint64_t gen);

    // Earliest expiry in the heap (stale or not), or nullopt if empty. The event loop uses this
    // to compute its epoll_wait timeout. TODO(richard)
    std::optional<TimeMs> next_expiry() const;

    // Pop and return every entry with expiry <= now, earliest first. TODO(richard)
    std::vector<LeaseEntry> pop_due(TimeMs now);

    size_t size() const { return heap_.size(); }
    bool empty() const { return heap_.empty(); }
    void clear() { heap_ = {}; }

   private:
    std::priority_queue<LeaseEntry, std::vector<LeaseEntry>, std::greater<>> heap_;
};

}  // namespace baton
