// citizen_justice.cpp — task 4.7's implementation (see citizen_justice.hpp
// for the pinned reading). Suspicion appears nowhere in this file's gate;
// the casus belli hook rides the IWarRegistry frozen stub (6.2 implements
// it in Phase 4, D-SINK-1) and the reward exclusion rides the
// IEventExclusionSink seam (5.3 consumes the set).

#include "justice/citizen_justice.hpp"

#include <algorithm>

#include "character/character.hpp"
#include "character/species.hpp"     // isSapient (the massacre gate)
#include "core/constants.hpp"        // A-97..A-102, A-71, A-54, A-60, SCALE
#include "core/constants_phase2.hpp" // WITNESS_RADIUS_M
#include "core/world_context.hpp"
#include "justice/cases.hpp"    // findEvidence (the warrant's bounty oracle)
#include "justice/evidence.hpp" // recordAccomplice (the smuggling evidence)
#include "justice/justice_store.hpp"
#include "justice/warrants.hpp"  // payWarrant + the warrant maps
#include "npc/relationships.hpp" // A-54's affinity read (the willing-citizen rule)
#include "npc/state.hpp"
#include "progression/power.hpp" // getCurrentPower (the odds comparison)
#include "world/world.hpp"

namespace harrowmere::justice
{

namespace
{

bool isProcessorRole(
    const std::string &roleKey
)
{
    // The claim's processor: guard, captain or militia_veteran ("claim via
    // guard/captain NPC" — the veterans process a posse's claims).
    return roleKey == "guard" || roleKey == "captain" || roleKey == "militia_veteran";
}

} // namespace

// ---- the gate ------------------------------------------------------------------

bool witnessedActiveCrime(
    characterId npc, characterId target, WorldContext &ctx
)
{
    // The in-progress state is 3.2's assault accumulator (noteAssault holds
    // the unconsented damage since the last recorded ASSAULT); "personally
    // witnesses" is the spatial seam: the target within WITNESS_RADIUS of
    // the NPC. Crossing ASSAULT_THRESHOLD records the evidence and resets
    // the pair — the accumulator is exactly the in-progress window.
    if (ctx.justiceStore == nullptr || ctx.characters == nullptr || ctx.perception == nullptr)
    {
        return false;
    }
    const auto pair = ctx.justiceStore->assaultDamage.find(target);
    if (pair == ctx.justiceStore->assaultDamage.end() || pair->second.empty())
    {
        return false; // no in-progress assault by the target
    }
    if (!ctx.characters->exists(npc) || !ctx.characters->exists(target))
    {
        return false;
    }
    const std::vector<characterId> seen
        = ctx.perception->perceiveWithin(ctx.characters->get(npc).body.position, WITNESS_RADIUS_M);
    return std::find(seen.begin(), seen.end(), target) != seen.end();
}

bool convictedRecord(
    characterId target, territoryId territory, WorldContext &ctx
)
{
    if (ctx.worldStore == nullptr)
    {
        return false;
    }
    const world::Territory *node = ctx.worldStore->territory(territory);
    if (node == nullptr)
    {
        return false;
    }
    // "Recent compact record": archived within the A-71 case window — the
    // same freshness a live case would carry.
    for (const world::CompactRecord &record : node->archive)
    {
        if (record.culprit == target && ctx.now >= record.worldTime
            && ctx.now - record.worldTime <= LIMIT_DAYS * core::DAY_LENGTH_SECONDS)
        {
            return true;
        }
    }
    return false;
}

bool npcMayViolate(
    characterId npc, characterId target, WorldContext &ctx
)
{
    // The doc's gate, verbatim — suspicion appears NOWHERE (the pinned test
    // fills a memory to SHUNNED and pins the refusal).
    if (ctx.worldStore == nullptr || ctx.characters == nullptr || !ctx.characters->exists(npc))
    {
        return false;
    }
    if (witnessedActiveCrime(npc, target, ctx))
    {
        return true;
    }
    const Position &npcPos = ctx.characters->get(npc).body.position;
    const territoryId here = ctx.worldStore->territoryAt(npcPos);
    if (here != 0 && hasWarrantIn(ctx, target, here))
    {
        return true;
    }
    return here != 0 && convictedRecord(target, here, ctx);
}

// ---- militias ------------------------------------------------------------------

Posse formPosse(
    const Warrant &warrant, WorldContext &ctx
)
{
    Posse posse;
    if (ctx.npcs == nullptr || ctx.characters == nullptr || ctx.data == nullptr || ctx.itemInstances == nullptr
        || ctx.justiceStore == nullptr)
    {
        return posse;
    }
    // The victim the willingness reads against (the warrant's evidence).
    std::uint64_t victimId = 0;
    if (const Evidence *e = findEvidence(*ctx.justiceStore, warrant.evidenceId); e != nullptr)
    {
        victimId = e->victimId;
    }

    // militia_veteran NPCs of the warrant's territory first (id-ascending,
    // R7), then willing citizens: affinity to the victim >= A-54 (2000) and
    // integrity >= MILITIA_CITIZEN_INTEGRITY.
    for (const auto &[id, npc] : ctx.npcs->all())
    {
        if (npc.homeTerritory != warrant.territory || !ctx.characters->exists(id))
        {
            continue;
        }
        const bool veteran = npc.roleKey == "militia_veteran";
        bool willing       = veteran;
        if (!willing)
        {
            const auto affinity = npc.affinity.find(victimId);
            willing = victimId != 0 && affinity != npc.affinity.end() && affinity->second >= ACQUAINTANCE_AFFINITY
                   && npc.integrity >= MILITIA_CITIZEN_INTEGRITY;
        }
        if (willing)
        {
            posse.members.push_back(id);
            posse.totalPower += getCurrentPower(ctx.characters->get(id), *ctx.data, *ctx.itemInstances);
        }
    }

    // "Veteran militias engage only at favorable odds": the summed posse
    // power reaches MILITIA_ODDS_RATIO (A-97, 2.0x) of the target's power —
    // else shadow & report.
    std::uint64_t targetPower = 0;
    if (ctx.characters->exists(warrant.culpritId))
    {
        targetPower = getCurrentPower(ctx.characters->get(warrant.culpritId), *ctx.data, *ctx.itemInstances);
    }
    posse.engages = posse.totalPower >= MILITIA_ODDS_RATIO * targetPower / SCALE && posse.totalPower > 0;
    return posse;
}

// ---- capture & bounties ----------------------------------------------------------

std::uint64_t bountyPayoutFor(
    const Warrant &warrant, bool capturedAlive, WorldContext &ctx
)
{
    std::uint64_t payout = capturedAlive ? warrant.bounty : warrant.bounty * DEAD_BOUNTY_RATIO / SCALE;
    if (!capturedAlive)
    {
        return payout; // the fatigue is a CAPTURE tax — a kill claim is not a capture
    }
    if (ctx.justiceStore == nullptr)
    {
        return payout;
    }
    // Each capture of the same culprit within the A-99 window compounds the
    // A-100 factor (0.5x, compounding — the doc's exploit ceiling: three
    // captures in the window pay 1.0x -> 0.5x -> 0.25x of one bounty).
    const auto stamps = ctx.justiceStore->captureStamps.find(warrant.culpritId);
    if (stamps == ctx.justiceStore->captureStamps.end())
    {
        return payout;
    }
    const std::uint64_t window = CAPTURE_FATIGUE_WINDOW_DAYS * core::DAY_LENGTH_SECONDS;
    for (auto it = stamps->second.rbegin(); it != stamps->second.rend(); ++it)
    {
        if (ctx.now - *it > window)
        {
            break; // the stamps are chronological; the rest is out of window
        }
        payout = payout * CAPTURE_FATIGUE_FACTOR / SCALE;
    }
    return payout;
}

BountyClaim claimBounty(
    std::uint64_t warrantId, characterId claimant, bool capturedAlive, WorldContext &ctx
)
{
    if (ctx.justiceStore == nullptr || ctx.characters == nullptr || ctx.npcs == nullptr)
    {
        return BountyClaim::UnknownWarrant;
    }
    const auto it = ctx.justiceStore->warrants.find(warrantId);
    if (it == ctx.justiceStore->warrants.end())
    {
        return BountyClaim::UnknownWarrant; // paid/expired — the pay-once rule's store form
    }
    const Warrant &warrant = it->second;

    // The claim processes through a guard/captain/militia_veteran present in
    // the warrant's territory (no processor, no claim).
    bool processor = false;
    for (const auto &[id, npc] : ctx.npcs->all())
    {
        if (npc.homeTerritory == warrant.territory && isProcessorRole(npc.roleKey) && ctx.characters->exists(id))
        {
            processor = true;
            break;
        }
    }
    if (!processor)
    {
        return BountyClaim::NoProcessor;
    }

    const std::uint64_t payout = bountyPayoutFor(warrant, capturedAlive, ctx);
    if (capturedAlive)
    {
        ctx.justiceStore->captureStamps[warrant.culpritId].push_back(ctx.now);
    }
    if (ctx.characters->exists(claimant))
    {
        ctx.characters->get(claimant).progression.currency += static_cast<std::int64_t>(payout);
    }
    payWarrant(warrantId, ctx); // the paid flag + BOTH maps + WarrantResolved{Paid}
    return BountyClaim::Paid;
}

// ---- smuggling -------------------------------------------------------------------

void recordSmuggling(
    characterId helper, characterId aidedCulprit, WorldContext &ctx
)
{
    // The doc's shape: "if a character IN AN EVENT with the culprit as a
    // *target-side* participant aids them" — the HELPER's shared event is
    // the gate, and the culprit rides that event's TARGET side (the manhunt
    // shape: targets are not participants and carry no eventId). "Watching
    // is never punished" — mere co-presence is not aiding.
    if (ctx.characters == nullptr || ctx.eventExclusion == nullptr || ctx.worldStore == nullptr)
    {
        return;
    }
    if (!ctx.characters->exists(helper) || !ctx.characters->exists(aidedCulprit))
    {
        return;
    }
    const std::uint64_t sharedEvent = ctx.characters->get(helper).eventId;
    if (sharedEvent == 0)
    {
        return;
    }
    if (!ctx.eventExclusion->isTargetSideParticipant(aidedCulprit, sharedEvent))
    {
        return; // the aided one is not the event's target side: aiding is not smuggling
    }
    // The accomplice evidence at the culprit's position (4.1's trigger list
    // routes here), then the reward exclusion (5.3 respects the set).
    recordAccomplice(helper, aidedCulprit, CrimeType::SMUGGLING, ctx.characters->get(aidedCulprit).body.position, ctx);
    ctx.eventExclusion->excludeFromRewards(helper, sharedEvent);
}

// ---- village slaughter -----------------------------------------------------------

void noteNpcKilled(
    characterId victim, characterId killer, WorldContext &ctx
)
{
    if (ctx.justiceStore == nullptr || ctx.worldStore == nullptr || ctx.data == nullptr || ctx.characters == nullptr)
    {
        return;
    }
    // The victim's species must be sapient (a hunting spree is not a
    // massacre — the doc's "sapient NPC deaths").
    if (!ctx.characters->exists(victim) || !isSapient(*ctx.data, ctx.characters->get(victim).species))
    {
        return;
    }
    const territoryId territory = ctx.worldStore->territoryAt(ctx.characters->get(victim).body.position);
    auto &tally                 = ctx.justiceStore->killTally[killer];
    tally.emplace_back(territory, ctx.now);

    // Prune to the A-102 window (chronological order; drop the stale head).
    const std::uint64_t window = MASSACRE_WINDOW_DAYS * core::DAY_LENGTH_SECONDS;
    const auto newHead
        = std::find_if(tally.begin(), tally.end(), [&](const std::pair<territoryId, std::uint64_t> &row) {
              return ctx.now - row.second <= window;
          });
    tally.erase(tally.begin(), newHead);

    // The threshold: >= MASSACRE_THRESHOLD (A-101) sapient deaths in ONE
    // territory inside the window.
    std::map<territoryId, std::uint32_t> perTerritory;
    for (const auto &[terr, at] : tally)
    {
        ++perTerritory[terr];
    }
    for (const auto &[terr, count] : perTerritory)
    {
        if (count < MASSACRE_THRESHOLD)
        {
            continue;
        }
        auto marker      = ctx.justiceStore->massacreMarkers.find(killer);
        const bool fresh = marker == ctx.justiceStore->massacreMarkers.end() || marker->second.count(terr) == 0;
        if (fresh)
        {
            ctx.justiceStore->massacreMarkers[killer][terr] = ctx.now;
            // The casus belli for the NEIGHBORING territories: the parent
            // and the parent's other children (the tree's adjacency in v1).
            // The registry stub records; 6.2 implements against the frozen
            // signature (D-SINK-1). The culprit's faction rides their
            // Character row (0 until 6.1 lands — the player's own case).
            const factionId culpritFaction = ctx.characters->exists(killer) ? ctx.characters->get(killer).faction : 0;
            const territoryId parent       = ctx.worldStore->parentOf(terr);
            if (ctx.wars != nullptr)
            {
                if (parent != 0)
                {
                    ctx.wars->registerCasusBelli(parent, killer, culpritFaction);
                }
                for (const territoryId sibling : ctx.worldStore->childrenOf(parent))
                {
                    if (sibling != terr)
                    {
                        ctx.wars->registerCasusBelli(sibling, killer, culpritFaction);
                    }
                }
            }
        }
    }
}

std::int64_t massacreLootCurrency(
    characterId killer, territoryId territory, std::int64_t purse, WorldContext &ctx
)
{
    // "Village slaughter grants zero economic gain": the victims' corpse
    // loot currency zeroes while the marker stands; the loot items remain.
    if (ctx.justiceStore == nullptr)
    {
        return purse;
    }
    const auto marker = ctx.justiceStore->massacreMarkers.find(killer);
    if (marker != ctx.justiceStore->massacreMarkers.end() && marker->second.count(territory) != 0)
    {
        return 0;
    }
    return purse;
}

} // namespace harrowmere::justice
