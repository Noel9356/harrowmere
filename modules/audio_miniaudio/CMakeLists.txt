# The module's own build switch, declared here so everything about the module
# lives in its directory: the root build discovers this CMakeLists and the
# option arrives with it. OFF keeps every target defined - consumers still
# compile against the published capability header below, and an in-tree
# consumer that links the implementation still pulls it - but removes the
# plugin binary from the default build; a run whose mods then demand a
# capability only this module provides fails startup with a precise
# diagnostic naming the consumer, never a link error.
option(ENGINE_BUILD_MODULE_AUDIO_MINIAUDIO "Include the audio_miniaudio module in the build" ON)

add_library(engine_module_audio_miniaudio SHARED
    module_plugin.cpp
    audio.cpp
)

# miniaudio: vendored, pinned single header (third_party/miniaudio). SYSTEM
# keeps its warnings out of the engine's warning-clean build; the C audio
# backend needs dl for its runtime linking of the platform device APIs.
target_link_libraries(engine_module_audio_miniaudio
    PUBLIC engine_kernel
    PUBLIC engine_capability_audio
    PUBLIC engine_capability_logging
    # engine_capability_lua_scripting publishes the LuaBridgeExtension contract
    # header from the lua_scripting module. The audio module implements that
    # contract to install engine.play_sound / set_master_volume / play_sound_3d
    # / set_listener_position / set_listener_orientation into every script
    # state through the host's acquireEachCapability - the host discovers this
    # extension at runtime, so this is the only build-time coupling between
    # the two modules (an INTERFACE target on a header, not a link against
    # the lua_scripting binary).
    PUBLIC engine_capability_lua_scripting
    PRIVATE engine_miniaudio_bindings
    PRIVATE engine_sol2_bindings
)

set_target_properties(engine_module_audio_miniaudio PROPERTIES
    PREFIX "lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime/modules"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime/modules"
    BUILD_RPATH "${ENGINE_PLUGIN_RPATH_BUILD}"
    INSTALL_RPATH "${ENGINE_PLUGIN_RPATH_INSTALL}"
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)
if(MINGW)
    target_link_options(engine_module_audio_miniaudio PRIVATE -Wl,--export-all-symbols)
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(engine_module_audio_miniaudio PRIVATE dl)
endif()

add_library(engine_capability_audio INTERFACE)
target_include_directories(engine_capability_audio INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

engine_guard_capability_header("${CMAKE_CURRENT_SOURCE_DIR}/audio_capability.hpp")

# Excluded from the default build when the option is OFF: the target stays
# defined for consumers, but nothing builds the plugin binary unless an
# in-tree consumer actually links it (the runtime then simply does not see
# the plugin, and a demand for its capability fails startup precisely).
if(NOT ENGINE_BUILD_MODULE_AUDIO_MINIAUDIO)
    set_target_properties(engine_module_audio_miniaudio PROPERTIES EXCLUDE_FROM_ALL ON)
endif()
