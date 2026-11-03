#ifndef HARROWMERE_CORE_JUSTICE_CONVICTION_HPP
#define HARROWMERE_CORE_JUSTICE_CONVICTION_HPP

// conviction.hpp — task 4.3: `convict()` with the wide-arithmetic penalty,
// reputation/notoriety consequences, the jail map, surrender and escape.
//
// Pinned here (design reference verbatim):
//   * The debt is the R3 wide path: mulDiv128(CONVICTION_PENALTY_FACTOR
//     (A-77, 2.0x), levelCosts[level], zoneMultiplier * reputationMultiplier,
//     SCALE * SCALE) — no UB/overflow at the level-100 safe-zone worst case
//     (the card's Done-when test runs the max factor).
//   * getZoneMultiplier reads the territory recorded IN THE EVIDENCE, never
//     the culprit's current position (the doc note; fleeing a safe zone
//     before conviction does not dodge the doubling — the pinned test).
//   * availableXp takes the debt through applyXpDelta (R4's -MAX_XP floor;
//     totalXp never decreases); the fine may push currency NEGATIVE (i64 on
//     Purpose — the doc struct's own rule).
//   * Warrant cleanup deletes from BOTH maps (the doc invariant).
//   * jailUntil + isArrested live HERE (on the JusticeStore), never on
//     Character; isArrested purges expired entries lazily on read.
//   * Surrender: a wanted player surrendering to any guard of the issuing
//     territory converts every live warrant active there into a conviction
//     (standard sentence) and ends any manhunt (WarrantResolved{Surrendered}).
//   * Escape: an ESCAPE skill check — pct = clamp(eff * 2 - A-81 + 2000, 200,
//     9000). Success erases jailUntil; failure DOUBLES the remaining sentence.
//     Either way the attempt leaves fresh OBSTRUCTION evidence
//     (ESCAPE_EVIDENCE_WEIGHT, A-82; a failure stacks ESCAPE_FAIL_WEIGHT,
//     A-83, onto it).
//   * Breakout: healing/releasing an arrested character is accomplice
//     evidence vs the rescuer (4.1's list, pinned trigger).

#include <cstdint>

#include "core/game_events.hpp" // WarrantResolved (the surrender path publishes Surrendered)
#include "core/types.hpp"       // characterId, territoryId
#include "justice/justice_store.hpp"

namespace harrowmere
{
struct Character;
struct WorldContext;
} // namespace harrowmere

namespace harrowmere::justice
{
struct Evidence; // evidence.hpp — the complete type; pointer/reference only here
} // namespace harrowmere::justice

namespace harrowmere::justice
{

// getZoneMultiplier — common.md §Death's verbatim helper: 2*SCALE inside a
// safe territory, SCALE outside. `territory` is the EVIDENCE territory (the
// doc note — callers pass e.territory, never a live position's walk).
std::uint64_t getZoneMultiplier(const WorldContext &ctx, territoryId territory);

// convict — the design reference's verbatim pipeline: status guard -> XP debt
// (the wide path above) -> reputation loss (A-78) at the evidence territory ->
// warrant cleanup (both maps) -> notoriety gain (crime data) -> sentence.
// `warrantVerdict` names the WarrantResolved path (Convicted by default; the
// surrender flow passes Surrendered — same cleanup, different verdict).
void convict(Evidence &e, WorldContext &ctx, WarrantResolved::How warrantVerdict = WarrantResolved::How::Convicted);

// applySentence — the jail notes: sentence = SENTENCE_BASE_DAYS (A-79) +
// severity / SENTENCE_SEVERITY_DIVISOR (A-80's "1 per severity point ×5")
// game-days onto jailUntil, minus fineFor (severity × 10 currency per point;
// see the constants block) which may go negative.
void applySentence(const Evidence &e, WorldContext &ctx);

// fineFor — the currency-side penalty (the docs name the function; the rate
// is pinned in constants.hpp as FINE_CURRENCY_PER_POINT — twice the bounty
// rate, which keeps D-BOUNTY-1's "bounty stays below the conviction penalty"
// structurally true at every severity).
std::int64_t fineFor(const Evidence &e, WorldContext &ctx);

// isArrested — design reference verbatim: lazy purge on read (an expired
// entry erases itself and answers false).
bool isArrested(JusticeStore &justice, characterId c, std::uint64_t now);

// The remaining sentence of an arrested character (0 when free) — the UI's
// serve-sentence read (7.3b) and the escape paths' "remaining term".
std::uint64_t remainingSentence(JusticeStore &justice, characterId c, std::uint64_t now);

// surrender — the doc's interaction: for each live warrant of the player
// active in the guard's territory, convict the underlying evidence; ends any
// manhunt (WarrantResolved{Surrendered} fires from the warrant cleanup).
// True when at least one warrant was converted.
bool surrender(characterId player, territoryId guardTerritory, WorldContext &ctx);

// attemptEscape — the player's jail action (while isArrested): the ESCAPE
// check above. True on success (jail records erased); failure extends the
// sentence to double the remaining term. Both outcomes leave the fresh
// OBSTRUCTION evidence at the jail territory (the evidence territory is where
// the character was convicted — the arrest's territory).
bool attemptEscape(characterId escapee, WorldContext &ctx);

// recordBreakout — the third-party rescue trigger ("release or heal of an
// arrested character"): accomplice evidence vs the rescuer (SMUGGLING —
// 4.1's aid list). No trigger exists for mere proximity; do not add one.
void recordBreakout(characterId rescuer, characterId prisoner, WorldContext &ctx);

} // namespace harrowmere::justice

#endif // HARROWMERE_CORE_JUSTICE_CONVICTION_HPP
