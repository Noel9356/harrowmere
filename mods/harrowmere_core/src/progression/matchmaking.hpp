// matchmaking.hpp — task 6.5: the matchmaking score's three SP consumers.
//
// The doc's multiplayer matchmaking brackets do not exist in single-player
// (D-SP-1); the score getMatchmakingScore (2.5) drives their SP stand-ins:
//
//   1. Duel opponent selection (2.7): a player requesting a duel with no
//      named target is proposed the NEAR+MID NPC with the CLOSEST score
//      (argmin |score(npc) − score(player)|, ties -> lower id, R7) and the
//      NPC answers with the A-116 acceptance roll.
//   2. Manhunt difficulty normalization (5.4): a spawned MANHUNT's
//      difficulty normalizes toward the culprit so the hunt resolves at
//      plausible odds rather than a fixed mob size — guards always join,
//      volunteers weigh in by score proximity (hunterJoinWeight).
//   3. Event difficulty calibration (5.1 -> 5.2): an all-NPC participant
//      set seeds the event's initialDifficulty toward the mean participant
//      score (calibratedEventDifficulty); the doc's difficulty regulation
//      then does the rest.
//
// Wiring contract (the card's Done-when): the consumers reach this module
// THROUGH interfaces — the event engine and any other sim-side caller go
// through WorldContext::matchmaking (IMatchmakingSink, world_context.hpp,
// D-SINK-1: null default = the template's own difficulty / the base join
// weight), never through an include of this header from events/ or combat/.
// The command path (core/command_apply.cpp) is core-internal runtime wiring
// and calls the free functions directly, exactly like every other command
// owner (progression/items/consent includes in command_apply.cpp).
//
// The 8:2 score weighting is the doc's own formula (2.5): no consumer
// "rebalances" it locally — the card pitfall. Every value below is a named
// constant (constants.hpp, Appendix A or a DEVIATIONS-row pin).
//
// RNG (R6): the acceptance roll draws the pinned "npc_life" stream (an NPC
// decision roll; the closed stream list has no duel-specific name and a new
// stream needs a deviation row — the decision domain is NPC life).

#pragma once

#include <cstdint>

#include "core/types.hpp"         // characterId
#include "core/world_context.hpp" // WorldContext + the IMatchmakingSink seam

namespace harrowmere
{

namespace progression
{

// ---- the pure math (the pinned tests exercise these directly) ----------------

// A-116 verbatim: clamp(SCALE − |Δscore| / 10, 1000, 9000). "Reasonable NPC
// sportsmanship" — a close score accepts readily, a hopeless one rarely, and
// the roll never leaves the [10%, 90%] band.
std::uint32_t duelAcceptancePct(std::uint64_t scoreDelta);

// Consumer 3's seeding math (the card's formula, verbatim):
//   initialDifficulty = clamp(meanScore / 1000,
//                             template.initialDifficulty / 2,
//                             template.initialDifficulty * 2)
// A zero template difficulty keeps zero (its clamp band is [0, 0] — a
// rewardless/flat template never gains difficulty from the seeding).
std::uint32_t calibratedEventDifficulty(std::uint64_t meanParticipantScore, std::uint32_t templateDifficulty);

// Consumer 2's volunteer arm: proximity-weighted join weight. Guards always
// join at base weight (the caller's role split); volunteers weigh in by
// score proximity to the culprit — the same A-116 falloff shape, clamped to
// [10%, 100%] of the base (a volunteer never weighs MORE than a guard):
//   weight = baseWeight * clamp(SCALE − |Δscore| / 10, 1000, SCALE) / SCALE
std::uint64_t hunterJoinWeight(std::uint64_t culpritScore, std::uint64_t volunteerScore, std::uint64_t baseWeight);

// ---- the SP consumers --------------------------------------------------------

// Consumer 1: the closest-score NEAR+MID duel proposal for `player`.
// Candidates: living sapient adult NPCs (3.4's records; the doc's duel
// participants), LOD tier NEAR or MID (the proposal is a NEARBY face, not a
// name from a list), not already dueling. Iteration is the NpcStore's
// id-ascending order (R7): the first of equal-distance candidates wins the
// tie rule without a sort. Returns 0 when no candidate exists (the caller
// rejects the request deterministically).
characterId proposeDuelOpponent(characterId player, WorldContext &ctx);

// Consumer 1's acceptance half: the NPC answers the challenge with the
// A-116 probability (draw from the pinned "npc_life" stream, R6).
bool npcAcceptsDuel(std::uint32_t acceptancePct, WorldContext &ctx);

// Consumer 2: the spawned manhunt's normalized difficulty — the guard pool
// of the warrant's territory always joins (their mean score anchors the
// normalization); volunteers within the manhunt's region weigh by proximity
// (hunterJoinWeight). The result runs through calibratedEventDifficulty's
// clamp band so a hunt never leaves the template's [0.5x, 2x] band.
std::uint32_t normalizedManhuntDifficulty(characterId culprit, std::uint32_t templateDifficulty, WorldContext &ctx);

// ---- the runtime sink ----------------------------------------------------------

// The real IMatchmakingSink: the plugin binds it into the context (the
// null-default D-SINK-1 stub keeps the template difficulty until then).
// Methods carry no WorldContext — the bound pointer is the seam, the same
// binding pattern the JusticeSystem/EventEngine adapters pin.
class MatchmakingSystem final : public IMatchmakingSink
{
public:
    void bind(
        WorldContext *ctx
    )
    {
        ctx_ = ctx;
    }

    // The participant ids' mean score seeds the clamp (the scores are read
    // here — the seam's caller never touches the power pipeline). Empty and
    // all-unknown sets keep the template difficulty (R2's zero guard).
    std::uint32_t calibratedEventDifficulty(
        const std::vector<characterId> &participants, std::uint32_t templateDifficulty
    ) const override;

    std::uint32_t normalizedManhuntDifficulty(characterId culprit, std::uint32_t templateDifficulty) const override;

private:
    WorldContext *ctx_ = nullptr;
};

} // namespace progression

} // namespace harrowmere
