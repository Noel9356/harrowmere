# Building

Requirements: CMake 3.24+, a C++20 compiler, a Vulkan loader, and `glslangValidator` (Vulkan SDK, or `apt install glslang-tools`). Linux additionally needs the X11 development packages GLFW's X11 backend compiles against (xlib, xrandr, xinerama, xcursor, xi, xfixes); the native Wayland backend is compiled in as well when `wayland-scanner` and the Wayland protocols are found at configure time (their dev packages, `libwayland-dev` + `wayland-protocols`), and left out with a status note otherwise - the built binary picks the backend at run time either way, so a Wayland session always works through XWayland at minimum. Windows and macOS need nothing beyond the tools above. No presets are used.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/runtime/engine
```

If the Vulkan loader is not on the default linker path, add `-DCMAKE_PREFIX_PATH=/your/prefix`. On Windows the build defines `WIN32_LEAN_AND_MEAN` and `NOMINMAX` once for the whole tree (engine targets and in-tree mods alike), so no source file defines them itself. MinGW builds additionally deploy the toolchain's runtime DLLs (`libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`) into `build/runtime/` after the link: without them a run started from a shell (or a double-click) whose `PATH` does not carry the compiler's `bin/` directory fails to start with no visible diagnostic - the Windows loader refuses the process before any engine code runs, so the terminal shows nothing at all. With the DLLs deployed the runtime directory is self-contained and the quick start works from any shell.

On a Windows machine whose only toolchain is MinGW, pass the generator explicitly (`cmake -B build -G Ninja`, with the MinGW `bin/` directory on `PATH`): CMake has no MinGW default, and with no Visual Studio to fall back to it selects NMake, whose compiler detection fails before anything is built (`Running 'nmake' '-?' failed ... CMAKE_C_COMPILER not set`). A stale `build/` from such a failed configure also carries the wrong generator, so delete the directory before reconfiguring.

## Running

The executable takes the optional command line documented in `docs/configuration.md`, "Command line" (`--config`, `--set`, `--scene`, `--mods-dir`, `--validate`, `--print-graph`, `--version` / `--help`); a run without flags loads the layered configuration, resolves the component graph, and runs the frame loop until the window closes. An invalid configuration (a missing provider for a demanded capability, a denied permission, an unknown configuration key) fails startup with a precise report. Exit codes: `0` success, `1` a failed `--validate` or a failed startup, `2` a command-line usage error (a malformed flag never touches a backend), `3` graphics-backend validation errors observed during the run. The kernel's `validate()` is a pure dry run - configuration, manifests, graph resolution, permission pass - and touches no backend; an embedder can call it as the first check after every configuration or manifest edit.

Communication latency and frame-time reporting is opt-in (`[diagnostics] latency_report = true`); a normal run stays quiet at shutdown.

On a display-less machine a run needs a Vulkan ICD; any software ICD works, for example Mesa lavapipe (`mesa-vulkan-drivers`), discovered through the standard loader variables:

```bash
export VK_DRIVER_FILES=/path/to/lvp_icd.json   # or VK_ICD_FILENAMES on older loaders
```

Screenshots are written to a `screenshots` folder in the process working directory by default - the project root for the documented quick start (`./build/runtime/engine` from the engine tree) - so captures land where the user actually looks; `[screenshot] output_directory` overrides this (relative paths also resolve against the working directory, absolute paths as-is; see `modules/screenshot_png/README.md`).

## Dependencies

Resolution order per dependency — nothing is fetched when a usable copy already exists:

1. Existing target: a superproject-provided target, or a system package (`find_package` probes GLFW 3.4+, a complete Vulkan SDK, and glm 1.0+).
2. Vendored copy: `third_party/<name>/` if you create it (full CMake project or plain header/source layout).
3. Populated build tree: a `<name>-src` directory from an earlier configure is reused; no network access.
4. Pinned download: release tags for git dependencies, SHA256-verified tarballs for URL downloads.

Every configure prints an origin report:

```text
-- engine dependency origins:
--   vulkan: already available (Vulkan SDK: C:/VulkanSDK/1.4.363.0/Include)
--   glfw: already available (existing target from a superproject or find_package)
--   glm: downloaded (pinned https://github.com/g-truc/glm.git at tag 1.0.3)
--   lua: downloaded (pinned https://www.lua.org/ftp/lua-5.4.8.tar.gz (SHA256 checked))
--   stb: cached single header reused from build/deps/stb (no download)
```

The Vulkan SDK is used whenever it ships `vulkan.hpp` next to `vulkan.h` (the SDK always does — both come from the same Vulkan-Headers release). Only an incomplete install falls back to the pinned Vulkan-Headers + Vulkan-Hpp pair; `-DENGINE_FORCE_PINNED_VULKAN=ON` ignores the SDK entirely. Lua is pinned to 5.4 because sol2 requires it exactly. glm (OpenGL Mathematics) is the module implementations' linear algebra vocabulary — vectors, matrices, the camera projection, the glTF transform composition — linked through the engine-owned `engine_glm_bindings` wrapper, which is also the one place glm's compile-time configuration lives (`GLM_FORCE_DEPTH_ZERO_TO_ONE`: Vulkan clip space, z in [0, 1]; the macro must hold for every translation unit that includes glm, so nothing links glm directly). The published capability headers deliberately stay free of glm — their vocabulary is plain data, so a mod compiles against a capability without inheriting a math library.

FetchContent declarations use plain names, so populated trees are `_deps/<name>-src` and the built-in local-source override is `FETCHCONTENT_SOURCE_DIR_<NAME>` (e.g. `-DFETCHCONTENT_SOURCE_DIR_GLFW=/path/to/glfw`). Engine-internal wrapper targets keep the `engine_` prefix (`engine_lua_library`, `engine_sol2_bindings`, ...) to avoid collisions.

### Where fetched dependencies live

FetchContent separates each dependency automatically:

| Directory | Contents |
|---|---|
| `build/_deps/<name>-src` | source tree, downloaded once, reused forever |
| `build/_deps/<name>-build` | its own CMake build tree (only GLFW ships one) |
| `build/_deps/<name>-subbuild` | FetchContent's populate bookkeeping |

Header-only dependencies the engine wraps itself (Vulkan-Hpp, sol2, toml++) are populate-only: a `-src` directory, no `-build`. stb is a single file cached in `build/deps/stb/`; the Lua tarball is cached in `build/deps/`.

### Offline and air-gapped builds

- `-DENGINE_ALLOW_FETCHCONTENT=OFF` never touches the network: configure fails naming the missing dependency unless it comes from a target, a vendored copy, a populated tree, or the cached Lua tarball.
- `FETCHCONTENT_SOURCE_DIR_<NAME>` points any dependency at a local source tree.
- One online configure, then offline reconfigures forever after. To vendor, drop the source into `third_party/<name>/` (directory does not exist in a fresh checkout; create it).

### Download integrity

Integrity of downloaded dependencies is guaranteed by SHA256 checks, not by TLS: the hash-checked downloads (Lua tarball, stb header) retry without certificate verification, which keeps them working under toolchains without a CA trust store (MinGW's bundled curl/GnuTLS). Under a toolchain with a trust store, installing `curl-ca-bundle.crt` next to `cmake.exe` enables verified TLS as well; vendoring under `third_party/<name>/` avoids downloads altogether.

## Options

The optimized build types (Release, MinSizeRel, RelWithDebInfo - the default) accept `-DENGINE_ENABLE_LTO=ON` to compile and link with link-time (interprocedural) optimization, which lets the compiler inline and specialize the kernel's small cross-boundary functions (capability resolution, event dispatch) across translation units and the kernel/app/plugin library edges. It is opt-in, not the default, because it couples the build to the toolchain's LTO plugin (the configure fails precisely when the toolchain cannot support it) and is left out on Windows, where the kernel's symbol-export mechanism does not compose with whole-program optimization. The runtime hot paths are written to be fast without it - allocation-free resolution, contiguous handler scans, branch-annotated common cases - so the flag is a tuning surface, never a correctness or feature surface. Third-party dependencies (GLFW, Lua, the rest) are unaffected either way: they build exactly as before.

Modules are discovered, not listed: every `modules/<name>/` directory that carries a `CMakeLists.txt` joins the build, and each module's own CMakeLists declares its option, named after the directory (`ENGINE_BUILD_MODULE_<NAME>`, where `<NAME>` is the upper-cased directory name). Adding a module means creating its directory; no shared build file enumerates modules. Excluding a module (`-DENGINE_BUILD_MODULE_<NAME>=OFF`) removes its plugin binary from the default build and the shader steps with it; its published capability header stays compilable, and a run that then demands a capability only it provides fails startup with a precise diagnostic naming the consumer, never a link error. Each module's build surface (its dependencies, its shaders) is described in its own `modules/<name>/README.md`.

Native mod packages are discovered the same way: a `mods/<id>/` directory that carries a `CMakeLists.txt` is added as a subdirectory and builds its library straight into the package's runtime directory.

`ENGINE_BUILD_DEMO=OFF` removes the app executable, the runtime layout assembly (configs and mod packages) and the native mod builds.

## Editor support (clangd)

clangd works out of the box against a configured build tree: the root CMakeLists sets `CMAKE_EXPORT_COMPILE_COMMANDS`, so every configure writes `build/compile_commands.json`, and the repository's `.clangd` points clangd at that database. Why the config file is needed at all: the build lives in `build/` — a SIBLING of the source directories, not a parent — and clangd only walks UP from an edited file when locating a compile database, so without the `.clangd` entry it would find no database and fall back to guessing flags (wrong language standard, unresolved includes, no cross-file index). The `.clangd` file additionally builds the background index in `.cache/clangd` (gitignored) so go-to-definition and find-references work across modules, the kernel and the app from the first session, and it sets fallback flags for headers no translation unit includes yet (the tree's `-std=c++20` and warning set, so editor diagnostics match the build).

A tagged build tree changes the path: with `cmake -B build-clang ...`, override the database from a user-level `~/.clangd` (`CompilationDatabase: build-clang`) instead of editing the repository file. Any editor that speaks clangd — VS Code, Neovim, Helix, Qt Creator — picks the same configuration up; nothing here is editor-specific.

## Runtime layout

After building, `build/runtime/` is relocatable: `engine` (executable), `config/engine.toml`, `shaders/*.spv` (module build output), and `mods/<id>/` holding each package's `mod.toml`, its entry (library or script), its shipped defaults (`config.toml`), its world content (`scene.toml`, when the manifest declares one) and its content assets (`assets/`, when it ships any). Configuration paths resolve relative to the executable directory. The engine ships no content of its own: everything under `mods/` comes from the packages, and the shipped demo package (`test_scene`) carries the demo world and its textures.

One file appears next to the executable at runtime and is derived state, not configuration: `module_plugins.cache`, the kernel's module plugin descriptor cache (docs/architecture.md, "Capability demand"). It remembers each plugin binary's static descriptor keyed by the binary's size and modification stamp, so a restart reads the descriptors of unchanged binaries without loading them and loads only the plugins the run demands. Deleting it is always safe - the next run re-reads every binary and writes it again - and a rebuilt or dropped-in module invalidates its own record automatically (the size or stamp changes, the real descriptor is read, the record is refreshed).

## Shaders

Shaders are module code, not content: the renderer keeps GLSL in `modules/renderer_vulkan/shaders/` and compiles it via the shared `engine_compile_module_shaders` helper into `runtime/shaders/`. There is no project-root shader directory and no engine-side shader asset tree. A missing `glslangValidator` is a precise configure-time error.
