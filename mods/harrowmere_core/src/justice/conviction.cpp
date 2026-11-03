// conviction.cpp — task 4.3: the wide-arithmetic conviction pipeline, the
// jail map, surrender and escape. Every rule is card-verbatim (design
// reference §Justice); see conviction.hpp for the pins.

#include "justice/conviction.hpp"

#include <algorithm>
#include <cassert>

#include "character/character.hpp" // Character/Progression (currency is i64 — the doc's rule)
#include "core/clock.hpp"          // DAY_LENGTH_SECONDS (A-3, the sentence math)
#include "core/constants.hpp"      // A-77..A-83, SCALE
#include "core/game_events.hpp"    // CrimeRecorded payload (the escape's fresh evidence)
#include "core/math.hpp"           // mulDiv128 (0.2's R3 wide helper), applyXpDelta (R4)
#include "core/world_context.hpp"
#include "data/registry.hpp"           // DataRegistry::crime (severity / notorietyGain)
#include "justice/cases.hpp"           // findEvidence (surrender's case resolution)
#include "justice/evidence.hpp"        // Report + the crime-commit family (the escape evidence)
#include "justice/reputation.hpp"      // changeReputation (A-78), getReputationMultiplier (D-REP-2), addNotoriety
#include "justice/warrants.hpp"        // the both-maps cleanup + the jurisdiction walk
#include "progression/progression.hpp" // levelCosts() (A-7/A-8)
#include "world/world.hpp"             // WorldStore::isSafeTerritory (the zone read)

namespace harrowmere::justice
{

std::uint64_t getZoneMultiplier(
    const WorldContext &ctx, territoryId territory
)
{
    // common.md §Death's verbatim helper. Reads the EVIDENCE territory — the
    // doc note: fleeing a safe zone before conviction does not dodge the
    // doubling (the pinned test convicts across a move).
    assert(ctx.worldStore != nullptr && "getZoneMultiplier: no WorldStore bound");
    if (ctx.worldStore == nullptr)
    {
        return SCALE;
    }
    return ctx.worldStore->isSafeTerritory(territory) ? 2 * SCALE : SCALE;
}

std::int64_t fineFor(
    const Evidence &e, WorldContext &ctx
)
{
    assert(ctx.data != nullptr && "fineFor: no DataRegistry bound");
    if (ctx.data == nullptr)
    {
        return 0;
    }
    const std::uint64_t severity = ctx.data->crime(e.type).severity;
    return static_cast<std::int64_t>(severity * FINE_CURRENCY_PER_POINT);
}

void applySentence(
    const Evidence &e, WorldContext &ctx
)
{
    assert(
        ctx.justiceStore != nullptr && ctx.data != nullptr && ctx.characters != nullptr
        && "applySentence: unbound context"
    );
    if (ctx.justiceStore == nullptr || ctx.data == nullptr || ctx.characters == nullptr)
    {
        return;
    }
    JusticeStore &js = *ctx.justiceStore;

    // A-79/A-80: sentence = 1 game-day + severity / 5 game-days (the row's
    // own form: "1 per severity point ×5").
    const std::uint64_t severity = ctx.data->crime(e.type).severity;
    const std::uint64_t sentenceDays
        = SENTENCE_BASE_DAYS + severity * SENTENCE_SEVERITY_DAYS / SENTENCE_SEVERITY_DIVISOR;
    js.jailUntil[e.culpritId] = ctx.now + sentenceDays * core::DAY_LENGTH_SECONDS;

    // The fine is i64 arithmetic on an i64 field — it MAY go negative (the
    // doc's own rule; on a u64 field it would wrap).
    ctx.characters->get(e.culpritId).progression.currency -= fineFor(e, ctx);
    // SP: the player teleports to the territory jail point (7.2 handles the
    // scene; D-JAIL-1 — an NPC "body activates at next login" has no meaning).
}

bool isArrested(
    JusticeStore &justice, characterId c, std::uint64_t now
)
{
    // Design reference verbatim — lazy purge on read.
    const auto it = justice.jailUntil.find(c);
    if (it == justice.jailUntil.end())
    {
        return false;
    }
    if (it->second <= now)
    {
        justice.jailUntil.erase(it);
        return false;
    }
    return true;
}

std::uint64_t remainingSentence(
    JusticeStore &justice, characterId c, std::uint64_t now
)
{
    const auto it = justice.jailUntil.find(c);
    if (it == justice.jailUntil.end() || it->second <= now)
    {
        return 0;
    }
    return it->second - now;
}

void convict(
    Evidence &e, WorldContext &ctx, WarrantResolved::How warrantVerdict
)
{
    if (e.status == EvidenceStatus::CONVICTED)
    {
        return; // one conviction per case (the doc's status guard)
    }
    e.status = EvidenceStatus::CONVICTED;
    assert(
        ctx.justiceStore != nullptr && ctx.worldStore != nullptr && ctx.characters != nullptr && ctx.data != nullptr
        && "convict: unbound context"
    );
    if (ctx.justiceStore == nullptr || ctx.worldStore == nullptr || ctx.characters == nullptr || ctx.data == nullptr)
    {
        return;
    }
    JusticeStore &js   = *ctx.justiceStore;
    Character &culprit = ctx.characters->get(e.culpritId);

    // The design reference's wide path (R3): the triple product rides the
    // __int128 helper — the level-100 safe-zone worst case overflows i64 on
    // the intermediate but fits the quotient easily.
    const std::int64_t debt         = static_cast<std::int64_t>(mulDiv128(
        CONVICTION_PENALTY_FACTOR,
        static_cast<std::int64_t>(levelCosts()[culprit.progression.level]),
        static_cast<std::int64_t>(getZoneMultiplier(ctx, e.territory))
            * static_cast<std::int64_t>(getReputationMultiplier(js, e.culpritId, e.territory, ctx.now)),
        SCALE * SCALE
    ));
    culprit.progression.availableXp = applyXpDelta(culprit.progression.availableXp, -debt);

    // A-78 at the EVIDENCE territory (the zone multiplier above is where §Safe
    // Zones' doubling lands — the card's verbatim form carries the flat loss).
    changeReputation(e.culpritId, e.territory, -CONVICTION_REPUTATION_LOSS, ctx);

    // Warrant cleanup: delete from BOTH maps (the doc invariant), publishing
    // the terminal verdict (Convicted, or Surrendered through the surrender
    // path).
    resolveWarrantsForEvidence(e.id, e.culpritId, warrantVerdict, ctx);

    addNotoriety(js, e.culpritId, static_cast<std::int32_t>(ctx.data->crime(e.type).notorietyGain));

    applySentence(e, ctx);
}

bool surrender(
    characterId player, territoryId guardTerritory, WorldContext &ctx
)
{
    assert(ctx.justiceStore != nullptr && ctx.worldStore != nullptr && "surrender: unbound context");
    if (ctx.justiceStore == nullptr || ctx.worldStore == nullptr)
    {
        return false;
    }

    // "A wanted player can surrender to any guard of the issuing territory":
    // every live warrant of the player active in the guard's territory
    // converts to conviction (the standard sentence); any manhunt for them
    // ends (5.4's completion path — the WarrantResolved fact is the hook).
    const std::vector<Warrant> live = activeWarrantsOf(ctx, player);
    bool any                        = false;
    for (const Warrant &w : live)
    {
        if (!warrantActiveIn(w, *ctx.worldStore, guardTerritory))
        {
            continue; // the guard's territory does not cover the issuing one
        }
        if (Evidence *e = findEvidence(*ctx.justiceStore, w.evidenceId); e != nullptr)
        {
            convict(*e, ctx, WarrantResolved::How::Surrendered); // the cleanup deletes the warrant
            any = true;
        } else
        {
            // No live case behind the warrant (already resolved elsewhere):
            // the surrender still clears the warrant itself.
            payWarrant(w.id, ctx); // deletes from both maps; the bounty is not claimed
            any = true;
        }
    }
    return any;
}

bool attemptEscape(
    characterId escapee, WorldContext &ctx
)
{
    assert(
        ctx.justiceStore != nullptr && ctx.worldStore != nullptr && ctx.characters != nullptr
        && "attemptEscape: unbound context"
    );
    if (ctx.justiceStore == nullptr || ctx.worldStore == nullptr || ctx.characters == nullptr)
    {
        return false;
    }
    JusticeStore &js = *ctx.justiceStore;
    if (!isArrested(js, escapee, ctx.now))
    {
        return false; // "player action while isArrested" — nothing to escape
    }
    const Character &escapeeChar = ctx.characters->get(escapee);

    // The jail territory: the escapee's most recent CONVICTED case's territory
    // (the arrest's ground); a live position's walk is the fallback.
    territoryId jailTerritory = ctx.worldStore->territoryAt(escapeeChar.body.position);
    for (auto &[territory, cases] : js.evidenceStore)
    {
        for (const Evidence &e : cases)
        {
            if (e.status == EvidenceStatus::CONVICTED && e.culpritId == escapee)
            {
                jailTerritory = territory; // insertion order: the LAST conviction is the most recent
            }
        }
    }

    // The skill check (A-81): pct = clamp(eff * 2 - DIFFICULTY + 2000, 200,
    // 9000) over the ESCAPE effectiveness. `eff` rides the SCALE grid — the
    // A-81 why-note ("a dedicated escapee (skill >= 40 eff) succeeds ~30-60%")
    // only reproduces with raw points x 100 (40 points -> pct 4000 = 40%; the
    // 30-60% band is raw 35..50). The pinned "justice" stream (0.3).
    const std::uint64_t effScale = getSkillEffectiveness(escapeeChar, SkillType::ESCAPE) * 100; // points -> SCALE grid
    const std::int64_t pct       = clampScalar<std::int64_t>(
        static_cast<std::int64_t>(effScale * 2) - static_cast<std::int64_t>(ESCAPE_DIFFICULTY)
            + static_cast<std::int64_t>(ESCAPE_PCT_BONUS),
        static_cast<std::int64_t>(ESCAPE_PCT_MIN),
        static_cast<std::int64_t>(ESCAPE_PCT_MAX)
    );
    assert(ctx.rng != nullptr && "attemptEscape: no SeedRng bound (the justice stream is a game-rule roll)");
    const std::uint32_t roll
        = ctx.rng != nullptr ? ctx.rng->pct("justice") : UINT32_MAX; // release: fails deterministically
    const bool success = static_cast<std::uint64_t>(roll) < static_cast<std::uint64_t>(pct);

    // The attempt leaves fresh OBSTRUCTION evidence at the jail territory in
    // BOTH branches (A-82); a failure stacks ESCAPE_FAIL_WEIGHT (A-83) onto
    // it — "failed attempts still build the case". The commit path caps,
    // stores and publishes.
    std::vector<Report> escapeReports{Report{0, ESCAPE_EVIDENCE_WEIGHT, false, false}}; // anonymous score
    if (!success)
    {
        escapeReports.push_back(Report{0, ESCAPE_FAIL_WEIGHT, false, false}); // the failed attempt's stack
    }
    commitEvidence(CrimeType::OBSTRUCTION, escapee, 0, jailTerritory, ctx, std::move(escapeReports), 0);

    if (success)
    {
        js.jailUntil.erase(escapee); // the records clear; the fresh case remains
    } else
    {
        // "sentence doubled (jailUntil extended by the remaining term)".
        const std::uint64_t remaining = remainingSentence(js, escapee, ctx.now);
        js.jailUntil[escapee]         = ctx.now + remaining * 2;
    }
    return success;
}

void recordBreakout(
    characterId rescuer, characterId prisoner, WorldContext &ctx
)
{
    // 4.1's accomplice list, pinned trigger: "release or heal of an arrested
    // character". The evidence lands vs the RESCUER at the prisoner's ground.
    if (!ctx.characters->exists(rescuer))
    {
        return;
    }
    const Position p = ctx.characters->exists(prisoner) ? ctx.characters->get(prisoner).body.position : Position{};
    recordAccomplice(rescuer, prisoner, CrimeType::SMUGGLING, p, ctx);
}

} // namespace harrowmere::justice
