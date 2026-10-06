#include "baton/protocol.h"

#include <unordered_map>

namespace baton {

using nlohmann::json;

const char* to_string(ErrorCode code) {
    switch (code) {
        case ErrorCode::BadRequest: return "BAD_REQUEST";
        case ErrorCode::NotFound: return "NOT_FOUND";
        case ErrorCode::TaskTaken: return "TASK_TAKEN";
        case ErrorCode::NotHolder: return "NOT_HOLDER";
        case ErrorCode::NoOpenTasks: return "NO_OPEN_TASKS";
        case ErrorCode::PathLocked: return "PATH_LOCKED";
        case ErrorCode::Internal: return "INTERNAL";
    }
    return "INTERNAL";
}

const char* to_string(TaskState state) {
    switch (state) {
        case TaskState::Open: return "open";
        case TaskState::Claimed: return "claimed";
        case TaskState::Done: return "done";
    }
    return "open";
}

const char* to_string(Op op) {
    switch (op) {
        case Op::Create: return "create";
        case Op::List: return "list";
        case Op::Claim: return "claim";
        case Op::Heartbeat: return "heartbeat";
        case Op::Lock: return "lock";
        case Op::Note: return "note";
        case Op::Complete: return "complete";
        case Op::Release: return "release";
    }
    return "unknown";
}

namespace {

const std::unordered_map<std::string, Op>& op_names() {
    static const std::unordered_map<std::string, Op> names = {
        {"create", Op::Create},     {"list", Op::List},         {"claim", Op::Claim},
        {"heartbeat", Op::Heartbeat}, {"lock", Op::Lock},       {"note", Op::Note},
        {"complete", Op::Complete}, {"release", Op::Release},
    };
    return names;
}

std::string get_string(const json& j, const char* key, uint64_t req_id, bool required) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        if (required) throw ProtocolError(req_id, std::string("missing field '") + key + "'");
        return {};
    }
    if (!it->is_string()) throw ProtocolError(req_id, std::string("'") + key + "' must be a string");
    return it->get<std::string>();
}

}  // namespace

Request parse_request(std::string_view line) {
    json j = json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) throw ProtocolError(0, "request is not a JSON object");

    Request req;
    if (auto it = j.find("req_id"); it != j.end()) {
        if (!it->is_number_unsigned()) throw ProtocolError(0, "'req_id' must be a non-negative integer");
        req.req_id = it->get<uint64_t>();
    }

    std::string op = get_string(j, "op", req.req_id, true);
    auto op_it = op_names().find(op);
    if (op_it == op_names().end()) throw ProtocolError(req.req_id, "unknown op '" + op + "'");
    req.op = op_it->second;

    bool needs_agent = req.op != Op::Create && req.op != Op::List;
    req.agent = get_string(j, "agent", req.req_id, needs_agent);

    if (auto it = j.find("task"); it != j.end() && !it->is_null()) {
        if (!it->is_number_unsigned()) throw ProtocolError(req.req_id, "'task' must be a task id");
        req.task = it->get<TaskId>();
    }
    bool needs_task = needs_agent && req.op != Op::Claim;
    if (needs_task && !req.task) throw ProtocolError(req.req_id, "missing field 'task'");

    req.title = get_string(j, "title", req.req_id, req.op == Op::Create);
    req.description = get_string(j, "description", req.req_id, false);
    req.text = get_string(j, "text", req.req_id, req.op == Op::Note);

    if (auto it = j.find("paths"); it != j.end() && !it->is_null()) {
        if (!it->is_array()) throw ProtocolError(req.req_id, "'paths' must be an array of strings");
        for (const auto& p : *it) {
            if (!p.is_string() || p.get<std::string>().empty())
                throw ProtocolError(req.req_id, "'paths' must contain non-empty strings");
            req.paths.push_back(p.get<std::string>());
        }
    }
    if (req.op == Op::Lock && req.paths.empty())
        throw ProtocolError(req.req_id, "'lock' needs at least one path");

    if (req.op == Op::List) {
        std::string state = get_string(j, "state", req.req_id, false);
        if (state == "open") req.state_filter = TaskState::Open;
        else if (state == "claimed") req.state_filter = TaskState::Claimed;
        else if (state == "done") req.state_filter = TaskState::Done;
        else if (!state.empty()) throw ProtocolError(req.req_id, "'state' must be open, claimed or done");
    }
    return req;
}

std::string ok_response(uint64_t req_id, const json& body) {
    json out = body.is_object() ? body : json{{"result", body}};
    out["req_id"] = req_id;
    out["ok"] = true;
    return out.dump() + "\n";
}

std::string error_response(uint64_t req_id, ErrorCode code, std::string_view message,
                           const json& extra) {
    json out = extra.is_object() ? extra : json::object();
    out["req_id"] = req_id;
    out["ok"] = false;
    out["code"] = to_string(code);
    out["message"] = message;
    return out.dump() + "\n";
}

json task_to_json(const Task& t, TimeMs now) {
    json notes = json::array();
    for (const auto& n : t.notes) notes.push_back({{"agent", n.agent}, {"ts_ms", n.ts_ms}, {"text", n.text}});
    json j = {
        {"id", t.id},           {"title", t.title},       {"description", t.description},
        {"state", to_string(t.state)}, {"paths", t.paths}, {"notes", notes},
        {"attempts", t.attempts},
    };
    if (t.state == TaskState::Claimed) {
        j["holder"] = t.holder;
        j["lease_expires_in_ms"] = t.lease_expiry > now ? t.lease_expiry - now : 0;
    }
    if (t.state == TaskState::Done) j["summary"] = t.summary;
    return j;
}

}  // namespace baton
