#ifndef HARROWMERE_CORE_DATA_REGISTRY_HPP
#define HARROWMERE_CORE_DATA_REGISTRY_HPP

// DataRegistry — task 1.3. ONE const, validated, cross-referenced view of all
// game data; the only thing systems query (card 1.3 goal). It wraps the
// per-content-type loaders of tasks 1.1/1.2 (species, items, classes, crime,
// laws, roles, territory templates, suspicion), runs them in the PINNED
// order, cross-references what they loaded, and exposes typed accessors.
//
// Contracts this service pins:
//
//   * Pinned load order (card 1.3, verbatim):
//       enums -> species -> items -> classes -> crime -> laws -> roles ->
//       territory templates -> suspicion -> event templates (5.1) -> quests (6.4)
//     The last two stages do not exist yet; they join the tail of load() when
//     their owning cards land (the order is part of the seam).
//     The "enums" stage is the closed-list contract itself: COUNT sentinels
//     are pinned at compile time (static_asserts below), the per-file loaders
//     reject unknown tags at runtime.
//
//   * Startup failure is loud and complete: a broken file fails with
//     file + JSON pointer + reason (every loader reports through 0.4's
//     DataError; the per-loader error structs carry the identical field set
//     and are converted here). load() stages everything and commits the
//     snapshot ONLY when zero errors were produced — a failed load leaves
//     "no partial state" (card Done-when): a fresh registry stays empty; a
//     reload keeps serving the previously committed snapshot.
//
//   * Cross-reference errors (the registry's own pass, after all load
//     stages): laws' suppression class ids valid; exemptions' and
//     cultureBias' species ids resolve in the species table; the roles
//     worldgen weights reference exist (the pinned v1 set); every
//     guardMarked item id sane; every territory template's lawSet key
//     resolves (the check territory_templates.hpp promises to this task).
//     Event-template action ids join when 5.1 lands.
//
//   * O(1) hot-path access where the card demands it: species rows live in
//     an id-indexed array (TestRegistry_SpeciesAccessIsO1Array pins the
//     array addressing); crime rows are array-indexed by CrimeType (the
//     loader's CrimeTable); territory templates by TerritoryType.
//
//   * Hot reload is OPTIONAL and ships OFF in v1 (card 1.3: "v1 may ship
//     reload off by default"). The contract for the later watcher: a
//     registry snapshot is immutable once committed; a reload builds a fresh
//     snapshot and swaps BETWEEN sim ticks, never mid-tick — systems may hold
//     const refs across a tick, never across a swap (card pitfall). Nothing
//     in v1 swaps; load() is called once at startup.
//
// Cross-reference error pointers: DataError.jsonPointer stays RFC-6901 for
// everything the per-file loaders report. The registry's OWN cross-ref
// errors use LOGICAL pointers into the file (`/sets/<setKey>/exemptions/42`
// rather than `/sets/2/exemptions/42`): the loader's typed output no longer
// knows array positions, and a set-key pointer is stable under edits and
// directly greppable. DEVIATIONS.md carries the row.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "data/classes.hpp"
#include "data/crime.hpp"
#include "data/items.hpp"
#include "data/json.hpp" // DataError (the canonical error row type)
#include "data/laws.hpp"
#include "data/roles.hpp"
#include "data/species.hpp"
#include "data/territory_templates.hpp"

namespace harrowmere::data
{

// The aggregated startup report type card 1.3's signature names:
// DataRegistry::load(packageDir, DataErrors&).
using DataErrors = std::vector<DataError>;

// The DEFAULT_LAWS key (design reference §Laws: lawsAt's root fallback). The
// laws.json loader hard-requires the set to exist; the registry re-verifies
// (cross-ref X2) because WorldStore's root fallback dereferences it.
inline constexpr std::string_view kDefaultLawSetKey = "default";

class DataRegistry
{
public:
    DataRegistry()                                = default;
    ~DataRegistry()                               = default;
    DataRegistry(const DataRegistry &)            = delete;
    DataRegistry &operator=(const DataRegistry &) = delete;

    // Runs the pinned load-order stages over the files of `packageDir` (the
    // base package's data directory — task 0.4's pinned decision:
    // GameConfig::coreDataDirectory(exeDir) resolves it), then the
    // cross-reference pass. Returns true iff THIS call committed a snapshot
    // (zero errors of any stage). Every failure appends DataError rows with
    // file + pointer + reason. A failed load commits nothing: a fresh
    // registry stays empty, a successful previous snapshot keeps serving.
    bool load(const std::string &packageDir, DataErrors &errors);

    // True iff a committed snapshot is present and serving. After a failed
    // FIRST load this is false (nothing was committed); after a failed
    // RELOAD it stays true for the still-serving previous snapshot (load()
    // returned false either way).
    bool ok() const noexcept
    {
        return committed_;
    }

    explicit operator bool() const noexcept
    {
        return ok();
    }

    // ---- species (card 1.3 pins this accessor verbatim) ----------------------
    //
    // O(1) id-indexed array access (TestRegistry_SpeciesAccessIsO1Array).
    // Debug builds assert presence — asking for an unvalidated speciesId is a
    // programmer error (the cross-ref pass validates every data reference at
    // startup); release builds return a deterministic all-zero row instead of
    // crashing. hasSpecies() is the O(1) presence check.
    const SpeciesData &species(speciesId id) const;
    bool hasSpecies(speciesId id) const noexcept;

    // Id-ascending table view (deterministic iteration, R7).
    const std::vector<SpeciesData> &speciesTable() const noexcept
    {
        return speciesById_;
    }
    std::size_t speciesCount() const noexcept
    {
        return speciesCount_;
    }

    const SpeciesData *speciesByKey(const std::string &key) const;

    // ---- items / classes -------------------------------------------------------
    const std::vector<ItemData> &items() const noexcept
    {
        return items_;
    }
    std::size_t itemCount() const noexcept
    {
        return items_.size();
    }
    const ItemData *itemById(std::uint32_t id) const;
    const ItemData *itemByKey(const std::string &key) const;

    const std::vector<ClassData> &classes() const noexcept
    {
        return classes_;
    }
    std::size_t classCount() const noexcept
    {
        return classes_.size();
    }
    const ClassData *classById(std::uint32_t id) const;
    const ClassData *classByKey(const std::string &key) const;

    // ---- crime -----------------------------------------------------------------
    // Array-indexed by CrimeType (the loader's CrimeTable); complete() was a
    // load requirement, so row() is safe to dereference on a committed
    // snapshot. Prefer crime() for the debug-asserted reference form.
    const CrimeTable &crimeData() const noexcept
    {
        return crime_;
    }
    const CrimeData &crime(CrimeType type) const;

    // ---- laws ------------------------------------------------------------------
    // Keyed by set key ("default", "tolerant_city", ...); deterministic
    // iteration (key-ascending, R7). Territory.laws pointers point INTO these
    // LawSets — they stay valid for the snapshot's lifetime (the swap
    // contract in the header doc).
    const std::map<std::string, LawSet> &lawSets() const noexcept
    {
        return lawSets_;
    }
    std::size_t lawSetCount() const noexcept
    {
        return lawSets_.size();
    }
    const LawSet *lawSet(std::string_view key) const;

    // The DEFAULT_LAWS set — WorldStore's root fallback (lawsAt at parent==0).
    const LawSet &defaultLaws() const;

    // ---- roles -----------------------------------------------------------------
    const std::map<std::string, RoleData> &roles() const noexcept
    {
        return roles_;
    }
    std::size_t roleCount() const noexcept
    {
        return roles_.size();
    }
    const RoleData *role(std::string_view key) const;

    // ---- territory templates / suspicion ---------------------------------------
    // Array-indexed by TerritoryType; complete() was a load requirement.
    const TerritoryTypeTemplate &territoryTemplate(TerritoryType type) const;

    // The whole template set (task 1.5 reads the tree multipliers —
    // kingdoms_per_empire / cities_per_kingdom / villages_per_city — and the
    // regionFlags table, which the per-type accessor above cannot carry).
    const TerritoryTemplateSet &territoryTemplates() const noexcept
    {
        return templates_;
    }

    // The global suspicion trigger table (laws.json sets with an empty
    // `suspicion` block inherit this verbatim; per-LawSet overrides REPLACE
    // it — laws.hpp's pinned semantics).
    const std::vector<SuspicionTrigger> &suspicionTriggers() const noexcept
    {
        return suspicion_;
    }
    std::size_t suspicionCount() const noexcept
    {
        return suspicion_.size();
    }

    // The package directory the committed snapshot was loaded from (task 7.5
    // re-reads it when enumerating mod overrides).
    const std::string &packageDir() const noexcept
    {
        return packageDir_;
    }

private:
    // The staged run of one load() call; commit() moves it into the members.
    struct Staged
    {
        std::vector<SpeciesData> speciesById; // index == speciesId (holes absent)
        std::vector<bool> speciesPresent;
        std::size_t speciesCount = 0;
        std::vector<ItemData> items;
        std::vector<ClassData> classes;
        CrimeTable crime;
        std::map<std::string, LawSet> lawSets;
        std::map<std::string, RoleData> roles;
        TerritoryTemplateSet templates;
        std::vector<SuspicionTrigger> suspicion;
        std::string packageDir;
    };

    // The pinned load-order stages (card 1.3). Each stage converts its
    // loader's error rows into DataErrors and reports whether it loaded
    // clean; the cross-reference pass consults the flags so a broken
    // upstream file suppresses (never fakes) the checks that read it.
    bool loadSpeciesStage(Staged &staged, const std::string &path, DataErrors &errors) const;
    bool loadItemsStage(Staged &staged, const std::string &path, DataErrors &errors) const;
    bool loadClassesStage(Staged &staged, const std::string &path, DataErrors &errors) const;
    bool loadCrimeStage(Staged &staged, const std::string &path, DataErrors &errors) const;
    bool loadLawsStage(Staged &staged, const std::string &path, DataErrors &errors) const;
    bool loadRolesStage(Staged &staged, const std::string &path, DataErrors &errors) const;
    bool loadTemplatesStage(Staged &staged, const std::string &path, DataErrors &errors) const;
    bool loadSuspicionStage(Staged &staged, const std::string &path, DataErrors &errors) const;

    // The cross-reference pass (runs after ALL load stages, per card 1.3).
    // Only reads stages that reported clean — a file that failed its own
    // load must not produce a hail of derived cross-ref errors.
    void crossReference(
        const Staged &staged,
        bool speciesClean,
        bool itemsClean,
        bool crimeClean,
        bool lawsClean,
        bool rolesClean,
        bool templatesClean,
        DataErrors &errors
    ) const;

    void commit(Staged &&staged);

    // ---- the committed snapshot (const-only through the public surface) ----
    std::vector<SpeciesData> speciesById_; // O(1): index == speciesId
    std::vector<bool> speciesPresent_;
    std::size_t speciesCount_ = 0;
    std::vector<ItemData> items_;
    std::vector<ClassData> classes_;
    CrimeTable crime_;
    std::map<std::string, LawSet> lawSets_;
    std::map<std::string, RoleData> roles_;
    TerritoryTemplateSet templates_;
    std::vector<SuspicionTrigger> suspicion_;
    std::string packageDir_;
    bool committed_ = false;
};

} // namespace harrowmere::data

#endif // HARROWMERE_CORE_DATA_REGISTRY_HPP
