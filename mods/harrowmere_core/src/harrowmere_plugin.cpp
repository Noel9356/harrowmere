// The native-mod plugin seam of 'harrowmere_core': one exported entry point
// that hands the kernel this binary's entry table, exactly the way a module's
// module_plugin.cpp hands over its descriptor. The contract a native mod
// answers to is the kernel's mod_api.hpp C ABI (engine_query_mod_entry_table
// -> EngineModEntryTable; the module plugin descriptor's capability lists are
// package data for mods and live in mod.toml's `provides`, which the compiled
// manifest below must agree with - the kernel verifies both at startup).
//
// v0.1 (task 0.1): the mod is load-bearing but carries zero game behavior -
// it acquires its requirements, logs its lifecycle, provides the inert
// SimCapability stub and claims the world through its manifest. Every later
// task copies this wiring pattern.
#include "engine/kernel/component_interface.hpp"
#include "engine/kernel/component_manifest.hpp"
#include "engine/kernel/mod_api.hpp"

#include <iterator>
#include <string>

#include "logging_capability.hpp"

#include "harrowmere_capability.hpp"

namespace
{

// The authoritative-simulation mod: construct acquires the logging sink (the
// graph's root dependency), start announces the build, stop flushes and says
// so. The sim capability is provided from the first constructor line - the
// kernel may answer an acquisition the moment this mod is started.
class HarrowmereCoreMod final : public engine::ModInterface
{
public:
    HarrowmereCoreMod()
    {
        // The compiled manifest must agree with mod.toml on identity and on
        // `provides`; the requirement wiring is package data (the kernel
        // resolves the graph from mod.toml alone). Keep the two files in
        // lockstep - a drift is a precise startup error, by design.
        manifest_.name                  = "harrowmere_core";
        manifest_.kind                  = engine::ComponentKind::SharedLibraryMod;
        manifest_.version               = "0.1.0";
        manifest_.providedCapabilities  = {"harrowmere.core"};
        manifest_.requiredCapabilities  = {"logging", "scene", "input"};
        manifest_.optionalCapabilities  = {"physics", "audio", "animation", "asset"};
        manifest_.requiredPermissions   = {"filesystem.write"};
        manifest_.knownConfigKeys       = {
            "harrowmere.tick_rate",
            "harrowmere.time_scale",
            "harrowmere.autosave_game_hours",
            "harrowmere.seed",
        };
    }

    const engine::ComponentManifest &manifest() const override
    {
        return manifest_;
    }

    void construct(engine::ComponentContext &context) override
    {
        // The logging capability is the graph's root dependency; every later
        // system logs through the wrapper task 0.5 pins on top of this sink.
        logging_ = &context.acquireCapability<engine::LoggerCapability>();
    }

    void start() override
    {
        // The pinned startup line: TestBootstrap_GameAssemblyBoots (task 0.1)
        // asserts this exact text in the log of a headless boot.
        log(engine::LogLevel::Information, "harrowmere_core 0.1.0 up");
    }

    void stop() override
    {
        // Flush-then-log: later tasks put save/state teardown here (7.1).
        log(engine::LogLevel::Information, "harrowmere_core stopping (state flushed)");
    }

    // The pull-based provider hook: the kernel asks at acquisition time,
    // every time - nothing is registered anywhere between the phases.
    void *capabilityPointerFor(const std::string &capabilityName) override
    {
        if (capabilityName == harrowmere::SimCapability::capabilityName)
        {
            return &sim_;
        }
        return nullptr;
    }

private:
    void log(engine::LogLevel level, const std::string &message) const
    {
        if (logging_ != nullptr)
        {
            logging_->logMessage(level, message);
        }
    }

    engine::ComponentManifest manifest_;
    engine::LoggerCapability *logging_ = nullptr;
    harrowmere::SimCapability sim_{};
};

engine::ModInterface *createHarrowmereCoreMod()
{
    return new HarrowmereCoreMod();
}

void destroyHarrowmereCoreMod(engine::ModInterface *mod)
{
    delete mod;
}

const EngineModEntryTable harrowmereCoreEntryTable{
    .interfaceVersion = engine::engineModInterfaceVersion,
    .createMod        = &createHarrowmereCoreMod,
    .destroyMod       = &destroyHarrowmereCoreMod,
};

} // namespace

// The one exported symbol of a shared library mod (mod_api.hpp). The kernel
// resolves it by name after dlopen and never links the mod's import library.
extern "C" const EngineModEntryTable *engine_query_mod_entry_table()
{
    return &harrowmereCoreEntryTable;
}
