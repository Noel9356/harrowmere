// matchmaking.cpp — task 6.5: the three SP consumers of the 2.5 score.
//
// Every value the formulas use is a named constant (core/constants.hpp):
// A-116's falloff/floor/ceiling for the acceptance roll and the volunteer
// join weight, the calibration divisor for the event seeding. The consumers
// read the stores through the WorldContext and never touch combat/ or
// events/ internals (the card's "no direct coupling" rule — the event
// engine reaches this module through the IMatchmakingSink seam instead).

#include "progression/matchmaking.hpp"

#include <cstdlib> // llabs on the widened delta

#include "character/character.hpp"
#include "character/store.hpp"
#include "core/constants.hpp"
#include "core/math.hpp"         // clampScalar (0.2's helpers)
#include "core/rng.hpp"          // the pinned "npc_life" stream (R6)
#include "data/registry.hpp"     // species sapience read
#include "npc/state.hpp"         // NpcStore, LodTier
#include "progression/power.hpp" // getMatchmakingScore (the 8:2 doc formula)

namespace harrowmere::progression
{

namespace
{

// The candidate filters of consumer 1 (the doc's duel participants): living
// (the character store holds only the living), sapient, adult (A-44), and
// not already dueling. The LOD tier gate (NEAR/MID only) rides the call site
// so the test can pin the filter separately.
bool duelCandidate(
    const Character &c, const NpcState &npc, const data::DataRegistry &data, std::uint64_t now
)
{
    if (c.duelOpponent != 0)
        return false; // already dueling
    if (!data.species(c.species).sapient)
        return false;
    if (npc.ageYears(now) < ADULT_YEARS)
        return false;
    return true;
}

std::uint64_t scoreOf(
    const Character &c, const data::DataRegistry &data
)
{
    return getMatchmakingScore(c, data);
}

} // namespace

std::uint32_t duelAcceptancePct(
    std::uint64_t scoreDelta
)
{
    // A-116 verbatim: clamp(SCALE − |Δscore| / 10, 1000, 9000). The delta is
    // a magnitude (unsigned); the falloff divides before the clamp (R2: the
    // divisor is the pinned constant 10, never zero).
    const std::uint64_t falloff = scoreDelta / DUEL_ACCEPTANCE_FALLOFF_DIVISOR;
    const std::int64_t pct      = static_cast<std::int64_t>(SCALE) - static_cast<std::int64_t>(falloff);
    return static_cast<std::uint32_t>(clampScalar<std::int64_t>(
        pct, static_cast<std::int64_t>(DUEL_ACCEPTANCE_FLOOR), static_cast<std::int64_t>(DUEL_ACCEPTANCE_CEILING)
    ));
}

std::uint32_t calibratedEventDifficulty(
    std::uint64_t meanParticipantScore, std::uint32_t templateDifficulty
)
{
    // The card's formula, verbatim: clamp(meanScore / 1000,
    // template.initialDifficulty / 2, template.initialDifficulty * 2).
    // A zero template keeps its zero band (the doc's "the difficulty
    // regulation then does the rest" has nothing to regulate from).
    const std::int64_t seed = static_cast<std::int64_t>(meanParticipantScore / EVENT_DIFFICULTY_SEED_DIVISOR);
    const std::int64_t lo   = static_cast<std::int64_t>(templateDifficulty) / 2;
    const std::int64_t hi   = static_cast<std::int64_t>(templateDifficulty) * 2;
    return static_cast<std::uint32_t>(clampScalar(seed, lo, hi));
}

std::uint64_t hunterJoinWeight(
    std::uint64_t culpritScore, std::uint64_t volunteerScore, std::uint64_t baseWeight
)
{
    // Proximity weighting (the card: "hunter NPC join-weights proportional
    // to score proximity to the culprit"): the A-116 falloff shape clamped
    // to [10%, 100%] of the base — a volunteer never out-weighs a guard,
    // a hopeless mismatch never drops below a token tenth.
    const std::uint64_t delta
        = culpritScore > volunteerScore ? culpritScore - volunteerScore : volunteerScore - culpritScore;
    const std::uint64_t falloff  = delta / DUEL_ACCEPTANCE_FALLOFF_DIVISOR;
    const std::int64_t proximity = static_cast<std::int64_t>(SCALE) - static_cast<std::int64_t>(falloff);
    const std::uint64_t clamped  = static_cast<std::uint64_t>(clampScalar<std::int64_t>(
        proximity, static_cast<std::int64_t>(DUEL_ACCEPTANCE_FLOOR), static_cast<std::int64_t>(SCALE)
    ));
    return baseWeight * clamped / SCALE;
}

characterId proposeDuelOpponent(
    characterId player, WorldContext &ctx
)
{
    // The proposal is a NEARBY face: the consumer needs the stores, the
    // registry and the LOD tiers; a missing surface degrades to "no
    // proposal" deterministically (§4.0's degrade-if-absent contract).
    if (ctx.characters == nullptr || ctx.npcs == nullptr || ctx.data == nullptr)
        return 0;
    if (!ctx.characters->exists(player))
        return 0;

    const Character &self         = ctx.characters->get(player);
    const std::uint64_t selfScore = scoreOf(self, *ctx.data);

    // argmin |score(npc) − score(player)| over the candidate set; ties ->
    // lower id (R7: the scan is the NpcStore's id-ascending order and the
    // strict < keeps the FIRST minimum — no sort, no tie ambiguity).
    characterId best        = 0;
    std::uint64_t bestDelta = 0;
    for (const auto &[id, state] : ctx.npcs->all())
    {
        if (!ctx.characters->exists(id) || ctx.characters->isPlayer(id))
            continue;
        // The proposal is a NEARBY face (NEAR+MID only — the card's own
        // scope line); a FAR candidate is a name from a list, not a duelist.
        if (state.tier != LodTier::NEAR && state.tier != LodTier::MID)
            continue;
        const Character &npc = ctx.characters->get(id);
        if (!duelCandidate(npc, state, *ctx.data, ctx.now))
            continue;

        const std::uint64_t npcScore = scoreOf(npc, *ctx.data);
        const std::uint64_t delta    = npcScore > selfScore ? npcScore - selfScore : selfScore - npcScore;
        if (best == 0 || delta < bestDelta)
        {
            best      = id;
            bestDelta = delta;
        }
    }
    return best;
}

bool npcAcceptsDuel(
    std::uint32_t acceptancePct, WorldContext &ctx
)
{
    // The acceptance roll: one draw from the pinned "npc_life" stream
    // (R6 — an NPC decision roll on its named stream; the stream list is
    // closed, so no duel-specific name exists).
    if (ctx.rng == nullptr)
        return false;
    const std::uint32_t roll = ctx.rng->pct("npc_life");
    return roll < acceptancePct;
}

std::uint32_t normalizedManhuntDifficulty(
    characterId culprit, std::uint32_t templateDifficulty, WorldContext &ctx
)
{
    // Consumer 2: the hunt must resolve at plausible odds. Guards of the
    // warrant's territory ALWAYS join — their mean score anchors the
    // normalization; volunteers of the region join by proximity weight, so
    // the effective participant mean blends the two arms (the weight is the
    // share, the score the value). A missing surface or an empty pool
    // degrades to the culprit's own score (the hunt still normalizes — a
    // lone culprit IS the opposition the hunt must balance).
    if (ctx.characters == nullptr || ctx.data == nullptr)
        return templateDifficulty;

    const Character &culpritChar     = ctx.characters->get(culprit);
    const std::uint64_t culpritScore = scoreOf(culpritChar, *ctx.data);

    std::uint64_t weightTotal = 0;
    std::uint64_t scoreTotal  = 0;
    if (ctx.npcs != nullptr)
    {
        for (const auto &[id, state] : ctx.npcs->all())
        {
            if (!ctx.characters->exists(id))
                continue;
            const Character &npc = ctx.characters->get(id);
            if (!ctx.data->species(npc.species).sapient)
                continue;
            // Guards always join (enforcement.cpp's garrison set); every
            // other sapient is a volunteer weighted by score proximity.
            const bool guard
                = state.roleKey == "guard" || state.roleKey == "captain" || state.roleKey == "militia_veteran";
            const std::uint64_t base   = 1;
            const std::uint64_t weight = guard ? base : hunterJoinWeight(culpritScore, scoreOf(npc, *ctx.data), base);
            if (weight == 0)
                continue;
            weightTotal += weight;
            scoreTotal += weight * scoreOf(npc, *ctx.data);
        }
    }

    std::uint64_t mean = culpritScore; // the empty-pool fallback (R2 guard below)
    if (weightTotal != 0)
        mean = scoreTotal / weightTotal;
    return calibratedEventDifficulty(mean, templateDifficulty);
}

// ---- the runtime sink ----------------------------------------------------------

std::uint32_t MatchmakingSystem::calibratedEventDifficulty(
    const std::vector<characterId> &participants, std::uint32_t templateDifficulty
) const
{
    // The mean participant score feeds the pinned clamp; the sink reads the
    // scores here (the engine never touches the power pipeline — D-SINK-1).
    if (ctx_ == nullptr || ctx_->characters == nullptr || ctx_->data == nullptr || participants.empty())
        return templateDifficulty; // not bound / nothing to seed from: the template stands
    std::uint64_t total = 0;
    std::uint64_t count = 0;
    for (const characterId p : participants)
    {
        if (!ctx_->characters->exists(p))
            continue;
        total += scoreOf(ctx_->characters->get(p), *ctx_->data);
        count += 1;
    }
    if (count == 0)
        return templateDifficulty; // R2: no live participant — the pinned fallback is the template
    return progression::calibratedEventDifficulty(total / count, templateDifficulty);
}

std::uint32_t MatchmakingSystem::normalizedManhuntDifficulty(
    characterId culprit, std::uint32_t templateDifficulty
) const
{
    if (ctx_ == nullptr)
        return templateDifficulty; // not bound: the template stands (D-SINK-1)
    return progression::normalizedManhuntDifficulty(culprit, templateDifficulty, *ctx_);
}

} // namespace harrowmere::progression
