# The module's own build switch, declared here so everything about the module
# lives in its directory: the root build discovers this CMakeLists and the
# option arrives with it. OFF keeps every target defined - consumers still
# compile against the published capability header below, and an in-tree
# consumer that links the implementation still pulls it - but removes the
# plugin binary from the default build; a run whose mods then demand a
# capability only this module provides fails startup with a precise
# diagnostic naming the consumer, never a link error.
option(ENGINE_BUILD_MODULE_CHARACTER_CUSTOMISATION "Include the character_customisation module in the build" ON)
# The published capability surface of this module: a header-only INTERFACE
# target. Consumers link it to include character_capability.hpp (and the
# std-only character_assembly.hpp beside it) WITHOUT linking this module's
# implementation - the capability graph stays explicit in the target links,
# and no consumer binary depends on another module's symbols at load time.
add_library(engine_capability_character_customisation INTERFACE)
target_include_directories(engine_capability_character_customisation INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

# The portability guard: the published header must stay implementation-,
# platform- and vendor-agnostic (see cmake/capability_header_guard.cmake).
engine_guard_capability_header("${CMAKE_CURRENT_SOURCE_DIR}/character_capability.hpp")

add_library(engine_module_character_customisation SHARED
    module_plugin.cpp
    character_customisation.cpp
)

target_link_libraries(engine_module_character_customisation
    PUBLIC engine_kernel
    PUBLIC engine_capability_character_customisation
    PUBLIC engine_capability_logging
)

# Module plugin deployment: the binary lands in runtime/modules/, the
# directory the kernel scans for plugins (see engine_kernel.cpp
# loadModulePlugins); the properties mirror every other module's.
set_target_properties(engine_module_character_customisation PROPERTIES
    PREFIX "lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime/modules"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime/modules"
    BUILD_RPATH "${ENGINE_PLUGIN_RPATH_BUILD}"
    INSTALL_RPATH "${ENGINE_PLUGIN_RPATH_INSTALL}"
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)
if(MINGW)
    target_link_options(engine_module_character_customisation PRIVATE -Wl,--export-all-symbols)
endif()

# Excluded from the default build when the option is OFF: the target stays
# defined for consumers, but nothing builds the plugin binary unless an
# in-tree consumer actually links it (the runtime then simply does not see
# the plugin, and a demand for its capability fails startup precisely).
if(NOT ENGINE_BUILD_MODULE_CHARACTER_CUSTOMISATION)
    set_target_properties(engine_module_character_customisation PROPERTIES EXCLUDE_FROM_ALL ON)
endif()
