#include "baton/path_trie.h"

#include "baton/todo.h"

namespace baton {

std::vector<std::string> split_path(std::string_view path) {
    (void)path;
    todo("split_path");
}

std::vector<LockConflict> PathTrie::try_lock(TaskId task, const std::vector<std::string>& paths) {
    (void)task;
    (void)paths;
    todo("PathTrie::try_lock");
}

void PathTrie::unlock(TaskId task, const std::vector<std::string>& paths) {
    (void)task;
    (void)paths;
    todo("PathTrie::unlock");
}

std::optional<TaskId> PathTrie::holder_of(std::string_view path) const {
    (void)path;
    todo("PathTrie::holder_of");
}

size_t PathTrie::size() const { todo("PathTrie::size"); }

}  // namespace baton
