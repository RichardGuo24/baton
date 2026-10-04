#pragma once

// All in-memory state and the rules for changing it. No I/O and no clock reads: time is passed in,
// which keeps this class deterministic and easy to test.
//
// Write path used by the server for every mutating request:
//   1. prepare(req, now)  -> validate against current state, build a Mutation (or an error)
//   2. wal.append(to_record(mutation)); ... wal.sync()
//   3. apply(mutation)    -> change state, return the response body
// Replay on startup calls apply() on every logged Mutation, in order, through the same code.

#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "baton/lease_heap.h"
#include "baton/mutation.h"
#include "baton/path_trie.h"
#include "baton/protocol.h"
#include "baton/types.h"

namespace baton {

struct StoreError {
    ErrorCode code = ErrorCode::Internal;
    std::string message;  // a sentence an LLM agent can act on
    nlohmann::json extra = nlohmann::json::object();  // e.g. {"held_by": {...}}
};

using PrepareResult = std::variant<Mutation, StoreError>;

class TaskStore {
   public:
    // Validate a mutating request (create, claim, lock, note, complete, release) and build the
    // Mutation that would carry it out. Must NOT change any state.
    // Examples of errors: claiming a non-open task -> TaskTaken; acting on a task you do not hold
    // -> NotHolder; lock conflict -> PathLocked with held_by details and open task suggestions.
    // TODO(richard)
    PrepareResult prepare(const Request& req, TimeMs now) const;

    // Apply a Mutation. Must be deterministic: replay depends on it producing the same state.
    // Returns the response body for the client (e.g. the claimed task including all prior notes).
    // TODO(richard)
    nlohmann::json apply(const Mutation& m);

    // Renew the lease on a task the agent holds (and so its locks). Not logged.
    // TODO(richard): set lease_expiry = now + kLeaseMs and push onto the heap.
    std::variant<TimeMs, StoreError> heartbeat(const AgentId& agent, TaskId task, TimeMs now);

    // Read-only view for the `list` op.
    nlohmann::json list(std::optional<TaskState> filter, TimeMs now) const;

    // Pop due heap entries and return an ExpireM for each one that is still current (not stale).
    // The caller logs them, syncs, then applies them. TODO(richard)
    std::vector<ExpireM> collect_expired(TimeMs now);

    // After replay: give every Claimed task a fresh lease of now + kLeaseMs and rebuild the heap.
    // TODO(richard)
    void rearm_leases(TimeMs now);

    std::optional<TimeMs> next_expiry() const { return leases_.next_expiry(); }
    const Task* find(TaskId id) const;
    size_t size() const { return tasks_.size(); }

   private:
    std::unordered_map<TaskId, Task> tasks_;
    std::set<TaskId> open_;  // ordered: oldest open task first
    PathTrie locks_;
    LeaseHeap leases_;
    TaskId next_id_ = 1;
};

}  // namespace baton
