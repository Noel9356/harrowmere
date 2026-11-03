add_library(example_species_mod SHARED src/example_species_mod.cpp)
target_link_libraries(example_species_mod PRIVATE engine_kernel)
target_include_directories(example_species_mod PRIVATE "${CMAKE_SOURCE_DIR}/kernel/include")
set_target_properties(example_species_mod PROPERTIES
    PREFIX "lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime/mods/example_species_mod"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime/mods/example_species_mod"
    BUILD_RPATH "${ENGINE_PLUGIN_RPATH_BUILD}"
    INSTALL_RPATH "${ENGINE_PLUGIN_RPATH_INSTALL}"
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)
if(MINGW)
    target_link_options(example_species_mod PRIVATE -Wl,--export-all-symbols)
endif()
if(MSVC)
    target_compile_options(example_species_mod PRIVATE /W4 /WX)
else()
    target_compile_options(example_species_mod PRIVATE -Wall -Wextra -Werror)
endif()
add_custom_command(TARGET example_species_mod POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/data"
            "${CMAKE_BINARY_DIR}/runtime/mods/example_species_mod/data"
    COMMENT "Packaging example_species_mod data"
)
