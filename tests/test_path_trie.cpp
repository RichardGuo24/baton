// Spec tests for PathTrie. They are DISABLED until you implement path_trie.cpp:
// remove the DISABLED_ prefix from each test as you get it passing.

#include <gtest/gtest.h>

#include "baton/path_trie.h"

using namespace baton;

TEST(PathTrie, DISABLED_SplitPath) {
    EXPECT_EQ(split_path("src/parser/"), (std::vector<std::string>{"src", "parser"}));
    EXPECT_EQ(split_path("/README.md"), (std::vector<std::string>{"README.md"}));
    EXPECT_EQ(split_path("a//b"), (std::vector<std::string>{"a", "b"}));
}

TEST(PathTrie, DISABLED_SiblingsDoNotConflict) {
    PathTrie t;
    EXPECT_TRUE(t.try_lock(1, {"src/parser/"}).empty());
    EXPECT_TRUE(t.try_lock(2, {"src/api/"}).empty());
    EXPECT_EQ(t.size(), 2u);
}

TEST(PathTrie, DISABLED_AncestorBlocksDescendant) {
    PathTrie t;
    ASSERT_TRUE(t.try_lock(1, {"src/"}).empty());
    auto conflicts = t.try_lock(2, {"src/parser/lexer.cpp"});
    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts[0].holder, 1u);
}

TEST(PathTrie, DISABLED_DescendantBlocksAncestor) {
    PathTrie t;
    ASSERT_TRUE(t.try_lock(1, {"src/parser/lexer.cpp"}).empty());
    auto conflicts = t.try_lock(2, {"src/"});
    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts[0].holder, 1u);
}

TEST(PathTrie, DISABLED_SameTaskNeverConflicts) {
    PathTrie t;
    ASSERT_TRUE(t.try_lock(1, {"README.md"}).empty());
    EXPECT_TRUE(t.try_lock(1, {"README.md"}).empty());
}

TEST(PathTrie, DISABLED_AllOrNothing) {
    PathTrie t;
    ASSERT_TRUE(t.try_lock(1, {"models/loader.py"}).empty());
    auto conflicts = t.try_lock(2, {"api/predict/", "models/loader.py"});
    EXPECT_FALSE(conflicts.empty());
    EXPECT_FALSE(t.holder_of("api/predict/").has_value());  // nothing from the failed call stuck
}

TEST(PathTrie, DISABLED_UnlockFreesPathsAndPrunes) {
    PathTrie t;
    ASSERT_TRUE(t.try_lock(1, {"src/parser/", "docs/"}).empty());
    t.unlock(1, {"src/parser/", "docs/"});
    EXPECT_EQ(t.size(), 0u);
    EXPECT_TRUE(t.try_lock(2, {"src/"}).empty());  // locked_below was decremented correctly
}

TEST(PathTrie, DISABLED_HolderOfSeesAncestors) {
    PathTrie t;
    ASSERT_TRUE(t.try_lock(4, {"api/"}).empty());
    EXPECT_EQ(t.holder_of("api/predict/route.py"), std::optional<TaskId>(4));
    EXPECT_FALSE(t.holder_of("models/").has_value());
}
