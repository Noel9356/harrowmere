# The game's UI surface native mod. Build pattern identical to harrowmere_core's
# (task 0.6's object-library shape): every game translation unit compiles
# exactly once, into OBJECT libraries; the runtime SHARED library and the
# unit-test binary are both built from those objects, so tests run without the
# engine's plugin scanner (card 0.6's pitfall verbatim).
#
# Source map (Phase 1):
#   src/ui_harrowmere_plugin.cpp - the mod component: manifest, lifecycle, the
#                                  7.3P1 overlay component + the 7.4P1
#                                  input-mapper wiring
#   src/ui_harrowmere.cpp        - the 7.3P1 overlay component (the overlay
#                                  recorder; F1 toggle; the per-frame draw)
#   src/backend/imgui_backend.cpp   - the ImGui GLFW+Vulkan wrapper
#   src/panels/debug.cpp         - the debug panel (7.3P1's single panel)
#   src/input_mapper.cpp         - the 7.4P1 input layer: the P1 binding set,
#                                  camera-relative walk intents, gated mouse
#                                  look (engine-free; tests link the objects)
#
# ImGui (card 7.3P1: "ImGui checked into the game repo, pinned tag"): vendored
# at third_party/imgui, tag v1.91.9b - the same pin the engine's imgui fetch
# carries. The static bindings library below compiles the core + the GLFW and
# Vulkan backends once; SYSTEM include paths so the game's warnings-as-errors
# never fires inside the vendor sources. The Vulkan backend compiles against
# the engine's pinned Vulkan-Headers fetch (engine_vulkan_hpp_bindings), and
# the GLFW backend against the engine's own GLFW fetch - the same targets the
# engine's ui_imgui module (the mirrored reference) links.
add_library(harrowmere_ui_imgui STATIC
    "${CMAKE_SOURCE_DIR}/third_party/imgui/imgui.cpp"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_draw.cpp"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_tables.cpp"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_widgets.cpp"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_glfw.cpp"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_vulkan.cpp"
)
target_include_directories(harrowmere_ui_imgui SYSTEM PUBLIC
    "${CMAKE_SOURCE_DIR}/third_party/imgui"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/backends"
)
# The backend never calls OpenGL: the window module's own pattern (modules/
# window_glfw) keeps glfw3.h from pulling GL/gl.h, which a Vulkan-only tree
# has no reason to ship.
target_compile_definitions(harrowmere_ui_imgui PUBLIC GLFW_INCLUDE_NONE)
target_link_libraries(harrowmere_ui_imgui PUBLIC
    glfw # the engine's pinned GLFW fetch: headers for imgui_impl_glfw.cpp
    engine_vulkan_hpp_bindings # the pinned Vulkan headers for imgui_impl_vulkan.cpp
)

# The plugin seam TU exports the mod's C-ABI entry table (engine_query_mod_
# entry_table). Every native mod defines that one symbol name, so two mods'
# objects must never meet in a single binary: the test harness links the
# logic objects + harrowmere_core's objects (the 7.4P1 roundtrip's drain
# side), and the runtime shared library is the only artifact that links the
# plugin objects. Splitting the TU out of harrowmere_ui_objects keeps every
# TU compiled exactly once AND the test link free of the collision.
add_library(harrowmere_ui_objects OBJECT
    src/input_mapper.cpp
    src/ui_harrowmere.cpp
    src/backend/imgui_backend.cpp
    src/panels/debug.cpp
)

add_library(harrowmere_ui_plugin_objects OBJECT
    src/ui_harrowmere_plugin.cpp
)

# The required capabilities' published headers (include path only): the mod
# compiles against the providers' types and stays free of their binaries -
# the implementations arrive at runtime as plugin libraries. harrowmere_capability_
# headers carries the sim capability's vocabulary (the ONLY harrowmere_core
# header this mod compiles against, common.md section 4.2 rule 5). The window
# capability header is the 7.3P1 optional probe's include path.
target_link_libraries(harrowmere_ui_objects PUBLIC
    engine_kernel
    engine_capability_input
    engine_capability_renderer
    engine_capability_window
    harrowmere_capability_headers
    harrowmere_ui_imgui
)

target_link_libraries(harrowmere_ui_plugin_objects PUBLIC
    engine_kernel
    engine_capability_input
    engine_capability_renderer
    engine_capability_window
    harrowmere_capability_headers
    harrowmere_ui_imgui
)

# The mod's own headers (input_mapper.hpp): PUBLIC so the test harness's
# include of "input_mapper.hpp" resolves - the core objects' include-path
# pattern, mirrored here (harrowmere_core/CMakeLists.txt, same shape).
target_include_directories(harrowmere_ui_objects PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_include_directories(harrowmere_ui_plugin_objects PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# The published capability surface (inert until 7.3P1) gets the same purity
# guard as every other published header.
engine_guard_capability_header("${CMAKE_CURRENT_SOURCE_DIR}/src/ui_harrowmere_capability.hpp")

# The objects join a SHARED library; every object must be position independent
# (the tree sets PIC globally, pinned here so the object library stands alone).
set_target_properties(harrowmere_ui_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
set_target_properties(harrowmere_ui_plugin_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# The runtime mod library: built from the shared objects (exactly one
# compilation of each TU).
add_library(harrowmere_ui SHARED
    $<TARGET_OBJECTS:harrowmere_ui_plugin_objects>
    $<TARGET_OBJECTS:harrowmere_ui_objects>
)

# engine_kernel joins imgui here for the same reason harrowmere_core links it
# (see that CMakeLists): $<TARGET_OBJECTS> carries object files only, and the
# plugin and overlay objects call non-inline kernel functions
# (configuration*InSection, executableDirectory, prettifyChannelName). ELF
# tolerates the undefined references until load time; MinGW does not and
# needs the kernel's import library on the link line.
target_link_libraries(harrowmere_ui PRIVATE
    harrowmere_ui_imgui
    engine_kernel
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
    target_compile_options(harrowmere_ui_plugin_objects PRIVATE /W4 /WX)
else()
    target_compile_options(harrowmere_ui_objects PRIVATE -Werror)
    target_compile_options(harrowmere_ui_plugin_objects PRIVATE -Werror)
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
