#include "lua_scripting.hpp"

#include <sol/sol.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <tuple>

namespace engine
{

namespace
{

// This module's own capability name, the implementation-bound member of the
// "scripting" family: it appears in the manifest, the plugin descriptor and
// the capability pointer dispatch below, so it is named once here (the kernel
// only ever receives it as data).
constexpr const char *scriptingCapabilityName = "scripting.lua";
// Bridge surface installed into every script state. Handlers live in plain
// Lua tables inside the state, so no sol types leak into the module header and
// no per event heap traffic happens outside the state.
//
// The engine table is the entire script mod ABI. It must stay implementation-,
// platform- and vendor-agnostic by construction: capability level concepts
// only (log lines, key names, window sizes, capture requests). A script
// written against this table runs unchanged on any operating system, any
// windowing backend and any rendering implementation - the renderer can be
// made from Vulkan or OpenGL without a script mod being able to tell.
// Native (C++) mods are not held to this: they may consume engine internals
// directly, at the cost of portability (mods/README.md, section "Portability").
void installEngineBridge(
    sol::state &lua, LuaScriptingModule &module
)
{
    sol::table engineTable = lua.create_named_table("engine");

    engineTable.set_function("log_trace", [&module](const std::string &message) {
        module.logForScripts(LogLevel::Trace, message);
    });
    engineTable.set_function("log_debug", [&module](const std::string &message) {
        module.logForScripts(LogLevel::Debug, message);
    });
    engineTable.set_function("log_information", [&module](const std::string &message) {
        module.logForScripts(LogLevel::Information, message);
    });
    engineTable.set_function("log_warning", [&module](const std::string &message) {
        module.logForScripts(LogLevel::Warning, message);
    });
    engineTable.set_function("log_error", [&module](const std::string &message) {
        module.logForScripts(LogLevel::Error, message);
    });

    engineTable["key_pressed_handlers"]  = lua.create_table();
    engineTable["key_released_handlers"] = lua.create_table();
    engineTable["frame_begin_handlers"]  = lua.create_table();
    engineTable["fixed_update_handlers"] = lua.create_table();
    engineTable["text_input_handlers"]   = lua.create_table();

    engineTable.set_function("subscribe_key_pressed", [&lua](sol::function handler) {
        sol::table handlers = lua["engine"]["key_pressed_handlers"];
        handlers.add(handler);
    });
    engineTable.set_function("subscribe_key_released", [&lua](sol::function handler) {
        sol::table handlers = lua["engine"]["key_released_handlers"];
        handlers.add(handler);
    });
    engineTable.set_function("subscribe_frame_begin", [&lua](sol::function handler) {
        sol::table handlers = lua["engine"]["frame_begin_handlers"];
        handlers.add(handler);
    });
    // The fixed-timestep update surface ([engine].fixed_update_hz > 0 makes
    // the composition root publish EngineFixedUpdateEvent at that rate): a
    // deterministic step callback for sim-shaped scripts, carrying the
    // constant step delta. The variable frame callback above stays the
    // render-shaped tick.
    engineTable.set_function("subscribe_fixed_update", [&lua](sol::function handler) {
        sol::table handlers = lua["engine"]["fixed_update_handlers"];
        handlers.add(handler);
    });
    engineTable.set_function("subscribe_text_input", [&lua](sol::function handler) {
        sol::table handlers = lua["engine"]["text_input_handlers"];
        handlers.add(handler);
    });

    engineTable.set_function("window_size", [&module]() { return module.windowSizeForScripts(); });

    // The frame clock as read-only numbers: the loop's wall-clock delta and
    // elapsed time from the host's own EngineFrameEvent subscription (the
    // scripts cannot measure time themselves - os/io are not opened).
    engineTable.set_function("frame_delta_seconds", [&module]() { return module.frameDeltaForScripts(); });
    engineTable.set_function("frame_elapsed_seconds", [&module]() { return module.frameElapsedForScripts(); });

    // Timers (self-contained; ticked between frames).
    engineTable.set_function("create_timer", [&module](double interval, bool repeating, sol::function callback) {
        return module.createTimerForScripts(interval, repeating, std::move(callback));
    });
    engineTable.set_function("cancel_timer", [&module](std::uint64_t handle) {
        return module.cancelTimerForScripts(handle);
    });

    // Every other engine.* bridge entry (input polling, scene objects, lights,
    // camera, tweens, glTF import, physics, audio, persistence, screenshot)
    // is contributed by the module that owns the capability through a
    // LuaBridgeExtension member of the "scripting.lua_bridge" family. The
    // host calls installBridgeExtensions(lua) AFTER this returns, so each
    // extension's bindings land on top of this host surface in the engine
    // table. Nothing in this host names any other module's capability: the
    // manifest's optionalCapabilities carries the "scripting.lua_bridge"
    // family plus "window" (the one concrete optional edge), and adding a
    // new module with its own Lua bridge is a change confined to that
    // module's directory.
}

} // namespace

LuaScriptingModule::LuaScriptingModule()
{
    componentManifest = ComponentManifest{
        .name    = "lua_scripting",
        .kind    = ComponentKind::Module,
        .version = "0.1.0",
        // This module is a script host: it provides the scripting capability (the
        // kernel resolves script mods' declared host capability to this module and
        // hands the packages over through the ScriptModHost contract).
        // "logging" is the only hard requirement; "window" rides the optional
        // edge (window_size degrades to {0, 0} without it - a headless script
        // host is a supported assembly, not a broken one).
        .providedCapabilities = {scriptingCapabilityName},
        .requiredCapabilities = {"logging"},
        // The bridge's optional capabilities are declared through
        // optionalCapabilities (the use-if-present edges ABI v3 added): a run
        // without, say, the scene module still starts (zero providers is a valid
        // outcome and nothing here pulls a provider in), the providers that DO
        // exist are ordered before this module, and the derived
        // capability.<name> grants replace the old hand-rolled permission list.
        //
        // "scripting.lua_bridge" is the family other modules provide members of to
        // install their own Lua bindings without touching this module: a module
        // that wants its own bridge declares "scripting.lua_bridge.<member>" in
        // its plugin descriptor and returns a LuaBridgeExtension pointer from
        // capabilityPointerFor for that name. acquireEachCapability collects every
        // active member at construct time; the host never names a capability it
        // does not own, so the optionalCapabilities list here never grows as new
        // extension modules are added. The family edge subsumes every per-module
        // optional capability the host used to declare (input, scene, lighting,
        // renderer, animation, asset, physics, audio, ui, persistence) - each of
        // those modules now declares its own bridge member of the family, and the
        // family edge here orders them all before this module when active.
        //
        // "window" is the one concrete optional edge: required, the scripting
        // capability dragged a window into every assembly that hosted scripts -
        // a headless server, a batch simulation, a CI run could not host script
        // mods at all. Optional, window_size answers {0, 0} when no window
        // module is active and nothing else in the surface changes; the derived
        // capability.window grant still covers the probe.
        .optionalCapabilities = {"scripting.lua_bridge", "window"},
        .requiredComponents   = {},
        .requiredPermissions  = {"capability.logging"},
        // The module reads its own section for the first time: the optional
        // per-invocation execution budget.
        .knownConfigKeys = {"scripting.instruction_budget"},
    };
}

LuaScriptingModule::~LuaScriptingModule()
{
    stop();
}

void LuaScriptingModule::construct(
    ComponentContext &context
)
{
    boundContext = &context;
    logger       = &context.acquireCapability<LoggerCapability>();
    // The window capability is the manifest's one concrete optional edge: a
    // rendered run resolves it, a headless run gets nullptr and the host
    // surface degrades exactly one call (window_size -> {0, 0}). The probe
    // and the pull are one walk inside tryAcquireCapability, so the answer
    // cannot go stale between the check and the pointer.
    windowCapability = context.tryAcquireCapability<WindowCapability>();

    // Acquire every active Lua bridge extension contributed by other modules.
    // The multi-provider acquisition of the "scripting.lua_bridge" family is
    // the extension point: any module that wants its own Lua bridge declares a
    // member of the family ("scripting.lua_bridge.<member>") and returns its
    // LuaBridgeExtension pointer from capabilityPointerFor for that name. The
    // resolver ordered every active provider before this module through the
    // optional capability edge, so they are all constructed here. Zero
    // providers is a valid state (the vector stays empty) and the host simply
    // installs nothing on top of its own engine table.
    std::vector<LuaBridgeExtension *> acquiredExtensions = context.acquireEachCapability<LuaBridgeExtension>();
    bridgeExtensions.reserve(acquiredExtensions.size());
    bridgeExtensionNames.reserve(acquiredExtensions.size());
    for (LuaBridgeExtension *extension : acquiredExtensions)
    {
        if (extension == nullptr)
        {
            continue;
        }
        bridgeExtensions.push_back(extension);
        bridgeExtensionNames.push_back(extension->extensionName());
    }
    if (!bridgeExtensions.empty() && logger != nullptr)
    {
        logger->logMessage(
            LogLevel::Information,
            "lua scripting module: discovered " + std::to_string(bridgeExtensions.size())
                + " Lua bridge extension(s) from the active assembly"
        );
    }

    // The optional execution budget ([scripting].instruction_budget, ms):
    // 0 (the default) installs no hook - the historical behavior, zero cost.
    // A positive value bounds every script invocation (handler, timer tick,
    // top-level load) to that many milliseconds; the count hook raises a
    // normal Lua error at overrun, which the existing protected-call paths
    // report like any other script error. The clamp keeps a typo from
    // configuring either a negative budget or a 60-second frame stall.
    instructionBudgetMs
        = std::clamp<std::int64_t>(context.configurationInteger("instruction_budget").value_or(0), 0, 60000);

    // Script mods are discovered by the kernel through their mod.toml files
    // and handed over one by one through the ScriptModHost contract during the
    // construct phase.

    EventBus &bus                                 = context.eventBus();
    EventChannel<KeyPressedEvent> &pressedChannel = bus.channel<KeyPressedEvent>();
    keyPressedSubscription                        = EventSubscription(
        &bus,
        typeid(KeyPressedEvent),
        pressedChannel.subscribe<LuaScriptingModule, &LuaScriptingModule::forwardKeyPressed>(this)
    );

    EventChannel<KeyReleasedEvent> &releasedChannel = bus.channel<KeyReleasedEvent>();
    keyReleasedSubscription                         = EventSubscription(
        &bus,
        typeid(KeyReleasedEvent),
        releasedChannel.subscribe<LuaScriptingModule, &LuaScriptingModule::forwardKeyReleased>(this)
    );

    // The frame clock: the composition root's EngineFrameEvent heartbeat
    // (the loop tick) plus the opt-in EngineFixedUpdateEvent stream. The
    // host needs no renderer capability include for either, and the frame
    // handlers tick in a headless run exactly like a rendered one.
    EventChannel<EngineFrameEvent> &frameChannel = bus.channel<EngineFrameEvent>();
    frameTickSubscription                        = EventSubscription(
        &bus,
        typeid(EngineFrameEvent),
        frameChannel.subscribe<LuaScriptingModule, &LuaScriptingModule::forwardFrameTick>(this)
    );

    EventChannel<EngineFixedUpdateEvent> &fixedUpdateChannel = bus.channel<EngineFixedUpdateEvent>();
    fixedUpdateSubscription                                  = EventSubscription(
        &bus,
        typeid(EngineFixedUpdateEvent),
        fixedUpdateChannel.subscribe<LuaScriptingModule, &LuaScriptingModule::forwardFixedUpdate>(this)
    );

    // Text input: the platform's layout-aware character stream, forwarded to
    // subscribing scripts as Unicode codepoints.
    EventChannel<TextInputEvent> &textInputChannel = bus.channel<TextInputEvent>();
    textInputSubscription                          = EventSubscription(
        &bus,
        typeid(TextInputEvent),
        textInputChannel.subscribe<LuaScriptingModule, &LuaScriptingModule::forwardTextInput>(this)
    );

    // Hot reload (optional module): a script change reloads that mod's state.
    EventChannel<HotReloadEvent> &hotReloadChannel = bus.channel<HotReloadEvent>();
    hotReloadSubscription                          = EventSubscription(
        &bus,
        typeid(HotReloadEvent),
        hotReloadChannel.subscribe<LuaScriptingModule, &LuaScriptingModule::handleHotReload>(this)
    );
}

ScriptModHost *LuaScriptingModule::scriptModHost()
{
    return this;
}

void LuaScriptingModule::hostScriptMod(
    const ScriptModDescription &description
)
{
    pendingScripts.push_back(description);
}

void LuaScriptingModule::start()
{
    for (const ScriptModDescription &scriptMod : pendingScripts)
    {
        loadScript(scriptMod);
    }
    logger->logMessage(
        LogLevel::Information,
        "lua scripting module started with " + std::to_string(environments.size()) + " script mod(s)"
    );
}

void LuaScriptingModule::stop()
{
    keyPressedSubscription.release();
    keyReleasedSubscription.release();
    frameTickSubscription.release();
    fixedUpdateSubscription.release();
    textInputSubscription.release();
    hotReloadSubscription.release();
    // Timers hold sol::function references into the script states: they must
    // die BEFORE the states, or their destructors would resurrect freed
    // Lua registries.
    timers.clear();
    environments.clear();
    pendingScripts.clear();
    // The bridge extensions are owned by their declaring modules (their
    // lifetime outlives this host): only the cached pointers are dropped here.
    bridgeExtensions.clear();
    bridgeExtensionNames.clear();
    boundContext         = nullptr;
    windowCapability     = nullptr;
    mainWindowIdentifier = invalidWindowIdentifier;
    logger               = nullptr;
}

void *LuaScriptingModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    // The scripting capability is realized by the ScriptModHost surface of
    // this module; the kernel resolves script hosting through it.
    if (capabilityName == scriptingCapabilityName)
    {
        return static_cast<void *>(static_cast<ScriptModHost *>(this));
    }
    return nullptr;
}

void LuaScriptingModule::logForScripts(
    LogLevel level, const std::string &message
)
{
    if (logger != nullptr)
    {
        logger->logMessage(level, message);
    }
}

std::tuple<std::int32_t, std::int32_t> LuaScriptingModule::windowSizeForScripts()
{
    if (windowCapability == nullptr)
    {
        return {0, 0};
    }
    // The main window identifier, cached like the ui module caches its
    // overlay target: the window set is fixed for the run (windows are
    // created at the window module's start and never removed), so the
    // per-call identifier-vector copy this used to fetch through
    // windowIdentifiers() paid one heap copy per script call to answer the
    // same question every time. The list now arrives as a read-only
    // reference to the provider's cache, so even the first resolve makes no
    // copy. A first call before any window exists stays uncached
    // (identifier 0 is invalid) and re-resolves on the next call.
    if (mainWindowIdentifier == invalidWindowIdentifier)
    {
        const std::vector<WindowIdentifier> &identifiers = windowCapability->windowIdentifiers();
        if (identifiers.empty())
        {
            return {0, 0};
        }
        mainWindowIdentifier = identifiers.front();
    }
    return {
        static_cast<std::int32_t>(windowCapability->framebufferWidth(mainWindowIdentifier)),
        static_cast<std::int32_t>(windowCapability->framebufferHeight(mainWindowIdentifier))
    };
}

double LuaScriptingModule::frameDeltaForScripts() const
{
    return lastFrameDeltaSeconds;
}

double LuaScriptingModule::frameElapsedForScripts() const
{
    return lastFrameElapsedSeconds;
}

std::uint64_t LuaScriptingModule::createTimerForScripts(
    double intervalSeconds, bool repeating, sol::function callback
)
{
    // The guard reads "not (positive)", not "<= 0": NaN fails both <= 0 and
    // > 0, and a NaN interval would build a timer that fires every frame
    // forever. The upper bound keeps a typo interval from overflowing the
    // nanosecond duration cast (1e300 ns does not fit in int64) and from
    // scheduling anything past a day, which no game timer wants.
    if (!(intervalSeconds > 0.0) || intervalSeconds > 86400.0 || !callback.valid())
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning,
                "lua scripting module: engine.create_timer rejected (interval must be finite and > 0, "
                "callback must be a function)"
            );
        }
        return 0;
    }
    ScriptTimer timer;
    timer.handle    = nextTimerHandle++;
    timer.interval  = intervalSeconds;
    timer.repeating = repeating;
    timer.callback  = std::move(callback);
    timer.nextFire  = std::chrono::steady_clock::now()
                    + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(intervalSeconds)
                    );
    timers.push_back(std::move(timer));
    return timers.back().handle;
}

bool LuaScriptingModule::cancelTimerForScripts(
    std::uint64_t handle
)
{
    // Tombstone, never erase: tickTimers may be on the stack ABOVE this call
    // (a timer callback cancelling another timer is the everyday pattern),
    // and an erase from the vector it is indexing shifts every later entry
    // under its cursor - an out-of-bounds read at best. A handle of 0 marks
    // the entry dead; the next tick skips it and compacts it away (handle 0
    // is never handed out, createTimerForScripts starts at 1).
    for (ScriptTimer &timer : timers)
    {
        if (timer.handle == handle)
        {
            timer.handle   = 0;
            timer.callback = sol::nil; // release the Lua reference immediately
            return true;
        }
    }
    return false;
}

void LuaScriptingModule::tickTimers()
{
    if (timers.empty())
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    // Due timers fire in registration order; a fired one-shot is dropped, a
    // repeating one schedules its next fire from NOW (catch-up would burst a
    // lagged frame into a storm of immediate refires).
    //
    // In-place compaction (the animation module's pattern): survivors move
    // down inside the same buffer - the previous survivor-vector swap
    // reallocated its way to a fresh buffer every tick and moved every
    // still-pending timer twice. Only the entries present at entry are
    // processed: a timer a callback creates during the tick (setTimeout from
    // inside a timer callback) lands past `count` and fires on its own
    // interval from next tick - its nextFire is in the future anyway. Moving
    // each entry out before firing also makes the loop safe against that
    // reentrant growth, which the old range-for over `timers` was not
    // (push_back could reallocate mid-iteration).
    //
    // Cancel-during-callback is handled by tombstoning (see
    // cancelTimerForScripts): a handle of 0 marks the entry dead and the
    // compaction below drops it, so a callback can never erase from the
    // vector this loop is indexing. The tombstoned callback was already
    // released at cancel time, so a dead entry moves as an empty shell.
    const std::size_t count = timers.size();
    std::size_t writeIndex  = 0;
    for (std::size_t readIndex = 0; readIndex < count; ++readIndex)
    {
        ScriptTimer timer = std::move(timers[readIndex]);
        if (timer.handle == 0)
        {
            continue; // cancelled from inside an earlier callback this tick
        }
        if (now < timer.nextFire)
        {
            timers[writeIndex++] = std::move(timer);
            continue;
        }
        // One budget window per callback: a runaway timer burns its own
        // budget, never the whole frame's (and with a configured budget the
        // hook turns the stall into a normal, logged script error).
        ScriptBudgetGuard budgetGuard(*this);
        const sol::protected_function_result result = timer.callback();
        if (!result.valid())
        {
            const sol::error error = result;
            if (logger != nullptr)
            {
                logger->logMessage(
                    LogLevel::Error,
                    "lua scripting module: timer error, the timer was cancelled: " + std::string(error.what())
                );
            }
            continue; // one-shot AND repeating timers die on error: a broken callback stays broken
        }
        if (timer.repeating)
        {
            timer.nextFire       = now
                                 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                     std::chrono::duration<double>(timer.interval)
                                 );
            timers[writeIndex++] = std::move(timer);
        }
    }
    // Shift anything appended during callbacks (indices >= count) down after
    // the compacted prefix, then cut the tail once.
    const std::size_t appended = timers.size() - count;
    for (std::size_t index = 0; index < appended; ++index)
    {
        timers[writeIndex + index] = std::move(timers[count + index]);
    }
    timers.resize(writeIndex + appended);
}

void LuaScriptingModule::installBudgetHook(
    sol::state &lua
) const
{
    if (instructionBudgetMs <= 0)
    {
        return; // the default: no hook, no per-instruction cost, historical behavior
    }
    // The hook recovers this module through the state's registry: one
    // light-userdata store per environment at load, no per-call lookup.
    lua_pushlightuserdata(lua.lua_state(), const_cast<LuaScriptingModule *>(this));
    lua_setfield(lua.lua_state(), LUA_REGISTRYINDEX, "engine_scripting_module");
    lua_sethook(lua.lua_state(), &LuaScriptingModule::instructionBudgetHook, LUA_MASKCOUNT, 1000);
}

void LuaScriptingModule::instructionBudgetHook(
    luaState *lua, lua_Debug * /*debug*/
)
{
    lua_getfield(lua, LUA_REGISTRYINDEX, "engine_scripting_module");
    auto *module = static_cast<LuaScriptingModule *>(lua_touserdata(lua, -1));
    lua_pop(lua, 1);
    if (module == nullptr || module->instructionBudgetMs <= 0 || module->budgetDepth == 0)
    {
        return;
    }
    if (std::chrono::steady_clock::now() > module->budgetDeadline)
    {
        // A normal Lua error: the protected-call wrappers around every entry
        // point report it like any other script error (and a timer dies, per
        // the timer error policy).
        luaL_error(
            lua,
            "script execution budget exceeded (%d ms per invocation)",
            static_cast<int>(module->instructionBudgetMs)
        );
    }
}

LuaScriptingModule::ScriptBudgetGuard::ScriptBudgetGuard(
    LuaScriptingModule &ownerModule
)
    : owner(ownerModule), previousDeadline(ownerModule.budgetDeadline), previousDepth(ownerModule.budgetDepth)
{
    if (owner.instructionBudgetMs > 0)
    {
        owner.budgetDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(owner.instructionBudgetMs);
        ++owner.budgetDepth;
    }
}

LuaScriptingModule::ScriptBudgetGuard::~ScriptBudgetGuard()
{
    owner.budgetDeadline = previousDeadline;
    owner.budgetDepth    = previousDepth;
}

void LuaScriptingModule::removeTimersOfState(
    const sol::state &state
)
{
    timers.erase(
        std::remove_if(
            timers.begin(),
            timers.end(),
            [&state](const ScriptTimer &timer) {
                sol::state_view view(timer.callback.lua_state());
                return view == state;
            }
        ),
        timers.end()
    );
}

void LuaScriptingModule::loadScript(
    const ScriptModDescription &scriptMod
)
{
    const std::filesystem::path &resolved = scriptMod.scriptPath;
    if (!std::filesystem::exists(resolved))
    {
        throw std::runtime_error(
            "lua scripting module: script '" + resolved.string() + "' of mod '" + scriptMod.modId + "' does not exist"
        );
    }

    ScriptEnvironment environment;
    environment.path  = resolved.string();
    environment.modId = scriptMod.modId;
    environment.state = std::make_unique<sol::state>();
    sol::state &lua   = *environment.state;
    // The sandbox boundary. Script mods are third-party content the user
    // installed - the permission system treats them exactly like untrusted
    // input - so the state opens ONLY the libraries a game script needs and
    // every escape hatch the opened libraries carry is removed:
    //   - "package" stays CLOSED: it ships require() with package.loadlib,
    //     which loads any shared library on disk (arbitrary native code from
    //     a script), plus package.cpath/path searchers that widen the reach.
    //   - base's dofile/loadfile (compile-and-run from the filesystem) are
    //     nilled right after the library opens; load()/loadstring stay, but
    //     they only compile strings the script itself already holds.
    //   - "os" and "io" are never opened (files, processes, environment),
    //     and "debug" neither (upvalue/registry introspection is a sandbox
    //     liability). Anything a mod legitimately needs from the outside
    //     world arrives through the engine table's capability bridges, where
    //     the kernel's permission system sees it.
    // The instruction budget hook (below) bounds CPU; the bridge surface
    // bounds reach. collectgarbage remains available (legitimate tuning),
    // bounded indirectly by the process's own memory limits.
    lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);
    lua["dofile"]   = sol::nil;
    lua["loadfile"] = sol::nil;

    installEngineBridge(lua, *this);
    // Each active bridge extension contributes its own bindings to the engine
    // table on top of the host's surface, AFTER installEngineBridge created it.
    // An extension that raises during install is logged with the offender's
    // name and skipped; the host's own surface and the other extensions'
    // contributions remain usable, and the failed extension's entries stay nil
    // in the engine table (a script calling them surfaces a precise Lua error
    // from the getfield, never a crash).
    installBridgeExtensions(lua);
    // Cache the five handler-table references once per environment: the
    // dispatch loops read them every frame instead of re-resolving the
    // engine table chain. The subscribe_* API appends into these same
    // tables, so the caches are the live tables, not copies.
    environment.frameBeginHandlers  = lua["engine"]["frame_begin_handlers"];
    environment.fixedUpdateHandlers = lua["engine"]["fixed_update_handlers"];
    environment.keyPressedHandlers  = lua["engine"]["key_pressed_handlers"];
    environment.keyReleasedHandlers = lua["engine"]["key_released_handlers"];
    environment.textInputHandlers   = lua["engine"]["text_input_handlers"];
    // The budget hook, when configured, also bounds the top-level run: a
    // script that loops forever at load fails startup with a precise error
    // instead of hanging the engine.
    installBudgetHook(lua);

    {
        ScriptBudgetGuard budgetGuard(*this);
        const sol::protected_function_result result = lua.safe_script_file(resolved.string());
        if (!result.valid())
        {
            const sol::error error = result;
            throw std::runtime_error(
                "lua scripting module: script '" + resolved.string() + "' failed: " + error.what()
            );
        }
    }
    if (logger != nullptr)
    {
        logger->logMessage(LogLevel::Information, "lua scripting module: loaded '" + resolved.string() + "'");
    }
    environments.push_back(std::move(environment));
}

void LuaScriptingModule::installBridgeExtensions(
    sol::state &lua
)
{
    // Walk every acquired extension in construction order; each one installs
    // its own bindings into the engine table that installEngineBridge already
    // created. The host stays type-blind end to end: it only forwards the
    // luaState pointer (the Lua C type) to the extension's virtual call, and
    // the extension wraps sol2 around it inside its own translation unit (the
    // published lua_bridge_extension.hpp header pulls in neither Lua nor sol2
    // on purpose, so the host binary's link surface never grows as extensions
    // are added).
    //
    // An extension that raises during install is logged with the offender's
    // extensionName and the install proceeds - the host's own engine table and
    // the other extensions' contributions remain usable. The failed
    // extension's entries never land in the engine table: a script calling one
    // of them surfaces "attempt to index a nil value" from the getfield, a
    // precise Lua error instead of a half-installed binding that calls into a
    // partially-constructed extension. The cache of pointers stays untouched
    // (the extension object itself is alive for the whole run; only its
    // install call failed for this one state).
    for (std::size_t index = 0; index < bridgeExtensions.size(); ++index)
    {
        LuaBridgeExtension *extension = bridgeExtensions[index];
        if (extension == nullptr)
        {
            continue;
        }
        const char *extensionName = bridgeExtensionNames[index];
        try
        {
            extension->installLuaBridge(lua.lua_state());
        } catch (const std::exception &error)
        {
            if (logger != nullptr)
            {
                logger->logMessage(
                    LogLevel::Error,
                    std::string("lua scripting module: bridge extension '")
                        + (extensionName != nullptr ? extensionName : "?")
                        + "' failed to install into a script state and was skipped for that state: " + error.what()
                );
            }
        } catch (...)
        {
            // The "logged and skipped" contract covers every failure shape:
            // an extension that throws something non-standard must not fail
            // the whole run or escape into Lua-land.
            if (logger != nullptr)
            {
                logger->logMessage(
                    LogLevel::Error,
                    std::string("lua scripting module: bridge extension '")
                        + (extensionName != nullptr ? extensionName : "?")
                        + "' failed to install with an unknown error and was skipped for that state"
                );
            }
        }
    }
}

void LuaScriptingModule::forwardKeyPressed(
    const KeyPressedEvent &event
)
{
    if (environments.empty())
    {
        return; // no script mods: no handler can be registered, skip the name build
    }
    dispatchKeyEvent(true, keyCodeName(event.keyCode));
}

void LuaScriptingModule::forwardKeyReleased(
    const KeyReleasedEvent &event
)
{
    if (environments.empty())
    {
        return;
    }
    dispatchKeyEvent(false, keyCodeName(event.keyCode));
}

void LuaScriptingModule::dispatchKeyEvent(
    bool pressed, const std::string &keyName
)
{
    for (ScriptEnvironment &environment : environments)
    {
        // One budget window per environment per event (with a configured
        // budget): a runaway handler raises inside the protected call below
        // and is logged like any other script error.
        ScriptBudgetGuard budgetGuard(*this);
        sol::table &handlers = pressed ? environment.keyPressedHandlers : environment.keyReleasedHandlers;
        // lua_rawlen once per dispatch instead of once per iteration (the
        // old per-iteration size() made the walk quadratic in the handler
        // count). The loop still reads the LIVE table per index, so handlers
        // a script adds directly (engine.frame_begin_handlers is a plain
        // table) keep working; a table mutated DURING dispatch to fewer
        // entries simply reads nil past the new end, which the valid() check
        // below skips.
        const std::size_t handlerCount = handlers.size();
        for (std::size_t index = 1; index <= handlerCount; ++index)
        {
            sol::protected_function handler = handlers[index];
            if (!handler.valid())
            {
                continue;
            }
            const sol::protected_function_result result = handler(keyName);
            if (!result.valid())
            {
                const sol::error error = result;
                if (logger != nullptr)
                {
                    logger->logMessage(
                        LogLevel::Error,
                        "lua scripting module: handler error in '" + environment.path + "' (mod '" + environment.modId
                            + "'): " + error.what()
                    );
                }
            }
        }
    }
}

void LuaScriptingModule::handleHotReload(
    const HotReloadEvent &event
)
{
    if (event.kind == hot_reload_kind::script && !event.modId.empty())
    {
        reloadScriptMod(event.modId);
    }
    // Manifest/scene/config changes need an assembly restart: log it once
    // per event (the watcher already debounces by mtime).
    else if (event.kind == hot_reload_kind::manifest || event.kind == hot_reload_kind::scene
             || event.kind == hot_reload_kind::config)
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Information,
                "lua scripting module: '" + event.relativePath
                    + "' changed; configuration and manifest changes apply on the next run"
            );
        }
    }
}

void LuaScriptingModule::reloadScriptMod(
    const std::string &modId
)
{
    // Keep-last-good replacement: the new state loads and runs FIRST; the
    // old environment retires only after the new one came up clean. A
    // reload that fails (syntax error, startup error, budget blowout at
    // load) erases nothing and KEEPS the previous environment running - a
    // mod is never unloaded by a typo in an edit. Full state replacement on
    // success: the script re-runs from the top (fresh globals, fresh
    // subscriptions into the engine table it rebuilds); the old table
    // entries die with the old state, so no stale handler can fire.
    // (The bus-level event subscriptions are module-level RAII guards tied
    // to THIS module's lifetime, not per-environment - they stay.)
    // The erase below is re-entrancy safe in practice: this handler runs on
    // the HotReloadEvent channel, whose dispatch loop holds no iterator into
    // `environments` - the frame/key dispatch loops are separate call
    // stacks, single-threaded by the engine contract.
    for (std::size_t environmentIndex = 0; environmentIndex < environments.size(); ++environmentIndex)
    {
        ScriptEnvironment &environment = environments[environmentIndex];
        if (environment.modId != modId)
        {
            continue;
        }
        // The timers this environment created die with its state; drop them
        // first (the sol::function members would outlive their Lua registry
        // once the state is destroyed below).
        removeTimersOfState(*environment.state);

        const std::string path        = environment.path;
        const std::string scriptModId = environment.modId;
        try
        {
            ScriptModDescription description;
            description.modId          = scriptModId;
            description.scriptPath     = path;
            description.hostCapability = "scripting";
            loadScript(description); // appends the new environment on success only
        } catch (const std::exception &error)
        {
            // The broken attempt never joined the run (loadScript appends on
            // success only), so the old environment is still live and stays.
            if (logger != nullptr)
            {
                logger->logMessage(
                    LogLevel::Error,
                    "lua scripting module: hot reload of '" + path
                        + "' FAILED, keeping the previous environment: " + error.what()
                );
            }
            return;
        }
        // Success: retire the old environment (its RAII subscriptions
        // release with it) and log. The new state's subscriptions went live
        // during loadScript; the overlap lasts only the microseconds between
        // those steps inside this single dispatch.
        environments.erase(environments.begin() + static_cast<std::ptrdiff_t>(environmentIndex));
        if (logger != nullptr)
        {
            logger->logMessage(LogLevel::Information, "lua scripting module: hot reloaded '" + path + "'");
        }
        return;
    }
}

void LuaScriptingModule::forwardTextInput(
    const TextInputEvent &event
)
{
    for (ScriptEnvironment &environment : environments)
    {
        ScriptBudgetGuard budgetGuard(*this);
        sol::table &handlers = environment.textInputHandlers;
        // lua_rawlen once per dispatch (see dispatchKeyEvent).
        const std::size_t handlerCount = handlers.size();
        for (std::size_t index = 1; index <= handlerCount; ++index)
        {
            sol::protected_function handler = handlers[index];
            if (!handler.valid())
            {
                continue;
            }
            const sol::protected_function_result result = handler(static_cast<std::uint64_t>(event.codepoint));
            if (!result.valid())
            {
                const sol::error error = result;
                if (logger != nullptr)
                {
                    logger->logMessage(
                        LogLevel::Error,
                        "lua scripting module: text input handler error in '" + environment.path + "' (mod '"
                            + environment.modId + "'): " + error.what()
                    );
                }
            }
        }
    }
}

void LuaScriptingModule::forwardFrameTick(
    const EngineFrameEvent &event
)
{
    // The host remembers the clock for the engine.frame_*_seconds getters
    // (scripts read it between events), then dispatches.
    lastFrameDeltaSeconds   = event.deltaSeconds;
    lastFrameElapsedSeconds = event.elapsedSeconds;

    // Timers tick BEFORE the frame handlers: a timer that fires at t=1s runs
    // during the frame that crossed the mark, not the one after it.
    tickTimers();
    for (ScriptEnvironment &environment : environments)
    {
        ScriptBudgetGuard budgetGuard(*this);
        sol::table &handlers = environment.frameBeginHandlers;
        // lua_rawlen once per dispatch (see dispatchKeyEvent): the frame
        // path walks this loop every frame per environment.
        const std::size_t handlerCount = handlers.size();
        for (std::size_t index = 1; index <= handlerCount; ++index)
        {
            sol::protected_function handler = handlers[index];
            if (!handler.valid())
            {
                continue;
            }
            // The delta and elapsed values ride along as extra arguments;
            // existing one-argument handlers ignore them (Lua's calling
            // convention), new scripts take them without a second
            // subscription.
            const sol::protected_function_result result
                = handler(event.frameIndex, event.deltaSeconds, event.elapsedSeconds);
            if (!result.valid())
            {
                const sol::error error = result;
                if (logger != nullptr)
                {
                    logger->logMessage(
                        LogLevel::Error,
                        "lua scripting module: frame handler error in '" + environment.path + "' (mod '"
                            + environment.modId + "'): " + error.what()
                    );
                }
            }
        }
    }
}

void LuaScriptingModule::forwardFixedUpdate(
    const EngineFixedUpdateEvent &event
)
{
    // The deterministic lane: every registered fixed-update handler advances
    // by the SAME constant step delta, however many steps the composition
    // root ran this iteration (the catch-up burst is clamped by
    // [engine].max_fixed_update_steps). No timers here - the scheduler's
    // wall-clock tick stays on the variable heartbeat, the one clock scripts
    // measure real seconds with.
    for (ScriptEnvironment &environment : environments)
    {
        ScriptBudgetGuard budgetGuard(*this);
        sol::table &handlers           = environment.fixedUpdateHandlers;
        const std::size_t handlerCount = handlers.size();
        for (std::size_t index = 1; index <= handlerCount; ++index)
        {
            sol::protected_function handler = handlers[index];
            if (!handler.valid())
            {
                continue;
            }
            const sol::protected_function_result result = handler(event.deltaSeconds);
            if (!result.valid())
            {
                const sol::error error = result;
                if (logger != nullptr)
                {
                    logger->logMessage(
                        LogLevel::Error,
                        "lua scripting module: fixed update handler error in '" + environment.path + "' (mod '"
                            + environment.modId + "'): " + error.what()
                    );
                }
            }
        }
    }
}

} // namespace engine
