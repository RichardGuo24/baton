add_library(baton_core
  src/protocol.cpp
  src/mutation.cpp
  src/task_store.cpp
  src/path_trie.cpp
  src/lease_heap.cpp
  src/wal.cpp
)
target_include_directories(baton_core PUBLIC include)
target_link_libraries(baton_core PUBLIC nlohmann_json::nlohmann_json)

# The event loop uses epoll, which only exists on Linux. On macOS we still build the core library
# and its unit tests; build and run the server itself in Linux (CI, or a Multipass VM, see README).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(baton_core PRIVATE src/event_loop.cpp)
  add_executable(baton-server src/main.cpp)
  target_link_libraries(baton-server PRIVATE baton_core)
else()
  message(STATUS "baton-server needs Linux (epoll); building only the core library and tests")
endif()
