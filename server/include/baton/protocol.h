#pragma once

// Wire protocol: newline-delimited JSON over TCP. One request per line, one response per line,
// matched by req_id. See docs/design.md ("API") for the full list of ops and fields.

#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "baton/types.h"

namespace baton {

enum class Op { Create, List, Claim, Heartbeat, Lock, Note, Complete, Release };

struct Request {
    uint64_t req_id = 0;
    Op op = Op::List;
    AgentId agent;               // required for everything except create and list
    std::optional<TaskId> task;  // claim: optional (oldest open if absent); others: required
    std::string title;           // create
    std::string description;     // create
    std::string text;            // note text, complete summary, release reason
    std::vector<std::string> paths;          // claim, lock
    std::optional<TaskState> state_filter;   // list
};

class ProtocolError : public std::runtime_error {
   public:
    ProtocolError(uint64_t req_id, const std::string& msg)
        : std::runtime_error(msg), req_id_(req_id) {}
    uint64_t req_id() const { return req_id_; }

   private:
    uint64_t req_id_;
};

// Parses one line (without the trailing '\n'). Throws ProtocolError on malformed input.
Request parse_request(std::string_view line);

// Each returns one complete response line, including the trailing '\n'.
std::string ok_response(uint64_t req_id, const nlohmann::json& body);
std::string error_response(uint64_t req_id, ErrorCode code, std::string_view message,
                           const nlohmann::json& extra = nlohmann::json::object());

// JSON view of a task as agents see it. lease time left is computed from `now`.
nlohmann::json task_to_json(const Task& task, TimeMs now);

}  // namespace baton
