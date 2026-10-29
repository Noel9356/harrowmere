# lua_scripting module

The script-mod host. This module embeds Lua 5.4 (via sol2) and implements the kernel's `ScriptModHost` surface (`kernel/include/engine/kernel/component_interface.hpp`): for every discovered script mod whose declared `host_capability` resolves to the `scripting` family member this module provides (`scripting.lua`), it creates an isolated Lua state, loads the package's entry file, and exposes a permission-checked `engine` bridge table. The capability publishes no C++ header on purpose: its consumer surface IS the Lua bridge, and the kernel reaches the hosting surface through the kernel-owned `ScriptModHost` interface. Script mods therefore need no build system, no compiler, and no C++ knowledge — a `mod.toml` plus a `.lua` file is a complete mod (`mods/README.md`). A mod never names its host: the kernel resolves the declared host capability to whichever active module provides it.

## The bridge

The `engine` table is the host's small in-tree surface, and every capability-level bridge function is contributed by the module that owns the capability through a `LuaBridgeExtension` (see "Lua bridge extensions" below). The host's own table lists only what it owns:

| Function | Effect |
|---|---|
| `engine.log_trace(msg)` / `log_debug` / `log_information` / `log_warning` / `log_error` | write through the logging capability (required by this module) |
| `engine.subscribe_key_pressed(fn)` / `engine.subscribe_key_released(fn)` | call `fn(key_name)` on key events (host-level event handler tables) |
| `engine.subscribe_frame_begin(fn)` | call `fn(frame_index, delta_seconds, elapsed_seconds)` once per loop iteration (the kernel heartbeat; one-argument handlers keep working) (host-level event handler tables) |
| `engine.subscribe_fixed_update(fn)` | call `fn(step_delta_seconds)` on the opt-in fixed-timestep lane (`[engine].fixed_update_hz`) (host-level event handler tables) |
| `engine.frame_delta_seconds()` / `engine.frame_elapsed_seconds()` | the loop clock as read-only numbers (host-owned; scripts cannot measure time themselves) |
| `engine.subscribe_text_input(fn)` | call `fn(codepoint)` on the platform's layout-aware character stream |
| `engine.window_size()` | returns `width, height` of the main window (optional capability: `{0, 0}` in a headless run) |
| `engine.create_timer(interval, repeating, fn)` / `engine.cancel_timer(handle)` | self-contained timer surface (host-owned) |

## Lua bridge extensions

The host's own engine table carries only the in-tree surface: logging, window_size, the subscribe_* event handler tables and the self-contained timer API. **Every other** `engine.*` bridge function is contributed by the module that owns the capability through a `LuaBridgeExtension`, an opt-in capability that ships next to the module whose capability it bridges. Adding a new module with its own Lua bridge therefore never touches the lua_scripting source, manifest, or build files: the new module declares a member of the `scripting.lua_bridge` family, implements the `LuaBridgeExtension` interface, and the host picks it up through the same `acquireEachCapability<LuaBridgeExtension>()` call every other extension answers.

### The contract

The published header `lua_bridge_extension.hpp` (this module's `engine_capability_lua_scripting` INTERFACE target) defines a single pure-virtual interface:

```cpp
class LuaBridgeExtension
{
public:
    static constexpr const char *capabilityName = "scripting.lua_bridge";

    virtual const char *extensionName() const = 0;
    virtual void installLuaBridge(luaState *lua) = 0;
};
```

The capability is a **family**: `scripting.lua_bridge` is the family name, and every member appends one further segment (`scripting.lua_bridge.screenshot`, `scripting.lua_bridge.renderer`, ...). The kernel's multi-provider acquisition collects every active member into one vector. The host acquires that vector at construct time through `ComponentContext::acquireEachCapability<LuaBridgeExtension>()` and calls `installLuaBridge` on each one for every freshly created script state, AFTER the host's own `engine` table is in place. Zero providers is a valid state (the vector stays empty) — the host simply installs nothing on top of its own surface.

### Adding a Lua bridge to a new module

A module that wants its own Lua bridge does four things — all confined to that module's directory:

1. **Implement `LuaBridgeExtension`** as a dedicated class owned by the module. Define it in the module's `.cpp` (in an anonymous namespace) so the concrete type stays private; the header holds only `std::unique_ptr<LuaBridgeExtension>`. The bridge holds a raw pointer back to the module's capability (the module IS its capability through inheritance, so `this` suffices), so bridge functions call the module's methods directly — no capability acquisition, no permission check, no shared state.
2. **Return the extension pointer from `capabilityPointerFor`** for the family member name `"scripting.lua_bridge.<member>"` (e.g. `"scripting.lua_bridge.screenshot"`).
3. **Declare the member in the plugin descriptor's `providedCapabilities`** in `module_plugin.cpp`.
4. **Link `engine_capability_lua_scripting` and `engine_sol2_bindings`** in `CMakeLists.txt` — the contract header is published by this module (the host), and sol2 is needed to wrap the `luaState*` the host hands the extension at install time.

That is the whole change. The lua_scripting host discovers the extension through the same `acquireEachCapability<LuaBridgeExtension>()` call every other extension answers, in the resolver's construction order; the host never names a capability it does not own, so its manifest, source and build files never grow as extensions are added. Every in-tree bridge ships as an extension through the same family edge; adding or removing any of them in a run is one toggle in the assembly, never a change to this module. To see which capabilities currently carry a Lua bridge, grep the tree for `scripting.lua_bridge.` member declarations in `module_plugin.cpp` files.

### Script isolation and reload

Each script runs in its own Lua state, so script mods cannot see each other's globals, and a crashing script surfaces as a load/call error diagnostic naming the mod instead of taking the app down. Hot reload is keep-last-good: the edited script loads into a fresh state first, and the previous environment retires only when the new one came up clean — a reload that fails keeps the old code running (a mod is never unloaded by a typo), with the failure logged precisely. Permissions are enforced at the startup boundary: a script mod's declared grants (for example `logging.write` — see `mods/README.md`) are validated when the run assembles, and a denied permission fails the run precisely; the sandbox does the rest (only whitelisted libraries open, `os`/`io`/`debug` never do), and bridge calls execute through pointers their providers captured at construct — the per-call cost is the call itself, no per-call grant re-check. The capabilities the host's own bridge calls into are resolved once at construct through the non-throwing probe (`tryAcquireCapability`): this module only requires `logging` (window rides the optional edge), so `engine.window_size` answers `{0, 0}` when no window provider is active instead of paying an exception, and the cached pointer makes the per-call cost a null check.

### Per-script isolation

The host calls every extension on every script state. A script mod runs in its own isolated Lua state (one state per mod), so an extension that captures per-mod data must do so through the Lua registry of the state it is given, not through shared C++ state — the extension object itself is shared across every script mod's state. An extension whose bridge functions only call back into its module's own capability surface (the common case) needs no per-state storage at all: the module is a singleton per process, and capability calls are stateless from the script's perspective.

### Failure modes

An extension that raises during `installLuaBridge` is logged with the offender's `extensionName` and skipped for that script state — the host's own engine table and the other extensions' contributions remain usable, and the failed extension's entries stay nil in the engine table. A script calling one of them surfaces "attempt to index a nil value" from the getfield, a precise Lua error instead of a half-installed binding that calls into a partially-constructed extension. An extension that the build excludes entirely (its module's `ENGINE_BUILD_MODULE_<NAME>` is `OFF`, or no mod demands its capability) is simply absent from the `acquireEachCapability` vector: a script calling its functions surfaces the same nil-getfield error, the once-logged `scripting.lua_bridge` host diagnostic confirms the assembly brought no extension for that member name.

## Wiring

| Manifest field | Value |
|---|---|
| provided capabilities | `scripting.lua` (the `scripting` family's implementation member — the script-host surface behind `ScriptModHost`) |
| required capabilities | `logging` |
| optional capabilities | `window` (the one concrete optional edge: `window_size` degrades to `{0, 0}` in a headless run — required, the scripting capability would drag a window into every assembly that hosts scripts, and a headless server, batch simulation or CI run could not host script mods at all) and `scripting.lua_bridge` (the family other modules provide members of to install their own Lua bindings — see "Lua bridge extensions" above). The family edge subsumes every per-module optional capability the host used to declare: each bridge-providing module now declares its own bridge member of the family, and the family edge here orders them all before this host when active. A run without any extension module still starts; the corresponding `engine.*` entries stay nil and a script calling them surfaces a precise Lua error from the getfield. |
| required permissions | `capability.logging` (the `capability.window` grant is derived from the optional edge) |

The module activates on demand: a script mod whose `host_capability` is `scripting` pulls it into the run (nothing else does — a run with no script mods never loads it). The kernel orders every script mod after its host automatically, so a script can assume the modules its bridge calls into are started.

## Configuration

| Key | Type | Default | Meaning |
|---|---|---|---|
| `instruction_budget` | integer | `0` | optional per-invocation execution budget in milliseconds (clamped 0..60000): every script invocation (a handler, a timer tick, a top-level load) that runs longer raises a normal Lua error instead of stalling the frame loop. `0` = off — the historical behavior, zero cost |

Script mods are discovered from disk (`[engine].mod_directory`, default `mods`), and per-mod behavior lives in each package's `mod.toml` and script, not in `engine.toml`.

## Files

- `lua_scripting.hpp` / `lua_scripting.cpp` — the module, the sol2 bridge, per-mod state management, the bridge-extension acquisition and dispatch
- `lua_bridge_extension.hpp` — the published contract every Lua bridge extension compiles against (the `engine_capability_lua_scripting` INTERFACE target)
- `module_plugin.cpp` — the static plugin descriptor (name, factory, provided capabilities)
- `CMakeLists.txt` — target and option wiring (`ENGINE_BUILD_MODULE_LUA_SCRIPTING`); the build acquires Lua 5.4 and sol2 through the dependency tiers in `docs/building.md` (Lua is pinned to 5.4 because sol2 requires it exactly)

The shipped content package `mods/test_scene/` is the minimal live script mod: discovery, manifest validation, hosting and the bridge's log path are exercised by every run that loads it.
