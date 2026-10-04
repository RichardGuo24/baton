add_library(baton_core
  src/protocol.cpp
  src/mutation.cpp
  src/task_store.cpp
  src/path_trie.cpp
  src/lease_heap.cpp
  src/wal.cpp
  src/event_loop.cpp
)
target_include_directories(baton_core PUBLIC include)
target_link_libraries(baton_core PUBLIC nlohmann_json::nlohmann_json)

add_executable(baton-server src/main.cpp)
target_link_libraries(baton-server PRIVATE baton_core)
