#include "baton/task_store.h"

#include <algorithm>

#include "baton/todo.h"

namespace baton {

using nlohmann::json;

PrepareResult TaskStore::prepare(const Request& req, TimeMs now) const {
    // Suggested shape: switch on req.op, one small private helper per op.
    // Remember: no state changes here. Assign new task ids as next_id_ (apply() bumps it).
    (void)req;
    (void)now;
    todo("TaskStore::prepare");
}

json TaskStore::apply(const Mutation& m) {
    // Suggested shape: std::visit over the Mutation variant (see mutation.cpp for an example).
    // Keep the open_ set, the lock trie and the lease heap in sync with tasks_ on every change.
    (void)m;
    todo("TaskStore::apply");
}

std::variant<TimeMs, StoreError> TaskStore::heartbeat(const AgentId& agent, TaskId task, TimeMs now) {
    (void)agent;
    (void)task;
    (void)now;
    todo("TaskStore::heartbeat");
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
