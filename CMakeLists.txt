# CMakeLists.txt — Task 1.2 standalone test harness.
#
# This file is a STANDALONE build harness for Task 1.2's pinned tests. It
# exists so the loader code can be compiled and tested in isolation before
# the rest of the engine tree (Task 0.1's assembly, Task 0.4's JSON
# layer, Task 0.6's test harness) lands.
#
# WHEN MERGING INTO THE GAME REPO: this top-level CMakeLists.txt is NOT
# used — the game repo's root CMakeLists.txt (kept stock per common.md
# §3.6) drives everything. Task 1.2's test target is declared in
# `mods/harrowmere_core/tests/CMakeLists.txt` (also shipped here) which
# the game repo's `ENGINE_NATIVE_MOD_PROJECTS` glob picks up.
#
# Dependencies:
#   * nlohmann_json v3.11.3 — vendored at third_party/nlohmann/json.hpp
#     (canonical owner: Task 0.4)
#   * doctest v2.4.12       — vendored at third_party/doctest/doctest.h
#     (canonical owner: Task 0.6)
#   * C++20 (the engine's standard; common.md §3.3).
cmake_minimum_required(VERSION 3.16)
project(harrowmere_task_1_2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Warnings-as-errors per common.md §1.5 global DoD.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter)
endif()

# Include paths: the harrowmere_core src tree + vendored third_party.
add_library(harrowmere_task_1_2_objs OBJECT
    mods/harrowmere_core/src/data/crime.cpp
    mods/harrowmere_core/src/data/laws.cpp
    mods/harrowmere_core/src/data/roles.cpp
    mods/harrowmere_core/src/data/territory_templates.cpp
)
target_include_directories(harrowmere_task_1_2_objs PUBLIC
    mods/harrowmere_core/src
    third_party
)

# Test binary: harrowmere_core_tests (named per common.md §3.1 D-NAME-1).
add_executable(harrowmere_core_tests
    mods/harrowmere_core/tests/test_data_loaders_2.cpp
)
target_link_libraries(harrowmere_core_tests PRIVATE
    harrowmere_task_1_2_objs
)

# Run the tests from the repo root so the relative data paths in
# test_data_loaders_2.cpp resolve correctly.
enable_testing()
add_test(NAME harrowmere_core_tests
         COMMAND harrowmere_core_tests
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
