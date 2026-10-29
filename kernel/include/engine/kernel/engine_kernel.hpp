#ifndef ENGINE_KERNEL_ENGINE_KERNEL_HPP
#define ENGINE_KERNEL_ENGINE_KERNEL_HPP

#include "component_interface.hpp"
#include "component_manifest.hpp"
#include "configuration.hpp"
#include "dependency_resolver.hpp"
#include "dynamic_library.hpp"
#include "event_bus.hpp"
#include "main_thread_scheduler.hpp"
#include "mod_api.hpp"
#include "mod_discovery.hpp"
#include "mod_manifest.hpp"
#include "module_plugin_cache.hpp"
#include "permission_system.hpp"

#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine
{

// Kernel level lifecycle events. They let modules and mods observe the engine
// itself without depending on any other module.
struct EngineStartupCompletedEvent
{};

struct EngineShutdownStartedEvent
{};

// ConfigurationReloadedEvent is declared in component_interface.hpp: it is
// part of the COMPONENT-facing kernel surface (consumers subscribe to it
// through their context's event bus), while the kernel class itself stays in
// this header.

// One component the structural risk assessment flagged as potentially
// harmful: the reasons are generic (running native machine code, requesting
// permissions outside the capability.* namespace, acquiring a broad set of
// capabilities) and never interpret what a capability means. The kernel
// collects one advisory per flagged mod and per flagged ACTIVE module, warns
// about all of them on every run, and requires the user to either trust them
// through the [trust] configuration section or agree to them at startup
// (the trust gate) before their code constructs.
struct ComponentTrustAdvisory
{
    ComponentKind kind = ComponentKind::Module;
    // The mod id or the module name.
    std::string name;
    // One human-readable reason per structural signal.
    std::vector<std::string> reasons;
};

// Owns the minimal kernel: configuration, component graph resolution, the
// permission system and the event bus, and drives the construct / start /
// stop lifecycle in dependency order.
//
// The kernel deliberately knows nothing about windows, rendering or scripting,
// and it does not know any module by name: modules and mods are anonymous
// capability providers and consumers that declare themselves through static
// descriptors and manifests. The kernel never registers a module, a service
// or a capability: it only mediates. It resolves who requires which capability,
// orders the construct phases, pulls every acquisition straight from the
// declaring provider and enforces permissions. Startup problems are collected
// into one precise report instead of failing with the first error.
class EngineKernel final : public CapabilityResolver
{
public:
    EngineKernel() = default;
    ~EngineKernel();

    EngineKernel(const EngineKernel &)            = delete;
    EngineKernel &operator=(const EngineKernel &) = delete;

    // Configuration file path, relative to the executable directory when not
    // absolute. Loading happens during validate/construct.
    void setConfigurationFile(const std::filesystem::path &configurationPath);

    // Scene file: replaces the assembly configuration's [scene]
    // section wholesale once every layer has merged (the file may define only
    // [scene]). Relative paths resolve against the executable directory, like
    // the configuration file.
    void setSceneFile(const std::filesystem::path &scenePath);

    // Stages one "section.path=value" override. The assignment is
    // parsed immediately so a malformed assignment is a usage error before any
    // backend starts; the value is applied after every configuration layer
    // has merged, because staged overrides are the highest layer. Returns
    // false with error filled in when the assignment is malformed.
    bool addCommandLineOverride(const std::string &assignment, std::string &error);

    // Directory that is scanned for mod packages (one subdirectory with a
    // mod.toml each), relative to the executable directory when not absolute.
    // Default: "mods". The configuration key [engine].mod_directory overrides
    // this default; an explicit call here overrides both.
    void setModsDirectory(const std::filesystem::path &modsDirectory);

    // One MORE mod package scan root, scanned after every root staged so far
    // ([engine].mod_directory or an earlier call). Composition roots with
    // several package locations stage them without copying packages into one
    // tree (an engine directory plus a user directory); the configuration's
    // [engine].extra_mod_directories declares the same thing per assembly.
    // Explicit roots (this call and setModsDirectory) REPLACE both
    // configuration keys together, preserving the historical single-flag
    // semantics of setModsDirectory. A package id that appears in more than
    // one scanned root is a precise startup problem reported with both
    // locations - the same rule one directory already applies to duplicate
    // ids, never a silent pick of whichever root scanned first.
    void addModsDirectory(const std::filesystem::path &modsDirectory);

    // Dry run: loads the configuration, builds every manifest, resolves the
    // graph and checks permissions. On failure the report contains one line per
    // diagnostic. No backend is initialized and no construct phase runs.
    bool validate(std::string &report);

    // Full startup: validate, then run construct for every component in
    // dependency order, verifying each provided capability has an
    // implementation behind it.
    bool construct(std::string &report);

    // Runs start for every component in dependency order and publishes
    // EngineStartupCompletedEvent. Throws std::runtime_error on failure.
    void start();

    // Publishes EngineShutdownStartedEvent, runs stop in reverse order and
    // releases every component, then unloads mod libraries in reverse order.
    void stop();

    // Live configuration reload, driven by the composition root: re-reads
    // every configuration file this run originally assembled (the assembly
    // file, the shipped module defaults, the recorded mod package defaults,
    // the world source and the staged overrides), computes the leaf-level
    // diff against the effective configuration and publishes
    // ConfigurationReloadedEvent with the changed key paths. Components that
    // care subscribe to that event and re-read their own keys; components
    // that do not subscribe keep running on whatever they read at construct
    // - a reload never forces work on a module that did not ask for it.
    //
    // Failure semantics: every file is parse-checked BEFORE the live store
    // is touched, so a half-edited file fails the reload with a precise
    // diagnostic and the running components keep their current effective
    // configuration untouched. World content ([scene]) is re-merged into the
    // store for consistency, but its changes are reported as "restart
    // required" and excluded from the event: the scene registry was seeded
    // at start and a live world swap is a different feature, not a config
    // diff. Modules are NOT re-discovered and manifests are NOT re-resolved:
    // a reload changes values, never the component graph (the graph is
    // fixed by the mods on disk; adding a mod remains a restart).
    bool reloadConfiguration(std::string &report);

    // One human readable line block per constructed component, in start
    // order: kind, provided/required/optional capabilities, permissions and
    // (for script mods) the resolved host. This is the introspection surface
    // behind the app's --print-graph flag and the [diagnostics] graph_report
    // key: a mod author asks "who provides scene? what order do frame
    // handlers run in?" and gets the assembly's answer without reading
    // source. Built on demand from the records - no cached state, no run
    // time cost for runs that never print it.
    std::string componentGraphReport() const;

    bool isStarted() const
    {
        return started;
    }

    EventBus &eventBus()
    {
        return bus;
    }

    // The pull surface behind ComponentContext::acquireCapability: resolves
    // every request straight from the declaring provider's
    // capabilityPointerFor hook at acquisition time. Nothing is stored.
    // The requester's kind mediates the capability directions: a module is
    // answered only by other modules, a mod by modules and mods alike, the
    // application (the trusted composition root) by everything.
    void *resolveCapabilityPointer(
        const std::string &requesterName, ComponentKind requesterKind, const std::string &capabilityName
    ) override;

    // The pull surface behind ComponentContext::acquireEachCapability: every
    // constructed provider of the named capability (exact declarations
    // first, then family members), each pointer pulled at call time. Empty
    // when nothing resolves - never throws for absence. A module requester's
    // answer covers only module providers.
    std::vector<void *> resolveEachCapabilityPointer(
        const std::string &requesterName, ComponentKind requesterKind, const std::string &capabilityName
    ) override;

    // The non-throwing probe behind ComponentContext::isCapabilityAvailable.
    bool hasSingleCapabilityProvider(const std::string &capabilityName, ComponentKind requesterKind) override;

    // The non-throwing pull behind ComponentContext::tryAcquireCapability:
    // the implementation pointer when exactly one provider resolves, nullptr
    // for absence and ambiguity; a declared-but-empty provider still throws
    // (see CapabilityResolver::tryResolveCapabilityPointer).
    void *tryResolveCapabilityPointer(
        const std::string &requesterName, ComponentKind requesterKind, const std::string &capabilityName
    ) override;

    const ConfigurationStore &configuration() const
    {
        return configurationStore;
    }

    // Warnings that do not fail a run (dangerous-mod advisories and similar
    // policy notices). validate() appends them to its report; a normal run
    // prints them through this accessor so the user always sees them.
    const std::string &startupWarnings() const
    {
        return startupWarnings_;
    }

    // The risk advisories the last gather produced, one per flagged mod and
    // per flagged ACTIVE module. Warnings also ride in startupWarnings();
    // this structured form is what the trust gate and the composition root
    // can present (a prompt, a UI list). Empty after a gather with nothing
    // flagged.
    const std::vector<ComponentTrustAdvisory> &trustAdvisories() const
    {
        return trustAdvisories_;
    }

    // The composition root installs the consent surface for the trust gate:
    // the handler receives every flagged-but-untrusted component and answers
    // whether the run may proceed to use them. Without a handler (headless,
    // CI), a run whose assembly carries untrusted flagged components FAILS
    // startup with the trust instructions - the agreement cannot be asked
    // for, so it cannot be given. The handler runs before any mod or module
    // construct phase; it must not touch engine state.
    using TrustConsentHandler = std::function<bool(const std::vector<ComponentTrustAdvisory> &untrustedAdvisories)>;
    void setTrustConsentHandler(TrustConsentHandler handler);

    // The kernel-owned timed-callback service (see
    // main_thread_scheduler.hpp). Components reach it through
    // ComponentContext::scheduler(); the composition root drives it with one
    // tick() per frame-loop iteration, next to the bus's deferred-event
    // flush. A run that schedules nothing pays one empty-vector check per
    // tick - and stop() clears the scheduler before any component stops, so
    // no timed callback can fire during teardown.
    MainThreadScheduler &scheduler()
    {
        return mainThreadScheduler_;
    }

    // Trusted context for the composition root (the frame loop). The app may
    // acquire every available capability.
    std::unique_ptr<ComponentContext> createAppContext();

private:
    // One module plugin binary found in the modules directory, described by
    // its own static descriptor (engine/kernel/module_api.hpp). The kernel
    // holds the descriptor WITHOUT creating an instance: only plugins whose
    // provided capabilities the demand closure requires are ever instantiated.
    // The library loads when the plugin becomes a component - active plugins
    // load it in instantiateActiveModules, dormant plugins never load at all
    // (their descriptor comes from the runtime descriptor cache, see
    // module_plugin_cache.hpp) - so the library pointer is null for every
    // record until its binary is first needed, and stays null forever for
    // the plugins a run keeps dormant.
    struct ModulePluginRecord
    {
        std::string name;
        // The binary the descriptor was read from (or remembered for): the
        // path an active plugin's library is loaded from at instantiation
        // time, also the cache record's key.
        std::filesystem::path libraryPath;
        ModuleInterface *(*createModule)() = nullptr;
        std::vector<std::string> providedCapabilities;
        std::vector<std::string> requiredCapabilities;
        std::vector<std::string> optionalCapabilities;
        // The environment-scope permission requests the descriptor declares
        // (ABI 7's declaredPermissions): what the module can reach outside
        // the capability.* namespace. Read before instantiation - the risk
        // assessment and the trust gate answer from it - and verified
        // against the instance manifest afterwards (drift is a startup
        // error, like every other descriptor/manifest disagreement).
        std::vector<std::string> declaredPermissions;
        std::unique_ptr<DynamicLibrary> library;
        bool active = false;
    };

    struct ComponentRecord
    {
        // Declaration order is destruction order reversed: the library is
        // declared first so it unloads last, and the context is declared
        // second so it outlives every module and mod instance that may still
        // hold the pointer it received during construct.
        std::unique_ptr<DynamicLibrary> library;
        // The per-component context is heap anchored: components are allowed
        // to cache the reference they get in construct and use it for event
        // publishing for the rest of the engine lifetime (input callbacks,
        // frame events, capture events). A stack-local context here would
        // dangle as soon as the construct phase returns.
        std::unique_ptr<ComponentContext> context;
        const EngineModEntryTable *modEntryTable = nullptr;
        std::string name;
        ComponentManifest manifest;
        ModuleInterface *moduleInstance = nullptr;
        ModInterface *modInstance       = nullptr;
        std::unique_ptr<ModuleInterface> ownedModule;
        std::unique_ptr<ModInterface> ownedMod;
        // Script mods have no instance of their own: the hosting module keeps
        // the script runtime. The description is what the kernel hands over.
        ScriptModDescription scriptMod;
        std::string scriptHostName;
        bool constructed = false;
        bool started     = false;
    };

    bool loadConfiguration(std::string &report);
    // Declares the known-key registry the unknown-key pass compares against:
    // the assembly's own sections plus every constructed record's manifest
    // keys. validate() and reloadConfiguration() both run the pass, so both
    // fill the registry first - a construct-startup run (whose composition
    // root never called validate) gets the same registry built from the same
    // frozen component set. Returns false when a declaration was malformed
    // (the report carries the diagnostic).
    bool declareKnownKeyRegistry(std::string &report);
    // Asks one constructed component for the implementation behind a
    // capability name it declared; shared by the exact and the family path of
    // resolveCapabilityPointer.
    void *capabilityPointerOf(ComponentRecord &provider, const std::string &capabilityName);
    // The provider classification of one capability name: the exact
    // declarations in record order, and the family members with the member
    // name that satisfied the request (one entry per record - a record that
    // declares several members of one family classifies once). Shared by all
    // four resolve entry points so the acquire, the probe and the plural
    // pull can never disagree about what is available.
    struct CapabilityProviderSet
    {
        std::vector<ComponentRecord *> exactProviders;
        // Each family member carries its OWN member name (e.g.
        // "scripting.lua_bridge.scene" and "scripting.lua_bridge.input" are
        // two members of the "scripting.lua_bridge" family), so the plural
        // pull below can ask every provider for exactly the name it declared
        // without re-deriving it per call.
        std::vector<std::pair<ComponentRecord *, std::string>> memberProviders;
    };
    // Walks the constructed records and classifies the providers of one
    // capability name into the given set: exact declarations, then family
    // members. Shared by the cached and the walk path of
    // capabilityProviderSetFor.
    void classifyCapabilityProviders(const std::string &capabilityName, CapabilityProviderSet &outSet);
    // The one classification answer behind all four resolve entry points.
    // While the component set is FROZEN (the construct phase completed; see
    // capabilityProviderIndexFrozen) the answer comes from a per-name cache:
    // the first acquisition of a name walks the records once, every later
    // acquisition of the same name reads the stored lists - a hash lookup
    // instead of the records-times-declarations string-compare walk, which
    // the lazy frame-time acquisitions (a runtime opportunist probing for an
    // optional provider, a module acquiring on first use) paid per call.
    //
    // The cache stores the CLASSIFICATION only, never an implementation
    // pointer: every resolved pointer still comes straight from the declaring
    // provider's capabilityPointerFor hook at call time (capabilityPointerOf
    // below), so the pull-based acquisition contract is untouched - a
    // provider that changes what its hook answers between calls is still
    // answered with what it answers NOW. The component set cannot change
    // while the cache is live: records and their constructed flags are
    // written only inside the construct phase and stop(), and both edges
    // drop the cache (stop() clears it before the first component stops;
    // construct() clears it before re-building), so a cached list can never
    // name a record that is not constructed - and the pointers it holds are
    // record pointers into the member records vector, which stays put for
    // the whole run. The cache lives on the engine's single main thread (the
    // same contract the event bus and the scheduler keep), so no two
    // classifications can overlap; while UNFROZEN (construct in progress,
    // constructed flags mid-flip) the walk fills the retained scratch below
    // and no cache entry is written.
    const CapabilityProviderSet &capabilityProviderSetFor(const std::string &capabilityName);
    // Scans config/modules/<name>/config.toml shipped defaults; the lowest
    // configuration layer (defaults fill gaps, they never override).
    bool loadModuleDefaults(std::string &report);
    // The front-door sequence validate and construct share: load the layered
    // configuration, build the component records, resolve the world source.
    // enforceTrust decides the trust gate's mode: construct enforces (the
    // agreement is asked or the startup fails), validate reports (a dry run
    // agrees to nothing).
    bool prepareComponents(std::string &report, bool enforceTrust);
    // The permission pass both phases run identically: apply the
    // configuration's denials, declare every component's effective grants,
    // and collect the (component, denied permission) pairs for reporting.
    std::vector<std::pair<const ComponentRecord *, std::string>> applyPermissionPass();
    std::vector<ComponentManifest> componentManifests() const;
    ComponentRecord *findRecord(const std::string &name);
    // Applies the layers that must wait for mod discovery (the world content
    // source: the scene file, or the content mods' declared scene files when the
    // assembly defines no [scene] of its own, then the staged
    // overrides), then applies [diagnostics] from the fully merged
    // configuration. Idempotent: the store guards its layers.
    bool finishConfiguration(std::string &report);
    bool gatherComponents(std::string &report, bool enforceTrust);
    void gatherModPackages(std::string &report);
    // Loads the shared libraries of the native mod packages the last
    // gatherModPackages discovered (its manifest-only records are complete
    // from mod.toml alone) and finishes their records: entry table, instance,
    // manifest agreement with the package manifest. Runs only AFTER the trust
    // gate - a native mod's binary is mapped only once the user trusted or
    // agreed to it. Failures drop the package (a warning line each), never
    // fail the startup by themselves.
    void loadNativeModLibraries(std::string &report);
    // Capability demand: seeds the worklist with every active mod's required
    // capabilities, then activates the module plugins that provide them -
    // transitively, because an activated plugin's own requirements demand
    // further providers. Runs after the mods are gathered and before any
    // module is instantiated; a plugin whose capabilities no mod requires is
    // never instantiated and its binary is released. With no active mods, no
    // module loads at all.
    void computeDemandClosure();
    // Runtime module discovery: scans the modules directory and reads one
    // static descriptor per plugin binary (engine/kernel/module_api.hpp).
    // No instance is created here. Runs inside gatherComponents, before the
    // demand closure decides which descriptors become components. The
    // descriptor of an unchanged binary comes from the runtime descriptor
    // cache (module_plugin_cache.hpp); only new, changed and actually
    // demanded plugins load their binaries during a run.
    void loadModulePlugins(std::string &report);
    // Loads the binary behind one active plugin record and re-reads the real
    // descriptor from it (the record's own descriptor may have come from the
    // cache): the fresh descriptor is authoritative and replaces the cached
    // one when they disagree, keeping the cache self-healing. Returns false
    // with a diagnostic in the report when the binary fails to load or the
    // descriptor is unusable.
    bool loadPluginLibrary(ModulePluginRecord &plugin, std::string &report);
    // Instantiates exactly the plugins the demand closure marked active,
    // verifies each instance manifest against its static descriptor and
    // turns it into a component record. Returns false (with diagnostics in
    // the report) when a factory fails or a manifest drifts from the
    // descriptor.
    bool instantiateActiveModules(std::string &report);
    // The construct phase both construct() and startup recovery share: walks
    // the resolved order, hosts script mods through their resolved provider
    // and verifies every provided capability has an implementation behind it
    // (nothing is stored - acquisition pulls from the provider at call time).
    bool runConstructPhase(
        const DependencyResolver::ResolutionResult &resolution,
        const std::vector<ComponentManifest> &manifests,
        std::string &report
    );
    // Resolves the hosting module of one script mod: the active component
    // that provides the host capability the package declared, honoring the
    // configuration's [providers] selection when several implementations are
    // active.
    // Returns nullptr (with a diagnostic in the report) when no provider or
    // an ambiguous provider set remains.
    ComponentRecord *resolveScriptHost(const ComponentRecord &scriptRecord, std::string &report);
    // Structural risk advisories for one active mod: native machine code,
    // environment permissions (anything outside the capability.* namespace)
    // and broad capability access. Appends one warning line per mod.
    void appendModRiskWarning(const ModManifest &modManifest, bool isNativeMod);
    // The same structural assessment for one ACTIVE module: environment
    // permissions (the descriptor's declaredPermissions) and broad
    // capability acquisition. Modules are native machine code by definition,
    // so that signal applies to mods only. Appends one warning line per
    // flagged module.
    void appendModuleRiskWarning(const ModulePluginRecord &plugin);
    // The trust gate: consults the [trust] configuration section, splits the
    // collected advisories into trusted and untrusted, and - when untrusted
    // ones remain - either asks the consent handler (enforce) or appends a
    // note to the warnings (the validate dry run, where no component code is
    // agreed to and none runs). Returns false only in enforce mode with a
    // denied or impossible agreement; the report then carries the abort
    // diagnostic and the trust instructions.
    bool evaluateTrustGate(std::string &report, bool enforce);
    DependencyResolver::ResolutionResult
    resolveGraph(const std::vector<ComponentManifest> &manifests, std::string &report) const;
    void appendDiagnosticsReport(
        const std::vector<DependencyResolver::ResolutionDiagnostic> &diagnostics, std::string &report
    ) const;

    std::filesystem::path configurationFilePath;
    std::filesystem::path sceneFilePath;
    std::filesystem::path explicitModsDirectory;
    // Additional explicit mod package scan roots staged by the composition
    // root (addModsDirectory). Together with explicitModsDirectory they form
    // the EXPLICIT root set: non-empty, it replaces [engine].mod_directory
    // and [engine].extra_mod_directories entirely (the multi-root extension
    // of setModsDirectory's historical replace semantics); empty, discovery
    // scans the configuration's roots - mod_directory first, then every
    // extra_mod_directories entry in order.
    std::vector<std::filesystem::path> explicitExtraModsDirectories;
    // World content claims the active packages declared: each claim's
    // package-relative scene file resolved against its package directory,
    // plus the owning mod id for diagnostics. Collected during mod
    // discovery; content mods coexist freely - the claims only matter when
    // the run has no explicit world source, and two or more claims then make
    // the world ambiguous (a startup error, never a silent pick).
    struct ModSceneClaim
    {
        std::filesystem::path sceneFile;
        std::string ownerId;
    };
    std::vector<ModSceneClaim> modSceneClaims;
    // The mod package defaults files the last gather merged (path + package
    // directory name for provenance). Recorded so reloadConfiguration can
    // re-merge exactly the same set without re-running mod discovery: a
    // reload changes configuration VALUES, never the component graph, and
    // re-scanning the mods directory could observe a different package set
    // mid-run (a package added after startup must not appear through a
    // reload).
    std::vector<std::pair<std::filesystem::path, std::string>> modDefaultsFiles;
    // Whether the assembly configuration defined its own [scene] section. The
    // kernel records it right after the assembly layer loads (before any
    // defaults merge): the assembly either owns the world or delegates it.
    bool assemblyDefinesScene = false;
    // validate-only diagnostics that must not fail the run (like the unknown
    // key warnings): printed after the failure check so a warning stays a
    // warning.
    std::string deferredConfigurationWarnings;
    std::string startupWarnings_;
    // The structured risk advisories of the last gather (see
    // trustAdvisories()) and the composition root's consent surface (see
    // setTrustConsentHandler).
    std::vector<ComponentTrustAdvisory> trustAdvisories_;
    TrustConsentHandler trustConsentHandler_;
    // The native mod packages the last gatherModPackages discovered, kept
    // beside their manifest-only records until the trust gate passed: their
    // libraries load afterwards (loadNativeModLibraries), so a package the
    // user never agreed to never has its binary mapped. Emptied once loaded.
    std::vector<DiscoveredModPackage> pendingNativeModPackages_;
    // Whether the pending native packages of the CURRENT component set have
    // had their libraries loaded (validate's dry gather defers them; a
    // following construct() then re-gathers from scratch - the enforced
    // gate and the library loads must both have run before components
    // construct).
    bool nativeModLibrariesLoaded_ = false;
    ConfigurationStore configurationStore;
    EventBus bus;
    PermissionSystem permissionSystem_;
    // Declared before modulePlugins and records so reverse destruction
    // releases every component instance first and the scheduler last: a
    // component's ScheduledTask handles cancel during their destruction while
    // the scheduler is still alive, and no timed callback can run after the
    // components it might touch are gone (stop() clears the task set first).
    MainThreadScheduler mainThreadScheduler_;

    // The plugin descriptors found in the modules directory. Declared before
    // records so reverse destruction releases every component instance first
    // and unloads the plugin libraries last (a factory's function pointer
    // into a plugin must outlive the instances the factory created).
    std::vector<ModulePluginRecord> modulePlugins;

    // Every capability the demand closure found a consumer for. ComponentContext
    // exposes it to modules ("was my layer capability demanded?"); it carries
    // no meaning the kernel would interpret. The transparent hasher (see
    // CapabilityNameHash in component_interface.hpp) lets the demand question
    // answer a string_view - a literal call site, typically - without
    // constructing a std::string for the lookup.
    std::unordered_set<std::string, CapabilityNameHash, std::equal_to<>> demandedCapabilities;

    // capabilityProviderSetFor's retained scratch for the UNFROZEN path
    // (see the declaration above): cleared per classification, capacity kept
    // across calls, touched only on the engine's single main thread. The
    // member names in the set are the providers' own strings copied into the
    // pairs (small capability names live in their SSO), so a capability
    // pointer's journey from the classifier to the provider's
    // capabilityPointerFor hook never reads a buffer a nested classification
    // could rewrite.
    CapabilityProviderSet providerScratch;
    // The post-freeze per-name classification cache behind
    // capabilityProviderSetFor, plus its edge flag: the construct phase sets
    // the flag when the component set completes, stop() (and a re-construct)
    // clears the flag and drops the entries before the component set
    // changes. A run pays one walk per DISTINCT capability name it resolves
    // after construct, then index lookups - the records-times-declarations
    // string-compare walk is gone from the frame-time acquisitions.
    //
    // The index is a deque of small entries scanned linearly, not a hash
    // map: the population is the run's distinct resolved capability names -
    // a few dozen at the very most - and at that size a scan of short string
    // compares beats hashing plus node chasing on every cache the acquisition
    // path touches. A deque, not a vector, because the resolve entry points
    // hold the returned reference ACROSS the provider's capabilityPointerFor
    // call (the pull), and a provider that re-enters the kernel with a new
    // capability name must not invalidate the entry the outer resolution is
    // still reading - deque growth moves entries, never the entries'
    // storage. Entries are appended on first resolution and never erased, so
    // every reference the index ever handed out stays valid until stop().
    struct CapabilityProviderIndexEntry
    {
        std::string capabilityName;
        CapabilityProviderSet providers;
    };
    std::deque<CapabilityProviderIndexEntry> capabilityProviderIndex;
    bool capabilityProviderIndexFrozen = false;

    std::vector<ComponentRecord> records;
    // The components that completed the construct phase, in the resolver's
    // dependency order (providers before consumers). start() and stop() walk
    // this sequence - not the discovery-ordered records vector - because the
    // start phase consumes provider work: the renderer needs the window the
    // window module creates during its own start. The records vector holds
    // the plugin scan order, which says nothing about providership.
    std::vector<ComponentRecord *> componentStartOrder;
    bool configurationLoaded = false;
    bool componentsGathered  = false;
    bool constructed         = false;
    bool started             = false;
};

} // namespace engine

#endif // ENGINE_KERNEL_ENGINE_KERNEL_HPP
