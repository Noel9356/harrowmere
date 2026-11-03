// cases.cpp — task 4.2's case half: the UNKNOWN -> ARCHIVED/CONVICTED state
// machine, the score/threshold math, the daily backstop and the player-near
// case ticks. Every rule is card-verbatim; see cases.hpp for the pins.

#include "justice/cases.hpp"

#include <algorithm>
#include <cassert>

#include "character/store.hpp"     // CharacterStore::isPlayer (the identity rule)
#include "core/clock.hpp"          // DAY_LENGTH_SECONDS / GAME_HOUR_SECONDS (A-3)
#include "core/constants.hpp"      // A-70..A-72, A-75/A-76, SCALE
#include "core/game_events.hpp"    // (payload types ride world_context's seam)
#include "core/world_context.hpp"  // WorldContext
#include "data/laws.hpp"           // the complete LawSet (suppression/threshold reads)
#include "data/registry.hpp"       // DataRegistry (the default-threshold fallback)
#include "justice/evidence.hpp"    // the complete Evidence + Report
#include "justice/enforcement.hpp" // notifyCaseStatusChanged (the signal-before-force hook)
#include "justice/reputation.hpp"  // getNotoriety (caseThreshold's repeat-offender arm)
#include "justice/warrants.hpp"    // issueWarrant / hasWarrantIn (the warrant arm)
#include "world/world.hpp"         // WorldStore walks (D-JUR-1) + Territory/CompactRecord

namespace harrowmere::justice
{

std::uint32_t suppressionFor(
    const data::LawSet &laws, SocialClassId cls, CrimeType type
)
{
    // D-CORRUPT-1: the LawSet's suppression value for (class, crime); missing
    // entry = SCALE (no damping) — the loader's own pinned default.
    const auto it = laws.suppression.find(cls);
    if (it == laws.suppression.end())
    {
        return static_cast<std::uint32_t>(SCALE);
    }
    return it->second[static_cast<std::size_t>(type)];
}

std::uint32_t caseScore(
    const Evidence &e, WorldContext &ctx
)
{
    assert(ctx.worldStore != nullptr && "caseScore: no WorldStore bound");
    if (ctx.worldStore == nullptr)
    {
        return 0;
    }
    std::uint64_t total
        = static_cast<std::uint64_t>(e.physicalScore) + static_cast<std::uint64_t>(e.aggregateReportWeight);
    for (const Report &r : e.reports)
    {
        total += r.weight;
    }
    const SocialClassId cls = ctx.worldStore->socialClassOf(e.culpritId, e.territory);
    const std::uint32_t sup = suppressionFor(ctx.worldStore->lawsAt(e.territory), cls, e.type);
    // R3 verbatim: widen BEFORE the multiply — suppression x total is the
    // overflow-prone spot.
    return static_cast<std::uint32_t>(total * sup / SCALE);
}

std::uint32_t caseThreshold(
    const Evidence &e, WorldContext &ctx
)
{
    assert(ctx.worldStore != nullptr && ctx.justiceStore != nullptr && "caseThreshold: unbound context");
    if (ctx.worldStore == nullptr || ctx.justiceStore == nullptr)
    {
        return 0;
    }
    // The territory's threshold for the crime; a 0 entry means "use
    // CrimeData.defaultThreshold" (the loader's pinned fallback semantics).
    std::uint32_t threshold = ctx.worldStore->lawsAt(e.territory).convictionThreshold[static_cast<std::size_t>(e.type)];
    if (threshold == 0 && ctx.data != nullptr)
    {
        threshold = ctx.data->crime(e.type).defaultThreshold;
    }
    // Repeat offenders convict on weaker evidence: the threshold scales down
    // by (SCALE + notoriety) — 4.4's read (decayed first), R3's widen.
    const std::uint64_t notoriety = static_cast<std::uint64_t>(getNotoriety(*ctx.justiceStore, e.culpritId, ctx.now));
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(threshold) * SCALE / (SCALE + notoriety));
}

bool hasIdentity(
    const Evidence &e, WorldContext &ctx
)
{
    // A-70: overwhelming physical evidence alone identifies.
    if (e.physicalScore >= PHYSICAL_IDENTITY_MIN)
    {
        return true;
    }
    // Only NPC reports satisfy identity: source 0 (ward/anonymous) never
    // counts, and a PLAYER report (the self-report, A-69) never does. The
    // merged aggregate carries no identity by construction.
    assert(ctx.characters != nullptr && "hasIdentity: no CharacterStore bound");
    if (ctx.characters == nullptr)
    {
        return false;
    }
    for (const Report &r : e.reports)
    {
        if (r.source != 0 && !ctx.characters->isPlayer(r.source) && r.knowsIdentity)
        {
            return true;
        }
    }
    return false;
}

Evidence *findEvidence(
    JusticeStore &justice, std::uint64_t evidenceId
)
{
    for (auto &[territory, cases] : justice.evidenceStore)
    {
        for (Evidence &e : cases)
        {
            if (e.id == evidenceId)
            {
                return &e;
            }
        }
    }
    return nullptr;
}

world::CompactRecord compactRecord(
    const Evidence &e
)
{
    // 1.4's POD row, layout finalized in 4.1 (the appended victim field).
    world::CompactRecord row;
    row.evidenceId = e.id;
    row.worldTime  = e.worldTime;
    row.crimeType  = static_cast<std::uint32_t>(e.type);
    row.culprit    = e.culpritId;
    row.victim     = e.victimId;
    return row;
}

void archiveEvidence(
    Evidence &e, WorldContext &ctx
)
{
    assert(ctx.worldStore != nullptr && ctx.justiceStore != nullptr && "archiveEvidence: unbound context");
    if (ctx.worldStore == nullptr || ctx.justiceStore == nullptr)
    {
        return;
    }
    e.status = EvidenceStatus::ARCHIVED;
    // The territory's archive holds the compact row (burning it destroys the
    // record — 6.3's hook; the pointer write goes THROUGH the vector).
    if (world::Territory *territory = ctx.worldStore->territory(e.territory); territory != nullptr)
    {
        territory->archive.push_back(compactRecord(e));
    }
    // The case leaves the store (§4.5: archived cases are REMOVED).
    auto &cases = ctx.justiceStore->evidenceStore[e.territory];
    std::erase_if(cases, [&e](const Evidence &c) { return c.id == e.id; });
}

void updateEvidence(
    Evidence &e, WorldContext &ctx
)
{
    if (e.status == EvidenceStatus::CONVICTED)
    {
        return; // the terminal verdict is immune
    }
    assert(ctx.justiceStore != nullptr && ctx.worldStore != nullptr && "updateEvidence: unbound context");
    if (ctx.justiceStore == nullptr || ctx.worldStore == nullptr)
    {
        return;
    }

    // The WANTED -> INVESTIGATING downgrade runs BEFORE the expiry check (the
    // doc order — an expired warrant pauses the case; running the check first
    // would archive a tick early).
    if (e.status == EvidenceStatus::WANTED && !hasWarrantIn(ctx, e.culpritId, e.territory))
    {
        const int oldStatus = static_cast<int>(e.status);
        e.status            = EvidenceStatus::INVESTIGATING; // expired warrant = pause, not eraser
        notifyCaseStatusChanged(e, oldStatus, ctx);          // the downgrade re-signals nothing (4.6)
    }

    if (ctx.now - e.worldTime > LIMIT_DAYS * core::DAY_LENGTH_SECONDS)
    {
        archiveEvidence(e, ctx); // the case window (A-71) closed
        return;
    }

    const std::uint32_t score     = caseScore(e, ctx);
    const std::uint32_t threshold = caseThreshold(e, ctx);
    if (score >= threshold && hasIdentity(e, ctx))
    {
        if (e.status != EvidenceStatus::WANTED)
        {
            const int oldStatus = static_cast<int>(e.status);
            issueWarrant(e, ctx);                       // a live warrant never duplicates (the store's invariant)
            notifyCaseStatusChanged(e, oldStatus, ctx); // Wanted + Poster (4.6's signal-before-force)
        }
    } else if (score >= threshold / INVESTIGATE_DIVISOR)
    {
        const int oldStatus = static_cast<int>(e.status);
        e.status            = EvidenceStatus::INVESTIGATING; // half the threshold opens the investigation (A-72)
        notifyCaseStatusChanged(e, oldStatus, ctx);          // Investigated on FIRST reach (4.6)
    }
}

void sweepExpiredEvidence(
    WorldContext &ctx
)
{
    assert(ctx.justiceStore != nullptr && ctx.worldStore != nullptr && "sweepExpiredEvidence: unbound context");
    if (ctx.justiceStore == nullptr || ctx.worldStore == nullptr)
    {
        return;
    }

    // The daily global backstop: archive every un-wanted, past-window case
    // everywhere. WANTED cases with a live warrant are skipped (the warrant's
    // own expiry path owns them). Iterate a COPY — archiveEvidence erases
    // from the vector mid-walk.
    for (auto &[territory, cases] : ctx.justiceStore->evidenceStore)
    {
        const std::vector<Evidence> snapshot = cases;
        for (const Evidence &e : snapshot)
        {
            if (e.status == EvidenceStatus::WANTED && hasWarrantIn(ctx, e.culpritId, territory))
            {
                continue;
            }
            if (ctx.now - e.worldTime > LIMIT_DAYS * core::DAY_LENGTH_SECONDS)
            {
                const auto it
                    = std::find_if(cases.begin(), cases.end(), [&e](const Evidence &c) { return c.id == e.id; });
                if (it != cases.end())
                {
                    archiveEvidence(*it, ctx);
                }
            }
        }
    }
}

void runCaseTicks(
    WorldContext &ctx
)
{
    assert(ctx.justiceStore != nullptr && ctx.worldStore != nullptr && "runCaseTicks: unbound context");
    assert(ctx.world != nullptr && ctx.characters != nullptr && "runCaseTicks: unbound context");
    if (ctx.justiceStore == nullptr || ctx.worldStore == nullptr || ctx.world == nullptr || ctx.characters == nullptr)
    {
        return;
    }

    // "Evidence ticks run only in territories with players nearby" (A-75):
    // the player's position against each territory's spatial anchor (the
    // region center — worldgen pins settlement centers there).
    const characterId player = ctx.world->playerCharacterId();
    if (player == 0 || !ctx.characters->exists(player))
    {
        return;
    }
    const Position &playerPos = ctx.characters->get(player).body.position;

    // Deterministic order: territory-id ascending (R7 map), insertion order
    // within a territory (R7 vector); one update per case per game-hour
    // (A-76 — the pacing map rides the JusticeStore).
    for (auto &[territory, cases] : ctx.justiceStore->evidenceStore)
    {
        // Lawless territories run no case ticks (4.6: "enforcement/case
        // ticks/suppression do not run — no one to enforce"); the sweep is
        // the backstop everywhere.
        if (ctx.worldStore->isLawless(territory))
        {
            continue;
        }
        const world::Territory *node = ctx.worldStore->territory(territory);
        if (node == nullptr || node->region == world::kNoRegion)
        {
            continue; // no spatial anchor — the sweep is the backstop
        }
        const Position center = world::regionCenter(node->region);
        if (world::distanceSquaredM(playerPos, center) > CASE_TICK_RADIUS_M * CASE_TICK_RADIUS_M)
        {
            continue;
        }
        for (Evidence &e : cases)
        {
            std::uint64_t &lastTick = ctx.justiceStore->caseLastTick[e.id];
            if (ctx.now - lastTick < GAME_HOUR_SECONDS)
            {
                continue; // the A-76 cadence: one update per game-hour per case
            }
            lastTick = ctx.now;
            updateEvidence(e, ctx);
        }
    }
}

} // namespace harrowmere::justice
