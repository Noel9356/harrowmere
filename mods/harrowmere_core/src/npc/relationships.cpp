// relationships.cpp — the social graph's mutation surface + witness
// qualification (task 3.9). Every rule is card-verbatim; the file exists so
// 4.1's witnessReports reads ONLY through this API (Done-when), never
// reimplementing knowing/testifying justice-side.

#include "npc/relationships.hpp"

#include <algorithm>

#include "character/species.hpp" // isSapient (2.2)
#include "core/constants.hpp"    // SCALE (A-1), GAME_HOUR_SECONDS, the 3.9 rows
#include "data/registry.hpp"     // DataRegistry (species + role rows)

namespace harrowmere::npc
{

// ---- affinity -----------------------------------------------------------------

std::int64_t clampAffinity(
    std::int64_t value
)
{
    // [-SCALE, +SCALE] — the deliberate span (see relationships.hpp's header
    // note): the weight formula's (SCALE - affinity) term reads the full
    // signed range, enemies included.
    return clampScalar<std::int64_t>(value, -static_cast<std::int64_t>(SCALE), static_cast<std::int64_t>(SCALE));
}

void accrueCoPresenceAffinity(
    NpcState &observer, characterId other, std::uint64_t elapsedGameSeconds
)
{
    // +20 per FULL game-hour of NEAR co-presence (the needs-decay pacing:
    // per-game-hour cadence, integer division — a partial hour accrues
    // nothing, a long co-presence accrues its full hours at once and the
    // caller folds the remainder into the next window).
    const std::uint64_t hours = elapsedGameSeconds / GAME_HOUR_SECONDS;
    if (hours == 0)
    {
        return;
    }
    const std::int64_t gain = static_cast<std::int64_t>(hours) * AFFINITY_CO_PRESENCE_PER_HOUR;

    // The SOURCE cap: proximity saturates at +4000. `cap = max(current,
    // CAP)` keeps the accrual from ever LOWERING a pair that sits above the
    // cap (trade/faction sources may have put it there) and from pushing one
    // past it; a negative pair (xenophobia base) climbs toward the cap.
    std::int64_t &value    = observer.affinity[other];
    const std::int64_t cap = std::max<std::int64_t>(value, AFFINITY_CO_PRESENCE_CAP);
    value                  = clampAffinity(std::min<std::int64_t>(value + gain, cap));
}

void addAffinityEvent(
    NpcState &observer, characterId other, std::int64_t delta
)
{
    // +100 per trade/gift event (AFFINITY_PER_TRADE_EVENT is the pinned call
    // site's constant; the signed parameter keeps R1 for future sources).
    // A zero-result entry on an absent pair is a recorded relationship with
    // no value change — the map entry is the pair's existence, harmless.
    std::int64_t &value = observer.affinity[other];
    value               = clampAffinity(value + delta);
}

void seedRelationship(
    NpcState &observer, characterId other, bool sameFaction, std::int64_t speciesBias
)
{
    // First-contact base: the same-faction base plus the species bias term
    // (card list order: "+same faction (+2000 base), ± species bias"). An
    // existing entry is never re-granted — the base is a creation value.
    if (observer.affinity.find(other) != observer.affinity.end())
    {
        return;
    }
    const std::int64_t base = (sameFaction ? AFFINITY_SAME_FACTION_BASE : 0) + speciesBias;
    observer.affinity.emplace(other, clampAffinity(base));
}

// ---- species bias (the 2.2 core's NpcState adapter) ----------------------------

std::int64_t speciesBiasFor(
    std::uint64_t observerXenophobiaBias, std::int64_t territoryCultureTowardOther
)
{
    // The wrap 2.2's DEVIATIONS row promises: the data-driven core
    // (xenophobiaBias + territory culture bias) with the observer's own
    // species row resolved by the caller through the registry.
    return speciesBiasToward(observerXenophobiaBias, territoryCultureTowardOther);
}

std::int64_t territoryCultureBiasToward(
    const IWorldQueries *world, territoryId observerTerritory, speciesId otherSpecies
)
{
    // 1.4's lawsAt walk through the world-queries seam (D-JUR-1); a null
    // world (the seam not wired — tests, headless slices) reads 0, the same
    // null-false pattern every D-SINK-1 seam keeps.
    if (world == nullptr)
    {
        return 0;
    }
    return world->cultureBiasToward(observerTerritory, otherSpecies);
}

// ---- witness qualification -------------------------------------------------------

bool canTestify(
    const Character &witness, const NpcState *witnessNpc, std::uint64_t now, const data::DataRegistry &data
)
{
    // alive && sapient && ageYears >= 16 (A-44) — the card's gate, verbatim.
    // See relationships.hpp for the per-term reading (the player is the
    // nullptr-NpcState case: adult by the v1 pin).
    if (witness.body.currentHp == 0)
    {
        return false; // the dead are removed from the store (3.3); 0 HP never testifies
    }
    if (!isSapient(data, witness.species))
    {
        return false; // "hunting deer creates no crime" — beasts are never witnesses
    }
    if (witnessNpc == nullptr)
    {
        return true; // the SP player: no NpcState, no age in v1 — an adult by the pin
    }
    return witnessNpc->ageYears(now) >= ADULT_YEARS;
}

Witness qualifyWitness(
    const Character &witness,
    const NpcState *witnessNpc,
    std::uint64_t now,
    const data::DataRegistry &data,
    characterId culpritId
)
{
    Witness w;
    w.id         = witness.id;
    w.canTestify = canTestify(witness, witnessNpc, now, data);

    if (witnessNpc != nullptr)
    {
        // ROLE_BASE_WEIGHT[w.role] (roles.json's baseReportWeight) + the
        // spawn integrity trait. Missing role key / role-less NPC: 0/0, the
        // deterministic "no basis" reading (the registry's own
        // unknown-key contract).
        if (const data::RoleData *role = data.role(witnessNpc->roleKey); role != nullptr)
        {
            w.roleBaseWeight = role->baseReportWeight;
        }
        w.integrity = witnessNpc->integrity;

        // affinity(w, culpritId): THIS witness's map entry toward the
        // culprit — the doc formula's input, 0 when the pair has none
        // (clamped again at the formula's door).
        const auto affinityIt = witnessNpc->affinity.find(culpritId);
        w.affinity            = affinityIt != witnessNpc->affinity.end() ? clampAffinity(affinityIt->second) : 0;
    }
    // witnessNpc == nullptr: the player — no role row, no integrity, no
    // affinity map; the fields keep their zeros (the doc formula then
    // yields weight 0 for an empty role, the honest value).

    return w;
}

// ---- the doc weight formula --------------------------------------------------------

std::uint32_t witnessReportWeight(
    std::uint32_t roleBaseWeight, std::uint64_t integrity, std::int64_t affinity
)
{
    // design reference verbatim, grouped left-to-right:
    //   max(0, ROLE_BASE_WEIGHT[w.role] * w.integrity / SCALE
    //           * (SCALE - affinity(w, culpritId)) / SCALE)
    // The affinity clamp is the card's pitfall note made code: the span term
    // is [0, 2*SCALE], never negative (a clamped-in affinity cannot produce
    // a negative product, so the max(0, ...) of the doc is the identity here
    // — and an UNclamped value > SCALE would zero the weight, which is why
    // the map clamps to ±SCALE and not to [0, SCALE]).
    const std::int64_t clamped = clampAffinity(affinity);
    const std::int64_t span    = static_cast<std::int64_t>(SCALE) - clamped; // [0, 2*SCALE]

    const std::uint64_t weighted
        = static_cast<std::uint64_t>(roleBaseWeight) * integrity / SCALE * static_cast<std::uint64_t>(span) / SCALE;

    // saturate(...) into u32 — the worst case (SCALE * SCALE / SCALE * 2*SCALE
    // / SCALE = 2*SCALE = 20000) is far inside the type; the saturate is the
    // doc's own shape, kept for exactly the reason the doc wrote it.
    constexpr std::uint64_t kU32Max = 0xFFFF'FFFFull;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(weighted, kU32Max));
}

} // namespace harrowmere::npc
