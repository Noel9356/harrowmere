add_executable(engine
    main.cpp
    app.cpp
)

# The app links only the kernel plus the published capability headers of the
# modules it acquires from: every module implementation arrives at runtime as
# a plugin loaded from <exe_dir>/modules (the kernel's loadModulePlugins
# phase), so the set of available modules is a property of the deployment, not
# of the link line. The capability targets below are INTERFACE (header path
# only) - the app compiles against the providers' published types and stays
# free of their binaries. The ENGINE_BUILD_MODULE_* options keep controlling
# which plugin binaries the build produces; a demanded capability whose
# provider is missing from the modules directory fails startup with a precise
# diagnostic.
target_link_libraries(engine
    PRIVATE engine_kernel
    PRIVATE engine_capability_window
    PRIVATE engine_capability_input
    PRIVATE engine_capability_renderer
    PRIVATE engine_capability_screenshot
)

# Windows only: the frame pacing's sleep-then-spin wait asks the OS timer for
# 1 ms resolution while a CPU-side frame cap is configured (timeBeginPeriod).
# Without it the scheduler's wakeup granularity is the default ~15.6 ms tick,
# and every capped frame's sleep overshoots by up to that much - jitter no
# spin window can pull back. The library is the timeBeginPeriod import.
if(WIN32)
    target_link_libraries(engine PRIVATE winmm)
endif()

# ---- MinGW runtime DLLs ----------------------------------------------------------
# A MinGW-built engine.exe depends on the toolchain's runtime DLLs
# (libstdc++-6.dll, libgcc_s_seh-1.dll, libwinpthread-1.dll), which live in the
# compiler's bin/ directory and are resolved through PATH at launch. A run
# started from a shell (or a double-click, an IDE run button) whose PATH does
# not carry that directory therefore fails to start with no visible
# diagnostic: the Windows loader refuses the process before any engine code -
# and so any engine error report - exists. Copying the DLLs next to the
# executable (the first entry of the DLL search order) makes build/runtime/
# self-contained: the documented quick start works from any shell and the
# folder stays relocatable as a whole. Files the toolchain does not ship are
# skipped, and a non-MinGW toolchain (MSVC, POSIX) needs no deployment. The
# rule lives in this file, next to the target it decorates: CMake only
# accepts add_custom_command(TARGET ...) from the directory that created the
# target, so a top-level copy of this block fails configure with "TARGET
# 'engine' was not created in this directory."
if(MINGW)
    get_filename_component(ENGINE_MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(engineMingwRuntimeDll
            libstdc++-6.dll
            libgcc_s_seh-1.dll
            libwinpthread-1.dll)
        if(EXISTS "${ENGINE_MINGW_BIN_DIR}/${engineMingwRuntimeDll}")
            add_custom_command(TARGET engine POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${ENGINE_MINGW_BIN_DIR}/${engineMingwRuntimeDll}"
                        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${engineMingwRuntimeDll}"
                COMMENT "deploying ${engineMingwRuntimeDll} next to the engine executable")
        endif()
    endforeach()
endif()
