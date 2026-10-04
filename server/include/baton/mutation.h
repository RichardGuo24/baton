#pragma once

// A Mutation is one state change. The server builds it, writes it to the WAL, then applies it.
// On startup, the WAL is replayed by decoding each record back into a Mutation and applying it
// with the exact same code path. That shared path is what makes recovery trustworthy.

#include <string>
#include <variant>
#include <vector>

#include "baton/types.h"
#include "baton/wal.h"

namespace baton {

struct CreateM {
    TaskId id = 0;
    std::string title;
    std::string description;
    TimeMs ts = 0;
};
struct ClaimM {
    TaskId task = 0;
    AgentId agent;
    std::vector<std::string> paths;
    TimeMs expiry = 0;
    TimeMs ts = 0;
};
struct LockM {
    TaskId task = 0;
    std::vector<std::string> paths;
};
struct NoteM {
    TaskId task = 0;
    AgentId agent;
    std::string text;
    TimeMs ts = 0;
};
struct CompleteM {
    TaskId task = 0;
    AgentId agent;
    std::string summary;
    TimeMs ts = 0;
};
struct ReleaseM {
    TaskId task = 0;
    AgentId agent;
    std::string reason;
    TimeMs ts = 0;
};
struct ExpireM {
    TaskId task = 0;
    uint64_t lease_gen = 0;
    TimeMs ts = 0;
};

using Mutation = std::variant<CreateM, ClaimM, LockM, NoteM, CompleteM, ReleaseM, ExpireM>;

// Payloads are JSON for v1: easy to debug with `strings baton.wal`. A binary payload is a
// possible later optimization if perf shows encoding cost matters.
Record to_record(const Mutation& m);
Mutation from_record(const Record& r);  // throws std::runtime_error on an unknown or bad record

}  // namespace baton
