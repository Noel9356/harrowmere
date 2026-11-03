// war.cpp — task 6.2: the war registry — declaration (five casus belli), the
// notice window, the battle-event progression (D-WAR-EVT-1), the collapse /
// timeout resolution, the outcome effects (region flags, claim rewiring,
// title transfers) and the mandatory peace treaty.
//
// Every magnitude is a named constant (core/constants.hpp): A-109..A-112 plus
// the two card-named magnitudes awaiting Appendix A promotion (DEVIATIONS.md
// carries the row). Determinism: every scan walks an id-ascending map (R7);
// the PowerRatio comparison is a two-factor product (u64-safe, R3 — the
// three-factor wide-intermediate rule does not trigger).
//
// The context binding (the EventEngine::bindContext pattern): the two
// context-free reads the frozen IWarRegistry signature carries (isCombatant,
// and FactionStore::isAtWar's registry read) answer against the bound
// context's worldTime — the store never holds a clock of its own (R8).

#include "factions/war.hpp"

#include <set>

#include "character/character.hpp"
#include "character/store.hpp"
#include "core/clock.hpp" // DAY_LENGTH_SECONDS (the game-time arms)
#include "core/constants.hpp"
#include "core/game_events.hpp" // PlayerSignal + TerritoryChanged (the 0.7 payloads)
#include "economy/economy.hpp"  // producedGoods (the ResourceConflict extraction ledger)
#include "factions/claims.hpp"  // noteOwnershipTransfer (the outcome's claim rewiring)
#include "factions/factions.hpp"
#include "justice/reputation.hpp" // the Reputation cb's 4.4 read
#include "npc/state.hpp"          // homeTerritory (the ResourceConflict extractor read)
#include "world/world.hpp"

namespace harrowmere::factions
{

namespace
{

// One game-month in world seconds (the lazy auto-declaration cadence quantum,
// the 6.1 drift's own).
constexpr std::uint64_t kMonthGameSeconds = 30ULL * core::DAY_LENGTH_SECONDS;

// Grid adjacency of two region anchors (the BorderDispute reading): the flat
// plane's 4-neighborhood, derived through the D-WORLD-1 decomposition — never
// raw id arithmetic (world/region.hpp owns the layout).
bool regionsAdjacent(
    regionId a, regionId b
) noexcept
{
    if (a == world::kNoRegion || b == world::kNoRegion || a == b)
        return false;
    std::uint32_t ax = 0, az = 0, bx = 0, bz = 0;
    world::regionIndicesOf(a, ax, az);
    world::regionIndicesOf(b, bx, bz);
    const std::int64_t dx = static_cast<std::int64_t>(ax) - static_cast<std::int64_t>(bx);
    const std::int64_t dz = static_cast<std::int64_t>(az) - static_cast<std::int64_t>(bz);
    return (dx == 0 && (dz == 1 || dz == -1)) || (dz == 0 && (dx == 1 || dx == -1));
}

} // namespace

// ---- the one flip instant ---------------------------------------------------------

bool warEffectivelyActive(
    const War &war, std::uint64_t now
) noexcept
{
    // The card pitfall, verbatim: IN_WAR and the exemption share the same
    // flip instant (noticeEndsAt) — one function, two consumers, no drift.
    return war.active && now >= war.noticeEndsAt;
}

// ---- reads -------------------------------------------------------------------------

const War *WarStore::find(
    std::uint64_t id
) const
{
    const auto it = wars_.find(id);
    return it == wars_.end() ? nullptr : &it->second;
}

const War *WarStore::activeWarBetween(
    factionId a, factionId b
) const
{
    for (const auto &[id, war] : wars_)
    {
        (void)id;
        if (war.active && ((war.attacker == a && war.defender == b) || (war.attacker == b && war.defender == a)))
        {
            return &war;
        }
    }
    return nullptr;
}

bool WarStore::isAtWarPair(
    factionId a, factionId b
) const
{
    // Notice-pending wars are not yet war (the pinned notice-window test):
    // only the effectively-active gate answers.
    if (ctx_ == nullptr)
        return false;
    for (const auto &[id, war] : wars_)
    {
        (void)id;
        if ((war.attacker == a && war.defender == b) || (war.attacker == b && war.defender == a))
        {
            if (warEffectivelyActive(war, ctx_->now))
            {
                return true;
            }
        }
    }
    return false;
}

std::vector<regionId> WarStore::contestedRegionsOf(
    const War &war, WorldContext &ctx
) const
{
    // The computed read (region-ascending, R7): the cb's named region plus
    // every region anchored to a territory either belligerent controls.
    std::set<regionId> regions;
    if (war.cb.region != world::kNoRegion)
    {
        regions.insert(war.cb.region);
    }
    if (ctx.worldStore != nullptr)
    {
        for (const auto &[tId, territory] : ctx.worldStore->territories())
        {
            (void)tId;
            if (territory.region == world::kNoRegion)
                continue;
            if (controlsTerritory(territory.id, war.attacker, ctx)
                || controlsTerritory(territory.id, war.defender, ctx))
            {
                regions.insert(territory.region);
            }
        }
    }
    return {regions.begin(), regions.end()};
}

bool WarStore::isRegionContested(
    regionId region, bool requireNoticeEnded
) const
{
    if (ctx_ == nullptr)
        return false;
    for (const auto &[id, war] : wars_)
    {
        (void)id;
        if (!war.active)
            continue;
        if (requireNoticeEnded && !warEffectivelyActive(war, ctx_->now))
            continue;
        for (const regionId r : contestedRegionsOf(war, *ctx_))
        {
            if (r == region)
            {
                return true;
            }
        }
    }
    return false;
}

bool WarStore::isCombatant(
    factionId faction
) const
{
    // IN_WAR's face (status.cpp): the effectively-active gate, never the
    // notice window.
    if (ctx_ == nullptr)
        return false;
    for (const auto &[id, war] : wars_)
    {
        (void)id;
        if ((war.attacker == faction || war.defender == faction) && warEffectivelyActive(war, ctx_->now))
        {
            return true;
        }
    }
    return false;
}

// ---- 4.7's casus belli hook ---------------------------------------------------------

void WarStore::registerCasusBelli(
    territoryId origin, characterId /*culprit*/, factionId culpritFaction
)
{
    // The marker persists until a Massacre declaration consumes it (no
    // validity window is pinned; the marker IS the record). Re-marks refresh
    // the stamp (the map assign), never stack.
    massacreMarkers_[origin] = MassacreMarker{culpritFaction, ctx_ != nullptr ? ctx_->now : 0};
}

// ---- declaration --------------------------------------------------------------------

bool WarStore::controlsTerritory(
    territoryId t, factionId faction, WorldContext &ctx
) const
{
    if (factionStore_ == nullptr || ctx.worldStore == nullptr)
        return false;
    const world::Territory *territory = ctx.worldStore->territory(t);
    if (territory == nullptr)
        return false;
    if (territory->owner != 0 && factionStore_->factionOfCharacter(territory->owner) == faction)
    {
        return true; // D-TERR-OWNER-1's claim
    }
    for (const auto &[holder, cls] : territory->titles)
    {
        (void)cls;
        if (factionStore_->factionOfCharacter(holder) == faction)
        {
            return true; // the noble/royal seat
        }
    }
    return false;
}

characterId WarStore::leadingMember(
    factionId faction
) const
{
    // The 6.1 ladder: rank 9 first, then the highest rank; ties by lower id
    // (the std::map scan order).
    if (factionStore_ == nullptr)
        return 0;
    const Faction *f = factionStore_->find(faction);
    if (f == nullptr)
        return 0;
    const characterId *best = nullptr;
    std::uint8_t bestRank   = 0;
    for (const auto &[member, rank] : f->ranks)
    {
        if (best == nullptr || rank > bestRank)
        {
            best     = &member;
            bestRank = rank;
        }
    }
    return best != nullptr ? *best : 0;
}

std::uint64_t WarStore::aggregatePower(
    factionId faction, WorldContext &ctx
) const
{
    // Living members only (a corpse contributes nothing); the cached power
    // read — cache freshness is the LOD tick's business (the 6.1 precedent).
    std::uint64_t total = 0;
    if (factionStore_ == nullptr || ctx.characters == nullptr)
        return 0;
    const Faction *f = factionStore_->find(faction);
    if (f == nullptr)
        return 0;
    for (const characterId member : f->members)
    {
        if (!ctx.characters->exists(member))
            continue;
        const Character &c = ctx.characters->get(member);
        if (c.body.currentHp == 0)
            continue;
        total += c.cachedPower;
    }
    return total;
}

bool WarStore::cbBorderDisputeValid(
    factionId attacker, factionId defender, WorldContext &ctx
) const
{
    if (ctx.worldStore == nullptr)
        return false;
    for (const auto &[aId, aTerritory] : ctx.worldStore->territories())
    {
        (void)aId;
        if (!controlsTerritory(aTerritory.id, attacker, ctx))
            continue;
        for (const auto &[bId, bTerritory] : ctx.worldStore->territories())
        {
            (void)bId;
            if (!controlsTerritory(bTerritory.id, defender, ctx))
                continue;
            if (regionsAdjacent(aTerritory.region, bTerritory.region))
            {
                return true;
            }
        }
    }
    return false;
}

bool WarStore::cbResourceConflictValid(
    factionId attacker, factionId defender, CasusBelli &cb, WorldContext &ctx
) const
{
    // The 3.7-data reading (DEVIATIONS.md carries the row): the region's
    // produced stock IS the extraction — a good with supply on the region's
    // stock is being extracted there; the co-resident members of both
    // factions are the extractors. First (region-ascending, R7) match fills
    // the cb's provenance region.
    if (ctx.worldStore == nullptr || ctx.npcs == nullptr || ctx.economy == nullptr)
        return false;
    for (const auto &[rId, region] : ctx.worldStore->regions())
    {
        (void)region;
        bool attackerExtracts = false;
        bool defenderExtracts = false;
        for (const auto &[id, npc] : ctx.npcs->all())
        {
            if (!ctx.characters->exists(id) || ctx.characters->get(id).body.currentHp == 0)
                continue;
            const factionId f = factionStore_ != nullptr ? factionStore_->factionOfCharacter(id) : 0;
            if (f != attacker && f != defender)
                continue;
            const world::Territory *home = ctx.worldStore->territory(npc.homeTerritory);
            if (home == nullptr || home->region != rId)
                continue;
            if (f == attacker)
                attackerExtracts = true;
            else
                defenderExtracts = true;
        }
        if (!attackerExtracts || !defenderExtracts)
            continue;
        if (!ctx.economy->producedGoods(rId).empty())
        {
            cb.region = rId;
            return true;
        }
    }
    return false;
}

bool WarStore::cbReputationValid(
    factionId attacker, factionId defender, WorldContext &ctx
) const
{
    // The defender's leader reputation in the ATTACKER's capital, below the
    // card's floor (constants.hpp). No justice store or no capital = the cb
    // cannot read = invalid (deterministic refusal, the R2 pattern).
    if (factionStore_ == nullptr || ctx.justiceStore == nullptr)
        return false;
    const Faction *a = factionStore_->find(attacker);
    const Faction *d = factionStore_->find(defender);
    if (a == nullptr || d == nullptr || a->capital == 0)
        return false;
    const characterId leader = leadingMember(defender);
    if (leader == 0)
        return false;
    return getReputationIn(*ctx.justiceStore, leader, a->capital, ctx.now) < WAR_REPUTATION_CB_FLOOR;
}

bool WarStore::cbMassacreValid(
    factionId attacker, factionId defender, CasusBelli &cb
) const
{
    // 4.7's hook: a marker on ground the DECLARING side controls, naming the
    // defender's faction as the culprit's. First (territory-ascending, R7)
    // match fills the cb's provenance.
    if (ctx_ == nullptr)
        return false;
    for (const auto &[territory, marker] : massacreMarkers_)
    {
        if (marker.culpritFaction != defender)
            continue;
        if (!controlsTerritory(territory, attacker, *ctx_))
            continue;
        cb.origin                      = territory;
        const world::Territory *origin = ctx_->worldStore != nullptr ? ctx_->worldStore->territory(territory) : nullptr;
        cb.region                      = origin != nullptr ? origin->region : world::kNoRegion;
        return true;
    }
    return false;
}

bool WarStore::cbPowerRatioValid(
    factionId attacker, factionId defender, WorldContext &ctx
) const
{
    // A-109: the attacker's aggregate power reaches 1.5x the defender's.
    // Zero-guard (R2): a memberless side never wars (both-zero would
    // otherwise read 0 >= 0 and pass).
    const std::uint64_t attackerPower = aggregatePower(attacker, ctx);
    const std::uint64_t defenderPower = aggregatePower(defender, ctx);
    if (attackerPower == 0 || defenderPower == 0)
        return false;
    return attackerPower * SCALE >= defenderPower * WAR_POWER_RATIO;
}

WarResult WarStore::declareWar(
    factionId attacker, factionId defender, CasusBelli &cb, WorldContext &ctx
)
{
    if (factionStore_ == nullptr)
        return WarResult::err(WarError::NoFactionBinding);
    const Faction *a = factionStore_->find(attacker);
    const Faction *d = factionStore_->find(defender);
    if (a == nullptr || d == nullptr)
        return WarResult::err(WarError::UnknownFaction);
    if (attacker == defender)
        return WarResult::err(WarError::SameFaction); // a faction holds no war against itself
    if (activeWarBetween(attacker, defender) != nullptr)
        return WarResult::err(WarError::AlreadyAtWar); // notice included: a pending war blocks too

    // The mandatory peace treaty (A-112), both directions.
    const std::uint64_t now = ctx.now;
    for (const auto &[from, to] : {std::pair{attacker, defender}, std::pair{defender, attacker}})
    {
        const Relation *row = factionStore_->relationBetween(from, to);
        if (row != nullptr && now < row->treatyUntil)
        {
            return WarResult::err(WarError::TreatyUntil);
        }
    }

    // The casus belli: the brought kind validates on its own merits; the
    // card's "— or PowerRatio" arm is the fallback (the power ratio needs no
    // marker). Both fail = NoCasusBelli.
    bool valid = false;
    switch (cb.kind)
    {
    case CasusBelliKind::BorderDispute:
        valid = cbBorderDisputeValid(attacker, defender, ctx);
        break;
    case CasusBelliKind::ResourceConflict:
        valid = cbResourceConflictValid(attacker, defender, cb, ctx);
        break;
    case CasusBelliKind::Reputation:
        valid = cbReputationValid(attacker, defender, ctx);
        break;
    case CasusBelliKind::Massacre:
        valid = cbMassacreValid(attacker, defender, cb);
        break;
    case CasusBelliKind::PowerRatio:
        valid = cbPowerRatioValid(attacker, defender, ctx);
        break;
    }
    if (!valid && cbPowerRatioValid(attacker, defender, ctx))
    {
        cb.kind   = CasusBelliKind::PowerRatio;
        cb.region = world::kNoRegion;
        cb.origin = 0;
        valid     = true;
    }
    if (!valid)
        return WarResult::err(WarError::NoCasusBelli);

    // The declaration: A-110's notice rides the record; IN_WAR flips at
    // noticeEndsAt (the derived status), never here.
    War war;
    war.id                    = nextWarId_++;
    war.attacker              = attacker;
    war.defender              = defender;
    war.cb                    = cb;
    war.declaredAt            = now;
    war.noticeEndsAt          = now + WAR_NOTICE_GAME_S;
    war.active                = true;
    const std::uint64_t warId = war.id;
    wars_.emplace(warId, war);

    // The defender auto-notification (doc): a PlayerSignal{War} when the
    // defender side is the player's (D-WAR-SIGNAL-1); the NPC court reads the
    // war record itself (the notice IS the record's noticeEndsAt).
    if (ctx.simEvents != nullptr && ctx.world != nullptr && ctx.world->playerCharacterId() != 0
        && factionStore_->factionOfCharacter(ctx.world->playerCharacterId()) == defender)
    {
        PlayerSignal signal;
        signal.kind = PlayerSignal::Kind::War;
        signal.text = "War declared: faction " + std::to_string(attacker) + " marches against faction "
                    + std::to_string(defender) + " — notice ends in 24 game-hours.";
        ctx.simEvents->onPlayerSignal(signal);
    }

    // The contested regions' map fact (0.7 rule 1: published AFTER the
    // mutation) — the Map panel's war overlays ride it.
    if (ctx.simEvents != nullptr && ctx.worldStore != nullptr)
    {
        for (const regionId r : contestedRegionsOf(wars_[warId], ctx))
        {
            const territoryId t = ctx.worldStore->territoryOfRegion(r);
            if (t != 0)
            {
                ctx.simEvents->onTerritoryChanged(TerritoryChanged{t, TerritoryChanged::What::War});
            }
        }
    }

    // The Massacre cause is now war: consume the markers that named it.
    for (auto it = massacreMarkers_.begin(); it != massacreMarkers_.end();)
    {
        if (it->second.culpritFaction == defender && controlsTerritory(it->first, attacker, ctx))
        {
            it = massacreMarkers_.erase(it);
        } else
        {
            ++it;
        }
    }
    return WarResult::ok();
}

// ---- resolution -----------------------------------------------------------------------

void WarStore::tick(
    WorldContext &ctx
)
{
    if (ctx_ == nullptr)
    {
        ctx_ = &ctx; // the first tick binds (the plugin's context is the run's one context)
    }
    monthlyAutoDeclare(ctx);

    // The progression sweep's cadence gate (the 8.3 as-built, DEVIATIONS.md):
    // the collapse/attrition arm and the battle-spawn attempt ride the
    // WAR_CHECK_INTERVAL_GAME_S sweep — re-walking both factions' full
    // membership (aggregate power) plus the territory graph (contested
    // regions) every tick measured ~4.5 ms/tick on the 8.3 soak's step
    // profile. The doc's resolution rule is a state condition, not a per-tick
    // poll: the sweep lands at most one game-hour late. The timeout arm below
    // stays per-tick (a stamp compare), and a freshly created store checks on
    // its first tick (the stamp starts at 0 = due).
    const bool sweepDue = ctx.now >= nextProgressCheckAt_;
    std::map<factionId, std::uint64_t> powerMemo; // one aggregate per faction per sweep pass
    auto memoizedAggregate = [&](factionId faction) -> std::uint64_t {
        const auto it = powerMemo.find(faction);
        if (it != powerMemo.end())
        {
            return it->second;
        }
        const std::uint64_t power = aggregatePower(faction, ctx);
        powerMemo.emplace(faction, power);
        return power;
    };

    // Resolution first (id-ascending, R7): a collapsed war ends before any
    // battle spawns — an empty battlefield never hosts a BATTLE event.
    for (auto &[id, war] : wars_)
    {
        (void)id;
        if (!war.active || !warEffectivelyActive(war, ctx.now))
            continue;
        if (sweepDue)
        {
            const std::uint64_t attackerPower = memoizedAggregate(war.attacker);
            const std::uint64_t defenderPower = memoizedAggregate(war.defender);
            if (attackerPower * SCALE < defenderPower * WAR_COLLAPSE_RATIO)
            {
                endWar(war.id, WarOutcome::DefenderWon, ctx); // the attacker collapsed
                continue;
            }
            if (defenderPower * SCALE < attackerPower * WAR_COLLAPSE_RATIO)
            {
                endWar(war.id, WarOutcome::AttackerWon, ctx); // the defender collapsed
                continue;
            }
        }
        if (ctx.now >= war.declaredAt + WAR_MAX_DURATION_DAYS * core::DAY_LENGTH_SECONDS)
        {
            endWar(war.id, WarOutcome::DefenderWon, ctx); // the timeout: the attacker loses
            continue;
        }

        // The battle progression (D-WAR-EVT-1): one live BATTLE per contested
        // region — the engine's scale-uniqueness gate is the throttle, the
        // BATTLE template's maxDurationDays the cadence. Both sides' living
        // members join as targets of each other (the war module computes the
        // lists: faction membership is its data, the engine stays
        // faction-blind — D-SINK-1 cuts both ways). The attempt rides the
        // progression sweep (the member-list build is a full both-factions
        // walk and contestedRegionsOf a territory-graph scan — per-tick cost
        // the 8.3 soak flagged; the scale-uniqueness throttle is unchanged).
        if (!sweepDue || ctx.eventSpawner == nullptr || factionStore_ == nullptr || ctx.characters == nullptr)
            continue;
        for (const regionId r : contestedRegionsOf(war, ctx))
        {
            WarBattleSeed seed;
            seed.warId    = war.id;
            seed.region   = r;
            seed.attacker = war.attacker;
            seed.defender = war.defender;
            for (const Faction *f : {factionStore_->find(war.attacker), factionStore_->find(war.defender)})
            {
                if (f == nullptr)
                    continue;
                std::vector<characterId> &side = f->id == war.attacker ? seed.attackerMembers : seed.defenderMembers;
                for (const characterId member : f->members)
                {
                    if (ctx.characters->exists(member) && ctx.characters->get(member).body.currentHp != 0)
                    {
                        side.push_back(member); // the set's own id-ascending order (R7)
                    }
                }
            }
            (void)ctx.eventSpawner->spawnWarBattle(seed); // refused = a battle already stands there
        }
    }

    if (sweepDue)
    {
        nextProgressCheckAt_ = ctx.now + WAR_CHECK_INTERVAL_GAME_S;
    }
}

void WarStore::endWar(
    std::uint64_t warId, WarOutcome outcome, WorldContext &ctx
)
{
    auto it = wars_.find(warId);
    if (it == wars_.end() || !it->second.active)
        return;
    War &war    = it->second;
    war.outcome = outcome;
    war.endedAt = ctx.now;
    war.active  = false;

    const factionId winner         = outcome == WarOutcome::AttackerWon ? war.attacker
                                   : outcome == WarOutcome::DefenderWon ? war.defender
                                                                        : 0;
    const factionId loser          = outcome == WarOutcome::AttackerWon ? war.defender
                                   : outcome == WarOutcome::DefenderWon ? war.attacker
                                                                        : 0;
    const characterId winnerLeader = winner != 0 ? leadingMember(winner) : 0;

    // The outcome effects (doc: "Outcomes modify region flags and territorial
    // control"), region-ascending (R7). Flags are data-defined (the shipped
    // regionFlags table's "war_torn" bit); a table without the row skips the
    // flag set deterministically (the R2 pattern).
    std::uint32_t warTornBit = 0;
    if (ctx.data != nullptr)
    {
        const auto &flags = ctx.data->territoryTemplates().regionFlags();
        const auto flagIt = flags.find("war_torn");
        if (flagIt != flags.end())
        {
            warTornBit = flagIt->second;
        }
    }
    if (ctx.worldStore != nullptr)
    {
        for (const regionId r : contestedRegionsOf(war, ctx))
        {
            world::Region *region = ctx.worldStore->region(r);
            if (region != nullptr && warTornBit != 0 && (region->flags & warTornBit) == 0)
            {
                region->flags |= warTornBit;
                const territoryId t = ctx.worldStore->territoryOfRegion(r);
                if (t != 0 && ctx.simEvents != nullptr)
                {
                    ctx.simEvents->onTerritoryChanged(TerritoryChanged{t, TerritoryChanged::What::War});
                }
            }
            if (winner == 0 || loser == 0)
                continue;

            const territoryId t         = ctx.worldStore->territoryOfRegion(r);
            world::Territory *territory = t != 0 ? ctx.worldStore->territory(t) : nullptr;
            if (territory == nullptr)
                continue;

            // Territorial control, player-owned arm: the loser's claim
            // transfers to the winner's leading member (the claim rewiring
            // per 6.3's rules — the ledger row follows with a fresh timer,
            // the installed laws stay).
            if (territory->owner != 0 && factionStore_ != nullptr
                && factionStore_->factionOfCharacter(territory->owner) == loser)
            {
                territory->owner = winnerLeader;
                if (ctx.claims != nullptr)
                {
                    ctx.claims->noteOwnershipTransfer(t, winnerLeader, ctx.realTimeSeconds);
                }
                if (ctx.simEvents != nullptr)
                {
                    ctx.simEvents->onTerritoryChanged(TerritoryChanged{t, TerritoryChanged::What::Owner});
                }
            }

            // NPC-ruled arm: the loser's title-holders are replaced by the
            // winner's appointees — the seat's social class preserved
            // (socialClassOf reads the new titles naturally, D-JUR-1).
            bool titlesMoved = false;
            for (auto titleIt = territory->titles.begin(); titleIt != territory->titles.end();)
            {
                if (factionStore_ != nullptr && factionStore_->factionOfCharacter(titleIt->first) == loser)
                {
                    const SocialClassId cls = titleIt->second;
                    titleIt                 = territory->titles.erase(titleIt);
                    if (winnerLeader != 0)
                    {
                        territory->titles[winnerLeader] = cls;
                    }
                    titlesMoved = true;
                } else
                {
                    ++titleIt;
                }
            }
            if (titlesMoved && ctx.simEvents != nullptr)
            {
                ctx.simEvents->onTerritoryChanged(TerritoryChanged{t, TerritoryChanged::What::War});
            }
        }
    }

    // The mandatory peace treaty (A-112): the pair may not re-declare for a
    // week — stamped both directions (the mirror-row rule).
    if (factionStore_ != nullptr)
    {
        factionStore_->stampTreaty(war.attacker, war.defender, ctx.now + PEACE_TREATY_DAYS * core::DAY_LENGTH_SECONDS);
    }

    // The war's Massacre cause is spent with it.
    for (auto markerIt = massacreMarkers_.begin(); markerIt != massacreMarkers_.end();)
    {
        if (loser != 0 && markerIt->second.culpritFaction == loser && winner != 0 && ctx.worldStore != nullptr
            && controlsTerritory(markerIt->first, winner, ctx))
        {
            markerIt = massacreMarkers_.erase(markerIt);
        } else
        {
            ++markerIt;
        }
    }
}

// ---- the monthly NPC auto-declaration sweep ----------------------------------------------

void WarStore::monthlyAutoDeclare(
    WorldContext &ctx
)
{
    if (factionStore_ == nullptr)
        return;
    // The lazy month gate (the 6.1 drift's pattern): one whole month applies
    // at most once, whatever the tick cadence.
    const std::uint64_t month = ctx.now / kMonthGameSeconds;
    if (month == lastAutoDeclareMonth_)
        return;
    lastAutoDeclareMonth_ = month;

    // Rival pairs, id-ascending (R7): the drift (6.1) is the driver — wars
    // arise from casus belli between factions that already feud, never from a
    // bare timer. First valid cb wins (the pinned enum order).
    for (const auto &[aId, factionA] : factionStore_->all())
    {
        (void)factionA;
        for (const auto &[bId, factionB] : factionStore_->all())
        {
            (void)factionB;
            if (bId <= aId)
                continue; // one visit per pair
            if (activeWarBetween(aId, bId) != nullptr)
                continue;
            const Relation *row = factionStore_->relationBetween(aId, bId);
            if (row == nullptr || !row->rivalry)
                continue;
            const Relation *mirror = factionStore_->relationBetween(bId, aId);
            const bool treatyHolds = (row != nullptr && ctx.now < row->treatyUntil)
                                  || (mirror != nullptr && ctx.now < mirror->treatyUntil);
            if (treatyHolds)
                continue;
            for (const CasusBelliKind kind :
                {CasusBelliKind::BorderDispute,
                    CasusBelliKind::ResourceConflict,
                    CasusBelliKind::Reputation,
                    CasusBelliKind::Massacre,
                    CasusBelliKind::PowerRatio})
            {
                CasusBelli cb{kind, world::kNoRegion, 0};
                if (declareWar(aId, bId, cb, ctx).isOk())
                {
                    break; // one declaration per pair per sweep
                }
            }
        }
    }
}

// ---- save seam (task 7.1) -------------------------------------------------------

WarStore::Snapshot WarStore::snapshot() const
{
    Snapshot out;
    out.wars                 = wars_;
    out.nextWarId            = nextWarId_;
    out.lastAutoDeclareMonth = lastAutoDeclareMonth_;
    out.nextProgressCheckAt  = nextProgressCheckAt_;
    for (const auto &[territory, marker] : massacreMarkers_)
    {
        out.massacreMarkers.emplace(territory, std::make_pair(marker.culpritFaction, marker.at));
    }
    return out;
}

void WarStore::restore(
    const Snapshot &saved
)
{
    wars_                 = saved.wars;
    nextWarId_            = saved.nextWarId;
    lastAutoDeclareMonth_ = saved.lastAutoDeclareMonth;
    nextProgressCheckAt_  = saved.nextProgressCheckAt;
    massacreMarkers_.clear();
    for (const auto &[territory, marker] : saved.massacreMarkers)
    {
        MassacreMarker restored;
        restored.culpritFaction = marker.first;
        restored.at             = marker.second;
        massacreMarkers_.emplace(territory, restored);
    }
}

} // namespace harrowmere::factions
