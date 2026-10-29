# The game's authoritative-simulation native mod: a shared library linking the
# kernel, packaged into its own runtime package directory (mod.toml, defaults,
# world claim, data/) by the engine's own runtime-layout target. This file
# copies the build pattern of a module's CMakeLists (the same plugin scanner
# loads both); the mod publishes its capability vocabulary from src/bridge/
# exactly the way a module does.
add_library(harrowmere_core SHARED
    src/harrowmere_plugin.cpp
)

# The mod's own translation units include the published header from src/bridge/.
target_include_directories(harrowmere_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge")

# The mod's core headers (src/core/: the pinned types, enums and the sim event
# channels) are internal to harrowmere_core - consumers never compile against
# them (§4.2: the capability header is the only published surface); the unit
# tests include them directly (see tests/CMakeLists.txt).
target_include_directories(harrowmere_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/core")

target_link_libraries(harrowmere_core
    PUBLIC engine_kernel
    # The required capabilities' published headers (include path only): the
    # mod compiles against the providers' types and stays free of their
    # binaries - the implementations arrive at runtime as plugin libraries.
    PUBLIC engine_capability_logging
    PUBLIC engine_capability_scene
    PUBLIC engine_capability_input
)

# The global Definition of Done (common.md §1.5): game targets compile with
# warnings-as-errors. The kernel's PUBLIC warning set (-Wall -Wextra
# -Wpedantic) is inherited through engine_kernel; -Werror upgrades it on the
# game's own translation units.
if(MSVC)
    target_compile_options(harrowmere_core PRIVATE /W4 /WX)
else()
    target_compile_options(harrowmere_core PRIVATE -Werror)
endif()

# Plugin deployment: the library lands in the mod's runtime package directory
# (runtime/mods/harrowmere_core/), where the kernel's mod discovery looks for
# it (mod.toml entry = "harrowmere_core" -> libharrowmere_core.so / .dll).
# PREFIX keeps one naming scheme on every platform (the module convention);
# the $ORIGIN entries resolve sibling packages and $ORIGIN/.. the kernel and
# GLFW runtimes in runtime/; WINDOWS_EXPORT_ALL_SYMBOLS publishes the mod's
# C++ surface across the library boundary.
set_target_properties(harrowmere_core PROPERTIES
    PREFIX "lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime/mods/harrowmere_core"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime/mods/harrowmere_core"
    BUILD_RPATH "${ENGINE_PLUGIN_RPATH_BUILD}"
    INSTALL_RPATH "${ENGINE_PLUGIN_RPATH_INSTALL}"
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)
# MinGW: one explicit exported symbol (the mod entry table) disables the
# linker's all-symbol auto-export; this flag publishes the rest of the C++
# surface the same way the modules do.
if(MINGW)
    target_link_options(harrowmere_core PRIVATE -Wl,--export-all-symbols)
endif()

# ---- The published capability surface ---------------------------------------
# harrowmere_capability.hpp is the ONLY header any consumer (the app,
# harrowmere_ui, third-party native mods) compiles against. The INTERFACE
# target carries the include path only - no consumer ever links this mod's
# implementation; acquisitions are resolved at runtime by the kernel.
add_library(harrowmere_capability_headers INTERFACE)
target_include_directories(harrowmere_capability_headers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge")

# The portability guard: the published header must stay implementation-,
# platform- and vendor-agnostic (see cmake/capability_header_guard.cmake) -
# the same mechanical rule the engine modules keep for their headers.
engine_guard_capability_header("${CMAKE_CURRENT_SOURCE_DIR}/src/bridge/harrowmere_capability.hpp")

# ---- Game data ships with the package ---------------------------------------
# All base content lives in data/ as JSON (D-DATA-1, task 0.4 fills the
# loader); the directory ships empty (with its .gitkeep) so the packaging
# step and the runtime layout exist from day one.
add_custom_command(TARGET harrowmere_core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/data"
            "${CMAKE_BINARY_DIR}/runtime/mods/harrowmere_core/data"
    COMMENT "packaging harrowmere_core game data")

# ---- Unit tests ---------------------------------------------------------------
# The harness nucleus lands with task 0.7 (its card's two pinned tests);
# task 0.6 owns the full harness (test-count gate, scripts/ci.sh). The switch
# keeps a packager's default path identical when the test binary is unwanted;
# the vendored doctest the tests compile against lives in third_party/doctest.
option(HARROWMERE_CORE_BUILD_TESTS "Build the harrowmere_core unit tests" ON)
if(HARROWMERE_CORE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
