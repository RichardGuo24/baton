#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace baton {

using TaskId = uint64_t;
using AgentId = std::string;
using TimeMs = uint64_t;  // milliseconds since the Unix epoch

// How long a claim stays valid without a heartbeat. Agents (via the shim) heartbeat every 20 s.
inline constexpr TimeMs kLeaseMs = 60'000;

enum class TaskState : uint8_t { Open, Claimed, Done };

struct Note {
    AgentId agent;
    TimeMs ts_ms = 0;
    std::string text;
};

struct Task {
    TaskId id = 0;
    std::string title;
    std::string description;
    TaskState state = TaskState::Open;
    AgentId holder;                  // empty when Open or Done
    TimeMs lease_expiry = 0;         // 0 when not Claimed
    uint64_t lease_gen = 0;          // bumped on every claim; lets us discard stale heap entries
    std::vector<std::string> paths;  // paths currently locked under this task
    std::vector<Note> notes;         // every note from every attempt, oldest first
    uint32_t attempts = 0;           // number of times this task has been claimed
    std::string summary;             // set on completion
};

// Machine-readable error codes sent back to agents.
enum class ErrorCode {
    BadRequest,
    NotFound,
    TaskTaken,
    NotHolder,
    NoOpenTasks,
    PathLocked,
    Internal,
};

const char* to_string(ErrorCode code);
const char* to_string(TaskState state);

}  // namespace baton
