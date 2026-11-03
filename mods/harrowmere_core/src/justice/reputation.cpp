// reputation.cpp — task 4.4's implementation. Every rule is card-verbatim
// (design reference §Reputation); see reputation.hpp for the pinned semantics.

#include "justice/reputation.hpp"

#include <algorithm>
#include <cassert>

#include "core/constants.hpp" // A-84..A-88, SCALE, D-REP-2's term
#include "core/math.hpp"      // clampScalar (R1's i64 grid)
#include "core/world_context.hpp"
#include "justice/justice_store.hpp"
#include "world/world.hpp" // WorldStore::chainToRoot (D-JUR-1 — the one walk)

namespace harrowmere::justice
{

void decayIfDue(
    Reputation &rep, std::uint64_t now
)
{
    // Whole decay quanta since the last stamp. A now below the stamp (a clock
    // rewind in a loaded save) applies zero steps — no u64 underflow (R1).
    if (now <= rep.lastDecayTime)
    {
        return;
    }
    const std::uint64_t steps = (now - rep.lastDecayTime) / REPUTATION_DECAY_INTERVAL;
    if (steps == 0)
    {
        return;
    }

    // Per-territory values decay TOWARD ZERO, sign-aware (the doc's
    // `v -= sign(v) * min(|v|, DECAY * steps)`): a hated name heals up, a
    // beloved one heals down, at A-85 per game-day.
    const std::int64_t decayBudget = static_cast<std::int64_t>(REPUTATION_DECAY) * static_cast<std::int64_t>(steps);
    for (auto &[territory, value] : rep.perTerritory)
    {
        const std::int64_t magnitude = std::min<std::int64_t>(std::abs(static_cast<std::int64_t>(value)), decayBudget);
        value -= static_cast<std::int32_t>((value >= 0 ? 1 : -1) * magnitude);
    }

    // Notoriety drops per step, floored at zero (A-86 — infamy never goes
    // negative through decay).
    rep.notoriety = static_cast<std::int32_t>(std::max<std::int64_t>(
        0,
        static_cast<std::int64_t>(rep.notoriety)
            - static_cast<std::int64_t>(NOTORIETY_DECAY) * static_cast<std::int64_t>(steps)
    ));

    // The stamp advances by exactly the applied steps — a partial interval
    // carries over (the lazy catch-up stays exact across reads).
    rep.lastDecayTime += steps * REPUTATION_DECAY_INTERVAL;
}

void changeReputation(
    characterId c, territoryId origin, std::int32_t delta, WorldContext &ctx
)
{
    assert(ctx.justiceStore != nullptr && "changeReputation: no JusticeStore bound in the context");
    assert(ctx.worldStore != nullptr && "changeReputation: no WorldStore bound in the context");
    if (ctx.justiceStore == nullptr || ctx.worldStore == nullptr)
    {
        return; // release: deterministic no-op (the wiring is a programmer error)
    }

    Reputation &rep    = ctx.justiceStore->reputations[c]; // the read path creates the record (the doc's map read)
    const auto nowCopy = ctx.now;
    decayIfDue(rep, nowCopy);

    // The propagation walk: origin first, then every ancestor, delta halving
    // per level (A-88). chainToRoot is 1.4's ONE walk (D-JUR-1) — origin..root,
    // empty only for the null territory (nothing propagates off the map).
    const std::vector<territoryId> chain = ctx.worldStore->chainToRoot(origin);
    std::int64_t factor                  = static_cast<std::int64_t>(SCALE);
    for (const territoryId t : chain)
    {
        // i64 arithmetic through the multiply, clamp to ±A-87, store back into
        // the i32 map (R1: delta x factor is the overflow-prone spot).
        const std::int64_t current = static_cast<std::int64_t>(rep.perTerritory[t]);
        const std::int64_t moved   = clampScalar<std::int64_t>(
            current + static_cast<std::int64_t>(delta) * factor / static_cast<std::int64_t>(SCALE),
            -MAX_REPUTATION,
            MAX_REPUTATION
        );
        rep.perTerritory[t] = static_cast<std::int32_t>(moved);
        factor = factor * static_cast<std::int64_t>(REPUTATION_PROPAGATION) / static_cast<std::int64_t>(SCALE);
    }
}

// ---- readers (decay-before-read) --------------------------------------------------

std::int32_t getNotoriety(
    JusticeStore &justice, characterId c, std::uint64_t now
)
{
    Reputation &rep = justice.reputations[c];
    decayIfDue(rep, now);
    return rep.notoriety;
}

std::int32_t getReputationIn(
    JusticeStore &justice, characterId c, territoryId territory, std::uint64_t now
)
{
    Reputation &rep = justice.reputations[c];
    decayIfDue(rep, now);
    const auto it = rep.perTerritory.find(territory);
    return it == rep.perTerritory.end() ? 0 : it->second; // no record = neutral (the honest zero)
}

std::uint64_t getReputationMultiplier(
    JusticeStore &justice, characterId c, territoryId territory, std::uint64_t now
)
{
    const std::int32_t rep = getReputationIn(justice, c, territory, now); // decays first
    // D-REP-2 verbatim: SCALE + max(0, -rep) * 2000 / SCALE. Only NEGATIVE
    // reputation worsens the debt; neutral/positive reads exactly SCALE.
    const std::int64_t negative = -static_cast<std::int64_t>(rep);
    const std::int64_t penalty  = negative > 0 ? negative : 0;
    return static_cast<std::uint64_t>(
        static_cast<std::int64_t>(SCALE) + penalty * REPUTATION_DEBT_MULTIPLIER_TERM / static_cast<std::int64_t>(SCALE)
    );
}

void addNotoriety(
    JusticeStore &justice, characterId c, std::int32_t gain
)
{
    Reputation &rep = justice.reputations[c];
    // The writer never decays (decay is the readers' job); i64 through the
    // add so a saturated i32 cannot wrap (R1).
    rep.notoriety = static_cast<std::int32_t>(clampScalar<std::int64_t>(
        static_cast<std::int64_t>(rep.notoriety) + static_cast<std::int64_t>(gain), 0, INT32_MAX
    ));
}

} // namespace harrowmere::justice
