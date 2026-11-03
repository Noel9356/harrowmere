#ifndef HARROWMERE_CORE_CHARACTER_STORE_HPP
#define HARROWMERE_CORE_CHARACTER_STORE_HPP

// store.hpp - CharacterStore (task 2.0): ONE storage truth for every person
// in the world.
//
// D-STORE-1: one unified store + a playerIds_ set, instead of separate
// player/NPC maps. The design docs read like two maps, but every doc
// structure is shared ("NPC actors share the same structure"); the set
// preserves the "is this a player" queries (recordCrime's culprit check,
// victim self-report, arrest targets - Phase 3) with one storage truth.
// NPC-only state (needs, role, brain, LOD tier, relationships, name, age)
// lives in NpcState (task 3.4), never here and never on Character.
//
// Determinism (R7): the character map is std::map keyed by the opaque
// characterId, so all() iterates ids ascending - deterministic everywhere
// the simulation or a view snapshot can observe the order. No unordered
// container anywhere in the store.
//
// Id spaces (R10): createNpc honors the seed's PRE-ASSIGNED plan id
// (worldgen 1.5 drew plan ids in strict plan order and hands the runtime the
// counter value to adopt - WorldGenResult::nextCharacterId, delivered via
// adoptWorldGen). Post-plan spawns (task 3.4's births, task 3.6's
// spawnBackupNpc) build a NpcSeed with id 0 and the store DRAWS the next id
// from the adopted counter, so the id sequence continues the plan's and ids
// are never reused. 0 itself is reserved in the character space and is
// never issued.
//
// Release-mode recovery (3.4's rule-level-vs-programmer-error split): get()
// on an unknown id is a programmer error - assert in debug; in release log
// an ERROR and serve the store's deterministic zero character (id 0,
// powerDirty true), the same pattern DataRegistry::species() pins. Creation
// collisions (a player already existing in SP; a seed id already used) are
// programmer errors too: assert in debug, no-op deterministically in
// release.

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>

#include "character/character.hpp" // Character, PlayerCreation
#include "core/types.hpp"          // characterId, Position, IdGenerator (task 0.2)
#include "world/worldgen.hpp"      // NpcSeed (task 1.5's population plan)

namespace harrowmere::character
{

// The card's unqualified spelling of the plan-NPC record (task 1.5).
using world::NpcSeed;

class CharacterStore
{
public:
    CharacterStore() = default;

    // New-game wiring, called ONCE from the plugin's start() after worldgen:
    // adopt worldgen's character-id counter (WorldGenResult::nextCharacterId
    // - ids the plan already consumed are never reissued) and the safe-zone
    // spawn point (WorldGenResult::playerSpawn - worldgen stage 4 pins it to
    // the beginner kingdom's safe village; the design rule "safe zones are
    // spawn points" is worldgen's guarantee, the store records the resolved
    // point verbatim and stays world-free). Debug-asserts that no character
    // exists yet - adoption happens before the first create, never mid-run.
    void adoptWorldGen(std::uint64_t nextCharacterId, const Position &playerSpawn);

    // Create the SP player: species 0 (the hardcoded "player" row), level 1,
    // all stats/skills at the doc floor 0, totalXp = 0, availableXp = 0,
    // currency = 100 (A-41 starter purse), maxHp = BASE_HP + 1 * HP_PER_LEVEL
    // (A-42/A-43) and currentHp = maxHp, spawned at the safe-zone spawn
    // point, powerDirty = true. SP has exactly one player: a second call is
    // a programmer error (assert in debug; release returns the existing
    // player's id unchanged).
    characterId createPlayer(const PlayerCreation &creation);

    // Create one NPC from the population plan (task 3.4's spawn path calls
    // this per NpcSeed): species, level and the seed's pre-assigned plan id
    // carry over; maxHp derives from the seed level; stats/skills/xp sit at
    // the doc floor; currency starts at 0 (no starter purse is pinned for
    // NPCs - earnings arrive with roles/economy, 3.4/3.7); body.position is
    // NOT set here (placement is 3.4's spawn policy, home settlement for FAR
    // NPCs). Age and role stay on the seed - they are NpcState fields
    // (D-STORE-1); 3.4 attaches the NpcState record by the id this call
    // returns. A seed with id 0 is a post-plan spawn (birth/backup): the
    // store draws the next id. A seed id already in use is a programmer
    // error (assert in debug; release returns the existing id unchanged).
    characterId createNpc(const NpcSeed &seed);

    // The character record; asserts existence (programmer error otherwise -
    // every caller validated the id or put it there). Release: logs an ERROR
    // and serves the deterministic zero character (never a crash, never a
    // random record).
    Character &get(characterId id);
    const Character &get(characterId id) const;

    // The single-lookup probe for hot loops that would otherwise pay
    // exists() + get() as two tree walks (the 8.3 soak's step profile: the
    // contribution snapshot refresh re-found every battle participant twice
    // per refresh). nullptr = no such living record. Read/write view over
    // the same map get() serves — the cache-freshness contract rides it.
    Character *find(characterId id) noexcept;
    const Character *find(characterId id) const noexcept;

    // The playerIds_ query (D-STORE-1): exactly the ids createPlayer issued.
    bool isPlayer(characterId id) const noexcept;

    bool exists(characterId id) const noexcept;

    // Remove one character (3.4's natural-death silent removal, 3.3's post-loot
    // cleanup). The parallel NpcState record is the caller's to erase (3.4 owns
    // that pairing). A missing id is a programmer error (debug assert; release
    // logs and returns false — never a crash).
    bool remove(characterId id);

    // The SP player's id (0 = no player in-world - main menu, pre-creation).
    // With more than one player registered (never in SP), the lowest id wins
    // deterministically (R7).
    characterId playerId() const noexcept;
    std::size_t playerCount() const noexcept;
    std::size_t population() const noexcept;

    // Deterministic iteration over every character, id-ascending (R7).
    std::map<characterId, Character> &all() noexcept;
    const std::map<characterId, Character> &all() const noexcept;

    // The SP player's creation record (name / appearance / class preset),
    // recorded verbatim at createPlayer. Debug-asserts a player exists;
    // release serves a deterministic empty record.
    const PlayerCreation &playerCreation() const;

    // ---- save seams (task 7.1; persist/ serializes them verbatim) --------

    // The character-id counter's next-to-issue value (IdGenerator "character"
    // space; the save's ids.character key).
    [[nodiscard]] std::uint64_t nextCharacterId() const noexcept
    {
        return ids_.peek("character");
    }

    // The safe-zone spawn point the store was adopted with (worldgen stage 4
    // on a new game; the save's world.playerSpawn key — D-WGEN-1: loading a
    // save never re-generates, so the point rides the save).
    [[nodiscard]] const Position &playerSpawn() const noexcept
    {
        return playerSpawn_;
    }

    // Restore one saved character verbatim (the load path; ids are NOT drawn
    // — the counter is adopted separately so restore order is free). A
    // duplicate id or the reserved 0 is a programmer error (assert in debug;
    // release logs and skips the row deterministically).
    void restoreCharacter(const Character &character, bool isPlayer, const PlayerCreation *creation);

private:
    std::map<characterId, Character> characters_;
    std::set<characterId> playerIds_;                       // exactly one entry in SP (D-STORE-1)
    std::map<characterId, PlayerCreation> playerCreations_; // parallel to playerIds_ (R7)
    IdGenerator ids_{};                                     // post-plan draws; adoptWorldGen() seeds the counter
    Position playerSpawn_{};                                // the safe-zone spawn point (worldgen stage 4)
};

} // namespace harrowmere::character

#endif // HARROWMERE_CORE_CHARACTER_STORE_HPP
