cmake_minimum_required(VERSION 3.20)
project(baton LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(BATON_SANITIZE "Build with AddressSanitizer + UBSan" OFF)
option(BATON_BUILD_TESTS "Build unit tests" ON)

add_compile_options(-Wall -Wextra -Wpedantic)
if(BATON_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

include(FetchContent)

# Prefer system packages (apt: nlohmann-json3-dev, libgtest-dev; brew: nlohmann-json, googletest),
# fall back to downloading them.
find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
  FetchContent_MakeAvailable(json)
endif()

add_subdirectory(server)

if(BATON_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
