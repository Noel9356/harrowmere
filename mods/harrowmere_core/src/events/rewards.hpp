#ifndef HARROWMERE_CORE_EVENTS_REWARDS_HPP
#define HARROWMERE_CORE_EVENTS_REWARDS_HPP

// rewards.hpp — task 5.4: the single-pass payout (quadratic → O(n log n) per
// the doc's note) and the justice-driven manhunt completion.
//
// distributeRewards is design reference verbatim: the total is computed
// ONCE (the per-partitioner effective-contribution evaluations are exactly
// n — TestRewards_TotalComputedOnce counts them through contribution.hpp's
// instrumentation seam), ineligible participants are excluded from the
// total so their presence never shrinks the others' shares (4.7's
// smuggling hook reads ev.excludedFromRewards), the sort is amount-
// descending with the R7 tie rule (lower characterId first), and the
// flooring remainder goes to the top contributor. The payout invariant
// Σpaid + remainder == pool holds for every input (the Done-when's fuzz
// property test).
//
// The pool (as-built reading, DEVIATIONS.md row): the template's
// perContribution rates are denominated against CONTRIBUTION POINTS —
// rewardPool = Σ participants' raw contribution × A-107's difficulty
// multiplier / SCALE, and giveReward converts a pay of `n` points into
// XP n × xpPerContribution and currency n × currencyPerContribution (the
// rates are per point of contribution — the shipped tables' rates are
// content data, the 8.4 balance pass owns their magnitudes). Rewardless
// templates (no rewardTable, or an all-zero one) pay nothing at any payout
// point — payout is a no-op, not an error.
//
// Payout timing (the doc order 5.1's header carries): rewards pay BEFORE
// endEvent's clears — the engine's finishEvent calls payEventRewards
// before the contribution books are wiped. A failed event still pays for
// work done (the leave-early rule's own logic: contribution-based, never
// success-gated, and never time-gated — "no reward is time-exclusive").
//
// MANHUNT completion: the card's exits (captured 4.7 / dead / duration
// expiry) are the template's own TargetCaptured/TargetDead transitions and
// the maxDuration backstop; ADDITIONALLY the engine polls the seed
// warrant at each tick — a warrant that left the active set (convicted,
// paid, surrendered, expired — the justice side's WarrantResolved
// publications) ends its manhunt at the same tick (the doc's "any manhunt
// ends" — 4.3's surrender path rides exactly this). The event itself
// publishes no duplicate WarrantResolved: 0.7's facts-that-already-
// happened rule leaves the publication to the fact's owner (the justice
// paths that resolved the warrant).

#include <cstdint>

#include "core/constants.hpp" // SCALE + A-107's named divisors
#include "core/types.hpp"
#include "events/engine.hpp"   // Event
#include "events/template.hpp" // EventTemplate

namespace harrowmere
{
struct WorldContext; // core/world_context.hpp — reference only here
struct Character;    // character/character.hpp — reference only here
} // namespace harrowmere

namespace harrowmere::events
{

// A-107's pinned mapping (the card: "reward quality and quantity scale with
// difficulty"):
//   * pool multiplier = SCALE + difficulty / 2;
//   * item rarity roll bonus = difficulty / 10 (rarity-grid quantity; it
//     lands with per-instance rarity — 6.4's quest tables consume it; v1's
//     items grant at their definition rarity, see giveReward).
inline std::uint64_t poolMultiplier(
    std::uint32_t difficulty
) noexcept
{
    return SCALE + difficulty / REWARD_POOL_DIFFICULTY_DIVISOR;
}

inline std::uint32_t itemRarityBonus(
    std::uint32_t difficulty
) noexcept
{
    return difficulty / REWARD_RARITY_BONUS_DIVISOR;
}

// rewardPool — the event's payout pool (see the header's denomination
// note): the participants' raw contribution total priced at A-107's
// difficulty multiplier. One O(n) read of the books (getContribution, not
// the damped effective read — the shares' own pass stays the single
// effective-contribution pass).
std::uint64_t rewardPool(const Event &ev, const EventTemplate &tpl);

// giveReward — the rewardTable routing for one participant's pay: XP via
// applyXpDelta + totalXp + markPowerDirty (2.5's audited grant family),
// currency through the i64 progression field (never an unsigned wrap), and
// one unit of each rewardTable item through the bag path (unknown item
// keys and absent bags skip deterministically; v1 grants at the
// definition rarity — A-107's rarity bonus is exported above for the
// per-instance consumer).
void giveReward(characterId who, std::uint64_t pay, const EventTemplate &tpl, WorldContext &ctx);

// distributeRewards — design reference verbatim (see the header doc). The
// template parameter is the same signature adaptation the other 5.x
// functions take (the card's Ctx carries the registry; our WorldContext is
// plain data — DEVIATIONS.md).
void distributeRewards(Event &ev, std::uint64_t pool, const EventTemplate &tpl, WorldContext &ctx);

// payEventRewards — the endEvent payout point (the engine calls it BEFORE
// the contribution clears): a populated rewardTable pays rewardPool through
// distributeRewards; a missing or all-zero table pays nothing (a no-op,
// not an error).
void payEventRewards(Event &ev, const EventTemplate &tpl, WorldContext &ctx);

} // namespace harrowmere::events

#endif // HARROWMERE_CORE_EVENTS_REWARDS_HPP
