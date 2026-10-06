#include "baton/task_store.h"

#include <algorithm>
#include <stdexcept>
#include <string>

#include "baton/todo.h"

namespace baton {

using nlohmann::json;

namespace {

// std::visit helper: build one callable out of several lambdas, one per variant alternative.
template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

std::string task_str(TaskId id) { return "task " + std::to_string(id); }

}  // namespace

// ---------------------------------------------------------------------------------------------
// prepare: validate a request against current state and describe the change as a Mutation.
//
// prepare() is const, so it cannot change state. That is the point: the caller may still decide
// not to go through with the change (for example if the WAL write fails), and in that case nothing
// should have happened. Every decision that depends on current state is made here, so that apply()
// can be a dumb, deterministic "do exactly what the Mutation says" step that replay can trust.
// ---------------------------------------------------------------------------------------------

PrepareResult TaskStore::prepare(const Request& req, TimeMs now) const {
    switch (req.op) {
        case Op::Create:
            return prepare_create(req, now);
        case Op::Claim:
            return prepare_claim(req, now);
        case Op::Complete:
            return prepare_complete(req, now);
        case Op::Lock:
        case Op::Note:
        case Op::Release:
            // Week 1 builds create/claim/complete only. Say so plainly instead of crashing.
            return StoreError{ErrorCode::BadRequest,
                              "This server does not support '" + std::string(to_string(req.op)) +
                                  "' yet. Use create, list, claim and complete for now."};
        case Op::List:
        case Op::Heartbeat:
            // Not mutations: the event loop answers these directly. Reaching here is a server bug.
            break;
    }
    return StoreError{ErrorCode::Internal,
                      "The server routed a read-only request to prepare(). This is a server bug; "
                      "please report it."};
}

PrepareResult TaskStore::prepare_create(const Request& req, TimeMs now) const {
    // The id is decided HERE, not in apply(), and written into the Mutation. On replay, apply()
    // just uses the logged id, so ids come out identical no matter how many times we replay.
    return CreateM{next_id_, req.title, req.description, now};
}

PrepareResult TaskStore::prepare_claim(const Request& req, TimeMs now) const {
    TaskId id = 0;
    if (req.task) {
        id = *req.task;
        const Task* t = find(id);
        if (!t) {
            return StoreError{ErrorCode::NotFound,
                              task_str(id) + " does not exist. Call list_tasks to see valid ids." +
                                  open_suggestions()};
        }
        if (t->state == TaskState::Claimed) {
            if (t->holder == req.agent) {
                return StoreError{ErrorCode::TaskTaken,
                                  "You already hold " + task_str(id) + ". Keep working on it."};
            }
            return StoreError{ErrorCode::TaskTaken,
                              task_str(id) + " is already claimed by " + t->holder + "." +
                                  open_suggestions(),
                              json{{"held_by", {{"agent", t->holder}, {"task", id}}}}};
        }
        if (t->state == TaskState::Done) {
            return StoreError{ErrorCode::TaskTaken,
                              task_str(id) + " is already done." + open_suggestions()};
        }
    } else {
        // No task given: take the oldest open one. open_ is a std::set, so begin() is the
        // smallest id, which is also the oldest because ids only ever increase.
        if (open_.empty()) {
            return StoreError{ErrorCode::NoOpenTasks,
                              "There are no open tasks right now. Create one, or try again later."};
        }
        id = *open_.begin();
    }

    // The lease expiry is computed here from `now` and stored in the Mutation, so replay
    // reproduces it exactly instead of reading a clock.
    // TODO(week 4): check req.paths against the lock trie and return PathLocked on conflict.
    return ClaimM{id, req.agent, req.paths, now + kLeaseMs, now};
}

PrepareResult TaskStore::prepare_complete(const Request& req, TimeMs now) const {
    TaskId id = *req.task;  // parse_request guarantees complete has a task
    const Task* t = find(id);
    if (!t) {
        return StoreError{ErrorCode::NotFound,
                          task_str(id) + " does not exist. Call list_tasks to see valid ids."};
    }
    // Only the current holder may complete. This is what stops an agent whose lease expired (and
    // whose task someone else has since claimed) from marking the other agent's work as done.
    if (t->state != TaskState::Claimed || t->holder != req.agent) {
        std::string why;
        if (t->state == TaskState::Done) why = " It is already done.";
        else if (t->state == TaskState::Open) why = " It is open; claim it first.";
        else why = " It is held by " + t->holder + ", possibly because your lease expired.";
        return StoreError{ErrorCode::NotHolder, "You do not hold " + task_str(id) + "." + why};
    }
    return CompleteM{id, req.agent, req.text, now};
}

// " Open tasks you could take instead: 4, 6." (at most 5 ids), or "" if there are none.
std::string TaskStore::open_suggestions() const {
    if (open_.empty()) return "";
    std::string s = " Open tasks you could take instead: ";
    int n = 0;
    for (TaskId id : open_) {
        if (n == 5) break;
        if (n++ > 0) s += ", ";
        s += std::to_string(id);
    }
    return s + ".";
}

// ---------------------------------------------------------------------------------------------
// apply: carry out a Mutation. Used by both the live server and WAL replay.
//
// Rules that keep replay trustworthy:
//   - No clock reads and no randomness: every value comes from the Mutation itself.
//   - No validation that could fail differently on replay: prepare() already checked everything.
//   - Keep every index (tasks_, open_, and later the lock trie and lease heap) in sync in one place.
// ---------------------------------------------------------------------------------------------

json TaskStore::apply(const Mutation& m) {
    return std::visit(
        overloaded{
            [this](const CreateM& c) -> json {
                Task t;
                t.id = c.id;
                t.title = c.title;
                t.description = c.description;
                tasks_[c.id] = std::move(t);
                open_.insert(c.id);
                // max() rather than ++: the next id must be past every id we have seen, and this
                // stays correct even if records were ever applied out of id order.
                next_id_ = std::max(next_id_, c.id + 1);
                return json{{"task", task_to_json(tasks_.at(c.id), c.ts)}};
            },
            [this](const ClaimM& c) -> json {
                Task& t = tasks_.at(c.task);
                t.state = TaskState::Claimed;
                t.holder = c.agent;
                t.lease_expiry = c.expiry;
                // A new generation per claim. Week 3's lease heap uses it to recognize entries
                // left over from an earlier claim of the same task as stale.
                t.lease_gen += 1;
                t.attempts += 1;
                t.paths = c.paths;  // TODO(week 4): lock these in the trie
                open_.erase(c.task);
                // TODO(week 3): leases_.push(c.expiry, c.task, t.lease_gen);
                // c.ts stands in for "now" so the response is the same on every replay.
                return json{{"task", task_to_json(t, c.ts)}, {"lease_expiry", c.expiry}};
            },
            [this](const CompleteM& c) -> json {
                Task& t = tasks_.at(c.task);
                t.state = TaskState::Done;
                t.holder.clear();
                t.lease_expiry = 0;
                t.summary = c.summary;
                t.paths.clear();  // TODO(week 4): unlock these in the trie
                // Not re-added to open_: done is final.
                return json{{"task", task_to_json(t, c.ts)}};
            },
            [](const LockM&) -> json { todo("apply(LockM), week 4"); },
            [](const NoteM&) -> json { todo("apply(NoteM), week 3"); },
            [](const ReleaseM&) -> json { todo("apply(ReleaseM), week 3"); },
            [](const ExpireM&) -> json { todo("apply(ExpireM), week 3"); },
        },
        m);
}

std::variant<TimeMs, StoreError> TaskStore::heartbeat(const AgentId& agent, TaskId task, TimeMs now) {
    (void)agent;
    (void)task;
    (void)now;
    // Week 3. Until then leases never expire, so a heartbeat has nothing to renew; answer with a
    // normal error instead of throwing, because the shim sends these automatically.
    return StoreError{ErrorCode::BadRequest,
                      "This server does not support heartbeats yet; leases do not expire."};
}

json TaskStore::list(std::optional<TaskState> filter, TimeMs now) const {
    json tasks = json::array();
    std::vector<TaskId> ids;
    ids.reserve(tasks_.size());
    for (const auto& [id, _] : tasks_) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    for (TaskId id : ids) {
        const Task& t = tasks_.at(id);
        if (!filter || t.state == *filter) tasks.push_back(task_to_json(t, now));
    }
    return json{{"tasks", tasks}};
}

std::vector<ExpireM> TaskStore::collect_expired(TimeMs now) {
    (void)now;
    todo("TaskStore::collect_expired");
}

void TaskStore::rearm_leases(TimeMs now) {
    (void)now;
    todo("TaskStore::rearm_leases");
}

const Task* TaskStore::find(TaskId id) const {
    auto it = tasks_.find(id);
    return it == tasks_.end() ? nullptr : &it->second;
}

}  // namespace baton
