// warrants.cpp — task 4.2's warrant half. Every rule is card-verbatim
// (design reference §Warrants / §Justice); see warrants.hpp for the pins.

#include "justice/warrants.hpp"

#include <algorithm>
#include <cassert>

#include "core/constants.hpp"     // A-73/A-74
#include "core/world_context.hpp" // WorldContext + the ISimEventSink seam
#include "data/registry.hpp"      // DataRegistry::crime (severity / lethalForce rows)
#include "justice/evidence.hpp"   // the complete Evidence (issueWarrant flips its status)
#include "world/world.hpp"        // WorldStore::isAncestorOrSelf (D-JUR-1 — the one walk)

namespace harrowmere::justice
{

namespace
{

// deleteFromBothMaps — the doc invariant's single engine: one erase, both
// maps, lockstep. Every terminal path (expire/convict/pay) funnels here.
void deleteFromBothMaps(
    JusticeStore &justice, std::map<std::uint64_t, Warrant>::iterator warrantIt
)
{
    const characterId culprit = warrantIt->second.culpritId;
    const std::uint64_t id    = warrantIt->first;
    justice.warrants.erase(warrantIt);
    const auto indexIt = justice.activeWarrantsByCulprit.find(culprit);
    if (indexIt != justice.activeWarrantsByCulprit.end())
    {
        indexIt->second.erase(id);
        if (indexIt->second.empty())
        {
            justice.activeWarrantsByCulprit.erase(indexIt);
        }
    }
}

void publishResolved(
    std::uint64_t warrantId, WarrantResolved::How how, WorldContext &ctx
)
{
    // 0.7 rule 1: the fact publishes after the mutation is complete; a null
    // sink drops the publication deterministically (D-SINK-1).
    if (ctx.simEvents != nullptr)
    {
        ctx.simEvents->onWarrantResolved(WarrantResolved{warrantId, how});
    }
}

} // namespace

std::uint64_t bountyFor(
    const Evidence &e, WorldContext &ctx
)
{
    assert(ctx.data != nullptr && "bountyFor: no DataRegistry in the context");
    if (ctx.data == nullptr)
    {
        return 0; // release: the honest zero (a wired runtime always carries the registry)
    }
    const data::CrimeData &row = ctx.data->crime(e.type);
    // D-BOUNTY-1 verbatim: min(BOUNTY_CAP, severity * 5 currency per point).
    return std::min<std::uint64_t>(BOUNTY_CAP, static_cast<std::uint64_t>(row.severity) * BOUNTY_CURRENCY_PER_POINT);
}

void issueWarrant(
    Evidence &e, WorldContext &ctx
)
{
    assert(ctx.justiceStore != nullptr && "issueWarrant: no JusticeStore bound");
    assert(ctx.data != nullptr && "issueWarrant: no DataRegistry bound");
    if (ctx.justiceStore == nullptr)
    {
        return;
    }
    JusticeStore &js = *ctx.justiceStore;

    Warrant w{};
    w.id          = js.nextWarrantId();
    w.evidenceId  = e.id;
    w.culpritId   = e.culpritId;
    w.territory   = e.territory; // the ISSUING territory = the evidence's snapshot (jurisdiction anchor)
    w.lethal      = ctx.data != nullptr ? ctx.data->crime(e.type).lethalForce : false; // copied at issuance (doc rule)
    w.bounty      = bountyFor(e, ctx);
    w.expiresTime = ctx.now + WARRANT_DURATION_GAME_S; // A-74

    // BOTH maps, lockstep (the doc invariant).
    js.warrants[w.id] = w;
    js.activeWarrantsByCulprit[e.culpritId].insert(w.id);
    e.status = EvidenceStatus::WANTED;

    if (ctx.simEvents != nullptr)
    {
        ctx.simEvents->onWarrantIssued(WarrantIssued{w.id, e.culpritId, e.territory, w.lethal});
    }
}

void purgeExpiredWarrants(
    WorldContext &ctx
)
{
    assert(ctx.justiceStore != nullptr && "purgeExpiredWarrants: no JusticeStore bound");
    if (ctx.justiceStore == nullptr)
    {
        return;
    }
    JusticeStore &js = *ctx.justiceStore;

    // The lazy purge: every warrant past its expiry leaves BOTH maps exactly
    // once (deletion is permanent, so the Expired event fires once). Map
    // order = deterministic sweep order (R7).
    std::vector<std::uint64_t> expired;
    for (const auto &[id, w] : js.warrants)
    {
        if (ctx.now >= w.expiresTime)
        {
            expired.push_back(id);
        }
    }
    for (const std::uint64_t id : expired)
    {
        const auto it = js.warrants.find(id);
        if (it == js.warrants.end())
        {
            continue;
        }
        deleteFromBothMaps(js, it);
        publishResolved(id, WarrantResolved::How::Expired, ctx);
    }
}

bool hasWarrantIn(
    WorldContext &ctx, characterId suspect, territoryId territory
)
{
    assert(ctx.justiceStore != nullptr && ctx.worldStore != nullptr && "hasWarrantIn: unbound context");
    if (ctx.justiceStore == nullptr || ctx.worldStore == nullptr)
    {
        return false;
    }
    purgeExpiredWarrants(ctx); // the doc-mandated mutating read

    const auto indexIt = ctx.justiceStore->activeWarrantsByCulprit.find(suspect);
    if (indexIt == ctx.justiceStore->activeWarrantsByCulprit.end())
    {
        return false;
    }
    for (const std::uint64_t id : indexIt->second)
    {
        const auto it = ctx.justiceStore->warrants.find(id);
        if (it != ctx.justiceStore->warrants.end() && warrantActiveIn(it->second, *ctx.worldStore, territory))
        {
            return true;
        }
    }
    return false;
}

bool hasLethalWarrantIn(
    WorldContext &ctx, characterId suspect, territoryId territory
)
{
    assert(ctx.justiceStore != nullptr && ctx.worldStore != nullptr && "hasLethalWarrantIn: unbound context");
    if (ctx.justiceStore == nullptr || ctx.worldStore == nullptr)
    {
        return false;
    }
    purgeExpiredWarrants(ctx); // the doc-mandated mutating read

    const auto indexIt = ctx.justiceStore->activeWarrantsByCulprit.find(suspect);
    if (indexIt == ctx.justiceStore->activeWarrantsByCulprit.end())
    {
        return false;
    }
    for (const std::uint64_t id : indexIt->second)
    {
        const auto it = ctx.justiceStore->warrants.find(id);
        if (it != ctx.justiceStore->warrants.end() && it->second.lethal
            && warrantActiveIn(it->second, *ctx.worldStore, territory))
        {
            return true;
        }
    }
    return false;
}

bool warrantActiveIn(
    const Warrant &warrant, const world::WorldStore &world, territoryId queried
)
{
    // 1.4's walk, verbatim: the warrant issued for X covers q exactly when
    // isAncestorOrSelf(q, X) — a village warrant covers its county, kingdom
    // and empire, never a sibling village.
    return world.isAncestorOrSelf(queried, warrant.territory);
}

std::vector<Warrant> activeWarrantsOf(
    WorldContext &ctx, characterId culprit
)
{
    std::vector<Warrant> out;
    assert(ctx.justiceStore != nullptr && "activeWarrantsOf: no JusticeStore bound");
    if (ctx.justiceStore == nullptr)
    {
        return out;
    }
    purgeExpiredWarrants(ctx); // reads are mutating (the doc's rule)

    const auto indexIt = ctx.justiceStore->activeWarrantsByCulprit.find(culprit);
    if (indexIt == ctx.justiceStore->activeWarrantsByCulprit.end())
    {
        return out;
    }
    out.reserve(indexIt->second.size());
    for (const std::uint64_t id : indexIt->second) // set order = ascending (R7)
    {
        const auto it = ctx.justiceStore->warrants.find(id);
        if (it != ctx.justiceStore->warrants.end())
        {
            out.push_back(it->second);
        }
    }
    return out;
}

void resolveWarrantsForEvidence(
    std::uint64_t evidenceId, characterId culprit, WarrantResolved::How how, WorldContext &ctx
)
{
    assert(ctx.justiceStore != nullptr && "resolveWarrantsForEvidence: no JusticeStore bound");
    if (ctx.justiceStore == nullptr)
    {
        return;
    }
    JusticeStore &js = *ctx.justiceStore;

    const auto indexIt = js.activeWarrantsByCulprit.find(culprit);
    if (indexIt == js.activeWarrantsByCulprit.end())
    {
        return;
    }
    std::vector<std::uint64_t> matching;
    for (const std::uint64_t id : indexIt->second)
    {
        const auto it = js.warrants.find(id);
        if (it != js.warrants.end() && it->second.evidenceId == evidenceId)
        {
            matching.push_back(id);
        }
    }
    for (const std::uint64_t id : matching)
    {
        const auto it = js.warrants.find(id);
        if (it == js.warrants.end())
        {
            continue;
        }
        deleteFromBothMaps(js, it);
        publishResolved(id, how, ctx);
    }
}

void payWarrant(
    std::uint64_t warrantId, WorldContext &ctx
)
{
    assert(ctx.justiceStore != nullptr && "payWarrant: no JusticeStore bound");
    if (ctx.justiceStore == nullptr)
    {
        return;
    }
    JusticeStore &js = *ctx.justiceStore;

    const auto it = js.warrants.find(warrantId);
    if (it == js.warrants.end())
    {
        return; // already resolved (payment deletes): the pay-once rule's form
    }
    it->second.paid = true; // the flag rides the record for the claim path's read (4.7)
    deleteFromBothMaps(js, it);
    publishResolved(warrantId, WarrantResolved::How::Paid, ctx);
}

} // namespace harrowmere::justice
