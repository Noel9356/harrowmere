# engine

A modular C++20 engine kernel. The kernel owns three things only: component lifecycle, the event bus and the permission system. Every feature — windows, rendering, input, screenshots, scripting — lives in an interchangeable module behind a capability it publishes, and mods (Lua scripts or C++ shared libraries) consume the same capabilities.

Capabilities have names, and names come in two shapes: implementation-agnostic families (`renderer`) and implementation-bound members (`renderer.vulkan`). Each module publishes its capability vocabulary from its own directory (`window_capability.hpp` lives in `modules/window_glfw/`), so the provider and its types are one unit: consumers include the provider's header directly, and there is no central contract directory that every new capability has to touch. Nothing is registered anywhere — not modules, not capabilities: acquisition is a pull that asks the declaring provider for its implementation at the moment of the call.

The kernel is a pure mediator. It never names a module and never interprets a capability: modules are anonymous capability providers that describe themselves through static plugin descriptors, mods are anonymous consumers that name only the capabilities they require, and a module is instantiated exactly when an active mod's manifest requires a capability it provides — transitively, through the providers' own requirements. Nothing registers a module anywhere; a run with no mods loads no modules at all. The kernel also enforces the capability directions — a mod consumes capabilities from modules and mods alike, a module is answered only by other modules — and the permissions: a component that touches a capability it did not declare is blocked, and every mod and module is risk-assessed for the user before its code runs, with harmful ones requiring either a standing trust decision in the configuration's `[trust]` section or the user's explicit agreement.

## Platforms and parallelism

The engine builds and runs on Windows, Linux and macOS from one source tree: the platform surface is the loader shim (`LoadLibrary` on Windows, `dlopen` on the POSIX systems), GLFW's window-system backends (Win32, X11/XWayland, native Wayland when present at configure time, Cocoa) and Vulkan's own loader/ICD model — no code path names a CPU or GPU vendor, and the device choice scores every physical device the loader reports (discrete and integrated alike) by the same rules. CPU-heavy batch work is parallel by default: the kernel sizes its worker pools from the machine's hardware thread count (`defaultWorkerCount()` in `kernel/include/engine/kernel/parallel_for.hpp`), the renderer's startup texture decode and the screenshot encoder fan out across the cores, and a module that needs strict ordering passes an explicit one-worker size instead (see `docs/architecture.md`, "Kernel utilities").

## Versioning

The kernel and every component carry their own version, all at `0.1.0`: the kernel's through `kernel/include/engine/kernel/version.hpp` (mirrored by the root CMake `project()` VERSION and the renderer's Vulkan instance identity — the three are one product version), every module's through `ComponentManifest::version` in its constructor, and every mod package's through the `version` key of its `mod.toml`. The component graph report (`--print-graph`, `[diagnostics].graph_report`) prints each component's version next to its name, so a run answers "which revision of which part is actually assembled". Module versions are informational identity; the compatibility gates are separate and older than them — the module plugin ABI version (the kernel refuses a mismatched plugin binary) and the mod manifest's `engine_api`.

## Quick start

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/runtime/engine
```

The executable takes an optional command line (see `docs/configuration.md`, "Command line"): `--config` selects the assembly file, `--set section.key=value` stages an override, `--scene` replaces the world content, `--mods-dir` re-points mod discovery, `--validate` dry-runs the whole startup (configuration, manifests, graph, permissions) without touching a backend - the CI entry point - and `--print-graph` dry-runs and then prints the resolved component graph (every active component with its version, capabilities, permissions and start order; the mod developer's map of the run).

Editor support ships with the tree: a `.clangd` configuration at the root plus the compile database every configure exports (see `docs/building.md`, "Editor support (clangd)") - code intelligence works out of the box.

## Documentation

Documentation lives NEXT TO THE CODE IT DESCRIBES: every module and the content package carry their own `README.md`, and `docs/` keeps only the cross-cutting guides.

| Document | Contents |
|---|---|
| `docs/architecture.md` | system map, capability surfaces and families, capability demand, pure-pull acquisition, capability directions, components and mods, event bus, permissions, risk advisories and the trust gate, lifecycle, extending and conventions |
| `docs/building.md` | configure/build, dependency tiers, `_deps` layout, offline controls, module options, editor support (clangd) |
| `docs/configuration.md` | global and shared `engine.toml` sections ([engine], [providers], [permissions], [trust], [scene], [diagnostics]); per-module keys are documented by their owning module |
| `mods/README.md` | mod packages: the `mod.toml` schema (including `provides`), capability demand, risk advisories and the trust gate, world content, script and native authoring, portability rules |
| `modules/<name>/README.md` | one guide per module: capability wiring, its configuration keys with shipped defaults, implementation notes |
