#ifndef ENGINE_KERNEL_COMPONENT_INTERFACE_HPP
#define ENGINE_KERNEL_COMPONENT_INTERFACE_HPP

#include "component_manifest.hpp"
#include "event_bus.hpp"
#include "main_thread_scheduler.hpp"
#include "permission_system.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace engine
{

// Transparent string hashing for the kernel's string-keyed unordered
// containers. std::hash<std::string> is not transparent, so a plain
// unordered_set<std::string> cannot look a name up without CONSTRUCTING a
// string for it - a copy of the caller's buffer that only exists to be
// hashed. With this hasher (and std::equal_to<>) the container answers
// find() on a std::string_view - and therefore on a string literal, the
// shape most call sites use - hashing and comparing the caller's characters
// in place: no temporary, no copy, one hash. The container's own keys are
// still std::string, so insertion and iteration are unchanged.
struct CapabilityNameHash
{
    using is_transparent = void;

    std::size_t operator()(
        std::string_view text
    ) const noexcept
    {
        return std::hash<std::string_view>{}(text);
    }
};

class ConfigurationStore;

// One script mod handed over to a hosting module. The kernel discovers script
// packages through their manifests; the host module owns the actual runtime
// (isolated Lua states, bridge tables) and the script lifecycle. Which of the
// package's required capabilities names its host is package data
// (ModManifest::hostCapability): the kernel resolves the provider of that
// capability without knowing what it is called.
struct ScriptModDescription
{
    std::string modId;
    std::filesystem::path scriptPath;
    // The capability, from the package's required set, whose provider hosts
    // the script. The kernel only matches this name against provider
    // declarations; the meaning of the capability lives in the provider's
    // published header.
    std::string hostCapability;
};

// Interface of a module that hosts script mods. A script mod's package
// declares which of its required capabilities the host must provide; the
// kernel resolves that requirement to the active provider and talks to it
// through this contract only - never to a concrete scripting backend.
class ScriptModHost
{
public:
    virtual ~ScriptModHost() = default;

    // Called once per discovered script mod during the construct phase, in
    // dependency order and after the host module itself has constructed.
    // Throwing from here fails startup with a precise diagnostic.
    virtual void hostScriptMod(const ScriptModDescription &description) = 0;
};

// Thrown by ComponentContext when a component acquires a capability it has not
// been granted, and by the kernel on lifecycle misuse. Startup problems are
// collected as diagnostics instead, so this exception means a contract breach.
class ComponentAccessError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// Published by the kernel's reloadConfiguration (driven by the composition
// root's [engine].config_reload_poll_seconds poll) after the layered
// configuration was re-assembled from disk. Declared HERE, in the
// component-facing kernel header, because its consumers are components: any
// module or mod subscribes through its context's event bus without pulling
// in the full kernel class.
//
// The event carries the DIFF, not the whole store: every dotted path whose
// effective value changed, was newly added or was removed (for example
// "renderer.exposure"), in the same spelling the typed context accessors
// read. A handler re-reads exactly the keys it owns and ignores the rest, so
// a reload stays cheap no matter how many keys the assembly carries. World
// content ([scene] paths) is never in the list: the world was pushed into the
// scene registry at start, and the kernel reports scene edits as "requires a
// restart" instead of pretending they applied live. The bus is main-thread
// only and the reload runs on the main thread, so handlers may touch
// anything a frame handler may.
struct ConfigurationReloadedEvent
{
    std::vector<std::string> changedKeys;
};

// Published by the composition root once per frame-loop iteration, after the
// kernel's per-frame services (scheduler tick, deferred flush, the opt-in
// reload poll) and before the renderer's renderFrame call. Declared HERE, in
// the component-facing kernel header, because its consumers are components:
// any module or mod subscribes through its context's event bus without
// pulling in a renderer capability header - the loop heartbeat is the
// engine's own clock, not the renderer's.
//
// The event decouples GAMEPLAY cadence from RENDERING cadence: the renderer's
// FrameBeginEvent stays the render-phase tick (capture queueing, UI new-frame
// work - anything that must run inside the rendered frame), while this event
// is the loop tick every simulation consumer follows. A run with a renderer
// publishes exactly one heartbeat per rendered frame (the loop calls
// renderFrame once per iteration); a run WITHOUT a renderer - a headless
// server, a simulation harness - keeps publishing it, so timers, physics,
// behavior trees, tweens, the net pump and script frame handlers all tick
// where they previously went inert (docs/architecture.md, "The frame clock").
//
// deltaSeconds is the wall time between the previous iteration's publish and
// this one (0.0 on the first iteration - the same seeded-first-frame contract
// the gameplay modules keep); elapsedSeconds accumulates it since the loop
// began. A run whose components never subscribe pays one subscriberCount()
// check per iteration: no clock read, no publish, no channel work.
struct EngineFrameEvent
{
    std::uint64_t frameIndex = 0;
    double deltaSeconds      = 0.0;
    double elapsedSeconds    = 0.0;
};

// Published by the composition root from the fixed-timestep update loop,
// opt-in through [engine].fixed_update_hz (0 = disabled, the default - a run
// that never configures it pays one branch per frame). Where EngineFrameEvent
// reports the variable wall-clock delta, this event carries the CONSTANT step
// delta: simulation consumers that need determinism across frame rates
// advance by exactly deltaSeconds per dispatch, however many steps the loop
// ran this iteration (catch-up steps are clamped by
// [engine].max_fixed_update_steps, so a stalled frame yields a bounded burst,
// never a spiral). Fire order is deterministic: zero or more fixed updates
// immediately after the iteration's EngineFrameEvent, before renderFrame.
struct EngineFixedUpdateEvent
{
    double deltaSeconds = 0.0;
};

// The kernel-side surface behind ComponentContext's capability acquisition.
// Implemented by EngineKernel. Every call resolves the implementation pointer
// PULL-BASED from the declaring provider at acquisition time: the request
// walks the active components, asks the declaring provider's
// capabilityPointerFor hook and hands the result straight to the caller -
// nothing is stored or registered anywhere between the phases.
//
// Every method carries the requester's ComponentKind next to its name: the
// kernel mediates the capability directions with it. Mods (shared library and
// script mods) may acquire capabilities from modules AND from other mods; a
// module is answered only by other modules - a mod-provided capability is
// invisible to a module requester, and the resolver reports a module that
// declares one as a precise startup problem (docs/architecture.md,
// "Capability directions"). The application context (ComponentKind::Application)
// is the composition root's trusted surface and resolves against everything.
class CapabilityResolver
{
public:
    virtual ~CapabilityResolver() = default;

    // Returns the implementation pointer of the named capability. Throws
    // std::logic_error with a precise diagnostic when the request cannot be
    // resolved (no provider, several providers, a provider that returned
    // no implementation, or - for a module requester - no MODULE provider).
    virtual void *resolveCapabilityPointer(
        const std::string &requesterName, ComponentKind requesterKind, const std::string &capabilityName
    ) = 0;

    // The multi-provider acquisition behind ComponentContext::acquireEachCapability:
    // every constructed provider of the named capability (exact declarations
    // first, then family members), each pointer pulled straight from its
    // declaring provider at call time. Empty when nothing resolves - the
    // multi form treats "no provider" as a valid answer (the consumer asked
    // for ALL of them; all of zero is zero), so this never throws for
    // absence. Providers whose capabilityPointerFor returns nullptr are
    // skipped, not errors: the single-acquire path still reports them. For a
    // module requester the answer covers only module providers.
    virtual std::vector<void *> resolveEachCapabilityPointer(
        const std::string &requesterName, ComponentKind requesterKind, const std::string &capabilityName
    ) = 0;

    // The non-throwing probe behind ComponentContext::isCapabilityAvailable:
    // true when exactly one constructed provider resolves the name right now
    // (an exact declaration, or a single family member). Ambiguous or absent
    // resolutions return false - the acquire above would fail either way. A
    // module requester probes only module providers: a capability whose
    // providers are all mods reads as unavailable to a module.
    virtual bool hasSingleCapabilityProvider(const std::string &capabilityName, ComponentKind requesterKind) = 0;

    // The non-throwing pull behind ComponentContext::tryAcquireCapability:
    // the implementation pointer of the named capability when exactly one
    // provider resolves it (the same single-resolution rule as
    // resolveCapabilityPointer), and nullptr for every state the acquire
    // would report as a diagnostic - absent, ambiguous, not granted at the
    // caller's side. A provider that DECLARED the capability but returned no
    // implementation is not an availability question but a contract breach,
    // and still throws here exactly like the throwing acquire: the probing
    // consumer gets the same precise exception instead of a silent nullptr
    // that would hide a broken provider.
    virtual void *tryResolveCapabilityPointer(
        const std::string &requesterName, ComponentKind requesterKind, const std::string &capabilityName
    ) = 0;
};

// Everything a module or a mod may touch from the kernel. The context enforces
// the permission system at the only point where cross component access happens:
// capability acquisition. Acquisition is type-erased by name here; the caller
// casts to the capability interface published by the providing module.
class ComponentContext final
{
public:
    // demandedCapabilities is the kernel's demand-closure set (which capability
    // names some component of this run actually requires). It carries no
    // meaning to the kernel; providers consult it to learn whether one of
    // THEIR OWN capabilities is wanted before contributing work. The app
    // context receives null (the app never contributes capability work).
    // mainThreadScheduler is the kernel-owned timed-callback service
    // (scheduleAfter / scheduleEvery / tick - see main_thread_scheduler.hpp)
    // every component reaches through scheduler() below.
    ComponentContext(
        EventBus &eventBus,
        CapabilityResolver &capabilityResolver,
        PermissionSystem &permissionSystem,
        const ConfigurationStore &configuration,
        std::string componentName,
        ComponentKind componentKind,
        const std::unordered_set<std::string, CapabilityNameHash, std::equal_to<>> *demandedCapabilities,
        MainThreadScheduler &mainThreadScheduler
    );

    EventBus &eventBus()
    {
        return eventBus_;
    }

    // Acquires the implementation of one capability by name and returns it
    // type-erased. Fails when the permission "capability.<name>" was not
    // granted to this component (the engine blocks the access), or when the
    // name has no single active implementation - the resolver's exception
    // carries the precise diagnostic.
    void *acquireCapability(
        const std::string &capabilityName
    ) const
    {
        // The permission name assembles into the context's retained scratch
        // (see the member declaration below): the derived grant is checked on
        // every acquisition, and the lazy frame-time ones - a runtime
        // opportunist probing for an optional provider - must not pay a heap
        // round trip for a string that is pure function of the argument.
        permissionScratch_.assign(capabilityPermissionPrefix);
        permissionScratch_ += capabilityName;
        if (!permissionSystem_.isGranted(componentName_, permissionScratch_))
        {
            const std::string permissionName = permissionScratch_;
            throw ComponentAccessError(
                "component '" + componentName_ + "' was blocked by the permission system while acquiring '"
                + capabilityName + "': the permission '" + permissionName
                + "' was not granted. Declare the requirement in the manifest."
            );
        }
        return capabilityResolver_.resolveCapabilityPointer(componentName_, componentKind_, capabilityName);
    }

    // Typed convenience over the name-based acquire above: the caller states
    // the capability interface it compiled against (published by the providing
    // module) and the context resolves the pointer through the kernel's pull.
    // The cast is the caller's statement of intent; the kernel stays type
    // blind - this template lives at the consumer side and never names a
    // concrete provider.
    template <typename CapabilityType> CapabilityType &acquireCapability() const
    {
        void *capabilityPointer = acquireCapability(std::string(CapabilityType::capabilityName));
        return *static_cast<CapabilityType *>(capabilityPointer);
    }

    // The multi-provider form of the acquire above: resolves EVERY active
    // implementation of the named capability - exact providers first, then
    // family members - and returns them in one pass. This is what turns a
    // capability family into an extension point several mods can fill at
    // once: a consumer that wants every implementation (all scene
    // decorators, every overlay source, all extra lighting contributors)
    // takes the plural form instead of forcing the assembly to pick one
    // provider in [providers]. Permission model is the acquire's: the same
    // "capability.<name>" grant covers the whole family resolution; a
    // missing grant throws exactly like the singular acquire. No provider at
    // all resolves to an empty vector - a plural request with zero
    // implementations is a valid state, not a failure.
    std::vector<void *> acquireEachCapability(
        const std::string &capabilityName
    ) const
    {
        // Same retained scratch as the singular acquire above (see that
        // comment): one assign-and-append, no per-call allocation.
        permissionScratch_.assign(capabilityPermissionPrefix);
        permissionScratch_ += capabilityName;
        if (!permissionSystem_.isGranted(componentName_, permissionScratch_))
        {
            const std::string permissionName = permissionScratch_;
            throw ComponentAccessError(
                "component '" + componentName_ + "' was blocked by the permission system while acquiring '"
                + capabilityName + "': the permission '" + permissionName
                + "' was not granted. Declare the requirement in the manifest."
            );
        }
        return capabilityResolver_.resolveEachCapabilityPointer(componentName_, componentKind_, capabilityName);
    }

    // Typed convenience over the plural name-based acquire: the caller states
    // the capability interface it compiled against; the kernel stays type
    // blind exactly like the singular form.
    template <typename CapabilityType> std::vector<CapabilityType *> acquireEachCapability() const
    {
        std::vector<void *> capabilityPointers = acquireEachCapability(std::string(CapabilityType::capabilityName));
        std::vector<CapabilityType *> typed;
        typed.reserve(capabilityPointers.size());
        for (void *capabilityPointer : capabilityPointers)
        {
            typed.push_back(static_cast<CapabilityType *>(capabilityPointer));
        }
        return typed;
    }

    // The non-throwing form of the acquire: the implementation pointer of the
    // named capability when the permission is granted AND exactly one provider
    // resolves it, nullptr otherwise - never an exception for absence or
    // ambiguity (the throwing acquire's diagnostic stays the way to learn WHY
    // it is absent; this form answers only "can I have it right now"). The
    // probe and the pull are one walk, so the answer cannot go stale between
    // the two calls. Optional dependencies pair naturally with this form: a
    // component probes with isCapabilityAvailable for the manifest-declared
    // optional edge, and a runtime opportunist (the app's window/renderer
    // acquisition, a mod that degrades when a provider is missing) asks here
    // without a try/catch block. A provider that declared the capability but
    // returns no implementation still throws - a broken provider is a
    // contract breach, not an unavailability.
    void *tryAcquireCapability(
        const std::string &capabilityName
    ) const
    {
        permissionScratch_.assign(capabilityPermissionPrefix);
        permissionScratch_ += capabilityName;
        if (!permissionSystem_.isGranted(componentName_, permissionScratch_))
        {
            return nullptr;
        }
        return capabilityResolver_.tryResolveCapabilityPointer(componentName_, componentKind_, capabilityName);
    }

    // Typed convenience over the non-throwing acquire above.
    template <typename CapabilityType> CapabilityType *tryAcquireCapability() const
    {
        void *capabilityPointer = tryAcquireCapability(std::string(CapabilityType::capabilityName));
        return capabilityPointer != nullptr ? static_cast<CapabilityType *>(capabilityPointer) : nullptr;
    }

    // The kernel-owned timed-callback service: scheduleAfter / scheduleEvery
    // return RAII handles (ScheduledTask), the composition root's frame loop
    // ticks the scheduler every iteration, and a run that schedules nothing
    // pays one empty check per tick. See main_thread_scheduler.hpp for the
    // ordering and cancellation contracts.
    MainThreadScheduler &scheduler()
    {
        return mainThreadScheduler_;
    }

    // Whether the demand closure of this run contains the named capability.
    // The kernel treats capability names as opaque strings; the question is
    // only meaningful to a provider asking about one of its OWN capabilities
    // ("should I contribute the work this capability stands for?"). Returns
    // false when the context has no demand set (the app context).
    //
    // The parameter is a std::string_view and the demand set answers
    // heterogeneous lookups (see CapabilityNameHash), so a call site passing a
    // string literal - the usual spelling - hashes and compares its characters
    // in place instead of first materializing a std::string copy of the name.
    bool isCapabilityDemanded(
        std::string_view capabilityName
    ) const
    {
        return demandedCapabilities_ != nullptr
            && demandedCapabilities_->find(capabilityName) != demandedCapabilities_->end();
    }

    // Whether the named capability can be acquired right now: the permission
    // is granted AND exactly one constructed provider resolves the name. This
    // is the probe for a manifest-declared OPTIONAL capability
    // (ComponentManifest::optionalCapabilities): construct asks before
    // acquiring, because a missing provider is a valid state for an optional
    // dependency, not a failure. The resolver ordered this component after
    // the provider when one exists, so the answer is a property of the
    // assembly - never of scan order. Ambiguous or absent -> false; the
    // acquire would fail either way.
    bool isCapabilityAvailable(
        const std::string &capabilityName
    ) const
    {
        permissionScratch_.assign(capabilityPermissionPrefix);
        permissionScratch_ += capabilityName;
        if (!permissionSystem_.isGranted(componentName_, permissionScratch_))
        {
            return false;
        }
        return capabilityResolver_.hasSingleCapabilityProvider(capabilityName, componentKind_);
    }

    // Typed configuration access into the component's own section.
    std::optional<std::string> configurationString(const std::string &key) const;
    std::optional<std::int64_t> configurationInteger(const std::string &key) const;
    std::optional<bool> configurationBoolean(const std::string &key) const;
    std::optional<double> configurationFloating(const std::string &key) const;

    // Typed configuration access into a named shared section (for example the
    // world content in [scene]). Read-only by convention: components may read
    // shared content, only the app authors it.
    std::optional<std::string> configurationStringInSection(const std::string &section, const std::string &path) const;
    std::vector<std::string>
    configurationStringArrayInSection(const std::string &section, const std::string &path) const;
    std::optional<std::int64_t>
    configurationIntegerInSection(const std::string &section, const std::string &path) const;
    std::optional<bool> configurationBooleanInSection(const std::string &section, const std::string &path) const;
    std::optional<double> configurationFloatingInSection(const std::string &section, const std::string &path) const;
    std::vector<double> configurationFloatingArrayInSection(const std::string &section, const std::string &path) const;
    std::size_t configurationArraySizeInSection(const std::string &section, const std::string &path) const;
    // Key names of the table at the path (dynamic-key sections, e.g. the
    // input module's [input.actions] action map); empty when absent.
    std::vector<std::string> configurationTableKeysInSection(const std::string &section, const std::string &path) const;

private:
    EventBus &eventBus_;
    CapabilityResolver &capabilityResolver_;
    PermissionSystem &permissionSystem_;
    const ConfigurationStore &configuration_;
    std::string componentName_;
    // The component's kind, carried so every acquisition can state it to the
    // kernel: the capability directions (a module is answered only by other
    // modules; a mod by modules and mods alike) are enforced on the requester's
    // kind, and the trusted application context resolves against everything.
    ComponentKind componentKind_ = ComponentKind::Application;
    const std::unordered_set<std::string, CapabilityNameHash, std::equal_to<>> *demandedCapabilities_ = nullptr;
    MainThreadScheduler &mainThreadScheduler_;
    // The "capability.<name>" string the four acquisition paths assemble for
    // the permission check, retained across calls: every acquisition - the
    // construct-time ones and the lazy frame-time ones alike - re-derives the
    // grant, and the concatenation this used to build per call is a heap
    // round trip for a value that is a pure function of the capability name.
    // Mutable because the acquires are const; safe because the context lives
    // on the engine's single main thread (the same contract the event bus
    // keeps), no component code runs between the assign and the isGranted
    // read, and the buffer is dead by the time the resolver (or anything
    // that could re-enter an acquisition) is reached. Capacity stabilizes at
    // the longest capability name the component acquires.
    mutable std::string permissionScratch_;
};

// Lifecycle contract for modules. construct acquires dependencies and
// subscribes to events without touching backends; start initializes backends
// and publishes initial state; stop releases what start created in reverse.
// The kernel calls these phases in dependency order and the reverse on
// shutdown.
class ModuleInterface
{
public:
    virtual ~ModuleInterface() = default;

    virtual const ComponentManifest &manifest() const = 0;

    virtual void construct(ComponentContext &context) = 0;
    virtual void start()                              = 0;
    virtual void stop()                               = 0;

    // Returns this module typed as the implementation behind the given
    // capability name, or nullptr when the module does not provide it. The
    // kernel calls this at acquisition time - nothing is stored or registered:
    // the pull goes straight to the declaring provider, every time.
    virtual void *capabilityPointerFor(const std::string &capabilityName) = 0;

    // Returns the script host surface of this module, or nullptr when the
    // module does not host script mods. The kernel asks through this hook when
    // it constructs a script mod whose manifest names this module as host.
    virtual ScriptModHost *scriptModHost()
    {
        return nullptr;
    }
};

// Lifecycle contract for shared library mods. Identical phase shape to
// ModuleInterface: the dependency graph does not care which side of the
// module/mod line a component comes from.
class ModInterface
{
public:
    virtual ~ModInterface() = default;

    virtual const ComponentManifest &manifest() const = 0;

    virtual void construct(ComponentContext &context) = 0;
    virtual void start()                              = 0;
    virtual void stop()                               = 0;

    virtual void *capabilityPointerFor(const std::string &capabilityName) = 0;
};

} // namespace engine

#endif // ENGINE_KERNEL_COMPONENT_INTERFACE_HPP
