#pragma once

// Hierarchical path locks. Paths are split on '/', one trie node per segment.
// Locking "src/" conflicts with any lock on "src/..." held by another task, and vice versa.
// Sibling paths ("src/api/" and "src/parser/") never conflict. A task never conflicts with itself.

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "baton/types.h"

namespace baton {

// "src/parser/" -> {"src", "parser"};  "/README.md" -> {"README.md"};  "a//b" -> {"a", "b"}.
// TODO(richard)
std::vector<std::string> split_path(std::string_view path);

struct LockConflict {
    std::string requested;  // the path the caller asked for
    std::string held;       // the path that is already locked
    TaskId holder = 0;      // the task holding `held`
};

class PathTrie {
   public:
    // All or nothing: if every path can be locked for `task`, lock them all and return {}.
    // Otherwise lock nothing and return every conflict found.
    // TODO(richard): check ancestors (walk down), the node itself, and descendants
    // (O(1) via locked_below when nothing is below; DFS only when something is).
    std::vector<LockConflict> try_lock(TaskId task, const std::vector<std::string>& paths);

    // Release the given paths held by `task`. Decrement locked_below on the way and prune
    // nodes that become empty. Unknown paths are ignored.
    // TODO(richard)
    void unlock(TaskId task, const std::vector<std::string>& paths);

    // The task holding `path` itself or any ancestor of it, if any. Useful for tests and errors.
    // TODO(richard)
    std::optional<TaskId> holder_of(std::string_view path) const;

    // Number of locked paths. TODO(richard)
    size_t size() const;

   private:
    struct Node {
        std::unordered_map<std::string, std::unique_ptr<Node>> children;
        TaskId holder = 0;          // task locking exactly this node, 0 if none
        uint32_t locked_below = 0;  // locks strictly inside this subtree
    };
    Node root_;
};

}  // namespace baton
