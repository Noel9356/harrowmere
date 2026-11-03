#ifndef HARROWMERE_CORE_CHARACTER_APPEARANCE_HPP
#define HARROWMERE_CORE_CHARACTER_APPEARANCE_HPP

// appearance.hpp — appearance changes with XP cost + cooldown, feeding the
// social-perception hooks (task 2.6).
//
// The doc's hard rule (character.md §Appearance), enforced by what this file
// does NOT do: "Appearance never affects stats, skills, or equipment."
// Appearance feeds social perception only — species bias and suspicion
// triggers may read it, and "frequent changes are themselves a suspicion
// trigger". TestAppearance_PowerUnchangedAfterChange pins the rule from the
// power side (2.5's formula reads stats/skills/gear — none of which a
// change touches).
//
// The cost math (design reference §Appearance, verbatim; A-22/A-23):
//     appearanceChangeCost(level) = BASE_APPEARANCE_COST + APPEARANCE_COST_RATE * level
//     canChangeAppearance(player):
//         if player.cooldowns[APPEARANCE] > worldTime: return false
//         if player.progression.availableXp < cost:    return false
// The cost is paid from availableXp — the doc's own comparison; a misread
// as currency is the card's classic mistake and the pinned test catches it.
//
// Frequent changes (A-25): every change records a worldTime stamp in the
// AppearanceHistory; 3 or more within 10 game-days fire the suspicion seam
// (WorldContext::suspicion, consumed by 4.5, weighted A-94). The history is
// also the read 3.9/4.5 need for "disguises defeat strangers, never
// acquaintances": strangers' memory of the culprit resets on a change,
// acquaintances' never does — both compare their memory age against the
// last-change stamp (AppearanceHistory::lastChangeTime).
//
// The Character struct stays doc-verbatim (D-STORE-1): the change timestamps
// live in the AppearanceHistory store, never on the character — the same
// split that keeps jail off Character.

#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "character/character.hpp" // Character (the doc struct)
#include "core/constants.hpp"      // A-22..A-25
#include "core/enums.hpp"          // CooldownType
#include "core/result.hpp"         // Result<void, E>
#include "core/types.hpp"          // characterId
#include "core/world_context.hpp"  // WorldContext + the appearance/suspicion seams

namespace harrowmere
{

namespace character
{

// ---- the change-timestamp history (the A-25 store + the 3.9/4.5 read) -----------

// AppearanceHistory — per-character worldTime stamps of every applied
// appearance change. worldTime is monotonic (R8: the sim reads no wall
// clock), so per-record append order IS time order; the window count scans
// that suffix only. std::map keyed by characterId (R7). Task 7.1 persists
// the stamps (the save schema extends in the same commit as the card).
//
// Store-namespace placement: the WorldContext field is
// character::AppearanceHistory (the seam bundle points at stores through
// their owning namespace, the CharacterStore pattern).
class AppearanceHistory
{
public:
    // Record one applied change (changeAppearance calls this on success).
    void record(characterId who, std::uint64_t worldTime);

    // Changes in the half-open window (now - windowGameS, now] — the A-25
    // count. windowGameS 0 counts the stamp at exactly `now`.
    [[nodiscard]] std::size_t changesWithin(characterId who, std::uint64_t now, std::uint64_t windowGameS) const;

    // The latest change's worldTime, or 0 when the character never changed
    // (the epoch's 0 is a "never" for every character born after it — the
    // A-129 epoch opens at 10:00 game time, so a real stamp is never 0).
    [[nodiscard]] std::uint64_t lastChangeTime(characterId who) const;

    // The appearance VERSION (task 3.9's disguise reset): the number of
    // recorded changes — every applied change bumps the version by one, so a
    // sighting keyed to an older version no longer matches the current look
    // (design rule: "disguises defeat strangers, never acquaintances" — the
    // acquaintance half matches by id and never consults the version).
    [[nodiscard]] std::size_t changeCount(characterId who) const;

    // The version a sighting taken at `worldTime` saw: changes recorded at or
    // before that stamp. A sighting whose stored version differs from
    // changeCount() is keyed to a superseded look.
    [[nodiscard]] std::size_t changeCountUpTo(characterId who, std::uint64_t worldTime) const;

    [[nodiscard]] bool empty() const noexcept
    {
        return changes_.empty();
    }

    // ---- save seam (task 7.1): the stamps are persisted per character (the
    // card's own note); persist/ serializes the map verbatim.
    [[nodiscard]] const std::map<characterId, std::vector<std::uint64_t>> &all() const noexcept
    {
        return changes_;
    }
    void restore(
        std::map<characterId, std::vector<std::uint64_t>> rows
    )
    {
        changes_ = std::move(rows);
    }

private:
    std::map<characterId, std::vector<std::uint64_t>> changes_; // id -> ascending stamps (R7)
};

} // namespace character

using character::AppearanceHistory; // the card's unqualified spelling

// ---- the change flow (design reference §Appearance, verbatim) -------------------

// appearanceChangeCost — BASE_APPEARANCE_COST + APPEARANCE_COST_RATE * level
// (A-22/A-23): one early level's worth of XP, growing linearly.
[[nodiscard]] inline std::uint64_t appearanceChangeCost(
    const Character &c
) noexcept
{
    return BASE_APPEARANCE_COST + APPEARANCE_COST_RATE * c.progression.level;
}

// canChangeAppearance — the doc's gate, verbatim: the APPEARANCE cooldown
// slot expired (cooldowns[type] > now blocks) AND availableXp covers the
// cost (availableXp, NEVER currency — the card's pinned test).
[[nodiscard]] bool canChangeAppearance(const Character &c, std::uint64_t now);

enum class AppearanceError
{
    CooldownActive, // the A-24 1-game-day cooldown still runs
    InsufficientXp  // availableXp below the cost (the doc's comparison)
};

using AppearanceResult = Result<void, AppearanceError>;

// changeAppearance — apply a new look:
//   1. gate: canChangeAppearance (cooldown, then XP),
//   2. deduct the cost from availableXp (applyXpDelta — R4's clamp; never
//      currency),
//   3. replace appearanceComponents with the new set (the social-perception
//      input — never stats, never equipment),
//   4. arm the APPEARANCE cooldown for 1 game-day (A-24),
//   5. record the stamp in the history (when ctx carries one) and fire the
//      A-25 trigger through the suspicion seam once the window holds
//      APPEARANCE_FREQUENT_COUNT stamps,
//   6. publish the fact through the appearance seam (the 0.7
//      AppearanceChanged; post-mutation per 0.7's rule).
//
// Seams may be null (D-SINK-1): no history = no A-25 tracking (the change
// still succeeds); no sinks = the publications drop. Steps 2-4 are not
// seams — they are the change.
AppearanceResult changeAppearance(Character &c, std::span<const std::uint64_t> newComponents, WorldContext &ctx);

} // namespace harrowmere

#endif // HARROWMERE_CORE_CHARACTER_APPEARANCE_HPP
