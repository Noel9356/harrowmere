// factions.cpp — task 6.1: the faction store's interactions, the worldgen
// adoption, the monthly drift sweep and the IFactionQueries adapter.
//
// Every magnitude is a named constant (core/constants.hpp): A-108's founding
// cost, the seeded relation scores and the drift/invite magnitudes the card
// names without pinning (DEVIATIONS.md carries the row, the FINE_CURRENCY_
// PER_POINT precedent). GoodId 0 inside sharedResources is the CURRENCY row
// (R10 reserves 0 in every id space — the sentinel costs nothing and the
// doc-verbatim struct carries no separate currency field).

#include "factions/factions.hpp"

#include <algorithm>
#include <utility>

#include "character/character.hpp"
#include "character/store.hpp"
#include "core/constants.hpp"
#include "core/math.hpp"
#include "core/rng.hpp" // the pinned "npc_life" stream (R6)
#include "core/world_context.hpp"
#include "economy/economy.hpp"    // addGoodsHeld (the dissolution's goods dispersal)
#include "factions/war.hpp"       // 6.2: the war registry isAtWar reads (the stub is gone)
#include "justice/reputation.hpp" // the drift's 4.4 leader-reputation read
#include "npc/state.hpp"
#include "world/world.hpp"    // socialClassOf / isAncestorOrSelf (D-JUR-1's walks)
#include "world/worldgen.hpp" // FactionSeed (1.5 stage 5)

namespace harrowmere::factions
{

namespace
{

constexpr goodId kCurrencyGood = 0; // sharedResources' currency row (R10's reserved 0)

// One game-month in world seconds (the drift's lazy cadence quantum).
constexpr std::uint64_t kMonthGameSeconds = 30ULL * core::DAY_LENGTH_SECONDS;

// The rank ladder of the adoption (deterministic): the highest social class
// leads, ties break by lower id (R7 — the scan is the character map's own
// order). ROYAL > NOBLE > the rest; the leader takes rank 9.
std::uint8_t rankForClass(
    SocialClassId cls
)
{
    switch (cls)
    {
    case SocialClassId::ROYAL:
        return 7;
    case SocialClassId::NOBLE:
        return 5;
    default:
        return 0;
    }
}

} // namespace

// ---- the store -----------------------------------------------------------------

const Faction *FactionStore::find(
    factionId id
) const
{
    const auto it = factions_.find(id);
    return it == factions_.end() ? nullptr : &it->second;
}

Faction *FactionStore::find(
    factionId id
)
{
    const auto it = factions_.find(id);
    return it == factions_.end() ? nullptr : &it->second;
}

factionId FactionStore::factionOfCharacter(
    characterId who
) const
{
    const auto it = membership_.find(who);
    return it == membership_.end() ? 0 : it->second;
}

const Relation *FactionStore::relationBetween(
    factionId a, factionId b
) const
{
    const Faction *from = find(a);
    if (from == nullptr)
        return nullptr;
    const auto it = from->relations.find(b);
    return it == from->relations.end() ? nullptr : &it->second;
}

bool FactionStore::isAtWar(
    factionId a, factionId b, WorldContext &ctx
) const
{
    // The war registry (6.2) answers: a pair is at war exactly while an
    // active war between them has passed its notice end — the ONE
    // warEffectivelyActive gate (the card pitfall: IN_WAR and 3.3's war
    // exemption share this exact read, never a second implementation). Null
    // registry = no wars (the D-SINK-1 reading 3.3 consumed all along).
    return ctx.warStore != nullptr && ctx.warStore->isAtWarPair(a, b);
}

void FactionStore::attach(
    characterId who, factionId faction, std::uint8_t rank, WorldContext &ctx
)
{
    Faction *f = find(faction);
    if (f == nullptr || ctx.characters == nullptr)
        return;
    f->members.insert(who);
    f->ranks[who]                    = rank;
    membership_[who]                 = faction;
    ctx.characters->get(who).faction = faction; // the doc field follows (lockstep)
}

void FactionStore::detach(
    characterId who, WorldContext &ctx
)
{
    const factionId faction = factionOfCharacter(who);
    if (faction == 0)
        return;
    if (Faction *f = find(faction); f != nullptr)
    {
        f->members.erase(who);
        f->ranks.erase(who);
    }
    membership_.erase(who);
    if (ctx.characters != nullptr && ctx.characters->exists(who))
    {
        ctx.characters->get(who).faction = 0;
    }
}

// ---- the worldgen adoption -------------------------------------------------------

void FactionStore::adoptWorldGen(
    const std::vector<world::FactionSeed> &seeds, WorldContext &ctx
)
{
    // One Faction per seed, relations opened BOTH directions at the seeded
    // scores (rivals: -6000 + rivalry, allies: +6000 + alliance), capitals
    // from the seeds' court territories. ids continue the seed sequence.
    for (const world::FactionSeed &seed : seeds)
    {
        Faction f;
        f.id      = seed.id;
        f.name    = seed.name;
        f.capital = seed.courtTerritory;
        for (const factionId rival : seed.rivals)
        {
            f.relations[rival] = Relation{FACTION_SEEDED_RIVALRY_SCORE, false, true};
        }
        for (const factionId ally : seed.allies)
        {
            f.relations[ally] = Relation{FACTION_SEEDED_ALLIANCE_SCORE, true, false};
        }
        factions_.emplace(seed.id, std::move(f));
        if (seed.id >= nextFactionId_)
        {
            nextFactionId_ = seed.id + 1;
        }
    }

    // The membership walk: every living NPC joins the faction of their home
    // territory's kingdom court; frontier wanderers (home 0) join the free
    // company (the seed whose courtTerritory is 0). Players never auto-join.
    const world::FactionSeed *freeCompany = nullptr;
    for (const world::FactionSeed &seed : seeds)
    {
        if (seed.courtTerritory == 0)
            freeCompany = &seed;
    }

    if (ctx.worldStore != nullptr && ctx.characters != nullptr)
    {
        for (const auto &[id, character] : ctx.characters->all())
        {
            if (ctx.characters->isPlayer(id))
                continue;
            const territoryId home
                = ctx.npcs != nullptr && ctx.npcs->contains(id) ? ctx.npcs->get(id).homeTerritory : 0;
            const world::FactionSeed *homeFaction = nullptr;
            if (home == 0)
            {
                homeFaction = freeCompany;
            } else
            {
                for (const world::FactionSeed &seed : seeds)
                {
                    if (seed.courtTerritory != 0 && ctx.worldStore->isAncestorOrSelf(seed.courtTerritory, home))
                    {
                        homeFaction = &seed;
                        break; // id-ascending scan: the first matching court wins
                    }
                }
            }
            if (homeFaction == nullptr)
                continue; // no faction covers this NPC (unclaimed frontier with no free company)

            // The rank ladder: leader 9 for the first ROYAL (ties by lower
            // id — the scan order), NOBLE 5, ROYAL 7, everyone else 0. The
            // first member of a faction keeps 9 when no royal exists.
            Faction &faction       = factions_[homeFaction->id];
            SocialClassId cls      = ctx.worldStore->socialClassOf(id, home);
            std::uint8_t rank      = rankForClass(cls);
            const bool leaderTaken = std::any_of(faction.ranks.begin(), faction.ranks.end(), [](const auto &row) {
                return row.second >= 9;
            });
            if (!leaderTaken && (rank == 7 || faction.members.empty()))
            {
                rank = 9;
            }
            attach(id, homeFaction->id, rank, ctx);
        }
    }
}

// ---- the player faction (6.1; D-SP-1's singleton) --------------------------------

FactionResult FactionStore::foundPlayerFaction(
    characterId founder, const std::string &name, WorldContext &ctx
)
{
    if (ctx.characters == nullptr || !ctx.characters->exists(founder))
        return FactionResult::err(FactionError::UnknownCharacter);
    if (factionOfCharacter(founder) != 0)
        return FactionResult::err(FactionError::AlreadyMember);
    for (const auto &[id, faction] : factions_)
    {
        (void)id;
        if (faction.playerFounded)
            return FactionResult::err(FactionError::PlayerFactionExists); // D-SP-1's singleton
    }
    if (name.empty())
        return FactionResult::err(FactionError::UnknownFaction); // an unnamed faction is a data error, not a founding
    if (ctx.world == nullptr || ctx.world->territoryAt(ctx.characters->get(founder).body.position) == 0)
        return FactionResult::err(FactionError::NoTerritoryPresence); // the founding needs a territory presence
    Character &founderChar = ctx.characters->get(founder);
    if (founderChar.progression.currency < FACTION_FOUND_COST)
        return FactionResult::err(FactionError::InsufficientCurrency); // A-108, meaningful not gatekeeping
    founderChar.progression.currency -= FACTION_FOUND_COST;

    const factionId id = nextFactionId_++;
    Faction f;
    f.id            = id;
    f.name          = name;
    f.capital       = ctx.world->territoryAt(founderChar.body.position);
    f.playerFounded = true;
    factions_.emplace(id, std::move(f));
    attach(founder, id, 9, ctx); // the founder is the rank-9 leader
    return FactionResult::ok();
}

FactionResult FactionStore::dissolveFaction(
    characterId leader, WorldContext &ctx
)
{
    const factionId factionIdOfLeader = factionOfCharacter(leader);
    Faction *faction                  = find(factionIdOfLeader);
    if (ctx.characters == nullptr || !ctx.characters->exists(leader))
        return FactionResult::err(FactionError::UnknownCharacter);
    if (faction == nullptr)
        return FactionResult::err(FactionError::UnknownFaction);
    const auto rankIt = faction->ranks.find(leader);
    if (rankIt == faction->ranks.end() || rankIt->second != 9)
        return FactionResult::err(FactionError::NotLeader); // leader-only (the card's dissolution rule)

    // The rank-share distribution: weight = rank + 1 (a grunt keeps a token
    // share); currency rows pay into the members' i64 progression fields,
    // goods rows disperse into the members' held-goods ledger (3.7's
    // addGoodsHeld — the member-side home the bag model has for goods).
    const std::map<goodId, std::uint64_t> resources = faction->sharedResources;
    std::uint64_t weightTotal                       = 0;
    for (const characterId member : faction->members)
    {
        if (ctx.characters->exists(member))
        {
            const auto r = faction->ranks.find(member);
            weightTotal += (r != faction->ranks.end() ? r->second : 0) + 1;
        }
    }
    if (weightTotal != 0) // R2: a memberless faction simply forfeits its stock
    {
        // Deterministic pass: members id-ascending (R7); the flooring
        // remainder of the currency goes to the highest weight, ties to the
        // lower id (the leader's rank makes them the usual taker).
        std::uint64_t paid      = 0;
        characterId topMember   = 0;
        std::uint64_t topWeight = 0;
        for (const characterId member : faction->members)
        {
            if (!ctx.characters->exists(member))
                continue;
            const auto r               = faction->ranks.find(member);
            const std::uint64_t weight = (r != faction->ranks.end() ? r->second : 0) + 1;
            if (weight > topWeight || topMember == 0)
            {
                topWeight = weight;
                topMember = member;
            }
            for (const auto &[good, amount] : resources)
            {
                const std::uint64_t share = amount * weight / weightTotal;
                if (share == 0)
                    continue;
                if (good == kCurrencyGood)
                {
                    ctx.characters->get(member).progression.currency += static_cast<std::int64_t>(share); // i64 (R1)
                    paid += share;
                } else if (ctx.economy != nullptr)
                {
                    ctx.economy->addGoodsHeld(member, good, static_cast<std::int64_t>(share));
                }
            }
        }
        const auto currencyIt = resources.find(kCurrencyGood);
        if (currencyIt != resources.end() && topMember != 0)
        {
            ctx.characters->get(topMember).progression.currency
                += static_cast<std::int64_t>(currencyIt->second - paid); // the flooring remainder (R7 tie rule above)
        }
    }

    // The faction and every membership row disappear (members keep their
    // purses and held goods — the stock is gone from the faction, not from
    // the people).
    const std::set<characterId> members = faction->members;
    for (const characterId member : members)
    {
        detach(member, ctx);
    }
    factions_.erase(factionIdOfLeader);
    return FactionResult::ok();
}

FactionResult FactionStore::inviteMember(
    characterId inviter, characterId candidate, WorldContext &ctx
)
{
    if (ctx.characters == nullptr || !ctx.characters->exists(inviter) || !ctx.characters->exists(candidate))
        return FactionResult::err(FactionError::UnknownCharacter);
    Faction *faction = find(factionOfCharacter(inviter));
    if (faction == nullptr)
        return FactionResult::err(FactionError::UnknownFaction);
    const auto rankIt = faction->ranks.find(inviter);
    if (rankIt == faction->ranks.end() || rankIt->second != 9)
        return FactionResult::err(FactionError::NotLeader); // management is the leader's alone
    if (factionOfCharacter(candidate) != 0)
        return FactionResult::err(FactionError::AlreadyMember);
    const NpcState *candidateNpc
        = ctx.npcs != nullptr && ctx.npcs->contains(candidate) ? &ctx.npcs->get(candidate) : nullptr;
    if (candidateNpc == nullptr)
        return FactionResult::err(FactionError::UnknownCharacter); // the invite targets an NPC (D-SP-1's NPC actors)
    if (ctx.data == nullptr || !ctx.data->species(ctx.characters->get(candidate).species).sapient)
        return FactionResult::err(FactionError::UnknownCharacter);
    if (candidateNpc->ageYears(ctx.now) < ADULT_YEARS)
        return FactionResult::err(FactionError::UnknownCharacter); // children do not join factions

    // The acceptance roll: affinity toward the inviter moves the base, a
    // weaker candidate discounts (constants.hpp's pinned magnitudes), the
    // band clamps to the [10%, 90%] family.
    std::int64_t pct      = FACTION_INVITE_BASE_PCT;
    const auto affinityIt = candidateNpc->affinity.find(inviter);
    if (affinityIt != candidateNpc->affinity.end())
    {
        pct += affinityIt->second / FACTION_INVITE_AFFINITY_DIVISOR; // ±SCALE -> ±5000 (R1: signed)
    }
    // The power comparison reads the cached powers (a candidate's cache
    // freshness is the LOD tick's business, not the invite's).
    if (ctx.characters->get(candidate).cachedPower < ctx.characters->get(inviter).cachedPower)
        pct -= FACTION_INVITE_WEAKER_PENALTY;
    pct = clampScalar<std::int64_t>(
        pct, static_cast<std::int64_t>(DUEL_ACCEPTANCE_FLOOR), static_cast<std::int64_t>(DUEL_ACCEPTANCE_CEILING)
    );
    if (ctx.rng == nullptr)
        return FactionResult::err(
            FactionError::UnknownCharacter
        ); // no RNG: the roll cannot run (deterministic refusal)
    if (ctx.rng->pct("npc_life") >= static_cast<std::uint32_t>(pct))
        return FactionResult::ok(); // declined: a rule-level outcome, not an error — nothing changed

    attach(candidate, faction->id, 0, ctx); // a grunt joins (the leader ranks them later)
    return FactionResult::ok();
}

FactionResult FactionStore::setMemberRank(
    characterId leader, characterId member, std::uint8_t rank, WorldContext &ctx
)
{
    if (ctx.characters == nullptr || !ctx.characters->exists(leader) || !ctx.characters->exists(member))
        return FactionResult::err(FactionError::UnknownCharacter);
    if (rank > 9)
        return FactionResult::err(FactionError::BadRank); // the closed 0..9 band (the card's ranks)
    Faction *faction = find(factionOfCharacter(leader));
    if (faction == nullptr)
        return FactionResult::err(FactionError::UnknownFaction);
    const auto rankIt = faction->ranks.find(leader);
    if (rankIt == faction->ranks.end() || rankIt->second != 9)
        return FactionResult::err(FactionError::NotLeader);
    if (!faction->members.contains(member))
        return FactionResult::err(FactionError::NotMember);
    faction->ranks[member] = rank;
    return FactionResult::ok();
}

FactionResult FactionStore::setRelation(
    factionId a, factionId b, std::int32_t score, WorldContext &
)
{
    Faction *fa = find(a);
    Faction *fb = find(b);
    if (fa == nullptr || fb == nullptr)
        return FactionResult::err(FactionError::UnknownFaction);
    if (a == b)
        return FactionResult::err(FactionError::UnknownFaction); // a faction holds no relation to itself

    // Clamp on the i64 grid first (R1), store back into the i32 fields.
    const std::int32_t clamped = static_cast<std::int32_t>(clampScalar<std::int64_t>(score, -SCALE, SCALE));
    Relation row;
    row.score = clamped;
    // The standing flags re-derive at the seeded magnitude (±6000): an
    // alliance up top, a rivalry at the bottom, neither in between.
    row.alliance     = clamped >= FACTION_SEEDED_ALLIANCE_SCORE;
    row.rivalry      = clamped <= FACTION_SEEDED_RIVALRY_SCORE;
    fa->relations[b] = row;
    fb->relations[a] = row; // symmetric (one relation, two views)
    return FactionResult::ok();
}

// ---- the monthly dynamics sweep ----------------------------------------------------

void FactionStore::stampTreaty(
    factionId a, factionId b, std::uint64_t treatyUntil
)
{
    // One treaty, both views (the mirror-row rule): an absent row gains a
    // neutral one carrying only the treaty stamp. The score and the standing
    // flags are untouched — a treaty outlives a relation score change.
    Faction *fa = find(a);
    Faction *fb = find(b);
    if (fa == nullptr || fb == nullptr)
        return;
    fa->relations[b].treatyUntil = treatyUntil;
    fb->relations[a].treatyUntil = treatyUntil;
}

void FactionStore::monthlyDrift(
    WorldContext &ctx
)
{
    // The mirror write: one relation, two views (the pair drives from the
    // lower id; the mirror row's score follows — the flags stand).
    auto mirrorScore = [&](factionId from, factionId toward, std::int32_t score) {
        if (Faction *mirror = find(toward); mirror != nullptr)
        {
            if (Relation *back = mirror->relations.find(from) != mirror->relations.end()
                                   ? &mirror->relations.find(from)->second
                                   : nullptr;
                back != nullptr)
            {
                back->score = score;
            }
        }
    };

    // The lazy month gate: one whole month applies at most once, whatever
    // the tick cadence (the reputation decay's lazy pattern).
    const std::uint64_t month = ctx.now / kMonthGameSeconds;
    if (month == lastDriftMonth_)
        return;
    lastDriftMonth_ = month;

    // Pairwise, id-ascending (R7): each relation row moves once per month.
    for (auto &[idA, factionA] : factions_)
    {
        for (auto &[idB, relation] : factionA.relations)
        {
            if (idB <= idA)
                continue; // one visit per pair (the lower id drives)
            Faction *factionB = find(idB);
            if (factionB == nullptr)
                continue;

            if (relation.rivalry)
            {
                // Rivalry pairs harden or soften by the leaders' mean
                // reputation (4.4's read): reviled leaders harden the
                // feud, respected ones soften it. Trade volume and war
                // memory are the dormant arms (no v1 driver records them).
                std::int64_t leaderRepSum = 0;
                std::uint32_t leaders     = 0;
                for (const Faction *f :
                    {static_cast<const Faction *>(&factionA), static_cast<const Faction *>(factionB)})
                {
                    characterId leader = 0;
                    for (const auto &[member, rank] : f->ranks)
                    {
                        if (rank >= 9)
                        {
                            leader = member;
                            break;
                        }
                    }
                    if (leader != 0 && ctx.justiceStore != nullptr && f->capital != 0)
                    {
                        leaderRepSum += getReputationIn(*ctx.justiceStore, leader, f->capital, ctx.now);
                        leaders += 1;
                    }
                }
                const std::int64_t meanRep = leaders != 0 ? leaderRepSum / static_cast<std::int64_t>(leaders) : 0;
                const std::int64_t step    = meanRep < 0 ? -static_cast<std::int64_t>(FACTION_RIVALRY_STEP_PER_MONTH)
                                                         : static_cast<std::int64_t>(FACTION_RIVALRY_STEP_PER_MONTH);
                relation.score             = static_cast<std::int32_t>(clampScalar<std::int64_t>(
                    static_cast<std::int64_t>(relation.score) + step, -static_cast<std::int64_t>(SCALE), 0
                ));
                mirrorScore(idA, idB, relation.score);
            } else if (!relation.alliance)
            {
                // Every other non-alliance pair drifts toward neutral.
                if (relation.score > 0)
                    relation.score = std::max<std::int32_t>(0, relation.score - FACTION_DRIFT_PER_MONTH);
                else if (relation.score < 0)
                    relation.score = std::min<std::int32_t>(0, relation.score + FACTION_DRIFT_PER_MONTH);
                mirrorScore(idA, idB, relation.score);
            }
        }
    }
}

// ---- the adapters ----------------------------------------------------------------

bool FactionQueries::isAtWar(
    factionId a, factionId b
) const
{
    // The live store's war read: 6.2's real registry answers through the
    // bound context (the Phase-2 null default is gone — 3.3's isCriminalAct
    // now consumes the real exemption window).
    if (store_ == nullptr || ctx_ == nullptr)
        return false;
    return store_->isAtWar(a, b, *ctx_);
}

// ---- save seam (task 7.1) -------------------------------------------------------

FactionStore::Snapshot FactionStore::snapshot() const
{
    Snapshot out;
    out.factions       = factions_;
    out.membership     = membership_;
    out.tradeVolume    = tradeVolume_;
    out.lastDriftMonth = lastDriftMonth_;
    out.nextFactionId  = nextFactionId_;
    return out;
}

void FactionStore::restore(
    const Snapshot &saved
)
{
    factions_       = saved.factions;
    membership_     = saved.membership;
    tradeVolume_    = saved.tradeVolume;
    lastDriftMonth_ = saved.lastDriftMonth;
    nextFactionId_  = saved.nextFactionId;
}

} // namespace harrowmere::factions
