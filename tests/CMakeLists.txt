find_package(GTest QUIET)
if(NOT GTest_FOUND)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

add_executable(baton_tests
  test_protocol.cpp
  test_mutation.cpp
  test_path_trie.cpp
  test_lease_heap.cpp
  test_wal.cpp
  test_task_store.cpp
)
target_link_libraries(baton_tests PRIVATE baton_core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(baton_tests)
