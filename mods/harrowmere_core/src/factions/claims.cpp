// claims.cpp — task 6.3: the claim interactions, the custom-law installation,
// the presence ledger and the abandonment sweep, the destruction-protection
// gate.
//
// Every magnitude is a named constant (core/constants.hpp): A-113..A-115 plus
// the A-58 AFK bound the stint-gap read reuses. THE TWO-CLOCKS RULE (the card
// pitfall, D-TIME-1): abandonment math runs on realTime (ctx.realTimeSeconds),
// the war-involvement gate runs on worldTime (the war store's
// effectively-active read) — the two never mix inside one expression.

#include "factions/claims.hpp"

#include "character/character.hpp"
#include "character/store.hpp"
#include "core/clock.hpp" // DAY_LENGTH_SECONDS (the game-time half of the two-clocks rule)
#include "core/constants.hpp"
#include "core/game_events.hpp"  // TerritoryChanged (the 0.7 payload)
#include "factions/factions.hpp" // the tolerance read (relationBetween)
#include "factions/war.hpp"      // the war-involvement gates (6.2's exception)
#include "world/world.hpp"

namespace harrowmere::factions
{

namespace
{

// A-115 in real seconds: the abandonment horizon (REAL seconds — the doc's
// "14 real-world days"; D-TIME-1's real-clock rule). The 24/3600 factors are
// the universal unit conversion (hours per day, seconds per hour), the same
// arithmetic constants.hpp's A-3 derivation carries — not game-rule values,
// so they claim no row of their own (the world/region.hpp sentinel reading).
constexpr std::uint64_t kAbandonRealSeconds = ABANDON_DAYS_REAL * 24ull * 3'600ull;

// A-114 in real seconds: the qualifying-session bound (the 60 factor is the
// seconds-per-minute unit conversion — the same universal-arithmetic reading
// as kAbandonRealSeconds's factors above).
constexpr std::uint64_t kActivityRealSeconds = ACTIVITY_MINUTES_REAL * 60ull;

} // namespace

// ---- the claim interaction -----------------------------------------------------------

ClaimResult ClaimStore::claim(
    territoryId t, characterId claimant, WorldContext &ctx
)
{
    if (ctx.worldStore == nullptr || !ctx.worldStore->hasTerritory(t))
        return ClaimResult::err(ClaimError::UnknownTerritory);
    if (ctx.characters == nullptr || !ctx.characters->exists(claimant))
        return ClaimResult::err(ClaimError::UnknownCharacter);
    if (!ctx.characters->isPlayer(claimant))
        return ClaimResult::err(ClaimError::NotPlayer); // the doc's claims are player claims
    if (ctx.world == nullptr || ctx.world->territoryAt(ctx.characters->get(claimant).body.position) != t)
        return ClaimResult::err(ClaimError::NotStandingThere); // the interaction at the territory center

    world::Territory *territory = ctx.worldStore->territory(t);
    const bool isAbandonedClaim = isAbandoned(t);

    // The claimable classes: unclaimed frontier, lawless (4.6), abandoned.
    if (!world::isUnclaimed(*territory) && !ctx.worldStore->isLawless(t) && !isAbandonedClaim)
    {
        return ClaimResult::err(ClaimError::NotClaimable);
    }
    // A live owner holds it (an abandoned row still names its owner until the
    // claim lands — the re-claim overwrites, a live owner blocks).
    if (territory->owner != 0 && !isAbandonedClaim)
    {
        return ClaimResult::err(ClaimError::AlreadyOwned);
    }

    // The war gate (6.2's exception in reverse): no active war involving it
    // — a DECLARED war contesting the territory's region blocks, notice
    // included (the protections may still hold during notice, the claim
    // never crosses a declared war).
    if (ctx.warStore != nullptr)
    {
        const regionId r = territory->region;
        if (r != world::kNoRegion && ctx.warStore->isRegionContested(r, /*requireNoticeEnded=*/false))
        {
            return ClaimResult::err(ClaimError::AtWar);
        }
    }

    // The kingdom's tolerance: a claim inside another faction's court
    // jurisdiction needs a non-rival relation (score >= 0 — neutral or
    // allied tolerates, rivalry refuses; the "or war" arm is 6.2's outcome
    // transfer, never this interaction). The claimant's own faction's court
    // and courtless ground are free.
    if (factionId kingdom = kingdomFactionAbove(t, ctx); kingdom != 0)
    {
        const factionId ownFaction = ctx.factionStore != nullptr ? ctx.factionStore->factionOfCharacter(claimant) : 0;
        if (ownFaction == 0)
        {
            return ClaimResult::err(ClaimError::NoTolerance); // a factionless settler cannot hold a kingdom's tolerance
        }
        if (ownFaction != kingdom)
        {
            const Relation *row = ctx.factionStore->relationBetween(ownFaction, kingdom);
            if (row == nullptr || row->score < 0)
            {
                return ClaimResult::err(ClaimError::NoTolerance); // the kingdom does not tolerate the claim
            }
        }
    }

    // The A-113 cost from the claimant's purse (half for an abandoned claim).
    const std::int64_t cost = isAbandonedClaim ? CLAIM_COST / 2 : CLAIM_COST;
    Character &claimantChar = ctx.characters->get(claimant);
    if (claimantChar.progression.currency < cost)
        return ClaimResult::err(ClaimError::InsufficientCurrency);
    claimantChar.progression.currency -= cost;

    // The D-TERR-OWNER-1 write + the ledger row (the timer starts at the
    // claim — the claiming session is presence).
    territory->owner = claimant;
    ClaimState state;
    state.owner                  = claimant;
    state.lastQualifyingRealTime = ctx.realTimeSeconds;
    state.lastStampRealTime      = ctx.realTimeSeconds;
    state.abandoned              = false;
    claims_[t]                   = std::move(state);

    // The lawless exit (4.6: the claim re-garrisons the ground under law).
    ctx.worldStore->clearLawless(t);

    if (ctx.simEvents != nullptr)
    {
        ctx.simEvents->onTerritoryChanged(TerritoryChanged{t, TerritoryChanged::What::Owner});
    }
    return ClaimResult::ok();
}

ClaimResult ClaimStore::installLaws(
    territoryId t, characterId by, const data::LawSet &set, WorldContext &ctx
)
{
    if (ctx.worldStore == nullptr || !ctx.worldStore->hasTerritory(t))
        return ClaimResult::err(ClaimError::UnknownTerritory);
    if (ctx.characters == nullptr || !ctx.characters->exists(by))
        return ClaimResult::err(ClaimError::UnknownCharacter);
    world::Territory *territory = ctx.worldStore->territory(t);
    const auto stateIt          = claims_.find(t);
    if (territory->owner == 0 || stateIt == claims_.end() || stateIt->second.owner != by)
        return ClaimResult::err(ClaimError::NotOwner); // the owner authors; nobody else installs

    // The schema constraint (1.2's shape): a named set with every
    // conviction threshold on the SCALE grid (the loader's own bound).
    if (set.key.empty())
        return ClaimResult::err(ClaimError::BadLawSet);
    for (const std::uint32_t threshold : set.convictionThreshold)
    {
        if (threshold > kScale)
            return ClaimResult::err(ClaimError::BadLawSet);
    }

    // The store owns the copy (stable std::map address — the registry-swap
    // contract); the territory's laws pointer lands on it and lawsAt
    // resolves it (the doc walk, untouched).
    const std::string key  = set.key;
    customLaws_[key]       = set;
    territory->laws        = &customLaws_[key];
    stateIt->second.lawKey = key;

    // Territory.safe is worldgen-owned: never read, never written here — the
    // spawn-point pin (a claimed safe territory keeps its ward reports and
    // its safe flag, whatever the custom set carries).
    if (ctx.simEvents != nullptr)
    {
        ctx.simEvents->onTerritoryChanged(TerritoryChanged{t, TerritoryChanged::What::Laws});
    }
    return ClaimResult::ok();
}

const data::LawSet *ClaimStore::lawsInstalledOn(
    territoryId t
) const
{
    const auto stateIt = claims_.find(t);
    if (stateIt == claims_.end() || stateIt->second.lawKey.empty())
        return nullptr;
    const auto lawIt = customLaws_.find(stateIt->second.lawKey);
    return lawIt == customLaws_.end() ? nullptr : &lawIt->second;
}

ClaimResult ClaimStore::dismantleAbandoned(
    territoryId t, characterId by, WorldContext &ctx
)
{
    if (ctx.worldStore == nullptr || !ctx.worldStore->hasTerritory(t))
        return ClaimResult::err(ClaimError::UnknownTerritory);
    if (ctx.characters == nullptr || !ctx.characters->exists(by))
        return ClaimResult::err(ClaimError::UnknownCharacter);
    if (!ctx.characters->isPlayer(by))
        return ClaimResult::err(ClaimError::NotPlayer);
    if (ctx.world != nullptr && ctx.world->territoryAt(ctx.characters->get(by).body.position) != t)
        return ClaimResult::err(ClaimError::NotStandingThere);
    const auto stateIt = claims_.find(t);
    if (stateIt == claims_.end() || !stateIt->second.abandoned)
        return ClaimResult::err(ClaimError::NotAbandoned);

    // The reversion: owner 0, the custom set discarded, the archive
    // PRESERVED (world memory — burning destroys archives, ownership changes
    // never do, 4.2's rule).
    world::Territory *territory = ctx.worldStore->territory(t);
    territory->owner            = 0;
    if (!stateIt->second.lawKey.empty())
    {
        customLaws_.erase(stateIt->second.lawKey);
        territory->laws = nullptr;
    }
    claims_.erase(stateIt);

    if (ctx.simEvents != nullptr)
    {
        ctx.simEvents->onTerritoryChanged(TerritoryChanged{t, TerritoryChanged::What::Owner});
    }
    return ClaimResult::ok();
}

// ---- the presence ledger ---------------------------------------------------------------

void ClaimStore::notePresence(
    territoryId t, characterId owner, bool afk, std::uint64_t realTimeSeconds
)
{
    auto it = claims_.find(t);
    if (it == claims_.end() || it->second.owner != owner)
        return;
    ClaimState &state = it->second;

    if (state.lastStampRealTime != 0)
    {
        const std::uint64_t gap = realTimeSeconds > state.lastStampRealTime
                                    ? realTimeSeconds - state.lastStampRealTime
                                    : 0; // R8: the real clock is monotonic; a rewind stamps 0
        if (gap > AFK_TIMEOUT_REAL_S)
        {
            state.stintRealSeconds  = 0; // the player left: the stint restarts (the gap itself never counts)
            state.lastStampRealTime = realTimeSeconds;
            return;
        }
        if (!afk)
        {
            state.stintRealSeconds += gap; // validated active minutes only (D-AFK-1)
        }
    }
    if (state.stintRealSeconds >= kActivityRealSeconds)
    {
        // The qualifying session (A-114): the abandonment timer resets.
        state.lastQualifyingRealTime = realTimeSeconds;
        state.stintRealSeconds       = 0; // the next 30-minute stint starts fresh
    }
    state.lastStampRealTime = realTimeSeconds;
}

void ClaimStore::tick(
    WorldContext &ctx
)
{
    // The NPC-owner stamps first: an NPC owner is present while alive and
    // never AFK (the deterministic reading — DEVIATIONS.md carries the row).
    // A dead owner stops stamping and their claim decays into abandonment.
    if (ctx.characters != nullptr)
    {
        for (auto &[t, state] : claims_)
        {
            (void)t;
            if (state.owner != 0 && !ctx.characters->isPlayer(state.owner) && ctx.characters->exists(state.owner)
                && ctx.characters->get(state.owner).body.currentHp != 0)
            {
                notePresence(t, state.owner, /*afk=*/false, ctx.realTimeSeconds);
            }
        }
    }

    // The abandonment flagging: 14 REAL days without a qualifying session
    // (the real-time half), paused while a war effectively involves the
    // territory (the game-time half — the war check asks the effectively
    // active read; D-TIME-1's two-clocks rule is exactly this split). A row's
    // timer armed at realTime 0 counts from the world epoch (the arithmetic
    // reads it naturally; no sentinel exists).
    for (auto &[t, state] : claims_)
    {
        if (state.abandoned)
            continue;
        if (ctx.realTimeSeconds < state.lastQualifyingRealTime
            || ctx.realTimeSeconds - state.lastQualifyingRealTime < kAbandonRealSeconds)
            continue;
        if (ctx.warStore != nullptr && ctx.worldStore != nullptr)
        {
            const world::Territory *territory = ctx.worldStore->territory(t);
            const regionId r                  = territory != nullptr ? territory->region : world::kNoRegion;
            if (r != world::kNoRegion && ctx.warStore->isRegionContested(r, /*requireNoticeEnded=*/true))
            {
                continue; // "no active war involvement": the war pauses the flagging
            }
        }
        state.abandoned = true;
    }
}

void ClaimStore::noteOwnershipTransfer(
    territoryId t, characterId newOwner, std::uint64_t realTimeSeconds
)
{
    // The war outcome's rewiring: a fresh timer for the new owner (armed at
    // the transfer's real time), the installed laws stay (the claim
    // transfers with its law set), the abandoned flag clears (the territory
    // changed hands, not decayed).
    ClaimState &state            = claims_[t];
    state.owner                  = newOwner;
    state.lastQualifyingRealTime = realTimeSeconds; // the transfer itself is the fresh timer's start
    state.stintRealSeconds       = 0;
    state.lastStampRealTime      = 0;
    state.abandoned              = false;
}

// ---- reads ------------------------------------------------------------------------------

bool ClaimStore::isProtectedFromDestruction(
    territoryId t, WorldContext &ctx
) const
{
    // The doc sentence, gated: PLAYER-owned only, and only while no
    // effectively-active war contests the territory's region (D-SP-WAR-1's
    // suspension starts at noticeEndsAt — the notice window's protections
    // still hold, the claim gate above is the stricter read).
    if (ctx.worldStore == nullptr || ctx.characters == nullptr)
        return false;
    const world::Territory *territory = ctx.worldStore->territory(t);
    if (territory == nullptr || territory->owner == 0 || !ctx.characters->isPlayer(territory->owner))
        return false;
    if (ctx.warStore != nullptr && territory->region != world::kNoRegion
        && ctx.warStore->isRegionContested(territory->region, /*requireNoticeEnded=*/true))
    {
        return false; // the active-war exception
    }
    return true;
}

bool ClaimStore::isAbandoned(
    territoryId t
) const
{
    const auto it = claims_.find(t);
    return it != claims_.end() && it->second.abandoned;
}

characterId ClaimStore::ownerOf(
    territoryId t, WorldContext &ctx
) const
{
    if (ctx.worldStore == nullptr)
        return 0;
    const world::Territory *territory = ctx.worldStore->territory(t);
    return territory != nullptr ? territory->owner : 0;
}

std::vector<territoryId> ClaimStore::claimsOwnedBy(
    characterId who
) const
{
    std::vector<territoryId> out;
    for (const auto &[t, state] : claims_)
    {
        if (state.owner == who)
        {
            out.push_back(t); // the map's own id-ascending order (R7)
        }
    }
    return out;
}

// ---- the tolerance read -----------------------------------------------------------------

factionId ClaimStore::kingdomFactionAbove(
    territoryId t, WorldContext &ctx
) const
{
    // The court walk (D-JUR-1's chain, never a re-implementation): the first
    // faction whose capital sits on the territory's chain to the root, lowest
    // faction id first (the store's own order).
    if (ctx.worldStore == nullptr || ctx.factionStore == nullptr)
        return 0;
    for (const territoryId node : ctx.worldStore->chainToRoot(t))
    {
        for (const auto &[fId, faction] : ctx.factionStore->all())
        {
            (void)faction;
            if (ctx.factionStore->find(fId)->capital == node)
            {
                return fId;
            }
        }
    }
    return 0;
}

// ---- save seam (task 7.1) -------------------------------------------------------

const data::LawSet *ClaimStore::customLawByKey(
    const std::string &key
) const
{
    const auto it = customLaws_.find(key);
    return it == customLaws_.end() ? nullptr : &it->second;
}

ClaimStore::Snapshot ClaimStore::snapshot() const
{
    Snapshot out;
    out.claims     = claims_;
    out.customLaws = customLaws_;
    return out;
}

void ClaimStore::restore(
    const Snapshot &saved
)
{
    claims_     = saved.claims;
    customLaws_ = saved.customLaws;
}

} // namespace harrowmere::factions
