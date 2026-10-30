# mods/harrowmere_core/tests/CMakeLists.txt - the test harness (task 0.6).
#
# Builds `harrowmere_core_tests` (doctest v2.4.12, vendored single header at
# third_party/doctest) and registers it with CTest. The target links the
# mod's compiled OBJECTS - never the runtime mod library - so tests execute
# without the engine's plugin scanner (card 0.6's pitfall, verbatim:
# "Don't link tests against the *mod runtime*; link objects").
#
# One test binary for the whole phase: every card's test file is listed here
# exactly once, and main.cpp carries the SINGLE doctest implement define. A
# test file defining its own main is a link error once all files join this
# binary - keep new test files free of DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN.
#
# ctest discovery is directory-local (the stock root CMakeLists does not call
# enable_testing; see DEVIATIONS.md task 0.7 row): run
#     ctest --test-dir build/mods/harrowmere_core/tests --output-on-failure
# scripts/ci.sh (task 0.6) wires exactly that into the one-command gate.

set(HARROWMERE_TEST_SOURCES
    main.cpp
    test_math.cpp                # task 0.2 - math helpers, IdGenerator, Result
    test_clock_rng.cpp           # task 0.3 - WorldClock + SeedRng
    test_data_loader.cpp         # task 0.4 - strict JSON, schema DSL, merge, config
    test_events.cpp              # task 0.7 - sim event channels
    test_enums_data_loaders.cpp  # task 1.1 - closed enums + species/items/classes
    test_data_loaders_2.cpp      # task 1.2 - crime/laws/roles/territory/suspicion
)

add_executable(harrowmere_core_tests ${HARROWMERE_TEST_SOURCES})

# Objects, not the runtime library (see the header comment).
target_link_libraries(harrowmere_core_tests PRIVATE harrowmere_core_objects)

# doctest v2.4.12 (vendored): SYSTEM so warnings-as-errors never fires inside
# the vendor header. Both include roots are exported because the test files
# use both spellings (<doctest.h> and "doctest/doctest.h").
target_include_directories(harrowmere_core_tests SYSTEM PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/doctest"
    "${CMAKE_SOURCE_DIR}/third_party"
)

# Warnings-as-errors on the game's own test translation units (global DoD,
# common.md section 1.5).
if(MSVC)
    target_compile_options(harrowmere_core_tests PRIVATE /W4 /WX)
else()
    target_compile_options(harrowmere_core_tests PRIVATE -Werror)
endif()

# Compile-time marker so game code can gate test-only seams (reserved for
# later tasks; none of the Phase-1 cards consume it yet).
target_compile_definitions(harrowmere_core_tests PRIVATE HARROWMERE_TEST=1)

# The shipped data directory (task 0.4's pinned decision), baked as an
# absolute path so the loader-facing tests pass regardless of the working
# directory ctest chooses.
target_compile_definitions(harrowmere_core_tests PRIVATE
    HARROWMERE_CORE_DATA_DIR="${CMAKE_SOURCE_DIR}/mods/harrowmere_core/data"
)

# CTest registration. The doctest binary is its own runner
# (DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN lives in main.cpp only). The working
# directory keeps the repo-root relative paths in test fixtures honest even
# without the baked define.
include(CTest)
add_test(
    NAME harrowmere_core_tests
    COMMAND harrowmere_core_tests
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# Friendly alias: cmake --build build --target harrowmere_tests
add_custom_target(harrowmere_tests
    DEPENDS harrowmere_core_tests
    COMMENT "Build harrowmere_core_tests (doctest harness, task 0.6)"
)
