// store.cpp - CharacterStore implementation (task 2.0).
//
// Every literal below is a named constant in core/constants.hpp (Appendix A
// rows A-41/A-42/A-43 and the hardcoded player-species row) - common.md
// section 1.3's "no invented numbers" rule. The doc's pinned starting state
// for a NEW PLAYER (card 2.0): level 1, all stats/skills at their floor 0,
// totalXp = 0, availableXp = 0, currency = 100 (A-41), maxHp by the A-42 +
// A-43 formula with currentHp = maxHp. NPCs share the doc struct; only the
// level (the seed's age-band level) and the id (the plan's pre-assigned one)
// differ from the player defaults at creation time.

#include "character/store.hpp"

#include <cassert>
#include <utility>

#include "core/constants.hpp" // PLAYER_SPECIES_ID (0 = player), STARTER_CURRENCY (A-41)
#include "core/log.hpp"       // HARROWMERE_LOG_* (task 0.5) - creation is a system-entry event

namespace harrowmere::character
{
namespace
{

// The deterministic zero character served by get() on a release-mode miss
// (DataRegistry::species()'s pattern). id 0 is reserved (R10) and never
// simulated; powerDirty stays true so a caller that somehow caches it into
// the power pipeline (2.5) recomputes rather than trusts it.
Character &invalidCharacter()
{
    static Character invalid{};
    return invalid;
}

PlayerCreation &invalidCreation()
{
    static PlayerCreation invalid{};
    return invalid;
}

} // namespace

void CharacterStore::adoptWorldGen(
    std::uint64_t nextCharacterId, const Position &playerSpawn
)
{
    // New-game wiring runs BEFORE the first create (store.hpp's contract).
    assert(
        characters_.empty() && playerIds_.empty()
        && "CharacterStore::adoptWorldGen: called after characters exist (new-game wiring only)"
    );

    ids_         = IdGenerator{nextCharacterId}; // clamps below FIRST_ID; worldgen hands plan_end + 1
    playerSpawn_ = playerSpawn;
}

characterId CharacterStore::createPlayer(
    const PlayerCreation &creation
)
{
    // SP invariant (D-STORE-1): exactly one player.
    assert(playerIds_.empty() && "CharacterStore::createPlayer: a player already exists (SP has exactly one)");
    if (!playerIds_.empty())
    {
        // Release recovery: deterministic no-op - the existing player wins,
        // the duplicate request is logged and ignored.
        const characterId existing = *playerIds_.begin();
        HARROWMERE_LOG_WARN(
            sim, "createPlayer rejected: player %llu already exists (SP)", static_cast<unsigned long long>(existing)
        );
        return existing;
    }

    Character c{};
    c.id      = ids_.nextCharacter();
    c.species = PLAYER_SPECIES_ID; // players are always the "player" species (design reference)

    // The pinned starting state (card 2.0). Every field the doc struct
    // defaults differently is set EXPLICITLY here so the review-checked
    // creation path is self-documenting; the rest ride the doc defaults
    // (duelOpponent/arenaId/eventId 0, statusFlags 0, faction 0,
    // lastCombatTime 0, cooldowns 0, powerDirty true).
    c.progression.level       = 1;
    c.progression.totalXp     = 0;
    c.progression.availableXp = 0;
    c.progression.currency    = STARTER_CURRENCY;                   // A-41 starter purse
    c.body.maxHp              = maxHpForLevel(c.progression.level); // A-42 + A-43
    c.body.currentHp          = c.body.maxHp;
    c.body.position           = playerSpawn_; // the safe-zone spawn point (worldgen stage 4)
    c.powerDirty              = true;

    // The creation flow's metadata: appearance components verbatim (empty =
    // the v1 default set), name + class preset recorded OUTSIDE the doc
    // struct (D-STORE-1 keeps Character doc-verbatim).
    c.appearanceComponents = creation.appearanceComponents;
    playerCreations_.emplace(c.id, creation);

    const characterId id = c.id;
    characters_.emplace(id, std::move(c));
    playerIds_.insert(id);

    HARROWMERE_LOG_INFO(
        sim,
        "player %llu created (species player, level 1) at spawn (%.1f, %.1f, %.1f)",
        static_cast<unsigned long long>(id),
        playerSpawn_.x,
        playerSpawn_.y,
        playerSpawn_.z
    );
    return id;
}

characterId CharacterStore::createNpc(
    const NpcSeed &seed
)
{
    // Plan seeds carry their pre-assigned id (worldgen 1.5, strict plan
    // order); post-plan spawns (3.4 births, 3.6 backups) leave id 0 and the
    // store draws the next one from the adopted counter. 0 is reserved
    // (R10) and never stored.
    const characterId id = seed.id != 0 ? seed.id : ids_.nextCharacter();

    // A colliding id is a programmer error (the plan's ids are unique and
    // adoptWorldGen starts the counter past them).
    assert(
        characters_.find(id) == characters_.end()
        && "CharacterStore::createNpc: characterId already in use (R10: ids are never reused)"
    );
    if (characters_.find(id) != characters_.end())
    {
        HARROWMERE_LOG_WARN(
            sim, "createNpc rejected: character %llu already exists", static_cast<unsigned long long>(id)
        );
        return id;
    }

    // The seed's starting level comes from worldgen's age band (>= 1);
    // level 0 would break maxHpForLevel's intent - assert, clamp in release.
    assert(seed.level >= 1 && "CharacterStore::createNpc: seed level 0 (worldgen's band starts at 1)");
    const std::uint16_t level = seed.level >= 1 ? seed.level : 1;

    Character c{};
    c.id      = id;
    c.species = seed.species;

    // The doc floor shared by every NPC: no XP, no purchases, no purse.
    // NPCs grow ONLY through the pinned paths (kills 3.3, event rewards 5.x)
    // and 2.3's auto-buy - never at creation.
    c.progression.level       = level;
    c.progression.totalXp     = 0;
    c.progression.availableXp = 0;
    c.progression.currency    = 0;
    c.body.maxHp              = maxHpForLevel(level);
    c.body.currentHp          = c.body.maxHp;
    c.body.position           = Position{0.0, 0.0, 0.0}; // placement is task 3.4's spawn policy
    c.powerDirty              = true;

    // Deliberately NOT carried from the seed (D-STORE-1): roleKey and
    // ageYears are NpcState fields - task 3.4 attaches the NpcState record
    // by the id returned here. faction stays 0 (factionless): faction seeds
    // (worldgen stage 5) link characters to factions in task 6.1.

    characters_.emplace(id, std::move(c));

    // Per-NPC line at DEBUG (not rule-3 INFO): the plan spawn is thousands of
    // these - the mass-spawn summary belongs to task 3.4's spawn path, which
    // logs one INFO with the decisive count.
    HARROWMERE_LOG_DEBUG(
        sim,
        "npc %llu created (species %u, level %u)%s",
        static_cast<unsigned long long>(id),
        static_cast<unsigned>(c.species),
        static_cast<unsigned>(level),
        seed.id != 0 ? " from plan" : " post-plan"
    );
    return id;
}

Character &CharacterStore::get(
    characterId id
)
{
    const auto it = characters_.find(id);
    assert(it != characters_.end() && "CharacterStore::get: unknown characterId (exists() first, or a store bug)");
    if (it == characters_.end())
    {
        HARROWMERE_LOG_ERROR(
            sim, "get: unknown character %llu - serving the zero character", static_cast<unsigned long long>(id)
        );
        return invalidCharacter();
    }
    return it->second;
}

Character *CharacterStore::find(
    characterId id
) noexcept
{
    const auto it = characters_.find(id);
    return it != characters_.end() ? &it->second : nullptr;
}

const Character *CharacterStore::find(
    characterId id
) const noexcept
{
    return const_cast<CharacterStore *>(this)->find(id);
}

const Character &CharacterStore::get(
    characterId id
) const
{
    return const_cast<CharacterStore *>(this)->get(id);
}

bool CharacterStore::isPlayer(
    characterId id
) const noexcept
{
    return playerIds_.count(id) != 0;
}

bool CharacterStore::exists(
    characterId id
) const noexcept
{
    return characters_.find(id) != characters_.end();
}

bool CharacterStore::remove(
    characterId id
)
{
    const auto it = characters_.find(id);
    if (it == characters_.end())
    {
        assert(false && "CharacterStore::remove: unknown id (programmer error)");
        HARROWMERE_LOG_ERROR(sim, "remove: unknown character %llu", static_cast<unsigned long long>(id));
        return false;
    }
    characters_.erase(it);
    playerIds_.erase(id);
    playerCreations_.erase(id);
    return true;
}

characterId CharacterStore::playerId() const noexcept
{
    // Exactly one entry in SP; begin() keeps even a malformed multi-player
    // store deterministic (R7).
    return playerIds_.empty() ? 0 : *playerIds_.begin();
}

std::size_t CharacterStore::playerCount() const noexcept
{
    return playerIds_.size();
}

std::size_t CharacterStore::population() const noexcept
{
    return characters_.size();
}

std::map<characterId, Character> &CharacterStore::all() noexcept
{
    return characters_;
}

const std::map<characterId, Character> &CharacterStore::all() const noexcept
{
    return characters_;
}

const PlayerCreation &CharacterStore::playerCreation() const
{
    const characterId id = playerId();
    assert(id != 0 && "CharacterStore::playerCreation: no player in-world");
    const auto it = playerCreations_.find(id);
    if (it == playerCreations_.end())
    {
        HARROWMERE_LOG_ERROR(
            sim, "playerCreation: player %llu has no creation record", static_cast<unsigned long long>(id)
        );
        return invalidCreation();
    }
    return it->second;
}

void CharacterStore::restoreCharacter(
    const Character &character, bool isPlayer, const PlayerCreation *creation
)
{
    // The reserved 0 and duplicates are programmer errors (the load path
    // built its rows from a validated save; a collision means a corrupted
    // document slipped through). Debug asserts; release logs and skips
    // deterministically — the store never ends up half-inserted.
    assert(character.id != 0 && "CharacterStore::restoreCharacter: the reserved id 0 is not restorable");
    assert(characters_.find(character.id) == characters_.end() && "CharacterStore::restoreCharacter: duplicate id");
    if (character.id == 0 || characters_.find(character.id) != characters_.end())
    {
        HARROWMERE_LOG_ERROR(
            sim,
            "restoreCharacter rejected: character %llu (%s)",
            static_cast<unsigned long long>(character.id),
            character.id == 0 ? "reserved id 0" : "already present"
        );
        return;
    }

    const characterId id = character.id;
    characters_.emplace(id, character);
    if (isPlayer)
    {
        playerIds_.insert(id);
        if (creation != nullptr)
        {
            playerCreations_.emplace(id, *creation);
        }
    }
}

} // namespace harrowmere::character
