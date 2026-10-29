# The module's own build switch, declared here so everything about the module
# lives in its directory: the root build discovers this CMakeLists and the
# option arrives with it. OFF keeps every target defined - consumers still
# compile against the published capability header below, and an in-tree
# consumer that links the implementation still pulls it - but removes the
# plugin binary from the default build; a run whose mods then demand a
# capability only this module provides fails startup with a precise
# diagnostic naming the consumer, never a link error.
option(ENGINE_BUILD_MODULE_LOCALIZATION_TOML "Include the localization_toml module in the build" ON)
# The published capability surface of this module: a header-only INTERFACE
# target. Consumers link it to include localization_capability.hpp WITHOUT
# linking this module's implementation - the capability graph stays explicit
# in the target links, and no consumer binary depends on another module's
# symbols at load time.
add_library(engine_capability_localization INTERFACE)
target_include_directories(engine_capability_localization INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

# The portability guard: the published header must stay implementation-,
# platform- and vendor-agnostic (see cmake/capability_header_guard.cmake).
engine_guard_capability_header("${CMAKE_CURRENT_SOURCE_DIR}/localization_capability.hpp")

add_library(engine_module_localization_toml SHARED
    module_plugin.cpp
    localization.cpp
)

target_link_libraries(engine_module_localization_toml
    PUBLIC engine_kernel
    # The file format is TOML, per the engine's configuration policy - the
    # same parser the kernel and the persistence module carry (toml++), so
    # the tree keeps exactly one TOML implementation.
    PUBLIC engine_tomlplusplus_bindings
    PUBLIC engine_capability_localization
    PUBLIC engine_capability_logging
    # engine_capability_lua_scripting publishes the LuaBridgeExtension contract
    # header from the lua_scripting module. The localization module implements
    # that contract to install engine.localize and the language switch into
    # every script state through the host's acquireEachCapability - the host
    # discovers this extension at runtime, so this is the only build-time
    # coupling between the two modules (an INTERFACE target on a header, not
    # a link against the lua_scripting binary).
    PUBLIC engine_capability_lua_scripting
    PRIVATE engine_sol2_bindings
)


# Module plugin deployment: the binary lands in runtime/modules/, the directory
# the kernel scans for plugins (see engine_kernel.cpp loadModulePlugins).
# PREFIX keeps one naming scheme on every platform (libengine_module_localization_toml.dll
# on Windows, libengine_module_localization_toml.so on POSIX); the $ORIGIN entries resolve
# the sibling plugins and $ORIGIN/.. the kernel and GLFW runtimes in runtime/;
# WINDOWS_EXPORT_ALL_SYMBOLS publishes the module's C++ surface to the other
# plugins that consume it across the boundary.
set_target_properties(engine_module_localization_toml PROPERTIES
    PREFIX "lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime/modules"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime/modules"
    BUILD_RPATH "${ENGINE_PLUGIN_RPATH_BUILD}"
    INSTALL_RPATH "${ENGINE_PLUGIN_RPATH_INSTALL}"
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)
# MinGW: one explicit __declspec(dllexport) (the plugin entry point) disables
# the linker's all-symbol auto-export, and WINDOWS_EXPORT_ALL_SYMBOLS above is
# implemented only for MS-compatible toolchains; this flag publishes the
# module's C++ surface to the other plugins' import libraries.
if(MINGW)
    target_link_options(engine_module_localization_toml PRIVATE -Wl,--export-all-symbols)
endif()

# Excluded from the default build when the option is OFF: the target stays
# defined for consumers, but nothing builds the plugin binary unless an
# in-tree consumer actually links it (the runtime then simply does not see
# the plugin, and a demand for its capability fails startup precisely).
if(NOT ENGINE_BUILD_MODULE_LOCALIZATION_TOML)
    set_target_properties(engine_module_localization_toml PROPERTIES EXCLUDE_FROM_ALL ON)
endif()
