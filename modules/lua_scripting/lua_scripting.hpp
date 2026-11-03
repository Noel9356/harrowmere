#ifndef ENGINE_MODULES_LUA_SCRIPTING_HPP
#define ENGINE_MODULES_LUA_SCRIPTING_HPP

#include "hot_reload_capability.hpp"
#include "input_capability.hpp"
#include "logging_capability.hpp"
#include "window_capability.hpp"
#include "lua_bridge_extension.hpp"
#include "engine/kernel/component_interface.hpp" // EngineFrameEvent /
                                                 // EngineFixedUpdateEvent (the
                                                 // script-visible frame clock:
                                                 // declared in the kernel's
                                                 // component-facing header, so
                                                 // the host needs no renderer
                                                 // capability include and the
                                                 // handlers tick in renderer-less
                                                 // runs too)
#include "engine/kernel/event_bus.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

// Forward-declared under Lua's own spelling so this declaration is the same
// type lua.h declares (see lua_bridge_extension.hpp for the full rationale);
// luaState is the engine-namespace alias the module's signatures use.
struct lua_State;
struct lua_Debug;

// sol2 in the module header: the timer registry stores sol::function values
// (copyable references into their owning script state), which needs the
// complete type. Only this module's translation units include this header,
// so the heavy include stays contained to the module.
#include <sol/sol.hpp>

namespace engine
{

class LoggerCapability;
class WindowCapability;

// Module "lua_scripting": hosts Lua 5.4 script mods through sol2. The kernel
// discovers script packages through their mod.toml manifests and hands each
// one over through the ScriptModHost contract; every script receives an
// "engine" table with a small, capability-level host surface plus the surface
// every active Lua bridge extension contributes:
//
//   logging        log_trace/debug/information/warning/error
//   window         window_size (the one capability-backed call in the host;
//                  an OPTIONAL capability - the call degrades to {0, 0} when
//                  no window module is active, so a headless script host is
//                  a supported assembly, not a broken one)
//   (self)         subscribe_key_pressed/released/frame_begin/text_input,
//                  subscribe_fixed_update, frame_delta_seconds,
//                  frame_elapsed_seconds, create_timer, cancel_timer
//   (extensions)   every active provider of the "scripting.lua_bridge"
//                  family installs its own bindings on top of this surface
//                  (see lua_bridge_extension.hpp); adding a new module with
//                  its own Lua bridge therefore never modifies this host.
//
// The host acquires every active Lua bridge extension at construct time
// through ComponentContext::acquireEachCapability<LuaBridgeExtension>()
// (the "scripting.lua_bridge" family, collected from every active member of
// every provider module in the assembly). Each extension installs its own
// bindings into the host's engine table for every freshly created script
// state, AFTER the host's own engine table is in place. The host never names
// a capability it does not own: its manifest's optionalCapabilities list
// carries "scripting.lua_bridge" (the family) plus "window" (the one
// concrete optional edge), so the host's source, manifest and build files
// never grow as new extension modules are added.
// Each script owns an isolated state; a script error fails startup with a
// precise report instead of silently skipping the mod.
class LuaScriptingModule final : public ModuleInterface, public ScriptModHost
{
public:
    LuaScriptingModule();
    ~LuaScriptingModule() override;

    const ComponentManifest &manifest() const override
    {
        return componentManifest;
    }

    void construct(ComponentContext &context) override;
    void start() override;
    void stop() override;

    void *capabilityPointerFor(const std::string &capabilityName) override;
    ScriptModHost *scriptModHost() override;

    // ScriptModHost: called by the kernel once per discovered script package.
    void hostScriptMod(const ScriptModDescription &description) override;

    // Script bridge entry points installed by the host's own engine table.
    // The logging bridge covers this module's required capability; the window
    // bridge answers through the OPTIONAL window edge (absent capability ->
    // {0, 0}); every other capability's bridge is contributed by the module
    // that provides it through a LuaBridgeExtension.
    void logForScripts(LogLevel level, const std::string &message);
    std::tuple<std::int32_t, std::int32_t> windowSizeForScripts();

    // Timers (self-contained; they tick on the frame event, between frames).
    std::uint64_t createTimerForScripts(double intervalSeconds, bool repeating, sol::function callback);
    bool cancelTimerForScripts(std::uint64_t handle);

    // The frame clock as the scripts see it: the wall-clock delta and the
    // elapsed time of the most recent EngineFrameEvent (0.0 before the first
    // tick). Scripts cannot measure time themselves (os/io are not opened),
    // so the host answers from its own heartbeat subscription.
    double frameDeltaForScripts() const;
    double frameElapsedForScripts() const;

private:
    struct ScriptEnvironment
    {
        std::string path;
        std::string modId;
        std::unique_ptr<sol::state> state;
        // Cached references to the five handler tables created with the
        // engine bridge (the subscribe_* API appends into exactly these
        // tables). The per-frame dispatch loops read them directly instead of
        // re-walking ["engine"]["..."] - two sol proxy getfields and a proxy
        // object per environment per dispatch. Declared after `state` so the
        // registry references die before the state they point into.
        sol::table frameBeginHandlers;
        sol::table fixedUpdateHandlers;
        sol::table keyPressedHandlers;
        sol::table keyReleasedHandlers;
        sol::table textInputHandlers;
    };

    // One registered timer. Timers are per-process (any script may create
    // them); the callback carries its own Lua state through the sol::function
    // reference, so one ticking loop serves every environment.
    struct ScriptTimer
    {
        std::uint64_t handle = 0;
        double interval      = 1.0;
        bool repeating       = false;
        sol::function callback;
        std::chrono::steady_clock::time_point nextFire{};
    };

    void loadScript(const ScriptModDescription &scriptMod);
    // Installs the bridge surfaces contributed by other modules into a freshly
    // created script state, AFTER the host's own engine table. Called once per
    // script state at load time. An extension that raises during install (a
    // sol error installing a binding) is logged with the offender's name and
    // the load proceeds - the host's own surface and the other extensions'
    // contributions remain usable, and the failed extension's functions stay
    // nil in the engine table (a script calling them surfaces a precise Lua
    // error from the getfield, never a crash).
    void installBridgeExtensions(sol::state &lua);

    // The optional Lua bridge extensions contributed by other modules (see
    // lua_bridge_extension.hpp). Acquired once at construct time through the
    // multi-provider acquisition of the "scripting.lua_bridge" family; invoked
    // once per script state at load time, AFTER the host's own engine table is
    // installed. Adding a new module with its own Lua bridge therefore never
    // touches this module: the new module declares a member of the family
    // ("scripting.lua_bridge.<member>") in its plugin descriptor, returns its
    // LuaBridgeExtension pointer from capabilityPointerFor for that name, and
    // this host discovers it through the same acquireEachCapability call. The
    // vector is empty when no module in the active assembly contributes a Lua
    // bridge - a valid state, not a failure.
    std::vector<LuaBridgeExtension *> bridgeExtensions;
    // Cached names of the acquired extensions, kept parallel to
    // bridgeExtensions for the install-error diagnostic (which names the
    // offender without re-walking the descriptor list at error time).
    std::vector<const char *> bridgeExtensionNames;

    void forwardKeyPressed(const KeyPressedEvent &event);
    void forwardKeyReleased(const KeyReleasedEvent &event);
    void dispatchKeyEvent(bool pressed, const std::string &keyName);
    void forwardFrameTick(const EngineFrameEvent &event);
    void forwardFixedUpdate(const EngineFixedUpdateEvent &event);
    void forwardTextInput(const TextInputEvent &event);
    void tickTimers();

    ComponentManifest componentManifest;
    ComponentContext *boundContext     = nullptr;
    LoggerCapability *logger           = nullptr;
    WindowCapability *windowCapability = nullptr;
    // The front window identifier, cached on first windowSizeForScripts call
    // after the window module's start (the window set is fixed for the run;
    // see the function comment). invalidWindowIdentifier = not resolved yet;
    // reset at stop so a restarted module re-resolves.
    WindowIdentifier mainWindowIdentifier = invalidWindowIdentifier;
    EventSubscription keyPressedSubscription;
    EventSubscription keyReleasedSubscription;
    EventSubscription frameTickSubscription;
    EventSubscription fixedUpdateSubscription;
    EventSubscription hotReloadSubscription;
    EventSubscription textInputSubscription;
    // EventChannels resolved in construct.
    EventChannel<EngineFrameEvent> *frameTickChannel         = nullptr;
    EventChannel<EngineFixedUpdateEvent> *fixedUpdateChannel = nullptr;
    // The frame clock as the scripts see it (see frameDeltaForScripts).
    double lastFrameDeltaSeconds   = 0.0;
    double lastFrameElapsedSeconds = 0.0;

    // Hot reload (modules/hot_reload_watcher): reloads the named script mod's state.
    void handleHotReload(const HotReloadEvent &event);
    void reloadScriptMod(const std::string &modId);
    // Removes every timer whose callback belongs to the given state (the
    // timers die with their environment; used by the hot reload path).
    void removeTimersOfState(const sol::state &state);

    // The optional per-invocation execution budget ([scripting]
    // .instruction_budget, milliseconds; 0 = off, the default). The hook is
    // installed per state at load; the entry points open a budget window
    // (RAII guard) and the hook raises a Lua error when a window overruns,
    // so a runaway handler or timer cannot stall the frame loop forever.
    static void instructionBudgetHook(luaState *lua, lua_Debug *debug);
    void installBudgetHook(sol::state &lua) const;

    struct ScriptBudgetGuard
    {
        ScriptBudgetGuard(LuaScriptingModule &ownerModule);
        ~ScriptBudgetGuard();

        LuaScriptingModule &owner;
        std::chrono::steady_clock::time_point previousDeadline{};
        std::size_t previousDepth = 0;
    };

    std::vector<ScriptModDescription> pendingScripts;
    std::vector<ScriptEnvironment> environments;
    std::vector<ScriptTimer> timers;
    std::uint64_t nextTimerHandle = 1;

    // The execution budget ([scripting].instruction_budget): 0 disables the
    // hook entirely (the historical behavior - zero cost when off). The
    // active window (the innermost entry point's deadline and the nesting
    // depth) lives here; single-threaded by the engine's contract.
    std::int64_t instructionBudgetMs = 0;
    std::chrono::steady_clock::time_point budgetDeadline{};
    std::size_t budgetDepth = 0;
};

} // namespace engine

#endif // ENGINE_MODULES_LUA_SCRIPTING_HPP
