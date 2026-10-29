# mods/harrowmere_core/tests/CMakeLists.txt
# Task 0.6 — Test harness for harrowmere_core.
#
# Builds the `harrowmere_core_tests` binary (doctest v2.4.12) and registers
# it with CTest. The test target links harrowmere_core's *compiled objects*
# — never the runtime mod library — so tests execute without the engine's
# plugin scanner. This is the spec's pitfall verbatim:
#
#     "Don't link tests against the *mod runtime*; link objects — tests
#      must run without the plugin scanner."
#
# Contract this file assumes from task 0.1's `mods/harrowmere_core/CMakeLists.txt`:
#   * An OBJECT library named `harrowmere_core_objects` exists, holding the
#     compiled sources of the mod (every TU under `src/`). The runtime mod
#     library `harrowmere_core` (SHARED) is built from the same objects so
#     there is exactly one compilation of each TU.
#   * The parent target adds this directory via `add_subdirectory(tests)`.
#   * `CMAKE_SOURCE_DIR/third_party/doctest/doctest.h` is the vendored
#     single-header doctest v2.4.12 (see ../../third_party/doctest/).
#
# When 0.1 lands, the only change required there is the OBJECT library
# shape + the `add_subdirectory(tests)` line. Nothing in this file changes.

set(HARROWMERE_TESTS_TARGET harrowmere_core_tests)

add_executable(${HARROWMERE_TESTS_TARGET}
    main.cpp
)

# doctest single header (vendored at the repo root's third_party/).
target_include_directories(${HARROWMERE_TESTS_TARGET}
    PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/doctest
)

# Link harrowmere_core's *objects* (compiled sources), NOT the runtime mod
# library. Tests must run without the engine's plugin scanner. If
# `harrowmere_core_objects` is absent (task 0.1 not yet landed), the
# configure step fails loudly here — which is the desired behaviour,
# because 0.6 depends on 0.1.
if(TARGET harrowmere_core_objects)
    target_link_libraries(${HARROWMERE_TESTS_TARGET}
        PRIVATE
            harrowmere_core_objects
    )
else()
    message(FATAL_ERROR
        "harrowmere_core_objects target not found — task 0.1 must define an "
        "OBJECT library of that name exposing the mod's compiled sources "
        "(the runtime SHARED library `harrowmere_core` and the test binary "
        "share these objects; this is what lets tests run without the "
        "plugin scanner). See mods/harrowmere_core/tests/CMakeLists.txt.")
endif()

# Warnings-as-errors on the game targets (per global DoD §1.5 and task 0.1's
# "warnings-as-errors" requirement). Mirrors the engine's own convention.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(${HARROWMERE_TESTS_TARGET}
        PRIVATE
            -Wall -Wextra -Werror -Wpedantic
    )
elseif(MSVC)
    target_compile_options(${HARROWMERE_TESTS_TARGET}
        PRIVATE
            /W4 /permissive- /WX
    )
endif()

# Compile-time marker so game code can `#ifdef HARROWMERE_TEST` to gate
# test-only seams (none in 0.6 itself; reserved for later tasks).
target_compile_definitions(${HARROWMERE_TESTS_TARGET}
    PRIVATE
        HARROWMERE_TEST=1
)

# CTest registration. The doctest binary is its own runner
# (DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN in main.cpp). CI invokes
# `ctest --output-on-failure` and asserts the reported test count
# (> 20 from Layer 2 onward; see scripts/ci.sh).
include(CTest)
add_test(
    NAME ${HARROWMERE_TESTS_TARGET}
    COMMAND ${HARROWMERE_TESTS_TARGET}
)

# Friendly alias so reviewers/devs can `cmake --build build --target harrowmere_tests`.
add_custom_target(harrowmere_tests
    DEPENDS ${HARROWMERE_TESTS_TARGET}
    COMMENT "Build harrowmere_core_tests (doctest harness, task 0.6)"
)
