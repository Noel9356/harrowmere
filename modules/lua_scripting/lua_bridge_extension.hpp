#ifndef ENGINE_MODULES_LUA_SCRIPTING_LUA_BRIDGE_EXTENSION_HPP
#define ENGINE_MODULES_LUA_SCRIPTING_LUA_BRIDGE_EXTENSION_HPP

// The published capability surface of the lua_scripting module's bridge
// extension point. A module that wants to expose Lua bindings for its own
// capability implements LuaBridgeExtension, declares
// "scripting.lua_bridge.<member>" among its provided capabilities, and returns
// its LuaBridgeExtension pointer from capabilityPointerFor() for that name.
//
// The lua_scripting module acquires every active provider of the
// "scripting.lua_bridge" family through ComponentContext::acquireEachCapability
// and asks each one to install its bindings into the host's engine table for
// every script state. A module that wants Lua bindings therefore adds one
// extension implementation next to its module code and one capability name to
// its plugin descriptor: nothing in modules/lua_scripting/ needs to know about
// it. Adding a new module with its own Lua bridge touches only files inside
// that module's directory - the lua_scripting host discovers whatever
// extensions the active assembly happens to bring.
//
// Like every published capability header this stays implementation-, platform-
// and vendor-agnostic. luaState is forward-declared so the header pulls in
// neither Lua's headers nor sol2's: the host and every extension wrap the
// pointer themselves with the Lua or sol2 API they already need privately.

// Forward-declared at GLOBAL scope under Lua's own spelling, then aliased into
// the engine namespace: engine::luaState must BE the Lua C API's ::lua_State,
// never a distinct engine-local type. Lua's own lua.h forward-declares exactly
// `struct lua_State;` (a transparent pointer-to-incomplete type), so declaring
// the same name here in translation units that never include lua.h produces a
// type that is identical to the real one everywhere. A plain in-namespace
// forward declaration of a NEW name would create a second, incomplete type
// that no sol::state_view or Lua C API call accepts - the extension signature
// and the host's real states have to name one type on both sides of the
// boundary, in translation units that never include Lua's headers and in
// those that include nothing else.
struct lua_State;

namespace engine
{

// The alias that makes the name usable inside the namespace without pulling
// Lua's headers: identical to ::lua_State in every translation unit, complete
// wherever Lua's own headers were included and incomplete (pointer-only)
// everywhere else - exactly what a capability surface needs.
using luaState = ::lua_State;

// Capability "scripting.lua_bridge": the family every Lua bridge extension
// provides a member of. The family name carries no dot, so the kernel's
// multi-provider acquisition (ComponentContext::acquireEachCapability)
// collects every active provider's member ("scripting.lua_bridge.scene",
// "scripting.lua_bridge.physics", ...) into one vector. Each provider
// installs its own bindings into the host's engine table when the host asks
// it to.
//
// Implementations live next to the module whose capability they bridge: the
// scene_registry module ships the scene bridge, the physics_builtin module
// ships the physics bridge, and so on. The lua_scripting host stays generic
// and never names a capability that does not belong to it - it discovers
// whatever extensions the active assembly happens to bring, in the resolver's
// construction order. The host acquires extensions through ONE optional
// capability ("scripting.lua_bridge"), so its manifest never grows as
// extensions are added: the manifest declares only the family, not its
// members, and adding a new extension module is therefore a change confined
// to that module's own directory.
//
// The bridge extension surface is a Lua-side contract: it speaks only in
// entries installed into the engine table of a script's state. It carries no
// C++ vocabulary of its own and never names another module's published
// capability header, so the published header stays free of every other
// module's types by construction (the portability guard in
// cmake/capability_header_guard.cmake would reject any platform include
// anyway).
class LuaBridgeExtension
{
public:
    // The capability family every extension provides a member of. A module
    // that wants to expose Lua bindings declares a member of this family
    // ("scripting.lua_bridge.scene", "scripting.lua_bridge.physics", ...) in
    // its plugin descriptor's providedCapabilities and returns its
    // LuaBridgeExtension pointer from capabilityPointerFor for that member
    // name. The host resolves every active member through the family.
    static constexpr const char *capabilityName = "scripting.lua_bridge";

    virtual ~LuaBridgeExtension() = default;

    // Human-readable name for diagnostic logging (for example, "scene",
    // "physics", "screenshot"). Reported by the host when an extension's
    // install path raises a Lua error so the operator can name the offender
    // without reading the descriptor's capability list. Plain string with
    // static storage duration.
    virtual const char *extensionName() const = 0;

    // Installs this extension's bindings into the given Lua state. The host
    // guarantees the engine table exists when this is called - the host's
    // own installEngineBridge runs first, creating the "engine" table with
    // its subscribe_* handlers and the timer surface, then every acquired
    // extension's installLuaBridge is called in turn. An extension typically
    // adds new entries to the engine table through sol2:
    //
    //   sol::state_view lua(state);
    //   sol::table engine = lua["engine"];
    //   engine.set_function("request_screenshot", [this]() { requestScreenshot(); });
    //
    // The state is owned by the host; bindings installed here live for the
    // lifetime of that state and are released with it. The state is a
    // different one per script mod (each mod runs in its own isolated Lua
    // state), so this call is invoked once per script state at load time.
    //
    // Per-script isolation: the host calls every extension on every state,
    // so an extension that captures per-mod data must do so through the Lua
    // registry of the state it is given, not through shared C++ state (an
    // extension object is shared across every script mod's state). An
    // extension whose bridge functions only call back into its module's own
    // capability surface - the common case - needs no per-state storage at
    // all: the module is a singleton per process and its capability calls
    // are stateless from the script's perspective.
    //
    // An extension that backs a capability its own module provides (the
    // common case) accesses its module's data directly - the extension is
    // part of the same module, so no capability acquisition is needed and the
    // permission system's grants are not involved. An extension that backs a
    // capability its module CONSUMES (rare) acquires that capability through
    // its own captured ComponentContext, exactly like any other module
    // consumer: it lists the capability as optional in its own manifest, the
    // resolver orders the provider before it, and the derived
    // "capability.<name>" grant covers the acquisition.
    virtual void installLuaBridge(luaState *lua) = 0;
};

} // namespace engine

#endif // ENGINE_MODULES_LUA_SCRIPTING_LUA_BRIDGE_EXTENSION_HPP
