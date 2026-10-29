# mods/harrowmere_core/tests/CMakeLists.txt — Task 1.2's contribution.
#
# This file is the drop-in snippet Task 1.2 owns in the game repo. When
# the game repo's root CMakeLists.txt globs `mods/*/CMakeLists.txt`, it
# also picks up `mods/*/tests/CMakeLists.txt` (per the engine's own
# demo-tree convention — task 0.6 spec).
#
# In the standalone build (this drop's top-level CMakeLists.txt), the
# `tests/` directory's CMakeLists.txt is NOT consumed — the standalone
# CMakeLists.txt builds the test binary directly. When merged, the game
# repo's CMake consumes this file via add_subdirectory().
#
# DO NOT ADD test target declarations here that conflict with the
# standalone harness. The two build systems share the same test names
# (`harrowmere_core_tests`) and source lists — but only ONE runs in any
# given build.

set(_task_1_2_test_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/test_data_loaders_2.cpp
)

# In the merged game repo, this would be wired into the existing
# `harrowmere_core_tests` target via target_sources(...). For the
# standalone harness, we build the test binary at the top level.
if(TARGET harrowmere_core_tests)
    target_sources(harrowmere_core_tests PRIVATE ${_task_1_2_test_sources})
    target_link_libraries(harrowmere_core_tests PRIVATE harrowmere_core)
endif()
