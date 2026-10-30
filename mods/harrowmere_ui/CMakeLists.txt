# The game's UI surface native mod. Build pattern identical to harrowmere_core's
# (task 0.6's object-library shape): every game translation unit compiles
# exactly once, into the OBJECT library `harrowmere_ui_objects`; the runtime
# SHARED library and the unit-test binary are both built from those objects, so
# tests run without the engine's plugin scanner (card 0.6's pitfall verbatim).
#
# Source map (Phase 1):
#   src/ui_harrowmere_plugin.cpp - the mod component: manifest, lifecycle, the
#                                  7.4P1 input-mapper wiring (task 0.1 shell,
#                                  wired at 7.4P1)
#   src/input_mapper.cpp         - the 7.4P1 input layer: the P1 binding set,
#                                  camera-relative walk intents, gated mouse
#                                  look (engine-free; tests link the objects)
#
# The 7.3P1 files (src/ui_harrowmere.cpp, src/backend/, src/panels/, and their
# test test_debug_panel.cpp) join THIS target when that task's ImGui dependency
# lands (third_party/imgui at the pinned tag, card 7.3P1): they are shipped but
# deliberately not compiled yet, exactly as that task's own test file documents
# ("this file is added to that binary's sources when the harrowmere_ui mod
# lands").
add_library(harrowmere_ui_objects OBJECT
    src/ui_harrowmere_plugin.cpp
    src/input_mapper.cpp
)

# The required capabilities' published headers (include path only): the mod
# compiles against the providers' types and stays free of their binaries - the
# implementations arrive at runtime as plugin libraries. harrowmere_capability_
# headers carries the sim capability's vocabulary (the ONLY harrowmere_core
# header this mod compiles against, common.md section 4.2 rule 5).
target_link_libraries(harrowmere_ui_objects PUBLIC
    engine_kernel
    engine_capability_input
    engine_capability_renderer
    harrowmere_capability_headers
)

# The published capability surface (inert until 7.3P1) gets the same purity
# guard as every other published header.
engine_guard_capability_header("${CMAKE_CURRENT_SOURCE_DIR}/src/ui_harrowmere_capability.hpp")

# The objects join a SHARED library; every object must be position independent
# (the tree sets PIC globally, pinned here so the object library stands alone).
set_target_properties(harrowmere_ui_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# The runtime mod library: built from the shared objects (exactly one
# compilation of each TU).
add_library(harrowmere_ui SHARED
    $<TARGET_OBJECTS:harrowmere_ui_objects>
)

# Plugin deployment: mirrors harrowmere_core (see that file for the full
# commentary on PREFIX, RPATH and the Windows export mechanics).
set_target_properties(harrowmere_ui PROPERTIES
    PREFIX "lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime/mods/harrowmere_ui"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime/mods/harrowmere_ui"
    BUILD_RPATH "${ENGINE_PLUGIN_RPATH_BUILD}"
    INSTALL_RPATH "${ENGINE_PLUGIN_RPATH_INSTALL}"
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)
if(MINGW)
    target_link_options(harrowmere_ui PRIVATE -Wl,--export-all-symbols)
endif()

# The global Definition of Done (common.md §1.5): game targets compile with
# warnings-as-errors. The kernel's PUBLIC warning set (-Wall -Wextra
# -Wpedantic) is inherited through engine_kernel; -Werror upgrades it on the
# game's own translation units (the object library compiles them all).
if(MSVC)
    target_compile_options(harrowmere_ui_objects PRIVATE /W4 /WX)
else()
    target_compile_options(harrowmere_ui_objects PRIVATE -Werror)
endif()

# ---- Unit tests ---------------------------------------------------------------
# The 7.4P1 harness: one doctest binary linking harrowmere_ui_objects (plus
# harrowmere_core's objects for the roundtrip's fixed-update drain side - the
# card's own test spec: "inject key state -> Move intent applied on the
# fixed-update subscription"). The switch keeps a packager's default path
# identical when the test binary is unwanted.
option(HARROWMERE_UI_BUILD_TESTS "Build the harrowmere_ui unit tests" ON)
if(HARROWMERE_UI_BUILD_TESTS)
    add_subdirectory(tests)
endif()
