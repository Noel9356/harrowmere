// registry.cpp — DataRegistry implementation (task 1.3).
//
// The load pipeline, in the card's pinned order, with the staged-commit
// contract from the header doc: every stage loads into `Staged`, every error
// is reported as a DataError (file + pointer + reason), and the snapshot is
// committed only when the whole run produced zero errors.

#include "data/registry.hpp"

#include <cassert>
#include <cstddef>
#include <filesystem>
#include <utility> // std::move

#include "core/log.hpp"

namespace harrowmere::data
{

namespace
{

namespace fs = std::filesystem;

// Stage 0 — the closed enum registries (task 1.1). The COUNT sentinels the
// sized tables below are indexed by are compile-time facts; pinning them
// here makes the "enums" stage of the pinned load order explicit and keeps
// the array-backed accessors honest (R9). Runtime unknown-tag rejection is
// each loader's job (it has the JSON pointer at hand).
static_assert(static_cast<std::size_t>(CrimeType::COUNT) == CrimeTable::kSize, "CrimeType::COUNT backs CrimeTable");
static_assert(
    static_cast<std::size_t>(TerritoryType::COUNT) == static_cast<std::size_t>(TerritoryType::EMPIRE) + 1,
    "TerritoryType::COUNT closes the territory template array"
);
static_assert(static_cast<std::size_t>(StatType::COUNT) > 0, "StatType::COUNT backs the stat-multiplier arrays");
static_assert(static_cast<std::size_t>(SkillType::COUNT) > 0, "SkillType::COUNT backs the skill-weight arrays");

// The pinned per-stage file names (tasks 1.1/1.2's shipped data set).
constexpr const char *kSpeciesFile   = "species.json";
constexpr const char *kItemsFile     = "items.json";
constexpr const char *kClassesFile   = "classes.json";
constexpr const char *kCrimeFile     = "crime_data.json";
constexpr const char *kLawsFile      = "laws.json";
constexpr const char *kRolesFile     = "roles.json";
constexpr const char *kTemplatesFile = "territory_templates.json";
constexpr const char *kSuspicionFile = "suspicion.json";

// SocialClassId spellings for cross-ref diagnostics (the loader's tag set —
// laws.cpp's socialClassFromKey — mirrored for the reverse direction).
std::string_view socialClassName(
    SocialClassId cls
) noexcept
{
    switch (cls)
    {
    case SocialClassId::NONE:
        return "NONE";
    case SocialClassId::COMMONER:
        return "COMMONER";
    case SocialClassId::NOBLE:
        return "NOBLE";
    case SocialClassId::ROYAL:
        return "ROYAL";
    case SocialClassId::COUNT:
        break;
    }
    return "<invalid>";
}

// The role keys worldgen's pinned population weights reference (card 1.3:
// "roles referenced by worldgen exist" — worldgen draws its per-NPC roles
// from the pinned v1 set card 1.2 ships; a missing key breaks the spawn
// plan, so it is a startup error, not a runtime surprise).
constexpr const char *const kWorldgenRoles[] = {
    "captain",
    "guard",
    "militia_veteran",
    "vendor",
    "healer",
    "noble",
    "farmer",
    "laborer",
    "child",
};

} // namespace

// ---------------------------------------------------------------------------
// The pinned load-order stages
// ---------------------------------------------------------------------------

bool DataRegistry::loadSpeciesStage(
    Staged &staged, const std::string &path, DataErrors &errors
) const
{
    SpeciesLoader loader;
    if (!loader.load(path, errors))
        return false;

    // Expand the loader's row list into the id-indexed array (holes absent —
    // ids are data-assigned and a mod may remove a row; card 1.3's O(1)
    // access contract is index == speciesId).
    std::size_t maxId = 0;
    for (const auto &row : loader.all())
        maxId = row.id > maxId ? row.id : maxId;
    staged.speciesById.assign(maxId + 1, SpeciesData{});
    staged.speciesPresent.assign(maxId + 1, false);
    for (const auto &row : loader.all())
    {
        staged.speciesById[row.id]    = row;
        staged.speciesPresent[row.id] = true;
    }
    staged.speciesCount = loader.all().size();
    return true;
}

bool DataRegistry::loadItemsStage(
    Staged &staged, const std::string &path, DataErrors &errors
) const
{
    ItemsLoader loader;
    if (!loader.load(path, errors))
        return false;
    staged.items = loader.all();
    return true;
}

bool DataRegistry::loadClassesStage(
    Staged &staged, const std::string &path, DataErrors &errors
) const
{
    ClassesLoader loader;
    if (!loader.load(path, errors))
        return false;
    staged.classes = loader.all();
    return true;
}

bool DataRegistry::loadCrimeStage(
    Staged &staged, const std::string &path, DataErrors &errors
) const
{
    // The loader's error row type carries the identical field set as
    // DataError (file/jsonPointer/message) — convert row by row.
    std::vector<CrimeDataError> stageErrors;
    if (!loadCrimeData(path, staged.crime, stageErrors))
    {
        for (const auto &e : stageErrors)
            errors.push_back(DataError{e.file, e.jsonPointer, e.message});
        return false;
    }
    return true;
}

bool DataRegistry::loadLawsStage(
    Staged &staged, const std::string &path, DataErrors &errors
) const
{
    LawSetRegistry registry;
    std::vector<LawDataError> stageErrors;
    if (!loadLawSets(path, registry, stageErrors))
    {
        for (const auto &e : stageErrors)
            errors.push_back(DataError{e.file, e.jsonPointer, e.message});
        return false;
    }
    staged.lawSets = registry.sets();
    return true;
}

bool DataRegistry::loadRolesStage(
    Staged &staged, const std::string &path, DataErrors &errors
) const
{
    RoleRegistry registry;
    std::vector<RoleDataError> stageErrors;
    if (!loadRoles(path, registry, stageErrors))
    {
        for (const auto &e : stageErrors)
            errors.push_back(DataError{e.file, e.jsonPointer, e.message});
        return false;
    }
    staged.roles = registry.roles();
    return true;
}

bool DataRegistry::loadTemplatesStage(
    Staged &staged, const std::string &path, DataErrors &errors
) const
{
    TerritoryTemplateSet templates;
    std::vector<TerritoryDataError> stageErrors;
    if (!loadTerritoryTemplates(path, templates, stageErrors))
    {
        for (const auto &e : stageErrors)
            errors.push_back(DataError{e.file, e.jsonPointer, e.message});
        return false;
    }
    staged.templates = std::move(templates);
    return true;
}

bool DataRegistry::loadSuspicionStage(
    Staged &staged, const std::string &path, DataErrors &errors
) const
{
    std::vector<SuspicionTrigger> triggers;
    std::vector<LawDataError> stageErrors;
    if (!loadSuspicionTriggers(path, triggers, stageErrors))
    {
        for (const auto &e : stageErrors)
            errors.push_back(DataError{e.file, e.jsonPointer, e.message});
        return false;
    }
    staged.suspicion = std::move(triggers);
    return true;
}

// ---------------------------------------------------------------------------
// The cross-reference pass (card 1.3, after ALL load stages)
// ---------------------------------------------------------------------------

void DataRegistry::crossReference(
    const Staged &staged,
    bool speciesClean,
    bool itemsClean,
    bool crimeClean,
    bool lawsClean,
    bool rolesClean,
    bool templatesClean,
    DataErrors &errors
) const
{
    // X1 — the crime table covers every CrimeType (one row per crime; the
    // closed list from task 1.1). caseScore/issueWarrant index it raw.
    if (crimeClean && !staged.crime.complete())
        errors.push_back(
            DataError{
                staged.packageDir + "/" + kCrimeFile,
                "",
                "crime table incomplete: every CrimeType needs exactly one row"
            }
        );

    // X2 — DEFAULT_LAWS exists: WorldStore::lawsAt's root fallback
    // dereferences it (design reference §Laws).
    if (lawsClean && staged.lawSets.find(std::string(kDefaultLawSetKey)) == staged.lawSets.end())
        errors.push_back(
            DataError{
                staged.packageDir + "/" + kLawsFile,
                "",
                "laws.json must define the '" + std::string(kDefaultLawSetKey) + "' set (DEFAULT_LAWS)",
            }
        );

    // X3 — species id 0 is the one hardcoded constant (the player species,
    // design reference §Conventions); the sim references it directly.
    if (speciesClean
        && (staged.speciesPresent.empty() || !staged.speciesPresent[0] || staged.speciesById[0].key != "player"))
        errors.push_back(
            DataError{
                staged.packageDir + "/" + kSpeciesFile,
                "",
                "species id 0 (\"player\") is the hardcoded player species and must be present"
            }
        );

    // X4 — the roles worldgen's pinned weights reference exist.
    if (rolesClean)
    {
        for (const char *required : kWorldgenRoles)
        {
            if (staged.roles.find(required) == staged.roles.end())
                errors.push_back(
                    DataError{
                        staged.packageDir + "/" + kRolesFile,
                        "",
                        std::string("role '") + required + "' is missing (worldgen's pinned role weights reference it)",
                    }
                );
        }
    }

    if (!lawsClean)
        return; // the checks below read law sets; none loaded clean

    // X5 — suppression class ids valid (card 1.3 verbatim). The loader maps
    // tag strings to SocialClassId and rejects unknown tags; the registry
    // re-verifies the closed range defensively (D-CORRUPT-1: the map is
    // keyed by SocialClassId, not by classes.json's ClassData).
    for (const auto &[key, set] : staged.lawSets)
    {
        for (const auto &[cls, damping] : set.suppression)
        {
            const auto raw = static_cast<unsigned>(cls);
            if (raw >= static_cast<unsigned>(SocialClassId::COUNT))
                errors.push_back(
                    DataError{
                        staged.packageDir + "/" + kLawsFile,
                        "/sets/" + key + "/suppression",
                        "suppression class id outside the closed SocialClassId range ("
                            + std::string(socialClassName(cls)) + ")",
                    }
                );
        }

        // X6 — exemption and cultureBias species ids resolve in the species
        // table (card 1.3 verbatim for exemptions; cultureBias is the same
        // reference shape and feeds 3.9's affinity). A dangling species id
        // here would silently void the exemption/bias at runtime.
        if (!speciesClean)
            continue; // species did not load: skip, never fake this check
        for (const auto &[species, exempt] : set.exemptions)
        {
            (void)exempt;
            if (species >= staged.speciesPresent.size() || !staged.speciesPresent[species])
                errors.push_back(
                    DataError{
                        staged.packageDir + "/" + kLawsFile,
                        "/sets/" + key + "/exemptions/" + std::to_string(species),
                        "exemption references speciesId " + std::to_string(species)
                            + " which species.json does not define",
                    }
                );
        }
        for (const auto &[species, bias] : set.cultureBias)
        {
            (void)bias;
            if (species >= staged.speciesPresent.size() || !staged.speciesPresent[species])
                errors.push_back(
                    DataError{
                        staged.packageDir + "/" + kLawsFile,
                        "/sets/" + key + "/cultureBias/" + std::to_string(species),
                        "cultureBias references speciesId " + std::to_string(species)
                            + " which species.json does not define",
                    }
                );
        }
    }

    // X7 — every territory template's lawSet key resolves (the check
    // territory_templates.hpp defers to this task). Needs BOTH stages clean:
    // a failed laws stage must not produce a hail of derived lawSet errors.
    if (templatesClean && lawsClean)
    {
        for (std::size_t i = 0; i < static_cast<std::size_t>(TerritoryType::COUNT); ++i)
        {
            const auto type         = static_cast<TerritoryType>(i);
            const auto *templateRow = staged.templates.byType(type);
            assert(templateRow != nullptr && "templates stage reported clean but a type row is missing");
            if (templateRow == nullptr)
                continue;
            const std::string &lawKey = templateRow->lawSet;
            if (staged.lawSets.find(lawKey) == staged.lawSets.end())
                errors.push_back(
                    DataError{
                        staged.packageDir + "/" + kTemplatesFile,
                        "/types/" + std::string(territoryTypeKey(type)) + "/lawSet",
                        "territory template references law set '" + lawKey + "' which laws.json does not define",
                    }
                );
        }
    }

    // X8 — every guardMarked item id sane (card 1.3 verbatim): the justice
    // hook (2.4/4.6) flags theft evidence BY item id, so a marked row with
    // the reserved id 0 would poison the evidence path. 0 is reserved in
    // every id space (core/types.hpp).
    if (itemsClean)
    {
        for (std::size_t i = 0; i < staged.items.size(); ++i)
        {
            const auto &item = staged.items[i];
            if (item.guardMarked && item.id == 0)
                errors.push_back(
                    DataError{
                        staged.packageDir + "/" + kItemsFile,
                        "/" + std::to_string(i) + "/id",
                        "guardMarked item carries the reserved id 0 (ids are opaque and non-zero, R10)",
                    }
                );
        }
    }
}

// ---------------------------------------------------------------------------
// load + commit
// ---------------------------------------------------------------------------

bool DataRegistry::load(
    const std::string &packageDir, DataErrors &errors
)
{
    // `errors` is THIS load's report (reset at entry); the return value says
    // whether this call committed.
    errors.clear();

    Staged staged;
    staged.packageDir = packageDir;
    const fs::path base(packageDir);

    // The pinned load order (card 1.3): enums -> species -> items -> classes
    // -> crime -> laws -> roles -> territory templates -> suspicion. The
    // "enums" stage is the static_assert contract above. Event templates
    // (5.1) and quests (6.4) extend the tail when their cards land.
    const bool speciesClean   = loadSpeciesStage(staged, (base / kSpeciesFile).string(), errors);
    const bool itemsClean     = loadItemsStage(staged, (base / kItemsFile).string(), errors);
    const bool classesClean   = loadClassesStage(staged, (base / kClassesFile).string(), errors);
    const bool crimeClean     = loadCrimeStage(staged, (base / kCrimeFile).string(), errors);
    const bool lawsClean      = loadLawsStage(staged, (base / kLawsFile).string(), errors);
    const bool rolesClean     = loadRolesStage(staged, (base / kRolesFile).string(), errors);
    const bool templatesClean = loadTemplatesStage(staged, (base / kTemplatesFile).string(), errors);
    const bool suspicionClean = loadSuspicionStage(staged, (base / kSuspicionFile).string(), errors);
    (void)classesClean;   // no cross-ref reads classes yet (worldgen 1.5 does)
    (void)suspicionClean; // the global table needs no cross-ref (loader-validated)

    crossReference(staged, speciesClean, itemsClean, crimeClean, lawsClean, rolesClean, templatesClean, errors);

    if (!errors.empty())
    {
        // No partial state: a fresh registry stays empty; a previously
        // committed snapshot keeps serving (ok() reflects commits, and the
        // caller sees the failure through the return value + report).
        return false;
    }

    commit(std::move(staged));

    // System-entry event (task 0.5 rule 3): the decisive numbers inline.
    HARROWMERE_LOG_INFO(
        sim,
        "data registry committed: %llu species, %llu items, %llu classes, %llu crime rows, %llu law sets, %llu roles, "
        "%llu territory templates, %llu suspicion rows",
        static_cast<unsigned long long>(speciesCount_),
        static_cast<unsigned long long>(items_.size()),
        static_cast<unsigned long long>(classes_.size()),
        static_cast<unsigned long long>(static_cast<std::size_t>(CrimeType::COUNT)),
        static_cast<unsigned long long>(lawSets_.size()),
        static_cast<unsigned long long>(roles_.size()),
        static_cast<unsigned long long>(static_cast<std::size_t>(TerritoryType::COUNT)),
        static_cast<unsigned long long>(suspicion_.size())
    );
    return true;
}

void DataRegistry::commit(
    Staged &&staged
)
{
    speciesById_    = std::move(staged.speciesById);
    speciesPresent_ = std::move(staged.speciesPresent);
    speciesCount_   = staged.speciesCount;
    items_          = std::move(staged.items);
    classes_        = std::move(staged.classes);
    crime_          = std::move(staged.crime);
    lawSets_        = std::move(staged.lawSets);
    roles_          = std::move(staged.roles);
    templates_      = std::move(staged.templates);
    suspicion_      = std::move(staged.suspicion);
    packageDir_     = std::move(staged.packageDir);
    committed_      = true;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

bool DataRegistry::hasSpecies(
    speciesId id
) const noexcept
{
    return id < speciesPresent_.size() && speciesPresent_[id];
}

const SpeciesData &DataRegistry::species(
    speciesId id
) const
{
    // O(1) array access (card 1.3, TestRegistry_SpeciesAccessIsO1Array): the
    // row lives at speciesById_[id] — no scan, no map. An unknown id is a
    // programmer error on a committed snapshot (every data reference was
    // validated at load): assert in debug, serve the deterministic zero row
    // in release.
    static const SpeciesData invalid{};
    const bool present = hasSpecies(id);
    assert(present && "DataRegistry::species: unknown speciesId (validate at load or hasSpecies() first)");
    return present ? speciesById_[id] : invalid;
}

const SpeciesData *DataRegistry::speciesByKey(
    const std::string &key
) const
{
    if (key.empty())
        return nullptr; // a hole row's empty key is never a match target
    for (const auto &row : speciesById_)
        if (row.key == key)
            return &row;
    return nullptr;
}

const ItemData *DataRegistry::itemById(
    std::uint32_t id
) const
{
    for (const auto &row : items_)
        if (row.id == id)
            return &row;
    return nullptr;
}

const ItemData *DataRegistry::itemByKey(
    const std::string &key
) const
{
    for (const auto &row : items_)
        if (row.key == key)
            return &row;
    return nullptr;
}

const ClassData *DataRegistry::classById(
    std::uint32_t id
) const
{
    for (const auto &row : classes_)
        if (row.id == id)
            return &row;
    return nullptr;
}

const ClassData *DataRegistry::classByKey(
    const std::string &key
) const
{
    for (const auto &row : classes_)
        if (row.key == key)
            return &row;
    return nullptr;
}

const CrimeData &DataRegistry::crime(
    CrimeType type
) const
{
    const CrimeData *row = crime_.row(type);
    assert(row != nullptr && "DataRegistry::crime: missing row (committed snapshots are complete)");
    static const CrimeData invalid{};
    return row != nullptr ? *row : invalid;
}

const LawSet *DataRegistry::lawSet(
    std::string_view key
) const
{
    const auto it = lawSets_.find(std::string(key));
    return it == lawSets_.end() ? nullptr : &it->second;
}

const LawSet &DataRegistry::defaultLaws() const
{
    const LawSet *set = lawSet(kDefaultLawSetKey);
    assert(set != nullptr && "DataRegistry::defaultLaws: no committed snapshot (load() must succeed first)");
    static const LawSet empty{};
    return set != nullptr ? *set : empty;
}

const RoleData *DataRegistry::role(
    std::string_view key
) const
{
    const auto it = roles_.find(std::string(key));
    return it == roles_.end() ? nullptr : &it->second;
}

const TerritoryTypeTemplate &DataRegistry::territoryTemplate(
    TerritoryType type
) const
{
    const TerritoryTypeTemplate *row = templates_.byType(type);
    assert(row != nullptr && "DataRegistry::territoryTemplate: missing row (committed snapshots are complete)");
    static const TerritoryTypeTemplate invalid{};
    return row != nullptr ? *row : invalid;
}

} // namespace harrowmere::data
