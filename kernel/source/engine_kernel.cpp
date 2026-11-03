#include "engine/kernel/engine_kernel.hpp"

#include "engine/kernel/capability_name.hpp"
#include "engine/kernel/filesystem_utilities.hpp"
#include "engine/kernel/mod_api.hpp"
#include "engine/kernel/mod_discovery.hpp"
#include "engine/kernel/module_api.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace engine
{

namespace
{

const std::string &appContextName()
{
    static const std::string name = "<app>";
    return name;
}

// A mod that acquires at least this many distinct capabilities is reported as
// a broad-access advisory: the number is a structural signal only, the kernel
// never interprets what the capabilities mean.
constexpr std::size_t broadCapabilityWarningThreshold = 5;

std::filesystem::path resolveAgainstExecutableDirectory(
    const std::filesystem::path &path
)
{
    if (path.is_absolute())
    {
        return path;
    }
    return executableDirectory() / path;
}

std::string describeComponentKind(
    ComponentKind kind
)
{
    switch (kind)
    {
    case ComponentKind::Module:
        return "module";
    case ComponentKind::SharedLibraryMod:
        return "shared library mod";
    case ComponentKind::ScriptMod:
        return "script mod";
    case ComponentKind::Application:
        return "application";
    }
    return "component";
}

// Effective permission grants of one component: the permissions it declared
// plus one "capability.<name>" grant per required OR optional capability.
// Requiring a capability and being authorized to acquire it are the same
// statement, and an optional capability states the same intent with the one
// difference that the provider may be absent - the grant covers the
// acquisition the component will make when the provider is active. The
// explicit permissions list remains for grants that are not capability
// acquisitions (filesystem.write). Configuration denials still win over every
// grant, declared or derived.
std::vector<std::string> effectivePermissions(
    const ComponentManifest &manifest
)
{
    std::set<std::string> unique(manifest.requiredPermissions.begin(), manifest.requiredPermissions.end());
    for (const std::string &capabilityName : manifest.requiredCapabilities)
    {
        unique.insert(std::string(capabilityPermissionPrefix) + capabilityName);
    }
    for (const std::string &capabilityName : manifest.optionalCapabilities)
    {
        unique.insert(std::string(capabilityPermissionPrefix) + capabilityName);
    }
    return std::vector<std::string>(unique.begin(), unique.end());
}

// Order-insensitive comparison of a manifest's capability declaration against
// the static descriptor declaration. Used after instantiation: the descriptor
// resolved the demand, the manifest must agree, and drift between them would
// silently break both directions of the contract.
bool sameStringSet(
    const std::vector<std::string> &left, const std::vector<std::string> &right
)
{
    const std::set<std::string> leftSet(left.begin(), left.end());
    const std::set<std::string> rightSet(right.begin(), right.end());
    return leftSet == rightSet;
}

// The config/modules/<name>/config.toml files the defaults layer merges, in
// the same deterministic (sorted) order loadModuleDefaults consumes them.
// Shared by the merge itself and by reloadConfiguration's pre-parse pass, so
// the two can never disagree about which files a reload must validate.
std::vector<std::filesystem::path> collectModuleDefaultsFiles()
{
    std::vector<std::filesystem::path> moduleDirectories;
    const std::filesystem::path defaultsDirectory
        = resolveAgainstExecutableDirectory(ConfigurationStore::moduleDefaultsDirectory);
    if (!std::filesystem::exists(defaultsDirectory) || !std::filesystem::is_directory(defaultsDirectory))
    {
        return moduleDirectories; // no module ships defaults: a valid state
    }
    for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(defaultsDirectory))
    {
        if (entry.is_directory() && std::filesystem::exists(entry.path() / ConfigurationStore::moduleDefaultsFileName))
        {
            moduleDirectories.push_back(entry.path());
        }
    }
    std::sort(moduleDirectories.begin(), moduleDirectories.end());
    std::vector<std::filesystem::path> defaultsFiles;
    defaultsFiles.reserve(moduleDirectories.size());
    for (const std::filesystem::path &moduleDirectory : moduleDirectories)
    {
        defaultsFiles.push_back(moduleDirectory / ConfigurationStore::moduleDefaultsFileName);
    }
    return defaultsFiles;
}

} // namespace

void EngineKernel::setConfigurationFile(
    const std::filesystem::path &configurationPath
)
{
    configurationFilePath = configurationPath;
    configurationLoaded   = false;
    configurationStore.reset();
    // A configuration swap invalidates what the previous gather discovered
    // (mod directories, disabled ids, provider selections all come from the
    // [engine] section): force a re-gather, whose guard tears the stale
    // component set down before the fresh scan (see gatherComponents).
    componentsGathered = false;
}

void EngineKernel::setSceneFile(
    const std::filesystem::path &scenePath
)
{
    sceneFilePath       = scenePath;
    configurationLoaded = false;
    configurationStore.reset();
    componentsGathered = false;
}

bool EngineKernel::addCommandLineOverride(
    const std::string &assignment, std::string &error
)
{
    // Parsed (not applied) immediately: a malformed assignment must fail at
    // the composition root before any backend starts.
    return configurationStore.stageCommandLineOverride(assignment, error);
}

void EngineKernel::setModsDirectory(
    const std::filesystem::path &modsDirectory
)
{
    explicitModsDirectory = modsDirectory;
    explicitExtraModsDirectories.clear();
    componentsGathered = false;
}

void EngineKernel::addModsDirectory(
    const std::filesystem::path &modsDirectory
)
{
    explicitExtraModsDirectories.push_back(modsDirectory);
    componentsGathered = false;
}

EngineKernel::~EngineKernel()
{
    // Guarantees that components are stopped and released even when a caller
    // forgets the explicit stop phase.
    stop();
}

std::unique_ptr<ComponentContext> EngineKernel::createAppContext()
{
    permissionSystem_.grantAllPermissions(appContextName());
    return std::make_unique<ComponentContext>(
        bus,
        *this,
        permissionSystem_,
        configurationStore,
        appContextName(),
        ComponentKind::Application,
        nullptr,
        mainThreadScheduler_
    );
}

void EngineKernel::setTrustConsentHandler(
    TrustConsentHandler handler
)
{
    trustConsentHandler_ = std::move(handler);
}

bool EngineKernel::loadConfiguration(
    std::string &report
)
{
    if (configurationLoaded)
    {
        return true;
    }
    deferredConfigurationWarnings.clear();
    assemblyDefinesScene = false;
    if (configurationFilePath.empty())
    {
        report += "- no configuration file was set on the kernel.\n";
        return false;
    }
    const std::filesystem::path resolvedPath = resolveAgainstExecutableDirectory(configurationFilePath);
    if (!std::filesystem::exists(resolvedPath))
    {
        report += "- configuration file '" + resolvedPath.string() + "' does not exist.\n";
        return false;
    }
    // The store API takes an error OUT parameter and clears it: the report is
    // an accumulator, so store errors always travel in local strings.
    std::string loadError;
    if (!configurationStore.loadFromFile(resolvedPath, loadError))
    {
        report += "- " + loadError + "\n";
        return false;
    }
    // Recorded from the assembly layer alone, before any defaults merge: the
    // assembly either owns the world ([scene] present) or delegates it to the
    // installed content mod's scene file. Defaults files can never define
    // [scene] (rejected at merge), so the check stays exact.
    assemblyDefinesScene = configurationStore.hasSection(ConfigurationStore::sceneSectionName);
    // Shipped module defaults (config/modules/<name>/config.toml) fill the
    // gaps the assembly left; a broken defaults file is a startup problem.
    if (!loadModuleDefaults(report))
    {
        return false;
    }
    configurationLoaded = true;
    return true;
}

// Scans the runtime module defaults directory: every config/modules/<name>/
// that contains a config.toml contributes that module's shipped defaults as
// the lowest configuration layer. Discovery is sorted by name so behavior
// never depends on filesystem iteration order; when two defaults files
// provide the same key, the alphabetically first module wins (defaults fill
// gaps, they never override).
bool EngineKernel::loadModuleDefaults(
    std::string &report
)
{
    const std::vector<std::filesystem::path> defaultsFiles = collectModuleDefaultsFiles();

    for (const std::filesystem::path &defaultsFile : defaultsFiles)
    {
        std::string defaultsError;
        if (!configurationStore.mergeDefaultsFile(
                defaultsFile,
                ConfigProvenance::Layer::ModuleDefaults,
                defaultsFile.parent_path().filename().string(),
                defaultsError
            ))
        {
            report += "- " + defaultsError + "\n";
            return false;
        }
    }
    return true;
}

// The world content source and the staged overrides: the layers that must
// wait for mod discovery. Precedence, highest first: the composition root's
// explicit scene file, the assembly's own [scene] section,
// the one content mod's declared scene file, the components' built-in
// defaults. After the scene is final the staged overrides are written
// on top. Then the configuration is complete, and [diagnostics] is applied
// from the fully merged stack - a staged diagnostics.latency_report=true is
// honored exactly like the configuration key.
bool EngineKernel::finishConfiguration(
    std::string &report
)
{
    if (!sceneFilePath.empty())
    {
        const std::filesystem::path resolvedScenePath = resolveAgainstExecutableDirectory(sceneFilePath);
        if (!std::filesystem::exists(resolvedScenePath))
        {
            report += "- scene file '" + resolvedScenePath.string() + "' does not exist.\n";
            return false;
        }
        // The store API takes an error OUT parameter and clears it: never pass
        // the accumulating report straight in, or earlier diagnostics vanish.
        std::string sceneError;
        if (!configurationStore.applySceneFile(resolvedScenePath, sceneError))
        {
            report += "- " + sceneError + "\n";
            return false;
        }
    } else if (!assemblyDefinesScene && modSceneClaims.size() == 1)
    {
        // The assembly defines no world of its own and exactly one active
        // content mod claims it: that claim provides the world, through the
        // same wholesale [scene] swap an explicit scene file uses, so partial assembly
        // scenes can never merge with mod scenes.
        const ModSceneClaim &claim = modSceneClaims.front();
        if (!std::filesystem::exists(claim.sceneFile))
        {
            report += "- mod '" + claim.ownerId + "' declares scene file '" + claim.sceneFile.string()
                    + "' which does not exist.\n";
            return false;
        }
        std::string sceneError;
        if (!configurationStore.applySceneFile(claim.sceneFile, sceneError))
        {
            report += "- " + sceneError + "\n";
            return false;
        }
    } else if (!assemblyDefinesScene && modSceneClaims.size() > 1)
    {
        // The one-world rule, enforced only where the world is actually
        // ambiguous: with no explicit world source and several active
        // content mods, picking one silently would depend on filesystem
        // listing order. Content mods may coexist freely - the run just has
        // to say which world it wants.
        report += "- the world is ambiguous: " + std::to_string(modSceneClaims.size())
                + " active mods declare scene files";
        for (const ModSceneClaim &claim : modSceneClaims)
        {
            report += " '" + claim.sceneFile.string() + "' (" + claim.ownerId + ")";
        }
        report += "; give the run a world: a [scene] section in the configuration, "
                  "or disable all but one of these mods.\n";
        return false;
    } else if (assemblyDefinesScene && !modSceneClaims.empty())
    {
        // Valid state, but the author should know the installed content is
        // not the world this run uses. validate() prints it; a normal run
        // stays quiet.
        for (const ModSceneClaim &claim : modSceneClaims)
        {
            deferredConfigurationWarnings += "warning: mod '" + claim.ownerId + "' declares scene file '"
                                           + claim.sceneFile.string()
                                           + "' but the configuration defines its own [scene]; the mod scene is "
                                             "not applied\n";
        }
    }
    std::string overridesError;
    if (!configurationStore.applyCommandLineOverrides(overridesError))
    {
        report += "- " + overridesError + "\n";
        return false;
    }
    // Diagnostics are opt-in: the communication latency report is a developer
    // tool and must stay invisible (and free) for normal runs. Applied after
    // every layer is merged so the whole run - module constructs included -
    // is covered once the key is set; recording enabled by the composition
    // root before construct is preserved (the configuration can only turn
    // recording ON, never off).
    bus.setLatencyRecordingEnabled(
        bus.latencyRecordingEnabled()
        || configurationStore
            .booleanValue(ConfigurationStore::diagnosticsSectionName, ConfigurationStore::diagnosticsLatencyReportKey)
            .value_or(false)
    );
    return true;
}

bool EngineKernel::prepareComponents(
    std::string &report, bool enforceTrust
)
{
    // Staged overrides are the highest configuration layer, but two of the
    // keys they can override ([engine].mod_directory, [engine].disabled_mods)
    // are consumed by mod discovery, which must run before the scene file is
    // resolved (a content mod's scene claim needs the discovered packages
    // first). The staged assignments are therefore applied once here, so
    // discovery sees the final runtime layout, and again after the scene
    // layer in finishConfiguration, where the general precedence contract
    // (overrides above everything) is decided. Re-applying the same
    // assignments is idempotent: each write replaces exactly the value it
    // names.
    if (!loadConfiguration(report))
    {
        return false;
    }
    std::string overridesError;
    if (!configurationStore.applyCommandLineOverrides(overridesError))
    {
        report += "- " + overridesError + "\n";
        return false;
    }
    return gatherComponents(report, enforceTrust) && finishConfiguration(report);
}

std::vector<std::pair<const EngineKernel::ComponentRecord *, std::string>> EngineKernel::applyPermissionPass()
{
    std::vector<std::pair<const ComponentRecord *, std::string>> denied;
    for (const auto &denial : configurationStore.permissionDenials())
    {
        permissionSystem_.denyPermission(denial.first, denial.second);
    }
    for (const ComponentRecord &record : records)
    {
        const std::vector<std::string> grants = effectivePermissions(record.manifest);
        permissionSystem_.declarePermissions(record.name, grants);
        const std::vector<std::string> missing = permissionSystem_.missingPermissions(record.name, grants);
        for (const std::string &permission : missing)
        {
            denied.emplace_back(&record, permission);
        }
    }
    return denied;
}

std::vector<ComponentManifest> EngineKernel::componentManifests() const
{
    std::vector<ComponentManifest> manifests;
    manifests.reserve(records.size());
    for (const ComponentRecord &record : records)
    {
        manifests.push_back(record.manifest);
    }
    return manifests;
}

EngineKernel::ComponentRecord *EngineKernel::findRecord(
    const std::string &name
)
{
    for (ComponentRecord &candidate : records)
    {
        if (candidate.name == name)
        {
            return &candidate;
        }
    }
    return nullptr;
}

// Builds one record per component. Modules are NOT registered anywhere: their
// plugin binaries describe themselves statically, the demand closure decides
// which of them this run actually needs, and only those are instantiated.
// Mods are the SOLE demand roots of the run: every active package becomes a
// component, and its requirements pull the providers it needs. A run with no
// mods instantiates no module at all.
//
// The gather runs in two trust-separated stages. Stage one builds every mod
// record from its manifest alone (no native library is mapped yet - a mod's
// binary is loaded only after the trust gate), computes the demand closure
// and collects the risk advisories for the mods AND for the now-known active
// modules. Stage two - only after the gate passed - maps the native mod
// libraries and instantiates the active module plugins. A component the user
// never trusted or agreed to therefore never has its construct or start code
// reached, and a native mod's binary is not even mapped.
bool EngineKernel::gatherComponents(
    std::string &report, bool enforceTrust
)
{
    if (componentsGathered)
    {
        return true;
    }

    // A previous gather can leave state behind: validate() populated the
    // records, then the composition root re-pointed the mods directory or
    // swapped the configuration file, and now construct() re-gathers. The
    // stale state must be torn down BEFORE the plugin inventory below clears
    // modulePlugins: records holds instances created by those plugin binaries
    // (createModule factories, native mod entry tables), and destroying the
    // instances only after the binaries unload would run destructors into
    // unmapped code. stop() is exactly that teardown (a no-op for phases
    // that never ran); it also resets componentsGathered, which this function
    // already found false.
    if (!records.empty() || !modulePlugins.empty())
    {
        stop();
    }

    // The advisories describe THIS component set: a re-gather rebuilds them
    // from the current manifests and descriptors (the flat warning lines
    // accumulate by design - the user sees every run's advisories - but the
    // structured list the gate answers from must stay a per-gather snapshot).
    trustAdvisories_.clear();

    // Read the module plugin inventory: one static descriptor per binary,
    // no instances created (see loadModulePlugins). Discovery problems are
    // NON-fatal by design (a broken package is dropped, never a failed
    // startup), so they accumulate in startupWarnings_ - report here is
    // reserved for errors that actually fail the phase.
    loadModulePlugins(startupWarnings_);

    // Mod packages discovered in the mods directory, one mod.toml per
    // package. Mods are always active - they are the reason the run exists -
    // and their required capabilities seed the demand closure.
    gatherModPackages(startupWarnings_);

    // Demand closure over opaque capability names: the mods' requirements
    // alone, transitively closed through the providers' own requirements.
    // Marks plugin descriptors active; nothing is instantiated yet.
    computeDemandClosure();

    // The risk advisories of the ACTIVE modules: from the descriptors, before
    // any plugin binary is loaded for instantiation (the advisories ride in
    // startupWarnings_ like the mods').
    for (const ModulePluginRecord &plugin : modulePlugins)
    {
        if (plugin.active)
        {
            appendModuleRiskWarning(plugin);
        }
    }

    // The trust gate. Enforce mode (construct): every flagged component must
    // be trusted through [trust] or agreed to by the user, or the startup
    // fails BEFORE any native mod library is mapped and before any module
    // plugin is instantiated - the verdict travels in report (the fatal
    // accumulator), the advisories stay in startupWarnings_. Report mode
    // (validate): a note rides in startupWarnings_ - the dry run agrees to
    // nothing and runs no construct phase.
    if (!evaluateTrustGate(report, enforceTrust))
    {
        return false;
    }

    // The gate passed (or this is a dry run): map the native mod libraries
    // the gate covered. In report mode the loads stay deferred - validate
    // resolves the graph from the manifests alone, and a following construct
    // re-gathers from scratch (see construct).
    if (enforceTrust)
    {
        loadNativeModLibraries(startupWarnings_);
        nativeModLibrariesLoaded_ = true;
    }

    // Instantiate only the plugins the closure marked active; verify each
    // instance manifest against its static descriptor; release the rest.
    // (Unlike the discovery passes above, every failure here is FATAL -
    // descriptor drift or a dead factory - so this one keeps writing to
    // report, not startupWarnings_.)
    if (!instantiateActiveModules(report))
    {
        return false;
    }

    // Component names are the graph's node ids: a mod id that collides with a
    // module plugin name (two sources check uniqueness within themselves;
    // nobody checked across them) would silently collapse in the resolver's
    // name-index map, leave the second record permanently unconstructed, and
    // make every diagnostic name the wrong component. A collision is a
    // startup error, not a drop - the operator must rename one of the two.
    for (std::size_t outer = 0; outer < records.size(); ++outer)
    {
        for (std::size_t inner = outer + 1; inner < records.size(); ++inner)
        {
            if (records[outer].name == records[inner].name)
            {
                report += "- component name '" + records[outer].name
                        + "' is declared twice (a mod id and a module plugin name, or two mods sharing an id); "
                          "component names must be unique across the whole assembly.\n";
                return false;
            }
        }
    }

    componentsGathered = true;
    return true;
}

// Turns the packages discovered on disk into component records. Every failure
// path in this function is a drop-and-report, never a failed startup: the
// report parameter therefore receives WARNING lines only, and callers pass
// the kernel's startupWarnings_ accumulator. The filesystem scan and the
// native library loading live in mod_discovery.cpp; this phase owns the
// kernel policy: scan roots, scene claims, shipped defaults, record wiring
// for native and script mods, and the structural risk advisories the user
// sees before any mod code runs.
//
// Native mod packages stop at the MANIFEST-ONLY record: the shared library
// is NOT mapped here (the trust gate runs between this phase and
// loadNativeModLibraries), so the record's wiring comes from mod.toml alone
// - which is authoritative for identity and wiring anyway. The parsed
// packages wait in pendingNativeModPackages_ until the gate passed.
void EngineKernel::gatherModPackages(
    std::string &report
)
{
    // Fresh scan state every call: the claims are re-collected from the
    // current packages, never carried over from an earlier gather. The same
    // reset applies to the defaults-file recording reloadConfiguration
    // replays (see modDefaultsFiles in the header) and to the pending native
    // packages the trust gate will release.
    modSceneClaims.clear();
    modDefaultsFiles.clear();
    pendingNativeModPackages_.clear();
    nativeModLibrariesLoaded_ = false;

    // The scan root set. Explicit roots (staged by the composition root)
    // replace the configuration's roots entirely - the multi-root extension
    // of the historical single --mods-dir replace semantics; with no explicit
    // root, the assembly's [engine].mod_directory scans first and every
    // [engine].extra_mod_directories entry follows in the listed order, so
    // an assembly composes its mod set from several install locations
    // without copying packages into one tree.
    std::vector<std::filesystem::path> scanRoots;
    if (!explicitModsDirectory.empty() || !explicitExtraModsDirectories.empty())
    {
        if (!explicitModsDirectory.empty())
        {
            scanRoots.push_back(resolveAgainstExecutableDirectory(explicitModsDirectory));
        }
        for (const std::filesystem::path &extraDirectory : explicitExtraModsDirectories)
        {
            scanRoots.push_back(resolveAgainstExecutableDirectory(extraDirectory));
        }
    } else
    {
        scanRoots.push_back(resolveAgainstExecutableDirectory(configurationStore.modDirectory()));
        for (const std::string &extraDirectory : configurationStore.extraModDirectories())
        {
            scanRoots.push_back(resolveAgainstExecutableDirectory(extraDirectory));
        }
    }

    // Package ids are unique across EVERY scanned root, not just within one:
    // a silent pick of whichever root scanned first would make the active mod
    // set depend on scan order. The later package is reported and dropped,
    // matching the duplicate rule a single directory already applies.
    std::map<std::string, std::filesystem::path> discoveredPackageRoots;
    std::vector<DiscoveredModPackage> packages;
    for (const std::filesystem::path &scanRoot : scanRoots)
    {
        for (DiscoveredModPackage &package : discoverModPackages(scanRoot, configurationStore.disabledMods(), report))
        {
            const auto previouslySeen = discoveredPackageRoots.find(package.manifest.id);
            if (previouslySeen != discoveredPackageRoots.end())
            {
                report += "- mod '" + package.manifest.id + "' appears in more than one mods directory ('"
                        + previouslySeen->second.string() + "' and '" + scanRoot.string()
                        + "'); mod ids are unique across every scanned directory and the later package is "
                          "dropped.\n";
                continue;
            }
            discoveredPackageRoots.emplace(package.manifest.id, scanRoot);
            packages.push_back(std::move(package));
        }
    }

    for (const DiscoveredModPackage &package : packages)
    {
        const ModManifest &modManifest = package.manifest;

        // World content claims: collected, never resolved here. Content mods
        // may coexist freely; finishConfiguration decides - an explicit world
        // source (an explicit scene file or the assembly's [scene]) makes the claims
        // irrelevant, a single claim provides the world, and two or more
        // claims without an explicit source are a startup error (the world
        // would be ambiguous - never a silent pick of whichever package the
        // filesystem listed first).
        if (!package.sceneFile.empty())
        {
            ModSceneClaim claim;
            claim.sceneFile = package.sceneFile;
            claim.ownerId   = modManifest.id;
            modSceneClaims.push_back(std::move(claim));
        }

        // Shipped defaults: an active package may carry config.toml with
        // default values for the sections it reads. Defaults fill only what
        // the assembly left unset; a broken defaults file is a precise
        // startup problem for the package (and drops it, like a broken
        // manifest does).
        if (!package.defaultsFile.empty())
        {
            std::string defaultsError;
            if (!configurationStore.mergeDefaultsFile(
                    package.defaultsFile,
                    ConfigProvenance::Layer::ModDefaults,
                    modManifest.directory.filename().string(),
                    defaultsError
                ))
            {
                report += "- " + defaultsError + "\n";
                continue;
            }
            // Recorded for reloadConfiguration: a reload re-merges exactly
            // this set instead of re-running mod discovery, so the component
            // graph stays frozen while the values refresh.
            modDefaultsFiles.emplace_back(package.defaultsFile, modManifest.directory.filename().string());
        }

        if (modManifest.isNative())
        {
            // Manifest-only record: the library is NOT mapped here. Everything
            // the graph needs (identity, wiring, the provided capabilities the
            // manifest's provides key declares) is package data; the binary is
            // loaded by loadNativeModLibraries after the trust gate, and only
            // then does the compiled manifest get checked against the package
            // manifest.
            ComponentRecord record;
            record.name                          = modManifest.id;
            record.manifest.kind                 = ComponentKind::SharedLibraryMod;
            record.manifest.name                 = modManifest.id;
            record.manifest.version              = modManifest.version;
            record.manifest.providedCapabilities = modManifest.providedCapabilities;
            record.manifest.requiredCapabilities = modManifest.capabilities;
            record.manifest.optionalCapabilities = modManifest.optionalCapabilities;
            record.manifest.requiredPermissions  = modManifest.permissions;
            record.manifest.requiredComponents   = modManifest.requiresComponents;
            record.manifest.knownConfigKeys      = modManifest.configKeys;
            appendModRiskWarning(modManifest, true);
            pendingNativeModPackages_.push_back(package);
            records.push_back(std::move(record));
        } else
        {
            // Script mod: hosted by whichever active module provides the host
            // capability the package declares (resolved in the construct
            // phase). The record exists so the resolver and the permission
            // system see the script's capabilities and permissions like any
            // other component's.
            ComponentRecord record;
            record.name                          = modManifest.id;
            record.manifest.kind                 = ComponentKind::ScriptMod;
            record.manifest.name                 = modManifest.id;
            record.manifest.version              = modManifest.version;
            record.manifest.requiredCapabilities = modManifest.capabilities;
            record.manifest.optionalCapabilities = modManifest.optionalCapabilities;
            record.manifest.requiredPermissions  = modManifest.permissions;
            record.manifest.requiredComponents   = modManifest.requiresComponents;
            record.manifest.knownConfigKeys      = modManifest.configKeys;
            record.scriptMod.modId               = modManifest.id;
            record.scriptMod.scriptPath          = modManifest.directory / modManifest.entry;
            record.scriptMod.hostCapability      = modManifest.hostCapability;
            appendModRiskWarning(modManifest, false);
            records.push_back(std::move(record));
        }
    }
}

// Capability demand closure over opaque names, seeded by the active mods
// alone: every mod's required capabilities join the worklist, and each
// demanded capability activates the module plugins that provide it - exact
// name matches first, then any member of the demanded family ("example"
// activates a plugin providing "example.impl") - while an activated
// plugin's own requirements join the worklist in turn. With no mods there is
// no demand: no module plugin is instantiated. Capability names are never
// interpreted: the kernel only matches strings between consumer requirements
// and provider declarations.
//
// Optional capabilities deliberately stay OUT of the closure: an optional use
// must never pull a provider into a run that has no required use for it. A
// consumer that declared an optional capability probes availability at
// construct time (ComponentContext::isCapabilityAvailable) and degrades
// gracefully when the provider never joined the assembly.
void EngineKernel::computeDemandClosure()
{
    demandedCapabilities.clear();
    for (ModulePluginRecord &plugin : modulePlugins)
    {
        plugin.active = false;
    }

    const std::vector<DependencyResolver::ProviderSelection> selections = configurationStore.providerSelections();

    std::vector<std::string> worklist;
    for (const ComponentRecord &record : records)
    {
        // At this point records holds exactly the active mods: the sole
        // demand roots of the run. Nothing else - not the configuration, not
        // the app - can pull a module in.
        for (const std::string &capabilityName : record.manifest.requiredCapabilities)
        {
            worklist.push_back(capabilityName);
        }
    }

    while (!worklist.empty())
    {
        const std::string capabilityName = std::move(worklist.back());
        worklist.pop_back();
        if (demandedCapabilities.find(capabilityName) != demandedCapabilities.end())
        {
            continue;
        }
        demandedCapabilities.insert(capabilityName);

        // Providers of this capability among the descriptors and the mod
        // records: exact name matches first, then - for a family demand -
        // every provider of a member of that family. The [providers]
        // selection narrows the choice when it names one of them; otherwise
        // every candidate activates and the resolver reports the ambiguity
        // (or the selection naming an implementation that does not exist).
        const auto providesCapability = [&capabilityName](const std::vector<std::string> &provided) {
            return std::any_of(provided.begin(), provided.end(), [&capabilityName](const std::string &candidate) {
                return capabilityRequirementSatisfiedBy(capabilityName, candidate);
            });
        };
        std::vector<std::string> providerNames;
        for (ModulePluginRecord &plugin : modulePlugins)
        {
            if (providesCapability(plugin.providedCapabilities))
            {
                providerNames.push_back(plugin.name);
            }
        }
        for (const ComponentRecord &record : records)
        {
            if (providesCapability(record.manifest.providedCapabilities))
            {
                providerNames.push_back(record.name);
            }
        }
        if (providerNames.size() > 1)
        {
            const auto selection = std::find_if(
                selections.begin(),
                selections.end(),
                [&capabilityName](const DependencyResolver::ProviderSelection &candidate) {
                    return candidate.capabilityName == capabilityName;
                }
            );
            if (selection != selections.end())
            {
                const bool selectionMatches
                    = std::find(providerNames.begin(), providerNames.end(), selection->providerName)
                   != providerNames.end();
                if (selectionMatches)
                {
                    providerNames.assign(1, selection->providerName);
                }
                // A selection that matches nothing stays unapplied: every
                // candidate activates and the resolver reports the stale
                // selection with its precise diagnostic.
            }
        }

        for (const std::string &providerName : providerNames)
        {
            for (ModulePluginRecord &plugin : modulePlugins)
            {
                if (plugin.name != providerName || plugin.active)
                {
                    continue;
                }
                plugin.active = true;
                // A family demand satisfied through a member also marks the
                // member itself as demanded: a provider consulting the closure
                // about its own capability sees the demand that activated it.
                for (const std::string &providedName : plugin.providedCapabilities)
                {
                    if (capabilityRequirementSatisfiedBy(capabilityName, providedName))
                    {
                        demandedCapabilities.insert(providedName);
                    }
                }
                for (const std::string &requiredName : plugin.requiredCapabilities)
                {
                    worklist.push_back(requiredName);
                }
            }
        }
    }
}

// Runtime module discovery: the plugin seam of the kernel.
//
// Every shared library in the modules directory declares itself through one
// exported entry point (engine/kernel/module_api.hpp): its name, its factory
// and its static capability declaration. Reading a descriptor is uniform -
// load the binary, resolve, version check, copy the declaration - and
// creating an instance is a separate decision the demand closure owns. The
// composition root registers nothing and the kernel registers nothing: the
// set of available modules comes from the runtime layout, and the set of
// ACTIVE modules comes from what this run actually requires.
//
// Startup latency contract: the demand closure only needs the DESCRIPTORS,
// but reading one through the seam means loading the binary - which is why
// this scan runs through the runtime descriptor cache (module_plugin_cache.hpp).
// A binary whose size and modification stamp match the remembered record
// contributes its descriptor without being loaded at all; only new, changed
// and actually demanded plugins touch the loader this run. The cache is
// derived state: any mismatch re-reads the real descriptor (the ground
// truth) and refreshes the record, so a rebuilt or dropped-in module is
// picked up on the very same run.
void EngineKernel::loadModulePlugins(
    std::string &report
)
{
    // Fresh scan every gather: release the libraries of a previous scan so a
    // re-gather never accumulates stale handles. (With the cache, most
    // records carry no library at all until they are instantiated; the
    // release here only matters for the re-gather path, where a previous
    // gather's active plugins were loaded.)
    modulePlugins.clear();

    std::filesystem::path modulesDirectory = configurationStore.modulesDirectory();
    const std::filesystem::path resolvedDirectory
        = modulesDirectory.is_absolute() ? modulesDirectory : executableDirectory() / modulesDirectory;

    std::error_code scanError;
    std::filesystem::directory_iterator plugin{
        resolvedDirectory, std::filesystem::directory_options::skip_permission_denied, scanError
    };
    if (scanError)
    {
        report += "- module plugins directory '" + resolvedDirectory.string()
                + "' cannot be scanned: " + scanError.message() + "\n";
        return;
    }

    std::vector<std::filesystem::path> pluginPaths;
    for (const std::filesystem::directory_entry &entry : plugin)
    {
        std::error_code statError;
        if (statError || !entry.is_regular_file())
        {
            continue;
        }
        const std::filesystem::path libraryPath = entry.path();
        const std::string extension             = libraryPath.extension().string();
        // Every shared library extension is accepted on every platform: the
        // modules directory is a deployment layout, and a plugin tree
        // assembled for one platform must not silently look empty when
        // inspected from the other. Windows builds .dll, Linux .so, macOS
        // .dylib (CMake's SHARED suffix there).
        if (extension == ".dll" || extension == ".so" || extension == ".dylib")
        {
            pluginPaths.push_back(libraryPath);
        }
    }
    // Sorted scan order: startup behavior and reports never depend on
    // filesystem iteration order.
    std::sort(pluginPaths.begin(), pluginPaths.end());

    // The runtime descriptor cache: one file next to the executable, written
    // by the last scan that read a binary. Load failures of the cache itself
    // are invisible (an empty cache is the pre-cache behavior); write
    // failures likewise - the file is an accelerator, never a requirement.
    ModulePluginCache descriptorCache;
    descriptorCache.setCacheFile(executableDirectory() / modulePluginCacheFileName);
    descriptorCache.load();
    bool cacheChanged = false;

    for (const std::filesystem::path &libraryPath : pluginPaths)
    {
        // The identity stat pair: two queries on one file the scan is about
        // to read (or remember). A stat failure makes the binary unreadable
        // as a descriptor source this run - reported and skipped, like every
        // other unusable plugin binary.
        std::error_code statError;
        const std::uintmax_t sizeBytes = std::filesystem::file_size(libraryPath, statError);
        if (statError)
        {
            report += "- '" + libraryPath.string() + "' cannot be stat-ed: " + statError.message() + "\n";
            continue;
        }
        const std::uintmax_t modificationStamp = static_cast<std::uintmax_t>(
            std::filesystem::last_write_time(libraryPath, statError).time_since_epoch().count()
        );
        if (statError)
        {
            report += "- '" + libraryPath.string() + "' cannot be stat-ed: " + statError.message() + "\n";
            continue;
        }

        ModulePluginRecord record;
        const ModulePluginCacheEntry *remembered = descriptorCache.find(libraryPath, sizeBytes, modificationStamp);
        if (remembered != nullptr)
        {
            // Cache hit: the binary contributed its descriptor to a previous
            // scan and has not changed since. The record assembles without
            // the loader - the load happens later, and only for a plugin the
            // demand closure actually activates.
            record.name                 = remembered->moduleName;
            record.libraryPath          = libraryPath;
            record.providedCapabilities = remembered->providedCapabilities;
            record.requiredCapabilities = remembered->requiredCapabilities;
            record.optionalCapabilities = remembered->optionalCapabilities;
            record.declaredPermissions  = remembered->declaredPermissions;
        } else
        {
            // Cache miss: new, changed or never-seen binary - read the real
            // descriptor, which is the ground truth, and remember it.
            auto library = std::make_unique<DynamicLibrary>();
            std::string loadError;
            if (!library->load(libraryPath, loadError))
            {
                report += "- " + loadError + "\n";
                continue;
            }

            const auto query = reinterpret_cast<const EngineModulePluginEntry *(*)()>(
                library->resolveSymbol("engineQueryModulePlugin")
            );
            if (query == nullptr)
            {
                report += "- '" + libraryPath.string()
                        + "' exports no engineQueryModulePlugin entry point; not a module plugin.\n";
                continue;
            }

            const EngineModulePluginEntry *pluginEntry = query();
            if (pluginEntry == nullptr || pluginEntry->abiVersion != engineModulePluginAbiVersion)
            {
                report += "- '" + libraryPath.string()
                        + "' was built against a different module plugin ABI (expected version "
                        + std::to_string(engineModulePluginAbiVersion) + "); rebuild the plugin with this kernel.\n";
                continue;
            }
            if (pluginEntry->moduleName == nullptr || pluginEntry->createModule == nullptr)
            {
                report += "- '" + libraryPath.string() + "' returns an incomplete plugin descriptor.\n";
                continue;
            }

            record.name         = pluginEntry->moduleName;
            record.libraryPath  = libraryPath;
            record.createModule = pluginEntry->createModule;
            for (std::uint32_t index = 0; index < pluginEntry->providedCapabilityCount; ++index)
            {
                if (pluginEntry->providedCapabilities != nullptr && pluginEntry->providedCapabilities[index] != nullptr)
                {
                    record.providedCapabilities.emplace_back(pluginEntry->providedCapabilities[index]);
                }
            }
            for (std::uint32_t index = 0; index < pluginEntry->requiredCapabilityCount; ++index)
            {
                if (pluginEntry->requiredCapabilities != nullptr && pluginEntry->requiredCapabilities[index] != nullptr)
                {
                    record.requiredCapabilities.emplace_back(pluginEntry->requiredCapabilities[index]);
                }
            }
            for (std::uint32_t index = 0; index < pluginEntry->optionalCapabilityCount; ++index)
            {
                if (pluginEntry->optionalCapabilities != nullptr && pluginEntry->optionalCapabilities[index] != nullptr)
                {
                    record.optionalCapabilities.emplace_back(pluginEntry->optionalCapabilities[index]);
                }
            }
            for (std::uint32_t index = 0; index < pluginEntry->declaredPermissionCount; ++index)
            {
                if (pluginEntry->declaredPermissions != nullptr && pluginEntry->declaredPermissions[index] != nullptr)
                {
                    record.declaredPermissions.emplace_back(pluginEntry->declaredPermissions[index]);
                }
            }

            // The record is complete and verified: remember it. The library
            // handle carries over - this run may need the binary right away,
            // and dlclose of a just-loaded binary the closure will demand
            // again would be pure overhead. Records the closure leaves
            // dormant keep their loaded library only for THIS scan; the
            // release at the bottom hands the dormant ones back.
            record.library = std::move(library);

            ModulePluginCacheEntry cacheEntry;
            cacheEntry.sizeBytes            = sizeBytes;
            cacheEntry.modificationStamp    = modificationStamp;
            cacheEntry.moduleName           = record.name;
            cacheEntry.providedCapabilities = record.providedCapabilities;
            cacheEntry.requiredCapabilities = record.requiredCapabilities;
            cacheEntry.optionalCapabilities = record.optionalCapabilities;
            cacheEntry.declaredPermissions  = record.declaredPermissions;
            descriptorCache.record(libraryPath, cacheEntry);
            cacheChanged = true;
        }

        if (std::find_if(
                modulePlugins.begin(),
                modulePlugins.end(),
                [&record](const ModulePluginRecord &existing) { return existing.name == record.name; }
            )
            != modulePlugins.end())
        {
            report += "- module '" + record.name + "' is provided by more than one plugin binary; '"
                    + libraryPath.string() + "' is ignored.\n";
            continue;
        }

        modulePlugins.push_back(std::move(record));
    }

    if (cacheChanged)
    {
        descriptorCache.save();
    }

    // Library ownership after the scan: a cache-miss record holds its loaded
    // binary (its createModule pointer points into it, so releasing it here
    // would dangle the record's factory), a cache-hit record holds none.
    // instantiateActiveModules finishes the split: active cache-hit records
    // load their binary through loadPluginLibrary, dormant cache-miss
    // records release theirs right after the closure is computed.
}

// Loads the binary behind one plugin record whose descriptor came from the
// runtime descriptor cache. The freshly read descriptor is authoritative:
// when it disagrees with the remembered record (a binary that changed
// without moving its size or stamp), the fresh values replace the record's
// and the run proceeds on what the binary really declares - the cache never
// gets the last word over the code.
bool EngineKernel::loadPluginLibrary(
    ModulePluginRecord &plugin, std::string &report
)
{
    auto library = std::make_unique<DynamicLibrary>();
    std::string loadError;
    if (!library->load(plugin.libraryPath, loadError))
    {
        report += "- " + loadError + "\n";
        return false;
    }

    const auto query
        = reinterpret_cast<const EngineModulePluginEntry *(*)()>(library->resolveSymbol("engineQueryModulePlugin"));
    if (query == nullptr)
    {
        report += "- '" + plugin.libraryPath.string()
                + "' exports no engineQueryModulePlugin entry point; not a module plugin.\n";
        return false;
    }

    const EngineModulePluginEntry *pluginEntry = query();
    if (pluginEntry == nullptr || pluginEntry->abiVersion != engineModulePluginAbiVersion)
    {
        report += "- '" + plugin.libraryPath.string()
                + "' was built against a different module plugin ABI (expected version "
                + std::to_string(engineModulePluginAbiVersion) + "); rebuild the plugin with this kernel.\n";
        return false;
    }
    if (pluginEntry->moduleName == nullptr || pluginEntry->createModule == nullptr)
    {
        report += "- '" + plugin.libraryPath.string() + "' returns an incomplete plugin descriptor.\n";
        return false;
    }

    // Adopt the fresh descriptor wholesale. The cached values only chose
    // which plugins to activate; from this point the binary's own answer is
    // what the manifest checks below verify against.
    plugin.name         = pluginEntry->moduleName;
    plugin.createModule = pluginEntry->createModule;
    plugin.providedCapabilities.clear();
    plugin.requiredCapabilities.clear();
    plugin.optionalCapabilities.clear();
    plugin.declaredPermissions.clear();
    for (std::uint32_t index = 0; index < pluginEntry->providedCapabilityCount; ++index)
    {
        if (pluginEntry->providedCapabilities != nullptr && pluginEntry->providedCapabilities[index] != nullptr)
        {
            plugin.providedCapabilities.emplace_back(pluginEntry->providedCapabilities[index]);
        }
    }
    for (std::uint32_t index = 0; index < pluginEntry->requiredCapabilityCount; ++index)
    {
        if (pluginEntry->requiredCapabilities != nullptr && pluginEntry->requiredCapabilities[index] != nullptr)
        {
            plugin.requiredCapabilities.emplace_back(pluginEntry->requiredCapabilities[index]);
        }
    }
    for (std::uint32_t index = 0; index < pluginEntry->optionalCapabilityCount; ++index)
    {
        if (pluginEntry->optionalCapabilities != nullptr && pluginEntry->optionalCapabilities[index] != nullptr)
        {
            plugin.optionalCapabilities.emplace_back(pluginEntry->optionalCapabilities[index]);
        }
    }
    for (std::uint32_t index = 0; index < pluginEntry->declaredPermissionCount; ++index)
    {
        if (pluginEntry->declaredPermissions != nullptr && pluginEntry->declaredPermissions[index] != nullptr)
        {
            plugin.declaredPermissions.emplace_back(pluginEntry->declaredPermissions[index]);
        }
    }

    plugin.library = std::move(library);
    return true;
}

// Instantiates exactly the plugins the demand closure marked active. The
// instance manifest must agree with the static descriptor (name, provided,
// required): the descriptor decided the demand, so drift would silently
// change what the run computed. Plugins nobody required are released here -
// their binaries are unloaded and they never exist as components.
bool EngineKernel::instantiateActiveModules(
    std::string &report
)
{
    bool successful = true;
    for (ModulePluginRecord &plugin : modulePlugins)
    {
        if (!plugin.active)
        {
            continue;
        }
        // The one load an active plugin pays: a record whose descriptor came
        // from the cache has no binary behind it yet. A record freshly read
        // during the scan carries its library already - the factory pointer
        // points into it, so loading again would be waste.
        if (!plugin.library && !loadPluginLibrary(plugin, report))
        {
            successful = false;
            continue;
        }
        ComponentRecord record;
        record.name          = plugin.name;
        record.manifest.kind = ComponentKind::Module;
        // A plugin factory runs inside the plugin's binary; a throwing one
        // must fail this phase with a diagnostic like every other factory
        // failure, never unwind through the gather into the composition root.
        try
        {
            record.ownedModule.reset(plugin.createModule());
        } catch (const std::exception &error)
        {
            report += "- module factory for '" + plugin.name + "' threw: " + error.what() + "\n";
            successful = false;
            continue;
        }
        record.moduleInstance = record.ownedModule.get();
        if (record.ownedModule == nullptr)
        {
            report += "- module factory for '" + plugin.name + "' returned no instance.\n";
            successful = false;
            continue;
        }
        record.manifest = record.moduleInstance->manifest();
        if (record.manifest.name != plugin.name)
        {
            report += "- module '" + plugin.name + "' reports the manifest name '" + record.manifest.name
                    + "'; the descriptor name and the manifest name must match.\n";
            successful = false;
            continue;
        }
        if (!sameStringSet(record.manifest.providedCapabilities, plugin.providedCapabilities))
        {
            report += "- module '" + plugin.name
                    + "' declares different provided capabilities in its manifest than in its plugin descriptor; "
                      "rebuild the plugin with this kernel.\n";
            successful = false;
            continue;
        }
        if (!sameStringSet(record.manifest.requiredCapabilities, plugin.requiredCapabilities))
        {
            report += "- module '" + plugin.name
                    + "' declares different required capabilities in its manifest than in its plugin descriptor; "
                      "rebuild the plugin with this kernel.\n";
            successful = false;
            continue;
        }
        if (!sameStringSet(record.manifest.optionalCapabilities, plugin.optionalCapabilities))
        {
            report += "- module '" + plugin.name
                    + "' declares different optional capabilities in its manifest than in its plugin descriptor; "
                      "rebuild the plugin with this kernel.\n";
            successful = false;
            continue;
        }
        // The environment-scope permissions: the descriptor's static
        // declaration is what the trust gate answered from BEFORE the binary
        // was instantiated, so the manifest's own list must agree with it.
        // Capability-scope grants are derived and excluded on both sides.
        std::vector<std::string> manifestEnvironmentPermissions;
        for (const std::string &permission : record.manifest.requiredPermissions)
        {
            if (permission.rfind(capabilityPermissionPrefix, 0) != 0)
            {
                manifestEnvironmentPermissions.push_back(permission);
            }
        }
        if (!sameStringSet(manifestEnvironmentPermissions, plugin.declaredPermissions))
        {
            report += "- module '" + plugin.name
                    + "' requests different environment permissions in its manifest than its plugin descriptor "
                      "declares; declare the environment permissions (anything outside the capability.* "
                      "namespace) in the descriptor's declaredPermissions so the trust gate sees them before "
                      "the module is instantiated.\n";
            successful = false;
            continue;
        }
        records.push_back(std::move(record));
    }

    // Release the descriptors nobody required: their binaries unload now,
    // before any construct phase. A module that never becomes a component
    // never touches a backend.
    for (ModulePluginRecord &plugin : modulePlugins)
    {
        if (!plugin.active && plugin.library)
        {
            plugin.library->release();
        }
    }
    return successful;
}

// Structural risk advisory for one active mod. The signals are generic (they
// never name or interpret a capability): running native machine code,
// requesting permissions outside the capability.* namespace (direct
// environment access such as writing files) and acquiring a broad set of
// capabilities. The engine blocks what it can enforce; the user decides
// whether to trust what it cannot. Every advisory is collected in the
// structured trustAdvisories_ list (the trust gate's input) AND printed into
// startupWarnings_ - a user sees what each package asked for on every run,
// trusted or not.
void EngineKernel::appendModRiskWarning(
    const ModManifest &modManifest, bool isNativeMod
)
{
    std::vector<std::string> reasons;
    if (isNativeMod)
    {
        reasons.emplace_back("it runs native machine code");
    }
    std::vector<std::string> environmentPermissions;
    for (const std::string &permission : modManifest.permissions)
    {
        if (permission.rfind(capabilityPermissionPrefix, 0) != 0)
        {
            environmentPermissions.push_back(permission);
        }
    }
    if (!environmentPermissions.empty())
    {
        std::string joined;
        for (const std::string &permission : environmentPermissions)
        {
            if (!joined.empty())
            {
                joined += ", ";
            }
            joined += "'" + permission + "'";
        }
        reasons.push_back("it requests permissions outside the engine's capability system: " + joined);
    }
    if (modManifest.capabilities.size() >= broadCapabilityWarningThreshold)
    {
        reasons.push_back(
            "it acquires a broad set of " + std::to_string(modManifest.capabilities.size()) + " capabilities"
        );
    }
    if (reasons.empty())
    {
        return;
    }
    ComponentTrustAdvisory advisory;
    advisory.kind    = modManifest.isNative() ? ComponentKind::SharedLibraryMod : ComponentKind::ScriptMod;
    advisory.name    = modManifest.id;
    advisory.reasons = reasons;
    trustAdvisories_.push_back(std::move(advisory));
    std::string joined;
    for (const std::string &reason : reasons)
    {
        if (!joined.empty())
        {
            joined += "; ";
        }
        joined += reason;
    }
    startupWarnings_ += "warning: mod '" + modManifest.id + "' requests elevated trust: " + joined
                      + ". Review this mod before trusting it with sensitive data.\n";
}

// The same structural assessment for one ACTIVE module, from its descriptor
// (available before the plugin binary is ever instantiated): environment
// permissions outside the capability.* namespace (the ABI 7 descriptor's
// declaredPermissions) and a broad capability acquisition surface. Modules
// are machine code by definition - the engine itself is - so that signal
// stays a mod-only one; what makes a module structurally sensitive is what
// it can reach beyond the capability system and how wide its acquisition
// surface is. The advisory is collected and printed exactly like a mod's.
void EngineKernel::appendModuleRiskWarning(
    const ModulePluginRecord &plugin
)
{
    std::vector<std::string> reasons;
    std::vector<std::string> environmentPermissions;
    for (const std::string &permission : plugin.declaredPermissions)
    {
        if (permission.rfind(capabilityPermissionPrefix, 0) != 0)
        {
            environmentPermissions.push_back(permission);
        }
    }
    if (!environmentPermissions.empty())
    {
        std::string joined;
        for (const std::string &permission : environmentPermissions)
        {
            if (!joined.empty())
            {
                joined += ", ";
            }
            joined += "'" + permission + "'";
        }
        reasons.push_back("it requests permissions outside the engine's capability system: " + joined);
    }
    const std::size_t acquiredCapabilities = plugin.requiredCapabilities.size() + plugin.optionalCapabilities.size();
    if (acquiredCapabilities >= broadCapabilityWarningThreshold)
    {
        reasons.push_back("it acquires a broad set of " + std::to_string(acquiredCapabilities) + " capabilities");
    }
    if (reasons.empty())
    {
        return;
    }
    ComponentTrustAdvisory advisory;
    advisory.kind    = ComponentKind::Module;
    advisory.name    = plugin.name;
    advisory.reasons = reasons;
    trustAdvisories_.push_back(std::move(advisory));
    std::string joined;
    for (const std::string &reason : reasons)
    {
        if (!joined.empty())
        {
            joined += "; ";
        }
        joined += reason;
    }
    startupWarnings_ += "warning: module '" + plugin.name + "' requests elevated trust: " + joined
                      + ". Review this module before trusting it with sensitive data.\n";
}

// The trust gate over the collected advisories. A flagged component is
// TRUSTED when the [trust] configuration section covers it - trustAllMods /
// trustAllModules for its kind, or its name listed in trustMods /
// trustModules. Everything else is untrusted, and the run may only USE it
// after the user agreed:
//
//   enforce (construct) - the consent handler answers for the untrusted
//   set; without a handler (headless, CI) the agreement cannot be asked
//   for, so the startup fails with the trust instructions. A denied
//   agreement fails the same way. Nothing untrusted constructs: the gate
//   runs before any native mod library is mapped and before any module
//   plugin is instantiated. The verdict travels in `report` (the callers'
//   failure path prints it exactly once); the advisories and the granted
//   note stay in startupWarnings_.
//
//   report (validate) - the dry run agrees to nothing and runs no
//   construct phase; a note listing the untrusted set rides in
//   startupWarnings_ so an author sees what a real run would ask.
//
// The advisories themselves are ALWAYS warnings, trusted or not: trust
// removes the question, never the visibility.
bool EngineKernel::evaluateTrustGate(
    std::string &report, bool enforce
)
{
    if (trustAdvisories_.empty())
    {
        return true;
    }

    const std::vector<std::string> trustedModNames    = configurationStore.trustedMods();
    const std::vector<std::string> trustedModuleNames = configurationStore.trustedModules();
    const bool allMods                                = configurationStore.trustAllMods();
    const bool allModules                             = configurationStore.trustAllModules();
    const auto isTrusted                              = [&](const ComponentTrustAdvisory &advisory) {
        if (advisory.kind == ComponentKind::Module)
        {
            return allModules
                || std::find(trustedModuleNames.begin(), trustedModuleNames.end(), advisory.name)
                       != trustedModuleNames.end();
        }
        return allMods
            || std::find(trustedModNames.begin(), trustedModNames.end(), advisory.name) != trustedModNames.end();
    };

    std::vector<ComponentTrustAdvisory> untrusted;
    for (const ComponentTrustAdvisory &advisory : trustAdvisories_)
    {
        if (!isTrusted(advisory))
        {
            untrusted.push_back(advisory);
        }
    }
    if (untrusted.empty())
    {
        return true;
    }

    std::string names;
    for (const ComponentTrustAdvisory &advisory : untrusted)
    {
        if (!names.empty())
        {
            names += ", ";
        }
        names += describeComponentKind(advisory.kind) + " '" + advisory.name + "'";
    }
    const char *trustInstructions
        = "To proceed without being asked, trust the components in the configuration's [trust] section:\n"
          "  trustMods = [\"<mod id>\"]       (or trustAllMods = true)\n"
          "  trustModules = [\"<module name>\"] (or trustAllModules = true)\n";

    if (!enforce)
    {
        // The dry run's note is a warning, never a failure: it rides in the
        // startup warnings, which validate() embeds in its report on every
        // exit path.
        startupWarnings_ += "note: " + std::to_string(untrusted.size())
                          + " component(s) that can be harmful are not covered by the trust configuration (" + names
                          + "); a real run requires trusting them under [trust] or agreeing to them at startup.\n";
        return true;
    }

    if (trustConsentHandler_ != nullptr && trustConsentHandler_(untrusted))
    {
        startupWarnings_ += "agreement granted at startup: proceeding with " + names + " for this run.\n";
        return true;
    }
    if (trustConsentHandler_ != nullptr)
    {
        report += "startup aborted: agreement was not granted for " + names + ".\n";
    } else
    {
        report += "startup aborted: " + std::to_string(untrusted.size()) + " component(s) that can be harmful (" + names
                + ") are not trusted and no agreement could be requested (no interactive consent "
                  "surface is connected).\n";
    }
    report += trustInstructions;
    return false;
}

// Maps the shared libraries of the native mod packages the trust gate
// covered and finishes their records: entry table, instance, compiled
// manifest. The package manifest stays authoritative for identity and
// wiring; the compiled manifest must AGREE with it - on the name (as
// before) and now also on the provided capabilities: mod.toml's `provides`
// is the static declaration the graph and the validate dry run resolve
// from, so a compiled manifest that provides something the manifest does
// not declare (or declares different capabilities) is drift, reported
// precisely. A package that fails here is dropped (its manifest-only record
// removed) - the same drop-and-report every other broken package gets.
void EngineKernel::loadNativeModLibraries(
    std::string &report
)
{
    for (const DiscoveredModPackage &package : pendingNativeModPackages_)
    {
        const ModManifest &modManifest = package.manifest;
        NativeModLoad load;
        if (!loadNativeModEntry(package, load, report))
        {
            // Drop the manifest-only record: the package cannot be used and
            // must not sit in the graph as an unconstructable component.
            auto recordIt
                = std::find_if(records.begin(), records.end(), [&modManifest](const ComponentRecord &candidate) {
                      return candidate.name == modManifest.id
                          && candidate.manifest.kind == ComponentKind::SharedLibraryMod;
                  });
            if (recordIt != records.end())
            {
                records.erase(recordIt);
            }
            continue;
        }
        auto recordIt = std::find_if(records.begin(), records.end(), [&modManifest](const ComponentRecord &candidate) {
            return candidate.name == modManifest.id && candidate.manifest.kind == ComponentKind::SharedLibraryMod;
        });
        if (recordIt == records.end())
        {
            report += "- native mod '" + modManifest.id + "' lost its component record; the package is dropped.\n";
            load.entryTable->destroyMod(load.modInstance);
            continue;
        }
        ComponentRecord &record                  = *recordIt;
        const ComponentManifest compiledManifest = load.modInstance->manifest();
        if (!compiledManifest.providedCapabilities.empty()
            && !sameStringSet(compiledManifest.providedCapabilities, modManifest.providedCapabilities))
        {
            report += "- native mod '" + modManifest.id
                    + "' provides different capabilities in its compiled manifest than mod.toml declares under "
                      "'provides'; declare the provided capabilities in mod.toml (and only there) so the graph "
                      "and the validate dry run see them.\n";
            load.entryTable->destroyMod(load.modInstance);
            records.erase(recordIt);
            continue;
        }
        if (compiledManifest.providedCapabilities.empty() && !modManifest.providedCapabilities.empty())
        {
            report += "- native mod '" + modManifest.id + "' declares 'provides' in mod.toml but its compiled "
                    + "manifest provides nothing; rebuild the mod against this engine.\n";
            load.entryTable->destroyMod(load.modInstance);
            records.erase(recordIt);
            continue;
        }
        record.library       = std::move(load.library);
        record.modEntryTable = load.entryTable;
        record.modInstance   = load.modInstance;
        // Identity and wiring stay the package manifest's, exactly as the
        // manifest-only record wired them; the compiled manifest's role is
        // the agreement check above.
    }
    pendingNativeModPackages_.clear();
}

// Resolves the hosting module of one script mod: the active component that
// provides the host capability the package declared (exact name first, then
// any member of the demanded family), honoring the configuration's
// [providers] selection when several implementations are active. The kernel
// never names the host or the capability: the script package required the
// capability, the graph picked the provider.
EngineKernel::ComponentRecord *EngineKernel::resolveScriptHost(
    const ComponentRecord &scriptRecord, std::string &report
)
{
    const std::string capabilityName = scriptRecord.scriptMod.hostCapability;
    const auto providesHost          = [&capabilityName](const ComponentRecord &record) {
        return record.manifest.kind == ComponentKind::Module
            && std::any_of(
                record.manifest.providedCapabilities.begin(),
                record.manifest.providedCapabilities.end(),
                [&capabilityName](const std::string &candidate) {
                    return capabilityRequirementSatisfiedBy(capabilityName, candidate);
                }
            );
    };
    std::vector<ComponentRecord *> candidates;
    for (ComponentRecord &record : records)
    {
        // Script hosting is a module surface: mod instances are consumers by
        // contract shape (ModInterface has no ScriptModHost hook).
        if (record.manifest.kind != ComponentKind::Module || !record.constructed)
        {
            continue;
        }
        if (providesHost(record))
        {
            candidates.push_back(&record);
        }
    }
    if (candidates.empty())
    {
        report += "- script mod '" + scriptRecord.name + "' cannot be hosted: no active module provides the '"
                + capabilityName + "' capability it names as its host.\n";
        return nullptr;
    }
    if (candidates.size() > 1)
    {
        const std::vector<DependencyResolver::ProviderSelection> selections = configurationStore.providerSelections();
        const auto selection                                                = std::find_if(
            selections.begin(),
            selections.end(),
            [&capabilityName](const DependencyResolver::ProviderSelection &candidate) {
                return candidate.capabilityName == capabilityName;
            }
        );
        if (selection != selections.end())
        {
            for (ComponentRecord *candidate : candidates)
            {
                if (candidate->name == selection->providerName)
                {
                    return candidate;
                }
            }
        }
        std::string names;
        for (const ComponentRecord *candidate : candidates)
        {
            if (!names.empty())
            {
                names += ", ";
            }
            names += "'" + candidate->name + "'";
        }
        report += "- script mod '" + scriptRecord.name + "' cannot be hosted: " + std::to_string(candidates.size())
                + " active modules provide the '" + capabilityName + "' capability (" + names
                + "); select one under [providers].\n";
        return nullptr;
    }
    return candidates.front();
}

DependencyResolver::ResolutionResult EngineKernel::resolveGraph(
    const std::vector<ComponentManifest> &manifests, std::string &report
) const
{
    DependencyResolver resolver(manifests, configurationStore.providerSelections());
    const DependencyResolver::ResolutionResult resolution = resolver.resolve();
    if (!resolution.successful)
    {
        appendDiagnosticsReport(resolution.diagnostics, report);
    }
    return resolution;
}

void EngineKernel::appendDiagnosticsReport(
    const std::vector<DependencyResolver::ResolutionDiagnostic> &diagnostics, std::string &report
) const
{
    report += "engine startup failed with " + std::to_string(diagnostics.size()) + " problem(s):\n";
    for (const DependencyResolver::ResolutionDiagnostic &diagnostic : diagnostics)
    {
        report += "  - " + diagnostic.message + "\n";
    }
}

bool EngineKernel::validate(
    std::string &report
)
{
    report.clear();
    if (!prepareComponents(report, false))
    {
        // The preparation accumulates warnings (plugin-scan and mod-scan
        // advisories) into startupWarnings_ while it runs; a failed
        // preparation still reports them HERE, inside the report - the
        // composition root prints this report as-is (validate's report is
        // the one artifact a CI log keeps), so the warnings must ride in it
        // on every exit path. The caller never adds a second warning print
        // of its own, and a double print would suggest two scans ran.
        report += startupWarnings_;
        return false;
    }

    // Permission denials from the configuration are precise startup problems.
    std::vector<DependencyResolver::ResolutionDiagnostic> permissionDiagnostics;
    for (const auto &[record, permission] : applyPermissionPass())
    {
        DependencyResolver::ResolutionDiagnostic diagnostic;
        diagnostic.kind    = DependencyResolver::ResolutionDiagnostic::Kind::UnknownCapability;
        diagnostic.message = describeComponentKind(record->manifest.kind) + " '" + record->name
                           + "' requires permission '" + permission + "' which the configuration denies.";
        permissionDiagnostics.push_back(std::move(diagnostic));
    }
    if (!permissionDiagnostics.empty())
    {
        appendDiagnosticsReport(permissionDiagnostics, report);
    }

    // Known-key registry: the assembly's own sections plus every component's
    // declared key surface. A malformed declaration is a startup problem -
    // it would silently weaken the unknown-key warnings otherwise.
    if (!declareKnownKeyRegistry(report))
    {
        report += startupWarnings_;
        return false;
    }

    if (!report.empty())
    {
        // Non fatal diagnostics accumulated: denied permissions, malformed
        // key declarations.
        report += startupWarnings_;
        return false;
    }

    // Warnings never fail validation and never fail a run: they are appended
    // after the failure check so a warning stays a warning.
    report += deferredConfigurationWarnings;
    report += startupWarnings_;

    std::string resolutionReport;
    const DependencyResolver::ResolutionResult resolution = resolveGraph(componentManifests(), resolutionReport);
    report += resolutionReport;
    if (!resolution.successful)
    {
        return false; // the warnings above are already in the report
    }

    // Unknown-key warnings never fail validation: a key nobody reads is dead
    // configuration (usually a typo), and the report should say so - but the
    // graph above it is fine. Normal runs stay quiet; validate() is the place
    // where the author is looking.
    for (const std::string &warning : configurationStore.unknownKeyWarnings())
    {
        report += "warning: " + warning + "\n";
    }
    return true;
}

bool EngineKernel::declareKnownKeyRegistry(
    std::string &report
)
{
    // The assembly's own sections plus every constructed record's declared
    // key surface. Declared from BOTH entry points that run the unknown-key
    // pass - validate() and reloadConfiguration() - so a reload in a
    // construct-startup run (the composition root never called validate)
    // compares against the same registry a validate would have built from
    // the same frozen component set. A malformed declaration is reported,
    // never silent: it would weaken the unknown-key warnings otherwise.
    configurationStore.declareAssemblyKnownKeys();
    for (const ComponentRecord &record : records)
    {
        for (const std::string &configKey : record.manifest.knownConfigKeys)
        {
            if (!configurationStore.declareKnownConfigKey(configKey))
            {
                report += "- " + describeComponentKind(record.manifest.kind) + " '" + record.name
                        + "' declares the malformed configuration key '" + configKey
                        + "'; keys are '<section>.<dotted path>' of bare key segments.\n";
            }
        }
    }
    return report.empty();
}

bool EngineKernel::runConstructPhase(
    const DependencyResolver::ResolutionResult &resolution,
    const std::vector<ComponentManifest> &manifests,
    std::string &report
)
{
    for (const std::size_t manifestIndex : resolution.constructionOrder)
    {
        const ComponentManifest &manifest = manifests[manifestIndex];
        ComponentRecord *record           = findRecord(manifest.name);
        if (record == nullptr || record->constructed)
        {
            continue;
        }

        // Script mods have no instance of their own: the record is a proxy in
        // the dependency graph, and the actual script runtime lives in the
        // module that provides the scripting capability. The resolver
        // guarantees that provider constructed first (the script's
        // requirement is a graph edge).
        if (record->manifest.kind == ComponentKind::ScriptMod)
        {
            ComponentRecord *hostRecord = resolveScriptHost(*record, report);
            if (hostRecord == nullptr)
            {
                return false;
            }
            if (!hostRecord->constructed)
            {
                report += "- script mod '" + record->name + "' cannot be hosted: module '" + hostRecord->name
                        + "' did not construct.\n";
                return false;
            }
            ScriptModHost *scriptHost = hostRecord->moduleInstance->scriptModHost();
            if (scriptHost == nullptr)
            {
                report += "- script mod '" + record->name + "' cannot be hosted: module '" + hostRecord->name
                        + "' provides the '" + record->scriptMod.hostCapability
                        + "' capability but does not host script mods.\n";
                return false;
            }
            try
            {
                scriptHost->hostScriptMod(record->scriptMod);
            } catch (const std::exception &error)
            {
                report += "- construct failed for script mod '" + record->name + "': " + error.what() + "\n";
                return false;
            }
            record->constructed = true;
            componentStartOrder.push_back(record);
            continue;
        }

        // Heap anchored: the component may cache this context for its whole
        // lifetime (event publishing from frame loops or input callbacks), so
        // it must not live in this loop's stack frame. The component's kind
        // rides along: the kernel mediates the capability directions with it
        // (a module is answered only by other modules).
        record->context = std::make_unique<ComponentContext>(
            bus,
            *this,
            permissionSystem_,
            configurationStore,
            record->name,
            record->manifest.kind,
            &demandedCapabilities,
            mainThreadScheduler_
        );
        ComponentContext &context = *record->context;
        try
        {
            if (record->moduleInstance != nullptr)
            {
                record->moduleInstance->construct(context);
            } else if (record->modInstance != nullptr)
            {
                record->modInstance->construct(context);
            }
        } catch (const std::exception &error)
        {
            report += "- construct failed for " + describeComponentKind(record->manifest.kind) + " '" + record->name
                    + "': " + error.what() + "\n";
            return false;
        }

        // The construct phase validates that every declared capability has an
        // implementation behind it - and nothing else. No pointer is stored:
        // acquisition pulls from the declaring provider at call time, so there
        // is no registry to fill and no shared instance to hand out.
        for (const std::string &capabilityName : record->manifest.providedCapabilities)
        {
            void *capabilityPointer = nullptr;
            if (record->moduleInstance != nullptr)
            {
                capabilityPointer = record->moduleInstance->capabilityPointerFor(capabilityName);
            } else if (record->modInstance != nullptr)
            {
                capabilityPointer = record->modInstance->capabilityPointerFor(capabilityName);
            }
            if (capabilityPointer == nullptr)
            {
                report += "- " + describeComponentKind(record->manifest.kind) + " '" + record->name
                        + "' declares capability '" + capabilityName + "' but returned no implementation.\n";
                return false;
            }
        }
        record->constructed = true;
        componentStartOrder.push_back(record);
    }
    return true;
}

// The provider classification behind every capability question the kernel
// answers at runtime: resolveCapabilityPointer (throwing acquire),
// hasSingleCapabilityProvider (non-throwing probe), tryResolveCapabilityPointer
// (non-throwing pull) and resolveEachCapabilityPointer (plural pull). Walks
// the constructed records once and collects the exact providers of the name
// plus the family members whose name satisfies the family request, each with
// the member name that satisfied it. Sharing the walk is the point: the
// acquire, the probe and the plural pull can never disagree about what is
// available. Whether the walk result is cached per name (the frozen component
// set) or handed out from the retained scratch (construct in progress) is
// capabilityProviderSetFor's decision - this function only fills the set it
// is given, clearing it first so the retained scratch keeps its capacity
// across calls (the discipline that keeps the unfrozen path heap-free after
// its high-water mark).
void EngineKernel::classifyCapabilityProviders(
    const std::string &capabilityName, CapabilityProviderSet &outSet
)
{
    outSet.exactProviders.clear();
    outSet.memberProviders.clear();
    for (ComponentRecord &record : records)
    {
        if (!record.constructed)
        {
            continue;
        }
        for (const std::string &providedName : record.manifest.providedCapabilities)
        {
            if (providedName == capabilityName)
            {
                outSet.exactProviders.push_back(&record);
            } else if (capabilityRequirementSatisfiedBy(capabilityName, providedName))
            {
                outSet.memberProviders.emplace_back(&record, providedName);
                // One record enters memberProviders once even when it declares
                // several members of the requested family: the plural pull
                // below would otherwise answer the same pointer twice, and the
                // singular ambiguity check counts providers, not declarations.
                break;
            }
        }
    }
}

// The single classification answer behind the four resolve entry points (see
// the declaration in the header for the caching contract). The frozen path
// stores one entry per DISTINCT requested name: the first acquisition of the
// name pays the walk, every later one reads the map - and the pointer the
// caller finally receives is still pulled from the provider at call time.
const EngineKernel::CapabilityProviderSet &EngineKernel::capabilityProviderSetFor(
    const std::string &capabilityName
)
{
    if (!capabilityProviderIndexFrozen)
    {
        // Construct in progress: the constructed flags are mid-flip, so no
        // cache entry may be written - the walk answer is handed out from the
        // retained scratch and consumed before any component code runs.
        classifyCapabilityProviders(capabilityName, providerScratch);
        return providerScratch;
    }
    // Linear scan over the small per-name index (see the member comment for
    // why a vector beats a hash map at this population): the comparison is
    // short string equality, the entries are contiguous, and the first
    // resolution of a name is the only one that pays the walk AND the only
    // one that grows the index.
    for (CapabilityProviderIndexEntry &entry : capabilityProviderIndex)
    {
        if (entry.capabilityName == capabilityName)
        {
            return entry.providers;
        }
    }
    // First resolution of this name after the freeze: walk the records into
    // the scratch, THEN cache the copy. The push must never store the scratch
    // unread: the scratch still holds whatever the LAST classification put in
    // it (the unfrozen path above rewrote it per query during construct), so
    // caching it unread answered every post-freeze first-resolution with
    // another capability's provider set - the start-phase probes for 'scene'
    // and 'renderer.vulkan.overlay' reported the scripting.lua_bridge members
    // as their own providers, and the run died on the false ambiguity.
    classifyCapabilityProviders(capabilityName, providerScratch);
    capabilityProviderIndex.push_back(CapabilityProviderIndexEntry{capabilityName, providerScratch});
    return capabilityProviderIndex.back().providers;
}

// The pull behind ComponentContext::acquireCapability. Resolves the requested
// capability name against the constructed components: exact declarations first,
// then - for a family request - the single active member of that family. Every
// resolved pointer comes straight from the declaring provider's
// capabilityPointerFor hook at call time; nothing is cached here between
// requests. A request that cannot resolve to exactly one implementation throws
// with a precise diagnostic.
//
// The capability directions are mediated here as well as in the resolver: a
// MODULE requester is answered only by other modules. A mod-provided
// capability is invisible to a module acquisition - the same classification
// answers every requester, so the module filter reads the cached set and
// never stores a second answer.
void *EngineKernel::resolveCapabilityPointer(
    const std::string & /*requesterName*/, ComponentKind requesterKind, const std::string &capabilityName
)
{
    // One classification answer (cached once the component set is frozen -
    // see capabilityProviderSetFor); the family case resolves through the
    // member pair's own name, so no shared scratch name can be clobbered.
    const CapabilityProviderSet &providers = capabilityProviderSetFor(capabilityName);
    const bool moduleRequester             = requesterKind == ComponentKind::Module;

    // The single-exact-provider shape is what every healthy run answers with
    // (one provider per capability, requested by its family or exact name):
    // no filtering vector, no ambiguity scan - one kind check, then the pull.
    // The filtering vectors below materialize only on the paths that are
    // about to produce a diagnostic (or the genuinely plural plural-pull
    // shape), so a frame-time acquisition allocates nothing on any path it
    // can take.
    if (providers.exactProviders.size() == 1) [[likely]]
    {
        ComponentRecord *provider = providers.exactProviders.front();
        if (!moduleRequester || provider->manifest.kind == ComponentKind::Module) [[likely]]
        {
            return capabilityPointerOf(*provider, capabilityName);
        }
    } else if (providers.exactProviders.size() > 1)
    {
        std::vector<ComponentRecord *> visibleProviders;
        visibleProviders.reserve(providers.exactProviders.size());
        for (ComponentRecord *provider : providers.exactProviders)
        {
            if (!moduleRequester || provider->manifest.kind == ComponentKind::Module)
            {
                visibleProviders.push_back(provider);
            }
        }
        if (visibleProviders.size() > 1)
        {
            std::string names;
            for (const ComponentRecord *provider : visibleProviders)
            {
                if (!names.empty())
                {
                    names += ", ";
                }
                names += "'" + provider->name + "'";
            }
            throw std::logic_error(
                "capability '" + capabilityName + "' is provided by several components (" + names
                + "); choose one in the configuration under [providers]"
            );
        }
        if (visibleProviders.size() == 1) [[unlikely]]
        {
            return capabilityPointerOf(*visibleProviders.front(), capabilityName);
        }
        // A module requester with only mod providers falls through to the
        // precise direction diagnostic below (the resolver already refuses
        // such a declaration at startup; this is the runtime backstop).
    }

    if (providers.memberProviders.size() == 1) [[likely]]
    {
        const auto &member = providers.memberProviders.front();
        if (!moduleRequester || member.first->manifest.kind == ComponentKind::Module)
        {
            return capabilityPointerOf(*member.first, member.second);
        }
    } else if (providers.memberProviders.size() > 1)
    {
        std::vector<std::pair<ComponentRecord *, std::string>> visibleMembers;
        visibleMembers.reserve(providers.memberProviders.size());
        for (const auto &member : providers.memberProviders)
        {
            if (!moduleRequester || member.first->manifest.kind == ComponentKind::Module)
            {
                visibleMembers.push_back(member);
            }
        }
        if (visibleMembers.size() > 1)
        {
            std::string names;
            for (const auto &member : visibleMembers)
            {
                if (!names.empty())
                {
                    names += ", ";
                }
                names += "'" + member.first->name + "'";
            }
            throw std::logic_error(
                "capability family '" + capabilityName + "' is ambiguous: " + std::to_string(visibleMembers.size())
                + " implementations are active (" + names + "); choose one in the configuration under [providers]"
            );
        }
        if (visibleMembers.size() == 1) [[unlikely]]
        {
            return capabilityPointerOf(*visibleMembers.front().first, visibleMembers.front().second);
        }
    }

    if (moduleRequester)
    {
        std::string providerNames;
        for (const ComponentRecord *provider : providers.exactProviders)
        {
            if (!providerNames.empty())
            {
                providerNames += ", ";
            }
            providerNames += "'" + provider->name + "'";
        }
        for (const auto &member : providers.memberProviders)
        {
            if (!providerNames.empty())
            {
                providerNames += ", ";
            }
            providerNames += "'" + member.first->name + "'";
        }
        if (!providerNames.empty())
        {
            throw std::logic_error(
                "capability '" + capabilityName + "' is provided only by mods (" + providerNames
                + "); modules can only use capabilities provided by other modules"
            );
        }
    }

    throw std::logic_error(
        "capability '" + capabilityName
        + "' has no active provider; a component acquired it without a "
          "requirement whose provider is available in this run"
    );
}

// The non-throwing probe behind ComponentContext::isCapabilityAvailable.
// Shares the classification walk with the acquire above, so "available" here
// means exactly "the acquire would return an implementation": one exact
// provider, or no exact provider and exactly one family member. Ambiguity
// and absence both read as unavailable - the consumer degrades instead of
// throwing, and a diagnostic for the ambiguous case already reached the
// startup report through the resolver (an ambiguous optional capability is a
// startup error, so a run that got here resolved it).
bool EngineKernel::hasSingleCapabilityProvider(
    const std::string &capabilityName, ComponentKind requesterKind
)
{
    const CapabilityProviderSet &providers = capabilityProviderSetFor(capabilityName);
    const bool moduleRequester             = requesterKind == ComponentKind::Module;
    if (!providers.exactProviders.empty())
    {
        if (!moduleRequester)
        {
            return providers.exactProviders.size() == 1;
        }
        std::size_t visibleCount = 0;
        for (const ComponentRecord *provider : providers.exactProviders)
        {
            if (provider->manifest.kind == ComponentKind::Module)
            {
                ++visibleCount;
            }
        }
        return visibleCount == 1;
    }
    if (!moduleRequester)
    {
        return providers.memberProviders.size() == 1;
    }
    std::size_t visibleMembers = 0;
    for (const auto &member : providers.memberProviders)
    {
        if (member.first->manifest.kind == ComponentKind::Module)
        {
            ++visibleMembers;
        }
    }
    return visibleMembers == 1;
}

// The non-throwing pull behind ComponentContext::tryAcquireCapability.
// Shares the classification walk with both throwing/probing siblings above,
// so the three can never disagree about what resolves. Absence and ambiguity
// answer nullptr (the caller degrades); a single resolution returns the same
// pointer the throwing acquire hands out, pulled from the declaring provider
// at call time. A provider that declares the capability but returns no
// implementation still throws through capabilityPointerOf: a broken provider
// is a contract breach the probing consumer must see, not an unavailability
// to silently swallow (see CapabilityResolver::tryResolveCapabilityPointer).
void *EngineKernel::tryResolveCapabilityPointer(
    const std::string & /*requesterName*/, ComponentKind requesterKind, const std::string &capabilityName
)
{
    const CapabilityProviderSet &providers = capabilityProviderSetFor(capabilityName);
    const bool moduleRequester             = requesterKind == ComponentKind::Module;

    std::size_t visibleExact  = 0;
    ComponentRecord *exactOne = nullptr;
    for (ComponentRecord *provider : providers.exactProviders)
    {
        if (moduleRequester && provider->manifest.kind != ComponentKind::Module)
        {
            continue;
        }
        ++visibleExact;
        exactOne = provider;
    }
    if (visibleExact == 1)
    {
        return capabilityPointerOf(*exactOne, capabilityName);
    }
    if (providers.exactProviders.empty() || visibleExact == 0)
    {
        std::size_t visibleMembers = 0;
        std::pair<ComponentRecord *, std::string> memberOne;
        for (const auto &member : providers.memberProviders)
        {
            if (moduleRequester && member.first->manifest.kind != ComponentKind::Module)
            {
                continue;
            }
            ++visibleMembers;
            memberOne = member;
        }
        if (visibleMembers == 1)
        {
            return capabilityPointerOf(*memberOne.first, memberOne.second);
        }
    }
    return nullptr;
}

// The multi-provider pull behind ComponentContext::acquireEachCapability.
// Shares the classification walk with the singular acquire above: exact
// providers first, then family members, in the records' (stable) order. Each
// pointer comes straight from the declaring provider at call time. The plural
// contract treats absence as an empty answer and skips providers that return
// no implementation for the name (the singular path still turns those into
// precise diagnostics) - a consumer asking for ALL implementations gets the
// ones that exist.
std::vector<void *> EngineKernel::resolveEachCapabilityPointer(
    const std::string & /*requesterName*/, ComponentKind requesterKind, const std::string &capabilityName
)
{
    const CapabilityProviderSet &providers = capabilityProviderSetFor(capabilityName);
    // A module requester sees only module providers - the same direction
    // rule the singular acquire and the resolver enforce (docs/architecture.md,
    // "Capability directions").
    const bool moduleRequester = requesterKind == ComponentKind::Module;

    std::vector<void *> pointers;
    if (!providers.exactProviders.empty())
    {
        pointers.reserve(providers.exactProviders.size());
        for (ComponentRecord *provider : providers.exactProviders)
        {
            if (moduleRequester && provider->manifest.kind != ComponentKind::Module)
            {
                continue;
            }
            if (provider->moduleInstance != nullptr)
            {
                if (void *capabilityPointer = provider->moduleInstance->capabilityPointerFor(capabilityName);
                    capabilityPointer != nullptr)
                {
                    pointers.push_back(capabilityPointer);
                }
            } else if (provider->modInstance != nullptr)
            {
                if (void *capabilityPointer = provider->modInstance->capabilityPointerFor(capabilityName);
                    capabilityPointer != nullptr)
                {
                    pointers.push_back(capabilityPointer);
                }
            }
        }
        return pointers;
    }
    if (!providers.memberProviders.empty())
    {
        pointers.reserve(providers.memberProviders.size());
        for (const auto &member : providers.memberProviders)
        {
            // Each family member carries its OWN member name (e.g.
            // "scripting.lua_bridge.scene", "scripting.lua_bridge.input" are
            // two different members of the "scripting.lua_bridge" family),
            // stored with the provider by the classification (and cached with
            // it once the component set is frozen) - the per-provider
            // re-derivation walk this loop used to run over every provider's
            // providedCapabilities is gone.
            ComponentRecord *provider = member.first;
            if (moduleRequester && provider->manifest.kind != ComponentKind::Module)
            {
                continue;
            }
            if (provider->moduleInstance != nullptr)
            {
                if (void *capabilityPointer = provider->moduleInstance->capabilityPointerFor(member.second);
                    capabilityPointer != nullptr)
                {
                    pointers.push_back(capabilityPointer);
                }
            } else if (provider->modInstance != nullptr)
            {
                if (void *capabilityPointer = provider->modInstance->capabilityPointerFor(member.second);
                    capabilityPointer != nullptr)
                {
                    pointers.push_back(capabilityPointer);
                }
            }
        }
    }
    return pointers;
}

// Asks one component for the implementation behind a capability name it
// declared. The construct phase verified the pointer exists, so nullptr here
// is a broken provider, not a resolution failure.
void *EngineKernel::capabilityPointerOf(
    ComponentRecord &provider, const std::string &capabilityName
)
{
    void *capabilityPointer = nullptr;
    if (provider.moduleInstance != nullptr)
    {
        capabilityPointer = provider.moduleInstance->capabilityPointerFor(capabilityName);
    } else if (provider.modInstance != nullptr)
    {
        capabilityPointer = provider.modInstance->capabilityPointerFor(capabilityName);
    }
    if (capabilityPointer == nullptr)
    {
        throw std::logic_error(
            "component '" + provider.name + "' declares capability '" + capabilityName
            + "' but returned no implementation"
        );
    }
    return capabilityPointer;
}

bool EngineKernel::construct(
    std::string &report
)
{
    report.clear();
    if (constructed)
    {
        return true;
    }
    // A validate-only gather defers the native mod libraries (the dry run
    // maps no mod binary and asks for no agreement). A construct that
    // follows such a gather re-gathers from scratch: the enforced trust gate
    // and the library loads must both have run before any component
    // constructs. stop() tears the manifest-only state down cheaply (nothing
    // was constructed) and resets componentsGathered.
    if (componentsGathered && !nativeModLibrariesLoaded_)
    {
        stop();
    }
    // Restart safety: the provider index describes the PREVIOUS run's
    // component set. stop() already dropped it, but a construct that follows
    // a failed phase (which never reached the freeze) starts clean too - the
    // walk path answers every acquisition until the new set completes.
    capabilityProviderIndexFrozen = false;
    capabilityProviderIndex.clear();
    if (!prepareComponents(report, true))
    {
        return false;
    }
    // The same permission pass as validate: configuration denials become
    // precise startup problems before any construct callback runs.
    for (const auto &[record, permission] : applyPermissionPass())
    {
        report += "- " + describeComponentKind(record->manifest.kind) + " '" + record->name + "' requires permission '"
                + permission + "' which the configuration denies.\n";
    }
    if (!report.empty())
    {
        report = "engine startup failed:\n" + report;
        return false;
    }

    // One manifest snapshot serves both the resolution and the construct
    // phase: componentManifests() copies every record's manifest (~25
    // manifests x several string vectors each), and the two back-to-back
    // calls were identical deep copies.
    const std::vector<ComponentManifest> manifests = componentManifests();
    std::string resolutionReport;
    const DependencyResolver::ResolutionResult resolution = resolveGraph(manifests, resolutionReport);
    report += resolutionReport;
    if (!resolution.successful)
    {
        return false;
    }

    if (!runConstructPhase(resolution, manifests, report))
    {
        stop();
        report = "engine startup failed:\n" + report;
        return false;
    }
    // The component set is complete and immutable from here until stop():
    // freeze the capability provider index so every post-construct
    // acquisition resolves through the per-name cache (see
    // capabilityProviderSetFor). A failed construct leaves the flag down -
    // the recovery stop() below runs the walk path throughout.
    capabilityProviderIndexFrozen = true;
    constructed                   = true;
    return true;
}

void EngineKernel::start()
{
    if (!constructed || started)
    {
        throw std::runtime_error("engine kernel: start requires a completed construct phase");
    }
    // The construct phase walks the resolver's dependency order and so does
    // the start phase: a module's start consumes provider work (the renderer
    // targets the window created during the window module's start, the input
    // module attaches to it). Iterating the discovery-ordered records vector
    // here would race the dependency graph - the plugin scan order says
    // nothing about providership, and on a filesystem that returns modules
    // alphabetically the renderer would start before the window existed.
    for (ComponentRecord *recordPointer : componentStartOrder)
    {
        ComponentRecord &record = *recordPointer;
        if (!record.constructed || record.started)
        {
            continue;
        }
        if (record.moduleInstance != nullptr)
        {
            record.moduleInstance->start();
        } else if (record.modInstance != nullptr)
        {
            record.modInstance->start();
        }
        record.started = true;
    }
    started = true;
    bus.publish(EngineStartupCompletedEvent{});
}

void EngineKernel::stop()
{
    // The capability provider index dies FIRST: the component set it
    // describes starts changing below (constructed flags flip, instances
    // stop, records clear), and an acquisition a stopping component makes
    // must answer from the live walk, never from a cached list (see
    // capabilityProviderSetFor for the cache's lifetime contract).
    capabilityProviderIndexFrozen = false;
    capabilityProviderIndex.clear();
    if (started)
    {
        // The last deferred flush: an event deferred during the final frame
        // still publishes while every component is alive, exactly like the
        // composition root's per-frame flush would have delivered it. One
        // atomic check when nothing was deferred. Handler exceptions are
        // contained here: stop() also runs from the destructor, and an
        // exception unwinding through it would terminate the process.
        try
        {
            bus.flushDeferredEvents();
            bus.publish(EngineShutdownStartedEvent{});
        } catch (const std::exception &error)
        {
            // Nothing left to route the diagnostic through but stderr: the
            // bus may already be tearing down the component that logged.
            std::fprintf(stderr, "engine kernel: shutdown event handler failed: %s\n", error.what());
        }
    }
    // No timed callback may fire during teardown: the components the tasks
    // touch are about to stop in reverse order, and a recurring task has
    // nothing left to schedule into. Handles held by components disarm
    // their shared flags here; their own release() calls become no-ops.
    mainThreadScheduler_.clear();
    // Tear down in the reverse of the start order: consumers stop while the
    // providers they depend on still exist (the renderer releases its device
    // and surface before the window module destroys the window they target).
    // Records that never completed the construct phase are not in the
    // sequence; their instances were never started, so there is nothing to
    // stop - the per-record pass below still releases them.
    for (auto iterator = componentStartOrder.rbegin(); iterator != componentStartOrder.rend(); ++iterator)
    {
        ComponentRecord &record = **iterator;
        if (record.started && record.moduleInstance != nullptr)
        {
            record.moduleInstance->stop();
        }
        if (record.started && record.modInstance != nullptr)
        {
            record.modInstance->stop();
        }
    }
    for (ComponentRecord &record : records)
    {
        // Shared library mods own their destruction function; destroying them
        // through delete here would use the engine allocator on mod memory.
        if (record.modEntryTable != nullptr && record.modInstance != nullptr)
        {
            record.modEntryTable->destroyMod(record.modInstance);
            record.modInstance = nullptr;
        }
        record.started     = false;
        record.constructed = false;
    }
    started     = false;
    constructed = false;

    // The bus's channels die HERE, while every provider library is still
    // loaded: a channel's concrete destructor is code inside the plugin
    // binary that created it (event_bus.hpp, releaseChannels), and the
    // record destruction right below unloads those binaries. The kernel
    // destructor's own bus teardown afterwards works on an empty map.
    bus.releaseChannels();

    records.clear();
    componentStartOrder.clear();
    componentsGathered = false;
}

// The live reload path. Shape: validate every file by parsing it standalone,
// snapshot the effective values, re-assemble the layers exactly like the
// startup path does (same order, same merge semantics), diff the snapshots,
// publish the change set. The pre-parse pass is what makes a half-edited
// file a clean "reload refused" instead of a torn effective configuration.
bool EngineKernel::reloadConfiguration(
    std::string &report
)
{
    report.clear();
    if (!constructed)
    {
        report = "engine reload refused:\n- reload requires a completed construct phase; before construct the "
                 "configuration is assembled for the first time, not re-assembled\n";
        return false;
    }

    // ---- pre-parse pass: every file the re-assembly will read --------------
    // The store is only touched after this whole pass succeeds, so a failure
    // here leaves the running components exactly what they were reading.
    const std::filesystem::path resolvedConfigurationPath = resolveAgainstExecutableDirectory(configurationFilePath);
    std::string parseError;
    if (!ConfigurationStore::fileParses(resolvedConfigurationPath, parseError))
    {
        report = "engine reload refused (the running configuration is untouched):\n- " + parseError + "\n";
        return false;
    }
    for (const std::filesystem::path &defaultsFile : collectModuleDefaultsFiles())
    {
        if (!ConfigurationStore::fileParses(defaultsFile, parseError))
        {
            report = "engine reload refused (the running configuration is untouched):\n- " + parseError + "\n";
            return false;
        }
    }
    for (const auto &[defaultsFile, source] : modDefaultsFiles)
    {
        (void)source;
        if (!ConfigurationStore::fileParses(defaultsFile, parseError))
        {
            report = "engine reload refused (the running configuration is untouched):\n- " + parseError + "\n";
            return false;
        }
    }
    // The world source, exactly the candidates finishConfiguration resolves:
    // the explicit scene file, or the single content mod's claim when the
    // assembly delegates the world.
    std::filesystem::path reloadScenePath;
    if (!sceneFilePath.empty())
    {
        reloadScenePath = resolveAgainstExecutableDirectory(sceneFilePath);
    } else if (!assemblyDefinesScene && modSceneClaims.size() == 1)
    {
        reloadScenePath = modSceneClaims.front().sceneFile;
    }
    if (!reloadScenePath.empty() && !ConfigurationStore::fileParses(reloadScenePath, parseError))
    {
        report = "engine reload refused (the running configuration is untouched):\n- " + parseError + "\n";
        return false;
    }

    // ---- snapshot, re-assemble, diff ---------------------------------------
    const std::map<std::string, std::string> previousValues = configurationStore.flattenedValueReprs();

    configurationStore.resetForReload();
    configurationLoaded = false;
    std::string reloadError;
    if (!loadConfiguration(reloadError))
    {
        report = "engine reload failed after the pre-parse pass (unexpected; the report names the file):\n- "
               + reloadError + "\n";
        return false;
    }
    if (!configurationStore.applyCommandLineOverrides(reloadError))
    {
        report = "engine reload failed after the pre-parse pass (unexpected):\n- " + reloadError + "\n";
        return false;
    }
    for (const auto &[defaultsFile, source] : modDefaultsFiles)
    {
        if (!configurationStore.mergeDefaultsFile(
                defaultsFile, ConfigProvenance::Layer::ModDefaults, source, reloadError
            ))
        {
            report = "engine reload failed after the pre-parse pass (unexpected):\n- " + reloadError + "\n";
            return false;
        }
    }
    if (!finishConfiguration(reloadError))
    {
        report = "engine reload failed after the pre-parse pass (unexpected):\n- " + reloadError + "\n";
        return false;
    }

    const std::map<std::string, std::string> currentValues = configurationStore.flattenedValueReprs();

    // Enforcement state follows the file: the re-assembled store carries the
    // fresh [permissions] denials, so re-apply them (after dropping the
    // previous run's denials - a denial removed from the file must stop
    // denying). The component set is frozen (the graph never re-resolves),
    // so the grant pass re-declares exactly the same sets; only the denial
    // side can actually change.
    permissionSystem_.clearDenials();
    applyPermissionPass();

    // Diff, and split the world content out: [scene] was pushed into the
    // scene registry at start, so a scene key that changed is reported as a
    // restart requirement, never as an applied change.
    std::vector<std::string> changedKeys;
    std::size_t changedCount = 0;
    std::size_t sceneChanges = 0;
    std::size_t removedCount = 0;
    const auto isScenePath   = [](const std::string &path) {
        return path == ConfigurationStore::sceneSectionName
            || path.rfind(std::string(ConfigurationStore::sceneSectionName) + ".", 0) == 0;
    };
    for (const auto &[path, representation] : currentValues)
    {
        (void)representation;
        const auto previous = previousValues.find(path);
        if (previous != previousValues.end() && previous->second == representation)
        {
            continue;
        }
        ++changedCount;
        if (isScenePath(path))
        {
            ++sceneChanges;
            continue;
        }
        changedKeys.push_back(path);
    }
    for (const auto &[path, representation] : previousValues)
    {
        (void)representation;
        if (currentValues.find(path) == currentValues.end())
        {
            ++removedCount;
            if (!isScenePath(path))
            {
                // A removed key is a change too: the component re-reading it
                // falls back to its built-in default, which is a behavior
                // difference the subscribers must see.
                changedKeys.push_back(path);
            }
        }
    }

    report += "engine configuration reloaded: " + std::to_string(changedCount) + " key(s) changed, "
            + std::to_string(removedCount) + " key(s) removed";
    if (sceneChanges > 0)
    {
        report += "; " + std::to_string(sceneChanges)
                + " world content ([scene]) change(s) require a restart and were not applied live";
    }
    report += "\n";
    if (changedKeys.empty())
    {
        report += "no live keys changed; nothing was published\n";
        return true;
    }
    report += "changed keys:";
    for (const std::string &key : changedKeys)
    {
        report += " " + key;
    }
    report += "\n";
    // The same unknown-key pass a validate runs, so a reload that introduced
    // a typo is reported the same way a startup with that typo would be.
    for (const std::string &warning : configurationStore.unknownKeyWarnings())
    {
        report += "warning: " + warning + "\n";
    }

    // The event carries only the live diff ([scene] excluded above). The bus
    // is single threaded and the reload runs on the main thread, so handlers
    // may do anything a frame handler may - re-read configuration, update
    // state, publish follow-up events.
    ConfigurationReloadedEvent reloaded;
    reloaded.changedKeys = std::move(changedKeys);
    bus.publish(reloaded);
    return true;
}

// The introspection report behind --print-graph and [diagnostics] graph_report.
// Built fresh from the records on every call: no cached state to drift, no
// cost for the runs that never print it.
std::string EngineKernel::componentGraphReport() const
{
    std::string report;
    // Start order once the run has one (post-construct). Before construct -
    // the --print-graph path - the resolver computes the dependency order
    // fresh from the manifests: the same pure function construct uses, so
    // the printed order IS the order a real run would start in. Discovery
    // order is deliberately never printed as if it were a start order - it
    // says nothing about providership.
    std::vector<const ComponentRecord *> orderedRecords;
    const char *orderSource = "start order";
    if (!componentStartOrder.empty())
    {
        orderedRecords.assign(componentStartOrder.begin(), componentStartOrder.end());
    } else if (!records.empty())
    {
        std::string resolutionReport;
        const DependencyResolver::ResolutionResult resolution = resolveGraph(componentManifests(), resolutionReport);
        orderSource                                           = "resolved construct order (nothing is constructed yet)";
        if (resolution.successful)
        {
            for (const std::size_t manifestIndex : resolution.constructionOrder)
            {
                orderedRecords.push_back(&records[manifestIndex]);
            }
        } else
        {
            // An unresolvable graph still deserves a report - the declared
            // order with the resolution diagnostics attached.
            orderSource = "declared order (graph resolution failed - see --validate)";
            for (const ComponentRecord &record : records)
            {
                orderedRecords.push_back(&record);
            }
            report += resolutionReport;
        }
    }

    report += "component graph (" + std::to_string(records.size()) + " component(s), " + orderSource + "):\n";
    for (const ComponentRecord *recordPointer : orderedRecords)
    {
        const ComponentRecord &record = *recordPointer;
        report += "  " + record.name + " (" + describeComponentKind(record.manifest.kind) + " "
                + record.manifest.version + ")\n";

        const auto appendList = [&report](const char *label, const std::vector<std::string> &names) {
            report += std::string("    ") + label + ":";
            if (names.empty())
            {
                report += " (none)";
            } else
            {
                for (const std::string &name : names)
                {
                    report += " " + name;
                }
            }
            report += "\n";
        };
        appendList("provides", record.manifest.providedCapabilities);
        appendList("requires", record.manifest.requiredCapabilities);
        if (!record.manifest.optionalCapabilities.empty())
        {
            appendList("optional", record.manifest.optionalCapabilities);
        }
        appendList("permissions", record.manifest.requiredPermissions);
        if (!record.scriptMod.modId.empty())
        {
            report += "    script host: "
                    + (record.scriptHostName.empty() ? std::string("(unresolved)") : record.scriptHostName) + "\n";
        }
        report += std::string("    state: ")
                + (record.started        ? "started"
                    : record.constructed ? "constructed"
                                         : "declared")
                + "\n";
    }

    // The event subscription map completes the picture the graph paints: the
    // graph says who provides and who requires, this says who is LISTENING
    // and how heavy each channel's dispatch scan is. Empty before construct
    // (a --print-graph dry run has no subscriptions yet) and for runs whose
    // components subscribed to nothing - then the section simply does not
    // print, like any other empty report.
    const std::string subscriptions = bus.subscriptionReport();
    if (!subscriptions.empty())
    {
        report += subscriptions;
    }
    return report;
}

} // namespace engine
