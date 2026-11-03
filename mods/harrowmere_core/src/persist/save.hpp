#ifndef HARROWMERE_CORE_PERSIST_SAVE_HPP
#define HARROWMERE_CORE_PERSIST_SAVE_HPP

// persist/save.hpp — task 7.1's save/load surface: ONE versioned JSON
// snapshot of the entire simulation, atomic on write, roundtrip-exact on
// load, refusing anything that does not validate cleanly.
//
// The card's rules, as implemented across save.cpp (serialize + atomic
// write), load.cpp (strict validate + apply) and state_hash.{hpp,cpp} (the
// canonical hasher the roundtrip test and the 8.3 soak failure dumps share):
//
//   (1) Atomic write — serialize to <path>.tmp, std::filesystem::rename over
//       the target (crash-safe on POSIX/NTFS). The tmp never survives a
//       completed call; a crashed write leaves the previous save untouched.
//   (2) What is not saved: cachedPower + powerDirty (recomputed on load —
//       every character restores powerDirty = true, the design reference's
//       contract), the spatial grid (rebuilt from positions), NpcState.tier
//       and the per-tick NPC fields (needsCarry, nextDutyWorldTime,
//       lastMigrationRollWorldTime, currentIntent, migrating,
//       migrationTarget — the do-not-save list on npc/state.hpp), the
//       CorpseStore pending_ list (same-tick staging, empty at every save
//       point), the CombatQueue pending requests (steps 9->10 scratch).
//       Transient EVENT state IS saved (spawned targets, granted tiers, the
//       price/weather biases — 5.5's world-state list).
//   (3) Deterministic order everywhere (R7): nlohmann's object type is
//       std::map (keys sorted), arrays serialize in map/insertion order.
//   (4) RNG stream states saved (0.3): a loaded world continues its exact
//       random sequence. readSaveFile reseeds from the save's seed before
//       applying the stream states, so the caller needs no pre-seeding.
//   (5) peakPower / totalXp / notoriety / lastDecayTime are the anti-abuse
//       fields — bit-exact through every roundtrip (the pinned test).
//   (6) Version policy: a `version` mismatch refuses with a clear message.
//       No migration in v1; old files stay untouched.
//
// Every array's schema is documented in persist/README.md (the Done-when's
// "self-describing" requirement — the README is the schema's home).

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/types.hpp" // characterId, regionId, territoryId, goodId, Position
#include "data/json.hpp"  // data::Json (the one nlohmann door, D-DATA-1)

namespace harrowmere
{
class NpcStore;
class EconomyStore;
class CorpseStore;
class RoleRegistry;
class CombatQueue;

namespace combat
{
class ConsentRegistry;
}

namespace character
{
class CharacterStore; // character/store.hpp (D-STORE-1's unified store)
class AppearanceHistory;
} // namespace character
namespace items
{
class ItemInstanceRegistry;
class InventoryStore;
} // namespace items
namespace world
{
class WorldStore;
}
namespace justice
{
class JusticeStore;
}
namespace events
{
class EventEngine;
}
namespace factions
{
class FactionStore;
class WarStore;
class ClaimStore;
} // namespace factions
namespace core
{
class WorldClock;
class SeedRng;
} // namespace core
namespace data
{
class DataRegistry;
}
} // namespace harrowmere

namespace harrowmere::persist
{

// The save format tag + version (7.1's version policy; an engineering format
// label, not a game-rule constant — the plan pins version 1 for v1 and the
// field is the migration seam Appendix E reserves).
inline constexpr const char *SAVE_FORMAT_ID        = "game-save";
inline constexpr std::uint32_t SAVE_FORMAT_VERSION = 1;

// The per-(event, actor) snapshot cadence book (5.3's ContributionTickSystem
// state; spelled out here so this header stays store-implementation-free).
using ContributionSnapshotBook = std::map<std::uint64_t, std::map<characterId, std::uint64_t>>;

// ---- the runtime pacing gates + plugin-side stamps ---------------------------
//
// The stores are the simulation; the step systems' lazy cadence stamps and
// the plugin's day gates are the runtime's bookkeeping AROUND them. They ride
// the save's "runtime" block so a loaded world resumes every cadence exactly
// (no extra recompute/evaluation on the load tick). They are NOT part of the
// canonical state hash (pacing, not world state — the hash covers the stores).
struct RuntimeState
{
    std::uint64_t tickNumber           = 0;     // the view counter (7.2P1's tick display)
    std::uint64_t gridLastSweepDay     = 0;     // GridSyncSystem's day gate (step 11 tail)
    std::uint64_t justiceSweepLastDay  = 0;     // DayGateJusticeSweepSystem's gate (step 1)
    std::uint64_t weatherPrevWorldTime = 0;     // WeatherStepSystem's roll anchor (step 2)
    bool economyStarted                = false; // EconomySystem's lazy hour/day gates (step 7)
    std::uint64_t economyLastHour      = 0;
    std::uint64_t economyLastDay       = 0;
    std::uint64_t suspicionLastRunHour = 0;          // SuspicionTickSystem's A-76-style cadence (step 6)
    ContributionSnapshotBook contributionSnapshotAt; // 5.3's cadence book
    std::uint64_t contributionNextWalkAt = 0;        // the walk's cadence gate (the 8.3 as-built)
    std::set<std::uint64_t> enforcementResponded;    // 4.6's signal-before-force memory
    Position playerPosition{};                       // the controller's transform (the sim-side truth)
    bool playerPositionValid = false;                // false = no player row (pre-creation saves)
};

// ---- save --------------------------------------------------------------------

// Every store the snapshot covers, by const pointer. All pointers are
// required (a null store is a wiring bug — buildSave refuses with a clear
// error naming the missing member). The economy goods table and the event
// template table are NOT sources: they are boot content, reloaded from the
// package before a save applies.
struct SaveSources
{
    std::uint64_t seed = 0;

    const core::WorldClock *clock                    = nullptr;
    const core::SeedRng *rng                         = nullptr;
    const character::CharacterStore *characters      = nullptr;
    const NpcStore *npcs                             = nullptr;
    const world::WorldStore *world                   = nullptr;
    const justice::JusticeStore *justice             = nullptr;
    const events::EventEngine *events                = nullptr;
    const EconomyStore *economy                      = nullptr;
    const factions::FactionStore *factions           = nullptr;
    const factions::WarStore *wars                   = nullptr;
    const factions::ClaimStore *claims               = nullptr;
    const CorpseStore *corpses                       = nullptr;
    const items::ItemInstanceRegistry *itemInstances = nullptr;
    const items::InventoryStore *bags                = nullptr;
    const character::AppearanceHistory *appearance   = nullptr;
    const RoleRegistry *roles                        = nullptr;
    const combat::ConsentRegistry *consents          = nullptr;
    const CombatQueue *combatQueue                   = nullptr;
    const RuntimeState *runtime                      = nullptr; // optional: nullptr omits the block
};

// The whole document as JSON (deterministic: sorted object keys, map-ordered
// arrays). Exposed for the tests and the module README's examples.
[[nodiscard]] data::Json buildSave(const SaveSources &sources);

// Atomic write: buildSave -> <path>.tmp -> rename over <path>. Returns false
// with `error` set on any IO failure; the target file is never partially
// written and a stale .tmp never survives a completed call.
bool writeSaveFile(const std::string &path, const SaveSources &sources, std::string &error);

// ---- load --------------------------------------------------------------------

// The restore targets. `registry` resolves the territories' law-set keys
// against the committed content (the load precondition: the registry is
// loaded and the economy goods table registered — content, not state).
// `runtime` is optional (nullptr = the block is skipped when present).
struct LoadTargets
{
    // Outputs: the world seed the save was taken from (the RNG reseed uses
    // it; the caller may log it).
    std::uint64_t seed = 0;

    core::WorldClock *clock                    = nullptr;
    core::SeedRng *rng                         = nullptr;
    character::CharacterStore *characters      = nullptr;
    NpcStore *npcs                             = nullptr;
    world::WorldStore *world                   = nullptr;
    justice::JusticeStore *justice             = nullptr;
    events::EventEngine *events                = nullptr;
    EconomyStore *economy                      = nullptr;
    factions::FactionStore *factions           = nullptr;
    factions::WarStore *wars                   = nullptr;
    factions::ClaimStore *claims               = nullptr;
    CorpseStore *corpses                       = nullptr;
    items::ItemInstanceRegistry *itemInstances = nullptr;
    items::InventoryStore *bags                = nullptr;
    character::AppearanceHistory *appearance   = nullptr;
    RoleRegistry *roles                        = nullptr;
    combat::ConsentRegistry *consents          = nullptr;
    CombatQueue *combatQueue                   = nullptr;
    const data::DataRegistry *registry         = nullptr;
    RuntimeState *runtime                      = nullptr; // optional
    // Non-fatal content notes (an unknown law-set key resolves to the
    // inherit walk; the reason lands here). Refusals come back through the
    // error out-param of readSaveFile, never through this list.
    std::vector<std::string> warnings;
};

// Reads, validates, and applies. Strict door to door: the file parses
// through data::loadJsonFile (duplicates rejected), the format tag and
// version are checked first, every section validates into typed snapshots
// BEFORE the first store write — a refusal leaves every target store
// untouched (no partial load; the 8.2 pitfall's rule). Returns false with
// `error` = "<file>: <json pointer>: <reason>" on any refusal.
bool readSaveFile(const std::string &path, LoadTargets &targets, std::string &error);

// Does a save file exist at `path` (the plugin's load-on-start probe)?
bool saveFileExists(const std::string &path);

} // namespace harrowmere::persist

#endif // HARROWMERE_CORE_PERSIST_SAVE_HPP
