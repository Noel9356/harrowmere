# The module's own build switch, declared here so everything about the module
# lives in its directory: the root build discovers this CMakeLists and the
# option arrives with it. OFF keeps every target defined - consumers still
# compile against the published capability header below, and an in-tree
# consumer that links the implementation still pulls it - but removes the
# plugin binary from the default build; a run whose mods then demand a
# capability only this module provides fails startup with a precise
# diagnostic naming the consumer, never a link error.
option(ENGINE_BUILD_MODULE_ASSET_GLTF "Include the asset_gltf module in the build" ON)

add_library(engine_module_asset_gltf SHARED
    module_plugin.cpp
    asset.cpp
    gltf_json.cpp
    gltf_import.cpp
)

target_link_libraries(engine_module_asset_gltf
    PUBLIC engine_kernel
    PUBLIC engine_capability_asset
    PUBLIC engine_capability_logging
    PUBLIC engine_capability_scene
    # engine_capability_lua_scripting publishes the LuaBridgeExtension contract
    # header from the lua_scripting module. The asset module implements that
    # contract to install engine.import_gltf into every script state through
    # the host's acquireEachCapability - the host discovers this extension at
    # runtime, so this is the only build-time coupling between the two
    # modules (an INTERFACE target on a header, not a link against the
    # lua_scripting binary).
    PUBLIC engine_capability_lua_scripting
    # GLM behind the importer's transform stack (gltf_import.hpp) - module
    # internal, deliberately not on the published asset capability header.
    PRIVATE engine_glm_bindings
    PRIVATE engine_sol2_bindings
)

set_target_properties(engine_module_asset_gltf PROPERTIES
    PREFIX "lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime/modules"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime/modules"
    BUILD_RPATH "${ENGINE_PLUGIN_RPATH_BUILD}"
    INSTALL_RPATH "${ENGINE_PLUGIN_RPATH_INSTALL}"
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)
if(MINGW)
    target_link_options(engine_module_asset_gltf PRIVATE -Wl,--export-all-symbols)
endif()

add_library(engine_capability_asset INTERFACE)
target_include_directories(engine_capability_asset INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

engine_guard_capability_header("${CMAKE_CURRENT_SOURCE_DIR}/asset_capability.hpp")

# Excluded from the default build when the option is OFF: the target stays
# defined for consumers, but nothing builds the plugin binary unless an
# in-tree consumer actually links it (the runtime then simply does not see
# the plugin, and a demand for its capability fails startup precisely).
if(NOT ENGINE_BUILD_MODULE_ASSET_GLTF)
    set_target_properties(engine_module_asset_gltf PROPERTIES EXCLUDE_FROM_ALL ON)
endif()
