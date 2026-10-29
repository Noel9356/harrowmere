cmake_minimum_required(VERSION 3.24)

project(engine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export the compile database every configure (build/compile_commands.json).
# clangd resolves it through the repository's .clangd file - see that file and
# docs/building.md, "Editor support (clangd)" - so code intelligence works
# out of the box with no per-user setup. Harmless to other generators: one
# JSON file per build tree, already gitignored.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Native mods and module plugins are shared libraries that link the kernel
# dynamically; every engine object must therefore be position independent.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- Platform preprocessor setup -------------------------------------------------
# windows.h must keep min/max as functions and stay lean. Defining this once
# for the whole tree replaces per-file defines, which clash with MinGW's
# libstdc++: its <filesystem> and <string> already define NOMINMAX, so a later
# #define in a source file produces a redefinition warning on every build.
if(WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ---- Runtime layout -------------------------------------------------------------
# Every engine runtime artifact lands in build/runtime/: the executable, the
# kernel shared library, the GLFW runtime the window and input plugins share,
# and the modules/ plugin directory. These two set() calls MUST run BEFORE
# include(engine_dependencies): a directory-scope set() writes the scope at
# the moment it executes, and the dependency fetch's add_subdirectory
# snapshots that scope when it runs. After the include, the fetched GLFW
# shared runtime keeps its default location under _deps/ and the runtime
# layout assembles without it - the window and input plugins then fail to
# load with an unsatisfied glfw dependency.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime")

# ---- Plugin RPATH ---------------------------------------------------------------
# Module plugins (and native mods) are shared libraries living one directory
# BELOW the runtime root (runtime/modules/), so their RPATH needs two entries:
# their own directory (sibling plugins) and its parent (the kernel and the
# fetched GLFW runtime). The dynamic loader token for "the directory this
# binary lives in" is platform-specific - $ORIGIN on Linux and the other
# POSIX systems, @loader_path on macOS - so the entries are defined once here,
# in directory scope, and every module's set_target_properties references the
# variables. Windows needs no RPATH: the loader searches the executable's
# directory, where the kernel DLL already sits.
if(APPLE)
    set(ENGINE_PLUGIN_RPATH_BUILD "@loader_path;@loader_path/..")
    set(ENGINE_PLUGIN_RPATH_INSTALL "@loader_path;@loader_path/..")
else()
    set(ENGINE_PLUGIN_RPATH_BUILD "$ORIGIN;$ORIGIN/..")
    set(ENGINE_PLUGIN_RPATH_INSTALL "$ORIGIN:$ORIGIN/..")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(engine_dependencies)

# ---- Interprocedural optimization (LTO), opt-in ------------------------------
# Cross-translation-unit inlining for the optimized builds: the kernel's
# capability resolution, the event bus and the module entry points all sit on
# small functions called across file boundaries (the app calls into the kernel
# shared library, modules call back through published headers), and link-time
# optimization is what lets the compiler inline and specialize across those
# boundaries instead of paying a call frame for every one. It also couples the
# build to the toolchain's LTO plugin (and, on Windows, interacts with
# WINDOWS_EXPORT_ALL_SYMBOLS' .def generation), so it ships OPT-IN rather
# than default: opt in with -DENGINE_ENABLE_LTO=ON on a toolchain known to
# support it. The runtime hot paths it would specialize are already
# allocation-free and branch-annotated in source, so a build without it keeps
# the same code-level guarantees. Fetched third-party trees are NOT affected:
# the variable is set after engine_dependencies, so GLFW and the other
# vendored dependencies build exactly as they always did.
option(ENGINE_ENABLE_LTO "Enable link-time / interprocedural optimization (opt-in)" OFF)

if(ENGINE_ENABLE_LTO)
    if(WIN32)
        message(STATUS "engine: ENGINE_ENABLE_LTO=ON is not applied on Windows (the kernel's symbol-export mechanism does not compose with whole-program optimization)")
    elseif(CMAKE_BUILD_TYPE MATCHES "Release|MinSizeRel|RelWithDebInfo")
        include(CheckIPOSupported)
        check_ipo_supported(RESULT engineIpoSupported OUTPUT engineIpoError)
        if(engineIpoSupported)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
            message(STATUS "engine: link-time optimization enabled for this build")
        else()
            message(FATAL_ERROR "engine: ENGINE_ENABLE_LTO=ON but this toolchain does not support IPO (${engineIpoError})")
        endif()
    endif()
endif()

# Threading support for the kernel's async task dispatcher and the
# parallelFor utility (worker pools sized from the machine's hardware
# thread count; the frame loop and the bus stay single threaded).
find_package(Threads REQUIRED)
# The portability guard for the modules' published capability headers; every
# module's CMakeLists calls it for the headers it publishes.
include(capability_header_guard)

# ---- Engine targets ---------------------------------------------------------------
# The tree is flat by design: kernel/, modules/<name>/, app/ and mods/ are
# siblings at the root. A module directory is self-contained (sources, its
# published capability headers, shaders); a mod directory is a runtime package
# (mod.toml plus entry sources). There is no central capability directory: a
# capability's types are published by the module that provides it, and
# consumers include that header directly.

add_subdirectory(kernel)

# ---- Modules: discovered, not listed ----------------------------------------------
# Every modules/<name>/ directory that carries a CMakeLists.txt joins the
# build automatically; each module's own CMakeLists declares its
# ENGINE_BUILD_MODULE_<NAME> option (the upper-cased directory name). Adding
# a module therefore means creating its directory - no shared build file
# lists modules or capabilities anywhere, matching the runtime side, where
# the kernel scans the deployed modules directory and reads each plugin's
# static descriptor. The loop runs after include(capability_header_guard)
# because the modules call that guard for the headers they publish.

file(GLOB ENGINE_MODULE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/modules/*")
list(SORT ENGINE_MODULE_DIRECTORIES)

# The demo switch MUST be declared BEFORE the module loop: every module's
# CMakeLists reads it to decide whether its GLSL compiles at build time
# (engine_compile_module_shaders). Declared after the loop - as it once was -
# the option is undefined while the modules configure, no module wires its
# shader steps, and the assembled runtime ships without SPIR-V: the frame
# loop then dies on the first draw with a raw "scene.vert.spv could not be
# opened". An explicitly passed -DENGINE_BUILD_DEMO=ON masked exactly that,
# because a command-line definition exists from the first processed line.
option(ENGINE_BUILD_DEMO "Build the demo app" ON)

foreach(moduleDirectory IN LISTS ENGINE_MODULE_DIRECTORIES)
    if(EXISTS "${moduleDirectory}/CMakeLists.txt")
        add_subdirectory("${moduleDirectory}")
    endif()
endforeach()

if(ENGINE_BUILD_DEMO)
    add_subdirectory(app)
endif()

# ---- Runtime layout -----------------------------------------------------------
# Mod packages are assembled into runtime/mods/<id>/: the manifest always, the
# package's shipped defaults (config.toml) when present, every script file for
# script mods (native libraries are placed there by the mod's own CMakeLists
# through LIBRARY_OUTPUT_DIRECTORY), and the package's content assets
# (assets/) when it ships any - the engine itself ships no content.

if(ENGINE_BUILD_DEMO)
    # Native mods are discovered too: a mods/<id>/ directory that carries a
    # CMakeLists.txt is a self-contained build (it links the kernel and the
    # capability targets it consumes and lands its library in
    # runtime/mods/<id>/ through LIBRARY_OUTPUT_DIRECTORY). Adding a native
    # mod therefore means creating its package directory - the manifest is
    # copied by the packaging rule below, the library is built here.
    file(GLOB ENGINE_NATIVE_MOD_PROJECTS "${CMAKE_CURRENT_SOURCE_DIR}/mods/*/CMakeLists.txt")
    foreach(nativeModProject IN LISTS ENGINE_NATIVE_MOD_PROJECTS)
        get_filename_component(nativeModDirectory "${nativeModProject}" DIRECTORY)
        add_subdirectory("${nativeModDirectory}")
    endforeach()

    set(ENGINE_MOD_PACKAGE_COMMANDS)
    file(GLOB ENGINE_MOD_PACKAGE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/mods/*")
    foreach(modPackageDirectory IN LISTS ENGINE_MOD_PACKAGE_DIRS)
        if(NOT EXISTS "${modPackageDirectory}/mod.toml")
            continue()
        endif()
        get_filename_component(modPackageId "${modPackageDirectory}" NAME)
        list(APPEND ENGINE_MOD_PACKAGE_COMMANDS
             COMMAND ${CMAKE_COMMAND} -E copy_if_different
                     "${modPackageDirectory}/mod.toml"
                     "${CMAKE_BINARY_DIR}/runtime/mods/${modPackageId}/mod.toml")
        if(EXISTS "${modPackageDirectory}/config.toml")
            list(APPEND ENGINE_MOD_PACKAGE_COMMANDS
                 COMMAND ${CMAKE_COMMAND} -E copy_if_different
                         "${modPackageDirectory}/config.toml"
                         "${CMAKE_BINARY_DIR}/runtime/mods/${modPackageId}/config.toml")
        endif()
        if(EXISTS "${modPackageDirectory}/scene.toml")
            # The world content the manifest may declare through its scene
            # key. The manifest accepts any package-relative path, but the
            # conventional name is scene.toml and this rule copies exactly
            # that; a package using a different name brings the file along
            # through its own packaging (native mod CMake rules or assets/).
            list(APPEND ENGINE_MOD_PACKAGE_COMMANDS
                 COMMAND ${CMAKE_COMMAND} -E copy_if_different
                         "${modPackageDirectory}/scene.toml"
                         "${CMAKE_BINARY_DIR}/runtime/mods/${modPackageId}/scene.toml")
        endif()
        file(GLOB modPackageScripts "${modPackageDirectory}/*.lua")
        foreach(modScript IN LISTS modPackageScripts)
            list(APPEND ENGINE_MOD_PACKAGE_COMMANDS
                 COMMAND ${CMAKE_COMMAND} -E copy_if_different
                         "${modScript}"
                         "${CMAKE_BINARY_DIR}/runtime/mods/${modPackageId}/")
        endforeach()
        if(EXISTS "${modPackageDirectory}/assets")
            list(APPEND ENGINE_MOD_PACKAGE_COMMANDS
                 COMMAND ${CMAKE_COMMAND} -E copy_directory
                         "${modPackageDirectory}/assets"
                         "${CMAKE_BINARY_DIR}/runtime/mods/${modPackageId}/assets")
        endif()
    endforeach()

    # Shipped module defaults: every module carries a config.toml next to its
    # code; the kernel scans runtime/config/modules/<name>/config.toml and
    # merges it as the lowest configuration layer (docs/configuration.md, "The
    # layer stack"). The assembly configuration (config/engine.toml) wins over
    # every defaults file.
    file(GLOB ENGINE_MODULE_DEFAULTS "${CMAKE_CURRENT_SOURCE_DIR}/modules/*/config.toml")
    foreach(moduleDefaultsFile IN LISTS ENGINE_MODULE_DEFAULTS)
        get_filename_component(moduleDefaultsDirectory "${moduleDefaultsFile}" DIRECTORY)
        get_filename_component(moduleDefaultsName "${moduleDefaultsDirectory}" NAME)
        list(APPEND ENGINE_MOD_PACKAGE_COMMANDS
             COMMAND ${CMAKE_COMMAND} -E copy_if_different
                     "${moduleDefaultsFile}"
                     "${CMAKE_BINARY_DIR}/runtime/config/modules/${moduleDefaultsName}/config.toml")
    endforeach()

    add_custom_target(engine_runtime_layout ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/config" "${CMAKE_BINARY_DIR}/runtime/config"
        ${ENGINE_MOD_PACKAGE_COMMANDS}
        COMMENT "assembling the runtime layout (config, mod packages with their content assets)")
endif()
