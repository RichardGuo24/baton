#include "baton/lease_heap.h"

#include "baton/todo.h"

namespace baton {

void LeaseHeap::push(TimeMs expiry, TaskId task, uint64_t gen) {
    (void)expiry;
    (void)task;
    (void)gen;
    todo("LeaseHeap::push");
}

std::optional<TimeMs> LeaseHeap::next_expiry() const { todo("LeaseHeap::next_expiry"); }

std::vector<LeaseEntry> LeaseHeap::pop_due(TimeMs now) {
    (void)now;
    todo("LeaseHeap::pop_due");
}

}  // namespace baton
