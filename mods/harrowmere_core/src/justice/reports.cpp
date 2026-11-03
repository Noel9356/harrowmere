// reports.cpp — task 4.1's report side: the witness loop (through 3.9's API —
// the card's Done-when: "no justice-side reimplementation"), the report cap,
// the delayed-report family (discovered bodies, missing persons, vendor
// marks), bribery, accomplices and the victim self-report.

#include <algorithm>
#include <cassert>

#include "character/character.hpp"     // Character (the qualification reads)
#include "character/species.hpp"       // isSapient (the missing-persons sweep's gate)
#include "core/clock.hpp"              // DAY_LENGTH_SECONDS / GAME_HOUR_SECONDS (A-3)
#include "core/constants.hpp"          // A-60..A-69, the night band, SCALE
#include "core/game_events.hpp"        // (payload types ride the commit path)
#include "core/math.hpp"               // clampScalar
#include "core/world_context.hpp"      // WorldContext
#include "justice/evidence.hpp"        // Report/Evidence + commitEvidence
#include "npc/relationships.hpp"       // 3.9's API: canTestify / qualifyWitness / witnessReportWeight
#include "npc/state.hpp"               // NpcState (the affinity half of the identity answer)
#include "progression/progression.hpp" // getSkillEffectiveness (the bribery roll)
#include "world/world.hpp"             // WorldStore: territoryAt (the MISSING flag's ground)

namespace harrowmere::justice
{

std::vector<Report> witnessReports(
    characterId culprit, const Position &p, WorldContext &ctx
)
{
    std::vector<Report> out;
    // The spatial half rides the perception seam (§4.6's trace: "witness
    // reports gathered via spatial query perceiveWithin(position,
    // WITNESS_RADIUS)"); an unbound seam answers no witnesses (D-SINK-1).
    if (ctx.perception == nullptr)
    {
        return out;
    }

    for (const characterId witnessId : ctx.perception->perceiveWithin(p, WITNESS_RADIUS_M))
    {
        if (witnessId == culprit)
        {
            continue; // the culprit never witnesses their own crime
        }
        assert(ctx.characters != nullptr && ctx.data != nullptr && "witnessReports: unbound context");
        if (ctx.characters == nullptr || ctx.data == nullptr || !ctx.characters->exists(witnessId))
        {
            continue;
        }
        const Character &witnessChar = ctx.characters->get(witnessId);
        // The player carries no NpcState (D-STORE-1) — a null sidecar means the player.
        const NpcState *witnessNpc
            = (ctx.npcs != nullptr && ctx.npcs->contains(witnessId)) ? &ctx.npcs->get(witnessId) : nullptr;

        // Qualification (alive && sapient && adult) is 3.9's gate — children
        // and non-sapients skip (TestWitness_ChildAndNonSapientSkip).
        if (!npc::canTestify(witnessChar, witnessNpc, ctx.now, *ctx.data))
        {
            continue;
        }
        const npc::Witness qualified = npc::qualifyWitness(witnessChar, witnessNpc, ctx.now, *ctx.data, culprit);

        // The weight formula is 3.9's pinned function (the doc's
        // `max(0, ROLE_BASE_WEIGHT * integrity / SCALE * (SCALE - affinity) /
        // SCALE)` — enemies testify harder); the identity answer rides the
        // seam's knowsIdentity (the sighting/affinity rule, no reimplementation).
        const std::uint32_t weight
            = npc::witnessReportWeight(qualified.roleBaseWeight, qualified.integrity, qualified.affinity);
        const bool knows = ctx.perception->knowsIdentity(witnessId, culprit);
        out.push_back(Report{witnessId, weight, knows, false});
    }
    return out;
}

void capReports(
    Evidence &e
)
{
    // Design reference verbatim: keep the heaviest REPORT_CAP (A-61) reports,
    // excess merged into aggregateReportWeight. The sort is STABLE — equal
    // weights keep insertion order (the determinism pin, R7).
    std::stable_sort(e.reports.begin(), e.reports.end(), [](const Report &a, const Report &b) {
        return a.weight > b.weight;
    });
    for (std::size_t i = REPORT_CAP; i < e.reports.size(); ++i)
    {
        e.aggregateReportWeight = static_cast<std::uint32_t>(clampScalar<std::uint64_t>(
            static_cast<std::uint64_t>(e.aggregateReportWeight) + e.reports[i].weight, 0, SCALE
        ));
    }
    if (e.reports.size() > REPORT_CAP)
    {
        e.reports.resize(REPORT_CAP);
    }
}

// ---- delayed reports ------------------------------------------------------------

void appendDiscoveredBodyReport(
    characterId victim, characterId discoverer, WorldContext &ctx
)
{
    assert(ctx.justiceStore != nullptr && "appendDiscoveredBodyReport: no JusticeStore bound");
    if (ctx.justiceStore == nullptr)
    {
        return;
    }
    JusticeStore &js = *ctx.justiceStore;

    // The ORIGINAL murder evidence for this victim: the first matching case
    // (a victim dies once) that is not ARCHIVED — the doc's exact condition.
    Evidence *target = nullptr;
    for (auto &[territory, cases] : js.evidenceStore)
    {
        for (Evidence &e : cases)
        {
            if (e.type == CrimeType::MURDER && e.victimId == victim && e.status != EvidenceStatus::ARCHIVED)
            {
                target = &e;
                break;
            }
        }
        if (target != nullptr)
        {
            break;
        }
    }
    if (target == nullptr)
    {
        return; // no case behind the body (a natural death, or the window closed)
    }

    // The discoverer's report: DELAYED_BODY_WEIGHT (A-63), identity knowledge
    // per 3.9 (does the discoverer know the culprit's face? — the seam's rule).
    const bool knows
        = discoverer != 0 && ctx.perception != nullptr && ctx.perception->knowsIdentity(discoverer, target->culpritId);
    target->reports.push_back(Report{discoverer, DELAYED_BODY_WEIGHT, knows, false});

    // The MISSING flag pays out here: a flagged person found dead adds
    // MISSING_WEIGHT (A-65) to the matching evidence and clears the flag.
    if (const auto missing = js.missingPersons.find(victim); missing != js.missingPersons.end())
    {
        target->reports.push_back(Report{0, MISSING_WEIGHT, false, false});
        js.missingPersons.erase(missing);
    }
    capReports(*target);
}

void markMissingPerson(
    characterId victim, territoryId territory, WorldContext &ctx
)
{
    assert(ctx.justiceStore != nullptr && "markMissingPerson: no JusticeStore bound");
    if (ctx.justiceStore == nullptr)
    {
        return;
    }
    MissingPerson &flag = ctx.justiceStore->missingPersons[victim];
    if (flag.territory == 0)
    {
        flag.territory = territory; // the first stamp wins (the sweep re-stamps nothing)
        flag.markedAt  = ctx.now;
    }
}

void sweepMissingPersons(
    WorldContext &ctx
)
{
    assert(
        ctx.justiceStore != nullptr && ctx.npcs != nullptr && ctx.data != nullptr
        && "sweepMissingPersons: unbound context"
    );
    if (ctx.justiceStore == nullptr || ctx.npcs == nullptr || ctx.data == nullptr)
    {
        return;
    }
    const std::uint64_t window = MISSING_DAYS * core::DAY_LENGTH_SECONDS;

    // The doc's condition per sapient NPC: no sighting for A-64 days. A
    // character younger than the window cannot be missing yet. A fresh
    // sighting clears an existing flag (found alive); its absence raises the
    // flag on the person's territory. The runtime driver that calls this
    // sweep joins with the 4.5 perception loop (nothing records sightings per
    // tick yet — the DEVIATIONS row).
    for (const auto &[id, npcState] : ctx.npcs->all())
    {
        if (!ctx.characters->exists(id))
        {
            continue;
        }
        const Character &c = ctx.characters->get(id);
        if (!isSapient(*ctx.data, c.species))
        {
            continue; // animals and monsters are nobody's missing persons
        }
        if (ctx.now < npcState.birthWorldTime + window)
        {
            continue; // too young to have gone unnoticed for a full window
        }
        const std::uint64_t lastSeen = ctx.perception != nullptr ? ctx.perception->lastSightingOf(id) : 0;
        if (lastSeen != 0 && ctx.now - lastSeen <= window)
        {
            ctx.justiceStore->missingPersons.erase(id); // seen again: found alive
            continue;
        }
        MissingPerson &flag = ctx.justiceStore->missingPersons[id];
        if (flag.territory == 0)
        {
            flag.territory
                = npcState.homeTerritory != 0 ? npcState.homeTerritory : ctx.worldStore->territoryAt(c.body.position);
            flag.markedAt = ctx.now;
        }
    }
}

void reportMarkedSale(
    characterId seller, WorldContext &ctx
)
{
    assert(ctx.justiceStore != nullptr && "reportMarkedSale: no JusticeStore bound");
    if (ctx.justiceStore == nullptr)
    {
        return;
    }
    JusticeStore &js = *ctx.justiceStore;

    // "Selling a guardMarked or reportedStolen item flags the vendor
    // transaction": the seller's most recent open THEFT evidence gains the
    // anonymous VENDOR_MARK_WEIGHT (A-66) report — score, never identity
    // (the doc's aggregate-only rule; source 0 carries no face).
    Evidence *target = nullptr;
    for (auto &[territory, cases] : js.evidenceStore)
    {
        for (Evidence &e : cases)
        {
            if (e.type == CrimeType::THEFT && e.culpritId == seller && e.status != EvidenceStatus::ARCHIVED
                && e.status != EvidenceStatus::CONVICTED)
            {
                target = &e; // insertion order: the LAST match is the most recent
            }
        }
    }
    if (target == nullptr)
    {
        return;
    }
    target->reports.push_back(Report{0, VENDOR_MARK_WEIGHT, false, false});
    capReports(*target);
}

// ---- bribery ----------------------------------------------------------------------

BriberyOutcome attemptBribery(
    characterId briber, characterId witness, WorldContext &ctx
)
{
    assert(ctx.justiceStore != nullptr && ctx.characters != nullptr && "attemptBribery: unbound context");
    if (ctx.justiceStore == nullptr || ctx.characters == nullptr)
    {
        return BriberyOutcome::NoReport;
    }
    JusticeStore &js = *ctx.justiceStore;

    // The bribe surface: the witness's active report on a WANTED-or-earlier
    // case of the briber. Deterministic scan: territory-id ascending, case
    // insertion order, report order — the LAST matching open report wins (the
    // freshest case). An already-bribed report auto-fails (bribed = the
    // surface is gone).
    Report *target = nullptr;
    for (auto &[territory, cases] : js.evidenceStore)
    {
        for (Evidence &e : cases)
        {
            if (e.culpritId != briber)
            {
                continue;
            }
            if (e.status != EvidenceStatus::UNKNOWN && e.status != EvidenceStatus::INVESTIGATING
                && e.status != EvidenceStatus::WANTED)
            {
                continue; // WANTED-or-earlier only (a convicted case cannot be bribed)
            }
            for (Report &r : e.reports)
            {
                if (r.source == witness && !r.bribed)
                {
                    target = &r;
                }
            }
        }
    }
    if (target == nullptr)
    {
        return BriberyOutcome::NoReport;
    }

    // Success chance = clamp(PERSUASION_eff * 2 - integrity, 500, 9000) —
    // the witness's integrity (their price) against the briber's silver
    // tongue. `eff` rides the SCALE grid (the A-81 why-note pins the same
    // reading for the escape check: raw points x 100 per point), so a
    // 50-point persuasion faces a 5000-integrity witness at 50%. The pinned
    // "justice" stream (0.3); a null rng is a wiring error that fails
    // deterministically in release (the manaBacklash pattern).
    const Character &briberChar = ctx.characters->get(briber);
    const NpcState *witnessNpc
        = (ctx.npcs != nullptr && ctx.npcs->contains(witness)) ? &ctx.npcs->get(witness) : nullptr;
    const std::uint64_t integrity = witnessNpc != nullptr ? witnessNpc->integrity : 0;
    const std::uint64_t effScale
        = getSkillEffectiveness(briberChar, SkillType::PERSUASION) * 100; // points -> SCALE grid
    const std::int64_t pct = clampScalar<std::int64_t>(
        static_cast<std::int64_t>(effScale * 2) - static_cast<std::int64_t>(integrity), 500, 9000
    );
    assert(ctx.rng != nullptr && "attemptBribery: no SeedRng bound (the justice stream is a game-rule roll)");
    const std::uint32_t roll = ctx.rng != nullptr ? ctx.rng->pct("justice") : UINT32_MAX;
    if (static_cast<std::uint64_t>(roll) < static_cast<std::uint64_t>(pct))
    {
        // Success: halve THAT report's weight, ONCE (the bribed flag is the
        // once — every later attempt on it finds the surface gone).
        target->weight /= 2;
        target->bribed = true;
        return BriberyOutcome::Bribed;
    }

    // Failure: a fresh OBSTRUCTION evidence against the briber, carrying the
    // doc's literal report `Report{briberId, OBSTRUCTION_WEIGHT, true}` — the
    // record that identifies the briber. The victim is the witness they tried
    // to buy.
    commitEvidence(
        CrimeType::OBSTRUCTION,
        briber,
        witness,
        ctx.worldStore->territoryAt(ctx.characters->get(briber).body.position),
        ctx,
        std::vector<Report>{Report{briber, OBSTRUCTION_WEIGHT, true, false}},
        0
    );
    return BriberyOutcome::Failed;
}

// ---- accomplices -------------------------------------------------------------------

void recordAccomplice(
    characterId helper, characterId aidedCulprit, CrimeType via, const Position &p, WorldContext &ctx
)
{
    assert(
        ctx.justiceStore != nullptr && ctx.worldStore != nullptr && ctx.characters != nullptr
        && "recordAccomplice: unbound context"
    );
    if (ctx.justiceStore == nullptr || ctx.worldStore == nullptr || ctx.characters == nullptr)
    {
        return;
    }
    if (!ctx.characters->exists(helper))
    {
        return; // only live characters aid (and only players generate evidence — commitEvidence's store accepts the
                // record either way, the doc's gate rides recordCrime)
    }
    // The doc's pinned v1 trigger set routes here from the action sites (the
    // heal path, the checkpoint walk, the flagged sale); this function IS the
    // evidence — witnessed at the aid position like any scene, type = the
    // caller's via (SMUGGLING for the aid list). "Watching is never punished":
    // nothing here fires on mere presence — the caller owns the trigger.
    commitEvidence(
        via,
        helper,
        aidedCulprit,
        ctx.worldStore->territoryAt(p),
        ctx,
        witnessReports(helper, p, ctx),
        scorePhysical(p, via, ctx)
    );
}

// ---- victim self-report --------------------------------------------------------------

bool selfReportVictim(
    characterId victim, WorldContext &ctx
)
{
    assert(ctx.justiceStore != nullptr && "selfReportVictim: no JusticeStore bound");
    if (ctx.justiceStore == nullptr)
    {
        return false;
    }
    JusticeStore &js = *ctx.justiceStore;

    // The matching murder/assault evidence: the victim's most recent open
    // case, within SELF_REPORT_WINDOW_DAYS (A-68) of the death (the evidence's
    // worldTime — murder records at the kill).
    Evidence *target           = nullptr;
    const std::uint64_t window = SELF_REPORT_WINDOW_DAYS * core::DAY_LENGTH_SECONDS;
    for (auto &[territory, cases] : js.evidenceStore)
    {
        for (Evidence &e : cases)
        {
            if (e.victimId != victim || (e.type != CrimeType::MURDER && e.type != CrimeType::ASSAULT))
            {
                continue;
            }
            if (e.status == EvidenceStatus::ARCHIVED || e.status == EvidenceStatus::CONVICTED)
            {
                continue;
            }
            if (ctx.now < e.worldTime || ctx.now - e.worldTime > window)
            {
                continue;
            }
            target = &e; // insertion order: the LAST match is the most recent
        }
    }
    if (target == nullptr)
    {
        return false;
    }
    // "adds score, never satisfies identity": knowsIdentity = false (and 4.2's
    // hasIdentity would reject a player source anyway — both doors stay shut).
    target->reports.push_back(Report{victim, SELF_REPORT_WEIGHT, false, false});
    capReports(*target);
    return true;
}

} // namespace harrowmere::justice
