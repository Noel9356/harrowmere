# The game's UI surface native mod (v0.1: an empty shell whose manifest wiring
# is validated from day one; panels arrive at 7.3P1/7.4P1). Build pattern
# identical to harrowmere_core's - a shared library linking the kernel,
# packaged into its own runtime package directory.
add_library(harrowmere_ui SHARED
    src/ui_harrowmere_plugin.cpp
)

target_link_libraries(harrowmere_ui
    PUBLIC engine_kernel
    # The demanded capabilities' published headers (include path only). The
    # renderer's overlay seam header is what 7.3P1 registers on (D-UI-1); the
    # sim capability's vocabulary arrives through harrowmere_capability_headers
    # when the panels start reading views.
    PUBLIC engine_capability_input
    PUBLIC engine_capability_renderer
    PUBLIC harrowmere_capability_headers
)

# The global Definition of Done (common.md §1.5): game targets compile with
# warnings-as-errors.
if(MSVC)
    target_compile_options(harrowmere_ui PRIVATE /W4 /WX)
else()
    target_compile_options(harrowmere_ui PRIVATE -Werror)
endif()

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

# The UI mod's published capability surface (inert until 7.3P1) gets the same
# purity guard as every other published header.
engine_guard_capability_header("${CMAKE_CURRENT_SOURCE_DIR}/src/ui_harrowmere_capability.hpp")
