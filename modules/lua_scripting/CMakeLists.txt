# The module's own build switch, declared here so everything about the module
# lives in its directory: the root build discovers this CMakeLists and the
# option arrives with it. OFF keeps every target defined - consumers still
# compile against the published capability header below, and an in-tree
# consumer that links the implementation still pulls it - but removes the
# plugin binary from the default build; a run whose mods then demand a
# capability only this module provides fails startup with a precise
# diagnostic naming the consumer, never a link error.
option(ENGINE_BUILD_MODULE_LUA_SCRIPTING "Include the lua_scripting module in the build" ON)

# The published capability surface of this module: a header-only INTERFACE
# target. Consumers link it to include lua_bridge_extension.hpp WITHOUT
# linking this module's implementation binary - the capability graph stays
# explicit in the target links, and no consumer binary depends on another
# module's symbols at load time. The header is the contract every Lua bridge
# extension compiles against: a module that wants its own bridge links this
# target to get the LuaBridgeExtension interface, implements it next to its
# own module code, and never includes a lua_scripting source file.
add_library(engine_capability_lua_scripting INTERFACE)
target_include_directories(engine_capability_lua_scripting INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(engine_capability_lua_scripting INTERFACE engine_kernel)

# The portability guard: the published header must stay implementation-,
# platform- and vendor-agnostic (see cmake/capability_header_guard.cmake). The
# LuaBridgeExtension contract speaks only in luaState (forward-declared) and
# pure-virtual C++ methods, so the guard sees no platform include.
engine_guard_capability_header("${CMAKE_CURRENT_SOURCE_DIR}/lua_bridge_extension.hpp")

add_library(engine_module_lua_scripting SHARED
    module_plugin.cpp
    lua_scripting.cpp
)

target_link_libraries(engine_module_lua_scripting
    PUBLIC engine_kernel
    PUBLIC engine_capability_lua_scripting
    PUBLIC engine_capability_logging
    PUBLIC engine_capability_window
    PUBLIC engine_capability_hot_reload
    # KeyPressedEvent / KeyReleasedEvent / TextInputEvent (the script
    # subscription surface) come from the input provider's published header -
    # the same header-include-only link every other consumer uses. The frame
    # clock (EngineFrameEvent / EngineFixedUpdateEvent) is declared in the
    # kernel's component_interface.hpp, which engine_kernel already exposes:
    # no renderer capability include at all - a headless run's script frame
    # handlers tick exactly like a rendered one's.
    PUBLIC engine_capability_input
    PRIVATE engine_sol2_bindings
)


# Module plugin deployment: the binary lands in runtime/modules/, the directory
# the kernel scans for plugins (see engine_kernel.cpp loadModulePlugins).
# PREFIX keeps one naming scheme on every platform (libengine_module_lua_scripting.dll
# on Windows, libengine_module_lua_scripting.so on POSIX); the $ORIGIN entries resolve
# the sibling plugins and $ORIGIN/.. the kernel and GLFW runtimes in runtime/;
# WINDOWS_EXPORT_ALL_SYMBOLS publishes the module's C++ surface to the other
# plugins that consume it across the boundary.
set_target_properties(engine_module_lua_scripting PROPERTIES
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
# module's C++ surface to the other plugins' import libraries (the lighting
# techniques reach RendererModule::attachLightingTechnique directly).
if(MINGW)
    target_link_options(engine_module_lua_scripting PRIVATE -Wl,--export-all-symbols)
endif()


# This module provides the "scripting.lua" capability but publishes no C++
# capability header: its consumer surface is the Lua bridge installed into
# every script state (mods/README.md), and the kernel reaches the hosting
# surface through the kernel-owned ScriptModHost interface. Nothing here
# needs a header-only include target.

# Excluded from the default build when the option is OFF: the target stays
# defined for consumers, but nothing builds the plugin binary unless an
# in-tree consumer actually links it (the runtime then simply does not see
# the plugin, and a demand for its capability fails startup precisely).
if(NOT ENGINE_BUILD_MODULE_LUA_SCRIPTING)
    set_target_properties(engine_module_lua_scripting PROPERTIES EXCLUDE_FROM_ALL ON)
endif()
