# mods/harrowmere_ui/tests/CMakeLists.txt - the UI mod's test harness (7.4P1).
#
# Builds `harrowmere_ui_tests` (doctest v2.4.12, vendored single header at
# third_party/doctest - the same vendor copy task 0.6 pinned) and registers it
# with CTest. The target links the mod's compiled OBJECTS - never the runtime
# mod library - so tests execute without the engine's plugin scanner (card
# 0.6's pitfall, verbatim: "Don't link tests against the *mod runtime*; link
# objects").
#
# The roundtrip test also links harrowmere_core's OBJECTS: the card pins
# "TestInput_IntentRoundTrip (headless: inject key state -> Move intent
# applied on the fixed-update subscription)", and the fixed-update application
# is harrowmere_core's player::applyIntents (task 3.10, step 9's home).
# Linking objects keeps both mods' test binaries free of the plugin scanner.
#
# main.cpp carries the SINGLE doctest implement define; keep new test files
# free of DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN (the core harness's rule, same
# reason: one binary, one main). The 7.3P1 test (test_debug_panel.cpp) joins
# a binary of its own when that task's ImGui dependency lands (see the mod's
# CMakeLists).
#
# ctest discovery is directory-local (the stock root CMakeLists does not call
# enable_testing; the core harness carries the DEVIATIONS.md row): run
#     ctest --test-dir build/mods/harrowmere_ui/tests --output-on-failure
# scripts/ci.sh wires exactly that into the one-command gate.

set(HARROWMERE_UI_TEST_SOURCES
    main.cpp
    test_input_mapper.cpp       # task 7.4P1 - P1 bindings, sim write path, intent roundtrip
)

add_executable(harrowmere_ui_tests ${HARROWMERE_UI_TEST_SOURCES})

# Objects, not the runtime libraries (see the header comment). The core
# objects come from the same OBJECT library the core tests link.
target_link_libraries(harrowmere_ui_tests PRIVATE
    harrowmere_ui_objects
    harrowmere_core_objects
)

# doctest v2.4.12 (vendored): SYSTEM so warnings-as-errors never fires inside
# the vendor header. Both include roots are exported because the test files
# use both spellings (<doctest.h> and "doctest/doctest.h") - the core
# harness's convention, kept.
target_include_directories(harrowmere_ui_tests SYSTEM PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/doctest"
    "${CMAKE_SOURCE_DIR}/third_party"
)

# Warnings-as-errors on the game's own test translation units (global DoD,
# common.md section 1.5).
if(MSVC)
    target_compile_options(harrowmere_ui_tests PRIVATE /W4 /WX)
else()
    target_compile_options(harrowmere_ui_tests PRIVATE -Werror)
endif()

# CTest registration. The doctest binary is its own runner
# (DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN lives in main.cpp only). The working
# directory keeps the repo-root relative paths in test fixtures honest.
include(CTest)
add_test(
    NAME harrowmere_ui_tests
    COMMAND harrowmere_ui_tests
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# Friendly alias: cmake --build build --target harrowmere_ui_tests_alias
add_custom_target(harrowmere_ui_tests_alias
    DEPENDS harrowmere_ui_tests
    COMMENT "Build harrowmere_ui_tests (doctest harness, task 7.4P1)"
)
