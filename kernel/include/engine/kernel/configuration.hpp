#ifndef ENGINE_KERNEL_CONFIGURATION_HPP
#define ENGINE_KERNEL_CONFIGURATION_HPP

#include "dependency_resolver.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Forward declaration matching toml++'s real shape: the type lives in the
// inline namespace toml::v3 and is re-exported under toml. Opening v3 as a
// regular namespace here would collide with the inline definition inside
// <toml.hpp>.
namespace toml
{
inline namespace v3
{
class table;
class node;
} // namespace v3
} // namespace toml

namespace engine
{

// Where one effective configuration value came from. Recorded for every leaf
// key while the layered configuration is assembled, surfaced by the
// provenance report and by the unknown-key warnings ("unknown key 'x', from
// '...'" only means something when the
// source layer is named).
struct ConfigProvenance
{
    // Layers in decreasing precedence: a staged override wins, the assembly
    // file wins over everything that is shipped, shipped defaults fill only
    // what nobody above them provided.
    enum class Layer
    {
        CommandLine,    // an override staged by the composition root
        EngineConfig,   // the assembly configuration file
        SceneFile,      // an applied scene file (wholesale [scene] swap)
        ModuleDefaults, // config/modules/<name>/config.toml
        ModDefaults     // <mod package>/config.toml
    };

    Layer layer = Layer::EngineConfig;

    // File path, mod package directory name, module name or "override".
    std::string source;

    std::string layerName() const;
};

// Loads and merges the layered engine configuration. The store owns the
// effective merged table; components read their own section through their
// ComponentContext. Paths inside the configuration are resolved relative to
// the executable directory by the kernel, not here.
//
// The layers, assembled in the order the kernel drives them (highest
// precedence last):
//
//   1. loadFromFile            the assembly file (config/engine.toml by
//                              default). Its values are final unless
//                              staged overrides replace them.
//   2. mergeDefaultsFile       shipped defaults, scanned by the kernel: one
//                              config.toml per module directory, one
//                              config.toml per active mod package. Defaults
//                              FILL: a key the assembly already provided is
//                              never touched, tables merge recursively,
//                              arrays and sub-tables are inserted only when
//                              absent.
//   3. applySceneFile          an explicit scene file: replaces the [scene]
//                              section wholesale. The file may define only
//                              [scene].
//   4. applyCommandLineOverrides  the staged "key=value" assignments, in the order
//                              they were given. Values use TOML syntax; a
//                              bare word becomes a literal string.
//
// Arrays and array-of-tables rows ([[scene.objects]]) are always replaced
// wholesale by a higher layer, never merged element-wise. The stack is
// assembled once before any component constructs; the kernel's explicit
// reloadConfiguration path (composition-root driven, see engine_kernel.hpp)
// re-assembles it at runtime through resetForReload.
class ConfigurationStore final
{
public:
    // File names the kernel looks for when scanning shipped defaults. Both
    // component kinds ship the same file name: a module's defaults live at
    // config/modules/<name>/config.toml, a mod package's next to its
    // mod.toml. One name, one meaning: "this file holds defaults", never
    // identity (the module's identity is its compiled manifest, the mod's is
    // its mod.toml).
    static constexpr const char *moduleDefaultsFileName = "config.toml";
    static constexpr const char *modDefaultsFileName    = "config.toml";

    // Section names shared by the kernel, the app and the authority policy.
    // Named once because each is spelled at several sites (assembly-authority
    // checks, typed accessors, the app's readers) and a misspelling would
    // silently read the wrong section.
    static constexpr const char *engineSectionName      = "engine";
    static constexpr const char *providersSectionName   = "providers";
    static constexpr const char *permissionsSectionName = "permissions";
    static constexpr const char *diagnosticsSectionName = "diagnostics";
    static constexpr const char *sceneSectionName       = "scene";
    static constexpr const char *trustSectionName       = "trust";

    // [trust] keys: the assembly's standing trust decisions over the risk
    // advisories (docs/architecture.md, "Risk advisories and the trust
    // gate"). A mod or module the risk assessment flags as structurally
    // sensitive must either be trusted here or the user agrees to it at
    // startup; trustAllMods / trustAllModules cover every component of one
    // kind, trustMods / trustModules name specific ones. The section is
    // assembly-owned: a shipped defaults file cannot define [trust] (a
    // package that could trust itself would defeat the gate).
    static constexpr const char *trustModsKey       = "trustMods";
    static constexpr const char *trustAllModsKey    = "trustAllMods";
    static constexpr const char *trustModulesKey    = "trustModules";
    static constexpr const char *trustAllModulesKey = "trustAllModules";

    // [diagnostics] keys. latency_report is read by the kernel (it flips the
    // bus's recording flag); frame_history is read by the composition root
    // (the frame-time percentile ring's capacity, frames); graph_report is
    // read by the composition root (prints the component graph once at
    // startup - a developer tool, off by default).
    static constexpr const char *diagnosticsLatencyReportKey = "latency_report";
    static constexpr const char *diagnosticsFrameHistoryKey  = "frame_history";
    static constexpr const char *diagnosticsGraphReportKey   = "graph_report";

    // [engine] keys read by the composition root (the app owns the frame
    // loop, so the pacing keys live with it, not in the kernel):
    // max_fps = the CPU-side frame cap (0 = uncapped, the default - a run
    // paced only by the swapchain reads no extra clock), idle_poll_ms = the
    // poll interval of the renderer-less event pump, config_reload_poll_seconds
    // = the live configuration reload poll interval (0 = reload disabled, the
    // default - the frame path pays nothing for a feature the run never
    // asked for), fixed_update_hz = the fixed-timestep update rate for the
    // EngineFixedUpdateEvent loop (0 = disabled, the default - the variable
    // heartbeat alone is the historical behavior), max_fixed_update_steps =
    // the catch-up clamp for one iteration's update burst, and
    // quit_after_seconds = a finite run length for headless and CI runs
    // (0 = run until the window closes, the default and the historical
    // behavior).
    static constexpr const char *engineMaxFpsKey                  = "max_fps";
    static constexpr const char *engineIdlePollMsKey              = "idle_poll_ms";
    static constexpr const char *engineConfigReloadPollSecondsKey = "config_reload_poll_seconds";
    static constexpr const char *engineFixedUpdateHzKey           = "fixed_update_hz";
    static constexpr const char *engineMaxFixedUpdateStepsKey     = "max_fixed_update_steps";
    static constexpr const char *engineQuitAfterSecondsKey        = "quit_after_seconds";

    // [engine].extra_mod_directories: additional mod package scan roots,
    // scanned after mod_directory in the listed order. Lets an assembly
    // compose its mod set from several install locations (an engine-owned
    // package directory plus a user directory) without copying packages
    // into one tree. Read by the kernel's mod discovery; explicit mod
    // directories staged by the composition root replace BOTH mod_directory
    // and this key, exactly like a single explicit directory always replaced
    // mod_directory alone.
    static constexpr const char *engineExtraModDirectoriesKey = "extra_mod_directories";

    // Directory scanned for per-module shipped defaults, relative to the
    // executable directory (one config.toml per subdirectory).
    static constexpr const char *moduleDefaultsDirectory = "config/modules";

    // Declared here, defined in the .cpp: the staged-override vector holds a
    // unique_ptr to the forward-declared TOML table, so every special member
    // that can destroy it must instantiate where the type is complete.
    ConfigurationStore();
    ~ConfigurationStore();

    ConfigurationStore(const ConfigurationStore &)            = delete;
    ConfigurationStore &operator=(const ConfigurationStore &) = delete;

    // Drops every layer, staged override and known key; the store returns to
    // its just-constructed state. The kernel calls this when the composition
    // root points it at a new configuration or scene file.
    void reset();

    // The reload variant of reset(): drops the assembled layers and
    // provenance but keeps the two things that describe STILL-LIVE state,
    // not files: the staged overrides (the composition root staged them
    // through the kernel; they stay the highest layer across a reload) and
    // the known-key declarations (the components that declared them are
    // still constructed; wiping their declarations would turn every key
    // into an unknown-key warning after a reload). The kernel's
    // reloadConfiguration pairs this with a fresh layer assembly.
    void resetForReload();

    // Layer 1: parses and merges the assembly file. On failure the effective
    // table stays empty and errorMessage holds the parser diagnostic including
    // source location.
    bool loadFromFile(const std::filesystem::path &path, std::string &errorMessage);

    // Layer 2: merges one shipped defaults file (a module's or a mod
    // package's config.toml).
    // Defaults fill gaps only, as described in the class comment. Assembly and
    // app authority sections ([engine], [providers], [permissions],
    // [diagnostics]) are rejected with a precise error: a shipped
    // package must not be able to grow a module list or deny permissions.
    // The [scene] section is rejected for the same authority reason: world
    // content has a dedicated channel (a mod's declared scene file or the
    // assembly's [scene]). A defaults file that
    // could drip scene keys into configurations that do not define them would
    // silently change OTHER configurations' worlds (a sealed-room test config
    // that does not name a sky gradient must not inherit one from an installed
    // content package).
    // layer selects ModuleDefaults or ModDefaults; source names the module or
    // package for the provenance report.
    bool mergeDefaultsFile(
        const std::filesystem::path &path,
        ConfigProvenance::Layer layer,
        const std::string &source,
        std::string &errorMessage
    );

    // Layer 3: replaces the [scene] section with the file's [scene] section
    // wholesale. The file may define ONLY [scene] (sub-tables and
    // array-of-tables rows included); any other top-level key, a non-table
    // [scene] or a missing [scene] is a precise error.
    bool applySceneFile(const std::filesystem::path &path, std::string &errorMessage);

    // Layer 4, staging: parses one "section.path=value" assignment now, so a
    // malformed assignment is a usage error before any backend starts. The value
    // side is parsed as TOML (numbers, booleans, quoted strings, arrays,
    // inline tables); anything that does not parse as a TOML value becomes a
    // literal string, so scene.note=My Title works without quotes.
    bool stageCommandLineOverride(const std::string &assignment, std::string &errorMessage);

    // Layer 4, application: writes the staged assignments into the effective
    // table in staging order, creating intermediate tables as needed.
    // Traversing through an existing non-table value is a precise error.
    bool applyCommandLineOverrides(std::string &errorMessage);

    // One "<dotted path> -> <canonical value representation>" pair per leaf
    // of the effective table (tables recurse, arrays of tables flatten as
    // "rows[0].key" subtrees, plain arrays serialize as one leaf at the
    // array's path). This is the reload diff's vocabulary: the kernel takes
    // two snapshots and publishes the paths whose values differ, so
    // subscribers see exactly the key spelling they re-read through the
    // typed accessors. Canonical representations carry a type prefix so a
    // string "1.0" never diffs as equal to the float 1.0.
    std::map<std::string, std::string> flattenedValueReprs() const;

    // Parse-only probe: true when the file parses as TOML, without merging
    // anything. The kernel's reload path validates every file it is about to
    // re-merge BEFORE touching the live store, so a half-edited file fails
    // the reload with a precise diagnostic instead of tearing the effective
    // configuration the running components are still reading.
    static bool fileParses(const std::filesystem::path &path, std::string &errorMessage);

    bool isLoaded() const;

    // Whether the section exists in the effective merged table. The kernel
    // uses it to decide whether the assembly provided its own world ([scene])
    // or delegates content to the installed content mod's scene file.
    bool hasSection(const std::string &sectionName) const;

    // [engine] section: runtime layout and discovery. Mod packages are
    // discovered in modDirectory and skipped when their id is in
    // disabledMods. There is deliberately no capability demand key here: the
    // active mods are the sole demand source, so a run without mods loads no
    // modules at all.
    std::string modDirectory() const;
    // Additional mod package scan roots ([engine].extra_mod_directories), in
    // the listed order, after mod_directory. Empty when the assembly composes
    // its mod set from one directory - the historical shape.
    std::vector<std::string> extraModDirectories() const;
    // Directory scanned for module plugin binaries, relative to the
    // executable directory when not absolute. Default: "modules".
    std::string modulesDirectory() const;
    std::vector<std::string> disabledMods() const;

    // [providers] capability name -> selected provider name.
    std::vector<DependencyResolver::ProviderSelection> providerSelections() const;

    // [permissions] denied = [[component, permission], ...].
    std::vector<std::pair<std::string, std::string>> permissionDenials() const;

    // [trust]: the assembly's standing trust decisions (see the key comments
    // above). trustMods / trustModules list component names the user trusts;
    // trustAllMods / trustAllModules flip the gate for every component of
    // one kind. The kernel consults these before asking the user to agree to
    // components the risk advisories flagged.
    std::vector<std::string> trustedMods() const;
    bool trustAllMods() const;
    std::vector<std::string> trustedModules() const;
    bool trustAllModules() const;

    // Typed value access into one component section, for example
    // stringValue("scene", "sky_radiance"). Returns empty when the section or
    // the key is absent; the owning component applies its own default.
    std::optional<std::string> stringValue(const std::string &sectionName, const std::string &key) const;
    std::optional<std::int64_t> integerValue(const std::string &sectionName, const std::string &key) const;
    std::optional<bool> booleanValue(const std::string &sectionName, const std::string &key) const;
    std::optional<double> floatingValue(const std::string &sectionName, const std::string &key) const;

    // Dotted-path access into one section, for example floatingAtPath(
    // "scene", "sun.intensity") or nested array-of-table rows such as
    // "objects[0].position". Shared world content ([scene]) and app-only
    // sections live outside any single component's own section; these helpers
    // keep that readable without exposing the TOML parser to modules.
    std::optional<std::string> stringAtPath(const std::string &sectionName, const std::string &path) const;
    std::vector<std::string> stringArrayAtPath(const std::string &sectionName, const std::string &path) const;
    std::optional<std::int64_t> integerAtPath(const std::string &sectionName, const std::string &path) const;
    std::optional<bool> booleanAtPath(const std::string &sectionName, const std::string &path) const;
    std::optional<double> floatingAtPath(const std::string &sectionName, const std::string &path) const;
    std::vector<double> floatingArrayAtPath(const std::string &sectionName, const std::string &path) const;
    // Number of elements of an array at the path (array-of-tables rows or
    // plain arrays); 0 when absent.
    std::size_t arraySizeAtPath(const std::string &sectionName, const std::string &path) const;
    // Key names of the TABLE at the path (dynamic-key sections such as
    // [input.actions]); empty when the path is absent or not a table. This is
    // how a module consumes a section whose keys the assembly authors freely
    // (the keys themselves are data, not schema).
    std::vector<std::string> tableKeysAtPath(const std::string &sectionName, const std::string &path) const;

    // ---- provenance -----------------------------------------------------------------

    // The layer and source that provided the value at the given path, for
    // example provenanceFor("scene", "sun.intensity") after staging the
    // override scene.sun.intensity=2. Arrays are tracked at the array's path,
    // not per element. Empty when the key is absent.
    std::optional<ConfigProvenance> provenanceFor(const std::string &sectionName, const std::string &path) const;

    // One "<path> <- <layer (source)>" line per effective value, sorted by
    // path. Empty string when nothing is loaded.
    std::string provenanceReport() const;

    // ---- known keys -----------------------------------------------------------------

    // Declares one configuration key a component reads, as
    // "<section>.<dotted path>" ("renderer.exposure", "scene.camera.fov").
    // A declaration without a dot ("providers") marks the whole section as
    // known: its keys are dynamic (capability names) and cannot be enumerated.
    // Array-of-tables rows are covered by declaring the array's path
    // ("scene.objects"). Returns false (and declares nothing) when the key is
    // malformed; the kernel reports malformed manifest declarations as
    // startup problems.
    bool declareKnownConfigKey(const std::string &dottedKeyPath);

    // The assembly-owned sections the kernel itself reads:
    // [engine], [providers], [permissions] and [diagnostics].
    void declareAssemblyKnownKeys();

    // One diagnostic per effective key that no component declares and no
    // shipped default owns. Warnings only: they are printed at startup and
    // startup still succeeds, because dead keys are noise, not breakage. Keys that only a
    // shipped defaults file provided are exempt - a package owns the keys it
    // ships.
    std::vector<std::string> unknownKeyWarnings() const;

private:
    const toml::table *sectionTable(const std::string &sectionName) const;

    // [trust] readers behind the four public accessors: one string-array and
    // one boolean lookup inside the trust section. Non-matching value types
    // read as absent (the unknown-key machinery reports a malformed entry); a
    // missing section reads as "nothing trusted" - the safe default.
    std::vector<std::string> trustSectionStringArray(const std::string &key) const;
    bool trustSectionBoolean(const std::string &key) const;

    // One leaf's canonical representation (see flattenedValueReprs).
    static std::string leafValueRepr(const toml::node &value);
    // Recursive walk behind flattenedValueReprs: tables and arrays of tables
    // recurse with dotted/indexed paths, everything else lands as one leaf.
    static void
    flattenNode(const toml::node &subtree, const std::string &path, std::map<std::string, std::string> &out);

    // Deep merge where every source leaf or subtree REPLACES the destination
    // (tables merge recursively, arrays and scalars overwrite wholesale).
    void mergeTableAsOverride(
        toml::table &destination,
        const toml::table &source,
        const std::string &prefix,
        const ConfigProvenance &provenance
    );

    // Deep merge where the source FILLS: keys the destination already has are
    // never touched; recursion continues into tables present on both sides.
    void mergeTableAsDefaults(
        toml::table &destination,
        const toml::table &source,
        const std::string &prefix,
        const ConfigProvenance &provenance
    );

    // Records provenance for every leaf of the subtree (tables recurse;
    // arrays and scalars are leaves), keyed by dotted path.
    void recordProvenance(const toml::node &subtree, const std::string &prefix, const ConfigProvenance &provenance);

    // Drops provenance entries at the path and everywhere below it, used when
    // a higher layer replaces a subtree wholesale.
    void eraseProvenanceUnder(const std::string &prefix);

    toml::table *parsedTable = nullptr;
    std::map<std::string, ConfigProvenance> provenanceByPath;

    struct StagedOverride
    {
        std::vector<std::string> pathSegments;
        // Unique pointer keeps toml++ out of this header (the type is only
        // forward declared here); the destructor instantiates in the .cpp.
        std::unique_ptr<toml::table> parsedValue; // holds the key "value" when isPlainString is false
        bool isPlainString = false;
        std::string plainStringValue;
    };
    std::vector<StagedOverride> stagedOverrides;

    // section name -> set of known dotted key paths ("" = the whole section
    // is known: every key in it is dynamic).
    std::map<std::string, std::set<std::string>> knownKeys;
};

} // namespace engine

#endif // ENGINE_KERNEL_CONFIGURATION_HPP
