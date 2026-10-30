# The game's authoritative-simulation native mod: a shared library linking the
# kernel, packaged into its own runtime package directory (mod.toml, defaults,
# world claim, data/) by the engine's own runtime-layout target. This file
# copies the build pattern of a module's CMakeLists (the same plugin scanner
# loads both); the mod publishes its capability vocabulary from src/bridge/
# exactly the way a module does.
#
# Object-library shape (task 0.6's contract): every game translation unit
# compiles exactly once, into the OBJECT library `harrowmere_core_objects`;
# the runtime SHARED library and the unit-test binary are both built from
# those objects. Tests therefore link compiled objects, not the mod runtime,
# and run without the engine's plugin scanner (card 0.6's pitfall verbatim).
add_library(harrowmere_core_objects OBJECT
    src/harrowmere_plugin.cpp
    # task 0.2 - fixed-point math, Result, id generation
    src/core/math.cpp
    # task 0.3 - WorldClock + SeedRng
    src/core/clock.cpp
    src/core/rng.cpp
    # task 0.4 - strict JSON layer, schema DSL, config seam
    src/data/json.cpp
    src/data/schema.cpp
    src/data/config.cpp
    # task 1.1 - species / items / classes loaders
    src/data/species.cpp
    src/data/items.cpp
    src/data/classes.cpp
    # task 1.2 - crime / laws / roles / territory templates loaders
    src/data/crime.cpp
    src/data/laws.cpp
    src/data/roles.cpp
    src/data/territory_templates.cpp
    # task 1.3 - the DataRegistry over all data files
    src/data/registry.cpp
    # task 1.4 - the world model: regions, territory tree, doc walks
    src/world/territory.cpp
    src/world/world.cpp
    # task 1.5 - world generation from seed
    src/world/worldgen.cpp
)

# The game source tree: "core/..." and "data/..." include paths, the
# published capability header in src/bridge/, and the internal core headers
# that name "rng/pcg64.hpp" relative to src/core/.
target_include_directories(harrowmere_core_objects PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
)

# The required capabilities' published headers (include path only): the mod
# compiles against the providers' types and stays free of their binaries -
# the implementations arrive at runtime as plugin libraries.
target_link_libraries(harrowmere_core_objects PUBLIC
    engine_kernel
    engine_capability_logging
    engine_capability_scene
    engine_capability_input
)

# nlohmann_json v3.11.3 (vendored at third_party/nlohmann_json, task 0.4):
# include path only, marked SYSTEM so the game's warnings-as-errors never
# fires inside the vendor header. Game code includes it only through
# src/data/json.hpp - the review-enforced single door (D-DATA-1).
target_include_directories(harrowmere_core_objects SYSTEM PUBLIC
    "${CMAKE_SOURCE_DIR}/third_party/nlohmann_json/single_include"
)

# The objects join a SHARED library; every object must be position independent
# (the tree sets PIC globally, pinned here so the object library stands alone).
set_target_properties(harrowmere_core_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# The global Definition of Done (common.md section 1.5): game targets compile
# with warnings-as-errors. The kernel's PUBLIC warning set (-Wall -Wextra
# -Wpedantic) is inherited through engine_kernel; -Werror upgrades it on the
# game's own translation units (the object library compiles them all).
if(MSVC)
    target_compile_options(harrowmere_core_objects PRIVATE /W4 /WX)
else()
    target_compile_options(harrowmere_core_objects PRIVATE -Werror)
endif()

# The runtime mod library: one exported entry table, built from the shared
# objects (exactly one compilation of each TU).
add_library(harrowmere_core SHARED
    $<TARGET_OBJECTS:harrowmere_core_objects>
)

# Plugin deployment: the library lands in the mod's runtime package directory
# (runtime/mods/harrowmere_core/), where the kernel's mod discovery looks for
# it (mod.toml entry = "harrowmere_core" -> libharrowmere_core.so / .dll).
# PREFIX keeps one naming scheme on every platform (the module convention);
# the ORIGIN entries resolve sibling packages and their parent (the kernel and
# GLFW runtimes) in runtime/; WINDOWS_EXPORT_ALL_SYMBOLS publishes the mod's
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
# All base content lives in data/ as JSON (D-DATA-1): the loaders of tasks
# 1.1/1.2 consume exactly these files at startup (1.3's DataRegistry wraps
# them). The build copies the directory into the package so a run sees the
# same content the repo ships.
add_custom_command(TARGET harrowmere_core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/data"
            "${CMAKE_BINARY_DIR}/runtime/mods/harrowmere_core/data"
    COMMENT "packaging harrowmere_core game data"
)

# ---- Unit tests ---------------------------------------------------------------
# The harness (task 0.6): one doctest binary linking harrowmere_core_objects;
# every card's test file is registered there. The switch keeps a packager's
# default path identical when the test binary is unwanted.
option(HARROWMERE_CORE_BUILD_TESTS "Build the harrowmere_core unit tests" ON)
if(HARROWMERE_CORE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
