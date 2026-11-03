// persist/state_hash.cpp — the canonical walk (see state_hash.hpp for the
// contract). The field order IS the serialization order of the simulation:
// every store in the save schema's order, every map in key order (R7), every
// character/NPC row in id order. The exact field set mirrors what save.cpp
// serializes — a field added to one and not the other fails the roundtrip
// hash test loudly, which is the point.

#include "persist/state_hash.hpp"

#include <cmath>
#include <limits>

#include "character/appearance.hpp"
#include "character/character.hpp"
#include "character/store.hpp"
#include "combat/attacks.hpp"
#include "combat/consent.hpp"
#include "combat/death.hpp"
#include "core/clock.hpp"
#include "core/constants_phase2.hpp"
#include "core/rng.hpp"
#include "economy/economy.hpp"
#include "events/engine.hpp"
#include "factions/claims.hpp"
#include "factions/factions.hpp"
#include "factions/war.hpp"
#include "items/inventory.hpp"
#include "justice/evidence.hpp"
#include "justice/reputation.hpp"
#include "justice/warrants.hpp"
#include "justice/justice_store.hpp"
#include "npc/roles.hpp"
#include "npc/state.hpp"
#include "world/region.hpp"
#include "world/world.hpp"

namespace harrowmere::persist
{
namespace
{

constexpr double kPositionScale = 1000.0; // llround(x * 1000) — 8.1's float exclusion

// A label prefix shared by the character/NPC rows (keeps the row labels
// short without losing uniqueness).
constexpr const char *kChar = "c";
constexpr const char *kNpc  = "n";

} // namespace

void StateHasher::mix(
    std::uint64_t value
)
{
    // FNV-1a 64-bit: multiply-then-xor, byte order low first (the standard
    // formulation; the prime is the algorithm's pinned constant).
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    for (int i = 0; i < 8; ++i)
    {
        state_ ^= (value >> (8 * i)) & 0xFFULL;
        state_ *= kPrime;
    }
}

void StateHasher::mixSigned(
    std::int64_t value
)
{
    mix(static_cast<std::uint64_t>(value)); // two's complement bit pattern — deterministic
}

void StateHasher::mixBool(
    bool value
)
{
    mix(value ? 1ULL : 0ULL);
}

void StateHasher::mixF64(
    double value
)
{
    // Floats are excluded (8.1): positions (the only f64 in persisted state)
    // hash as llround(x*1000). A NaN/inf can only mean a broken invariant
    // (the soak's no-NaN check exists for exactly that) — hash the sentinel
    // so the digest still distinguishes the state instead of looping.
    if (!std::isfinite(value))
    {
        mix(std::numeric_limits<std::uint64_t>::max());
        return;
    }
    const long long scaled = std::llround(value * kPositionScale);
    mix(static_cast<std::uint64_t>(scaled));
}

void StateHasher::mixText(
    const std::string &text
)
{
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    mix(static_cast<std::uint64_t>(text.size()));
    for (const char c : text)
    {
        state_ ^= static_cast<std::uint8_t>(c);
        state_ *= kPrime;
    }
}

namespace
{

void hashPosition(
    StateHasher &h, const char *label, const Position &p
)
{
    h.mixText(label);
    h.mixF64(p.x);
    h.mixF64(p.y);
    h.mixF64(p.z);
}

void hashClock(
    StateHasher &h, const core::WorldClock &clock
)
{
    h.mixText("clock.worldTime");
    h.mix(clock.worldTime);
    h.mixText("clock.realTime");
    h.mix(clock.realTime);
    h.mixText("clock.timeScale");
    h.mix(clock.timeScale);
    h.mixText("clock.tickRate");
    h.mix(clock.tickRate);
    h.mixText("clock.worldCarry");
    h.mix(clock.worldCarry);
    h.mixText("clock.realCarry");
    h.mix(clock.realCarry);
}

void hashRng(
    StateHasher &h, const core::SeedRng &rng
)
{
    // state(): 2 entries per pinned stream, alphabetical (R7 — the map's
    // own order). The label rides each pair's name.
    for (const auto &[name, value] : rng.state())
    {
        h.mixText("rng." + name);
        h.mix(value);
    }
}

void hashIds(
    StateHasher &h, const SaveSources &s
)
{
    h.mixText("ids.character");
    h.mix(s.characters->nextCharacterId());
    // The justice id spaces hash the values the save schema persists (the
    // peek pair save.cpp writes into /ids), not the raw counter map. The map
    // distinguishes "never drawn" (no entry) from "next id = start_" (an
    // entry at start_) — a distinction with no simulation meaning and none in
    // the save file (peek falls back to start_ either way, and a load
    // materializes the saved spaces). Hashing the raw map made two
    // behaviorally identical stores hash differently and flaked the 8.2
    // roundtrip property on every world whose save carried a never-drawn
    // space.
    h.mixText("ids.evidence");
    h.mix(s.justice->peekEvidenceId());
    h.mixText("ids.warrant");
    h.mix(s.justice->peekWarrantId());
    h.mixText("ids.event");
    h.mix(s.events->peekEventId());
    h.mixText("ids.itemInstance");
    h.mix(s.itemInstances->counter());
}

void hashWorld(
    StateHasher &h, const world::WorldStore &world
)
{
    for (const auto &[id, region] : world.regions())
    {
        const std::string p = "region[" + std::to_string(id) + "]";
        h.mixText(p + ".flags");
        h.mix(region.flags);
        h.mixText(p + ".mana");
        h.mix(region.manaRichness);
        h.mixText(p + ".climate");
        h.mix(static_cast<std::uint64_t>(region.climate));
        h.mixText(p + ".season");
        h.mix(region.season.index);
        h.mix(region.season.startedAtWorldTime);
        h.mixText(p + ".weather");
        h.mix(region.weather.kind);
        h.mix(region.weather.sinceWorldTime);
    }
    for (const auto &[id, territory] : world.territories())
    {
        const std::string p = "territory[" + std::to_string(id) + "]";
        h.mixText(p + ".parent");
        h.mix(territory.parent);
        h.mixText(p + ".type");
        h.mix(static_cast<std::uint64_t>(territory.type));
        h.mixText(p + ".safe");
        h.mixBool(territory.safe);
        h.mixText(p + ".lawless");
        h.mixBool(territory.lawless);
        h.mixText(p + ".owner");
        h.mix(territory.owner);
        h.mixText(p + ".region");
        h.mix(territory.region);
        h.mixText(p + ".lawKey");
        h.mixText(territory.laws != nullptr ? territory.laws->key : std::string());
        h.mixText(p + ".children");
        h.mix(territory.children.size());
        for (const territoryId child : territory.children)
        {
            h.mix(child);
        }
        h.mixText(p + ".titles");
        h.mix(territory.titles.size());
        for (const auto &[holder, cls] : territory.titles)
        {
            h.mix(holder);
            h.mix(static_cast<std::uint64_t>(cls));
        }
        h.mixText(p + ".archive");
        h.mix(territory.archive.size());
        for (const world::CompactRecord &row : territory.archive)
        {
            h.mix(row.evidenceId);
            h.mix(row.worldTime);
            h.mix(row.crimeType);
            h.mix(row.culprit);
            h.mix(row.victim);
        }
    }
    h.mixText("world.territoryIndex");
    h.mix(world.mappedRegionCount());
    for (const auto &[region, territory] : world.regionTerritories())
    {
        h.mix(region);
        h.mix(territory);
    }
    h.mixText("world.lawless");
    for (const auto &[id, territory] : world.territories())
    {
        (void)territory;
        if (world.isLawless(id))
        {
            h.mix(id);
        }
    }
    h.mixText("world.lawlessWatch");
    for (const auto &[id, territory] : world.territories())
    {
        (void)territory;
        if (world.isGarrisonLessWatched(id))
        {
            h.mix(id);
            h.mix(world.lawlessWatchStart(id));
        }
    }
    // playerSpawn lives on the CharacterStore (the adoption point); hashed there.
}

void hashInventory(
    StateHasher &h, const char *label, const items::Inventory &bag
)
{
    h.mixText(std::string(label) + ".stacks");
    h.mix(bag.stacks.size());
    for (const auto &[itemKey, count] : bag.stacks)
    {
        h.mix(itemKey);
        h.mix(count);
    }
    h.mixText(std::string(label) + ".equipped");
    h.mix(bag.equipped.size());
    for (const std::uint64_t id : bag.equipped)
    {
        h.mix(id);
    }
    h.mixText(std::string(label) + ".instances");
    h.mix(bag.instances.size());
    for (const std::uint64_t id : bag.instances)
    {
        h.mix(id);
    }
}

void hashNpcState(
    StateHasher &h, const NpcState &npc, const std::string &p
)
{
    // The persisted field set exactly. The per-tick block (the 8.3 as-built)
    // is persisted sim state now (the 7.1 continuation contract needs the
    // intent/migration walks and the pacing stamps once NEAR work runs from
    // tick 1) — so it hashes with everything else.
    h.mixText(p + ".name");
    h.mixText(npc.name);
    h.mixText(p + ".birth");
    h.mix(npc.birthWorldTime);
    h.mixText(p + ".roleKey");
    h.mixText(npc.roleKey);
    h.mixText(p + ".needs");
    h.mix(npc.needs.hunger);
    h.mix(npc.needs.rest);
    h.mix(npc.needs.wealth);
    h.mix(npc.needs.social);
    h.mixText(p + ".integrity");
    h.mix(npc.integrity);
    h.mixText(p + ".trust.score");
    h.mix(npc.trust.suspicionScore);
    h.mixText(p + ".trust.perCharacter");
    h.mix(npc.trust.perCharacterScore.size());
    for (const auto &[who, score] : npc.trust.perCharacterScore)
    {
        h.mix(who);
        h.mix(score);
    }
    h.mixText(p + ".trust.lastSeen");
    h.mix(npc.trust.lastSeen.size());
    for (const auto &[who, at] : npc.trust.lastSeen)
    {
        h.mix(who);
        h.mix(at);
    }
    h.mixText(p + ".trust.lastDecay");
    h.mix(npc.trust.lastSuspicionDecay);
    h.mixText(p + ".affinity");
    h.mix(npc.affinity.size());
    for (const auto &[who, value] : npc.affinity)
    {
        h.mix(who);
        h.mixSigned(value);
    }
    h.mixText(p + ".actionCounters");
    h.mix(npc.actionCounters.size());
    for (const auto &[key, value] : npc.actionCounters)
    {
        h.mixText(key);
        h.mix(value);
    }
    h.mixText(p + ".homeTerritory");
    h.mix(npc.homeTerritory);
    h.mixText(p + ".currentRegion");
    h.mix(npc.currentRegion);
    hashInventory(h, (p + ".inventory").c_str(), npc.inventory);
    h.mixText(p + ".invulnerable");
    h.mixBool(npc.invulnerable);
    h.mixText(p + ".migrated");
    h.mixBool(npc.migrated);
    h.mixText(p + ".fleeing");
    h.mixBool(npc.fleeing);
    h.mixText(p + ".fleeingUntil");
    h.mix(npc.fleeingUntil);
    // The per-tick block (the 8.3 as-built — the save's "perTick" object).
    h.mixText(p + ".perTick.tier");
    h.mix(static_cast<std::uint64_t>(npc.tier));
    h.mixText(p + ".perTick.needsCarry");
    h.mix(npc.needsCarry);
    h.mixText(p + ".perTick.nextDuty");
    h.mix(npc.nextDutyWorldTime);
    h.mixText(p + ".perTick.lastMigrationRoll");
    h.mix(npc.lastMigrationRollWorldTime);
    h.mixText(p + ".perTick.intent");
    h.mix(static_cast<std::uint64_t>(npc.currentIntent.kind));
    h.mix(npc.currentIntent.target);
    h.mixText(p + ".perTick.migrating");
    h.mixBool(npc.migrating);
    h.mixText(p + ".perTick.migrationTarget");
    h.mixF64(npc.migrationTarget.x);
    h.mixF64(npc.migrationTarget.y);
    h.mixF64(npc.migrationTarget.z);
}

void hashCharacters(
    StateHasher &h, const SaveSources &s
)
{
    for (const auto &[id, c] : s.characters->all())
    {
        const std::string p = std::string(kChar) + "[" + std::to_string(id) + "]";
        h.mixText(p + ".id");
        h.mix(c.id);
        h.mixText(p + ".species");
        h.mix(c.species);
        h.mixText(p + ".statusFlags");
        h.mix(c.statusFlags);
        h.mixText(p + ".duelOpponent");
        h.mix(c.duelOpponent);
        h.mixText(p + ".arenaId");
        h.mix(c.arenaId);
        h.mixText(p + ".appearanceComponents");
        h.mix(c.appearanceComponents.size());
        for (const std::uint64_t v : c.appearanceComponents)
        {
            h.mix(v);
        }
        h.mixText(p + ".equippedItems");
        h.mix(c.equippedItems.size());
        for (const std::uint64_t v : c.equippedItems)
        {
            h.mix(v);
        }
        h.mixText(p + ".hp");
        h.mix(c.body.currentHp);
        h.mix(c.body.maxHp);
        hashPosition(h, (p + ".position").c_str(), c.body.position);
        h.mixText(p + ".xp");
        h.mix(c.progression.totalXp);
        h.mixSigned(c.progression.availableXp);
        h.mixText(p + ".level");
        h.mix(c.progression.level);
        h.mixText(p + ".stats");
        for (const std::uint64_t v : c.progression.stats)
        {
            h.mix(v);
        }
        h.mixText(p + ".skills");
        for (const std::uint64_t v : c.progression.skills)
        {
            h.mix(v);
        }
        h.mixText(p + ".currency");
        h.mixSigned(c.progression.currency);
        h.mixText(p + ".cooldowns");
        for (const std::uint64_t v : c.cooldowns)
        {
            h.mix(v);
        }
        h.mixText(p + ".faction");
        h.mix(c.faction);
        h.mixText(p + ".eventId");
        h.mix(c.eventId);
        h.mixText(p + ".lastCombatTime");
        h.mix(c.lastCombatTime);
        h.mixText(p + ".peakPower");
        h.mix(c.peakPower); // cachedPower/powerDirty: transient, excluded (7.1's do-not-save)
        h.mixText(p + ".isPlayer");
        h.mixBool(s.characters->isPlayer(id));
        if (s.npcs->contains(id))
        {
            hashNpcState(h, s.npcs->get(id), p + "." + kNpc);
        }
    }
    hashPosition(h, "world.playerSpawn", s.characters->playerSpawn());
}

void hashCorpses(
    StateHasher &h, const CorpseStore &corpses
)
{
    for (const auto &[victim, corpse] : corpses.all())
    {
        const std::string p = "corpse[" + std::to_string(victim) + "]";
        h.mixText(p + ".victim");
        h.mix(corpse.victim);
        hashPosition(h, (p + ".position").c_str(), corpse.pos);
        h.mixText(p + ".deathTime");
        h.mix(corpse.deathTime);
        h.mixText(p + ".loot");
        h.mix(corpse.loot.size());
        for (const items::ItemStack &row : corpse.loot)
        {
            h.mix(row.itemKey);
            h.mix(row.count);
        }
        h.mixText(p + ".discovered");
        h.mixBool(corpse.discovered);
        h.mixText(p + ".discoveredBy");
        h.mix(corpse.discoveredBy);
        h.mixText(p + ".lootCurrency");
        h.mixSigned(corpse.lootCurrency);
    }
}

void hashJustice(
    StateHasher &h, const justice::JusticeStore &justice
)
{
    size_t evidenceCount = 0;
    for (const auto &[territory, rows] : justice.evidenceStore)
    {
        for (const justice::Evidence &e : rows)
        {
            const std::string p = "evidence[" + std::to_string(e.id) + "]";
            ++evidenceCount;
            h.mixText(p + ".territory");
            h.mix(territory);
            h.mixText(p + ".culprit");
            h.mix(e.culpritId);
            h.mixText(p + ".victim");
            h.mix(e.victimId);
            h.mixText(p + ".worldTime");
            h.mix(e.worldTime);
            h.mixText(p + ".type");
            h.mix(static_cast<std::uint64_t>(e.type));
            h.mixText(p + ".status");
            h.mix(static_cast<std::uint64_t>(e.status));
            h.mixText(p + ".physical");
            h.mix(e.physicalScore);
            h.mixText(p + ".aggregate");
            h.mix(e.aggregateReportWeight);
            h.mixText(p + ".reports");
            h.mix(e.reports.size());
            for (const justice::Report &r : e.reports)
            {
                h.mix(r.source);
                h.mix(r.weight);
                h.mixBool(r.knowsIdentity);
                h.mixBool(r.bribed);
            }
        }
    }
    h.mixText("justice.evidenceCount");
    h.mix(evidenceCount);
    h.mixText("justice.warrants");
    h.mix(justice.warrants.size());
    for (const auto &[id, w] : justice.warrants)
    {
        const std::string p = "warrant[" + std::to_string(id) + "]";
        h.mixText(p + ".evidence");
        h.mix(w.evidenceId);
        h.mixText(p + ".culprit");
        h.mix(w.culpritId);
        h.mixText(p + ".territory");
        h.mix(w.territory);
        h.mixText(p + ".lethal");
        h.mixBool(w.lethal);
        h.mixText(p + ".bounty");
        h.mix(w.bounty);
        h.mixText(p + ".expires");
        h.mix(w.expiresTime);
        h.mixText(p + ".paid");
        h.mixBool(w.paid);
    }
    h.mixText("justice.activeWarrantsByCulprit");
    h.mix(justice.activeWarrantsByCulprit.size());
    for (const auto &[culprit, ids] : justice.activeWarrantsByCulprit)
    {
        h.mix(culprit);
        h.mix(ids.size());
        for (const std::uint64_t wid : ids)
        {
            h.mix(wid);
        }
    }
    h.mixText("justice.reputations");
    h.mix(justice.reputations.size());
    for (const auto &[who, rep] : justice.reputations)
    {
        h.mix(who);
        h.mix(rep.perTerritory.size());
        for (const auto &[territory, score] : rep.perTerritory)
        {
            h.mix(territory);
            h.mixSigned(score);
        }
        h.mixSigned(rep.notoriety); // anti-abuse field: bit-exact (7.1 rule 5)
        h.mix(rep.lastDecayTime);
    }
    h.mixText("justice.jailUntil");
    h.mix(justice.jailUntil.size());
    for (const auto &[who, until] : justice.jailUntil)
    {
        h.mix(who);
        h.mix(until);
    }
    h.mixText("justice.assaultDamage");
    h.mix(justice.assaultDamage.size());
    for (const auto &[culprit, victims] : justice.assaultDamage)
    {
        h.mix(culprit);
        h.mix(victims.size());
        for (const auto &[victim, dmg] : victims)
        {
            h.mix(victim);
            h.mix(dmg);
        }
    }
    h.mixText("justice.missingPersons");
    h.mix(justice.missingPersons.size());
    for (const auto &[victim, row] : justice.missingPersons)
    {
        h.mix(victim);
        h.mix(row.territory);
        h.mix(row.markedAt);
    }
    h.mixText("justice.caseLastTick");
    h.mix(justice.caseLastTick.size());
    for (const auto &[evidence, at] : justice.caseLastTick)
    {
        h.mix(evidence);
        h.mix(at);
    }
    h.mixText("justice.signalledCulprits");
    h.mix(justice.signalledCulprits.size());
    for (const characterId who : justice.signalledCulprits)
    {
        h.mix(who);
    }
    h.mixText("justice.posters");
    h.mix(justice.posters.size());
    for (const auto &[territory, at] : justice.posters)
    {
        h.mix(territory);
        h.mix(at);
    }
    h.mixText("justice.captureStamps");
    h.mix(justice.captureStamps.size());
    for (const auto &[culprit, stamps] : justice.captureStamps)
    {
        h.mix(culprit);
        h.mix(stamps.size());
        for (const std::uint64_t at : stamps)
        {
            h.mix(at);
        }
    }
    h.mixText("justice.massacreMarkers");
    h.mix(justice.massacreMarkers.size());
    for (const auto &[culprit, territories] : justice.massacreMarkers)
    {
        h.mix(culprit);
        h.mix(territories.size());
        for (const auto &[territory, at] : territories)
        {
            h.mix(territory);
            h.mix(at);
        }
    }
    h.mixText("justice.killTally");
    h.mix(justice.killTally.size());
    for (const auto &[culprit, rows] : justice.killTally)
    {
        h.mix(culprit);
        h.mix(rows.size());
        for (const auto &[territory, at] : rows)
        {
            h.mix(territory);
            h.mix(at);
        }
    }
}

void hashEvents(
    StateHasher &h, const events::EventEngine &engine
)
{
    const auto snapshot = engine.snapshot();
    auto hashEvent      = [&h](const std::string &p, const events::Event &ev) {
        h.mixText(p + ".templateId");
        h.mix(ev.templateId);
        h.mixText(p + ".region");
        h.mix(ev.region);
        h.mixText(p + ".depth");
        h.mix(ev.depth);
        h.mixText(p + ".state");
        h.mix(ev.currentState.stateId);
        h.mix(ev.currentState.difficulty);
        h.mix(ev.currentState.attempts);
        h.mix(ev.currentState.successRate);
        h.mix(ev.currentState.powerSnapshots.size());
        for (const auto &[who, power] : ev.currentState.powerSnapshots)
        {
            h.mix(who);
            h.mix(power);
        }
        h.mixText(p + ".participants");
        h.mix(ev.participants.size());
        for (const characterId who : ev.participants)
        {
            h.mix(who);
        }
        h.mixText(p + ".actionAmounts");
        h.mix(ev.actionAmounts.size());
        for (const auto &[who, amounts] : ev.actionAmounts)
        {
            h.mix(who);
            for (const std::uint32_t amount : amounts)
            {
                h.mix(amount);
            }
        }
        h.mixText(p + ".hpLost");
        h.mix(ev.hpLost.size());
        for (const auto &[who, lost] : ev.hpLost)
        {
            h.mix(who);
            h.mix(lost);
        }
        h.mixText(p + ".targetIds");
        h.mix(ev.targetIds.size());
        for (const characterId who : ev.targetIds)
        {
            h.mix(who);
        }
        h.mixText(p + ".excludedFromRewards");
        h.mix(ev.excludedFromRewards.size());
        for (const characterId who : ev.excludedFromRewards)
        {
            h.mix(who);
        }
        h.mixText(p + ".startedAt");
        h.mix(ev.startedAt);
        h.mixText(p + ".lastTransitionAt");
        h.mix(ev.lastTransitionAt);
    };
    h.mixText("events.active");
    h.mix(snapshot.active.size());
    for (const auto &[id, ev] : snapshot.active)
    {
        hashEvent("event[" + std::to_string(id) + "]", ev);
    }
    h.mixText("events.ended");
    h.mix(snapshot.ended.size());
    for (const auto &[id, ev] : snapshot.ended)
    {
        hashEvent("ended[" + std::to_string(id) + "]", ev);
    }
    h.mixText("events.leftEarly");
    h.mix(snapshot.leftEarly.size());
    for (const auto &[id, who] : snapshot.leftEarly)
    {
        h.mix(id);
        h.mix(who.size());
        for (const characterId w : who)
        {
            h.mix(w);
        }
    }
    h.mixText("events.spawnedTargets");
    h.mix(snapshot.spawnedTargets.size());
    for (const auto &[id, who] : snapshot.spawnedTargets)
    {
        h.mix(id);
        h.mix(who.size());
        for (const characterId w : who)
        {
            h.mix(w);
        }
    }
    h.mixText("events.grantedTiers");
    h.mix(snapshot.grantedTiers.size());
    for (const auto &[id, tiers] : snapshot.grantedTiers)
    {
        h.mix(id);
        h.mix(tiers.size());
        for (const std::uint32_t tier : tiers)
        {
            h.mix(tier);
        }
    }
    h.mixText("events.seeds");
    h.mix(snapshot.seeds.size());
    for (const auto &[id, seed] : snapshot.seeds)
    {
        h.mix(id);
        h.mix(seed.warrantId);
        h.mix(seed.culprit);
        h.mix(seed.territory);
        h.mix(seed.region);
    }
    h.mixText("events.targetGroups");
    h.mix(snapshot.targetGroups.size());
    for (const auto &[id, groups] : snapshot.targetGroups)
    {
        h.mix(id);
        for (const auto &group : groups)
        {
            h.mix(group.size());
            for (const characterId w : group)
            {
                h.mix(w);
            }
        }
    }
    h.mixText("events.priceBias");
    h.mix(snapshot.priceBias.size());
    for (const auto &[region, goods] : snapshot.priceBias)
    {
        h.mix(region);
        h.mix(goods.size());
        for (const auto &[good, bias] : goods)
        {
            h.mix(good);
            h.mix(bias.first);
            h.mix(bias.second);
        }
    }
    h.mixText("events.weatherBias");
    h.mix(snapshot.weatherBias.size());
    for (const auto &[region, bias] : snapshot.weatherBias)
    {
        h.mix(region);
        h.mixText(bias.first);
        h.mix(bias.second);
    }
}

void hashEconomy(
    StateHasher &h, const EconomyStore &economy
)
{
    const auto snap = economy.snapshot();
    h.mixText("economy.stocks");
    h.mix(snap.stocks.size());
    for (const auto &[region, goods] : snap.stocks)
    {
        h.mix(region);
        h.mix(goods.size());
        for (const auto &[good, stock] : goods)
        {
            h.mix(good);
            h.mixSigned(stock.supply);
            h.mixSigned(stock.demand);
        }
    }
    h.mixText("economy.prices");
    h.mix(snap.prices.size());
    for (const auto &[region, goods] : snap.prices)
    {
        h.mix(region);
        h.mix(goods.size());
        for (const auto &[good, price] : goods)
        {
            h.mix(good);
            h.mix(price);
        }
    }
    h.mixText("economy.pendingTaxes");
    h.mix(snap.pendingTaxes.size());
    for (const auto &[territory, value] : snap.pendingTaxes)
    {
        h.mix(territory);
        h.mix(value);
    }
    h.mixText("economy.territoryWealth");
    h.mix(snap.territoryWealth.size());
    for (const auto &[territory, value] : snap.territoryWealth)
    {
        h.mix(territory);
        h.mix(value);
    }
    h.mixText("economy.goodsHeld");
    h.mix(snap.goodsHeld.size());
    for (const auto &[who, goods] : snap.goodsHeld)
    {
        h.mix(who);
        h.mix(goods.size());
        for (const auto &[good, value] : goods)
        {
            h.mix(good);
            h.mix(value);
        }
    }
    h.mixText("economy.lastRecompute");
    h.mix(snap.lastRecompute.size());
    for (const auto &[region, at] : snap.lastRecompute)
    {
        h.mix(region);
        h.mix(at);
    }
    h.mixText("economy.population");
    h.mix(snap.population.size());
    for (const auto &[region, count] : snap.population)
    {
        h.mix(region);
        h.mix(count);
    }
}

void hashFactions(
    StateHasher &h, const factions::FactionStore &store
)
{
    const auto snap = store.snapshot();
    h.mixText("factions.rows");
    h.mix(snap.factions.size());
    for (const auto &[id, faction] : snap.factions)
    {
        const std::string p = "faction[" + std::to_string(id) + "]";
        h.mixText(p + ".name");
        h.mixText(faction.name);
        h.mixText(p + ".members");
        h.mix(faction.members.size());
        for (const characterId who : faction.members)
        {
            h.mix(who);
        }
        h.mixText(p + ".ranks");
        h.mix(faction.ranks.size());
        for (const auto &[who, rank] : faction.ranks)
        {
            h.mix(who);
            h.mix(rank);
        }
        h.mixText(p + ".sharedResources");
        h.mix(faction.sharedResources.size());
        for (const auto &[good, value] : faction.sharedResources)
        {
            h.mix(good);
            h.mix(value);
        }
        h.mixText(p + ".relations");
        h.mix(faction.relations.size());
        for (const auto &[other, relation] : faction.relations)
        {
            h.mix(other);
            h.mixSigned(relation.score);
            h.mixBool(relation.alliance);
            h.mixBool(relation.rivalry);
            h.mix(relation.treatyUntil); // D-WAR-TREATY-1's field rides the row
        }
        h.mixText(p + ".capital");
        h.mix(faction.capital);
        h.mixText(p + ".playerFounded");
        h.mixBool(faction.playerFounded);
    }
    h.mixText("factions.membership");
    h.mix(snap.membership.size());
    for (const auto &[who, faction] : snap.membership)
    {
        h.mix(who);
        h.mix(faction);
    }
    h.mixText("factions.tradeVolume");
    h.mix(snap.tradeVolume.size());
    for (const auto &[key, value] : snap.tradeVolume)
    {
        h.mix(key);
        h.mix(value);
    }
    h.mixText("factions.lastDriftMonth");
    h.mix(snap.lastDriftMonth);
    h.mixText("factions.nextFactionId");
    h.mix(snap.nextFactionId);
}

void hashWars(
    StateHasher &h, const factions::WarStore &store
)
{
    const auto snap = store.snapshot();
    h.mixText("wars.rows");
    h.mix(snap.wars.size());
    for (const auto &[id, war] : snap.wars)
    {
        const std::string p = "war[" + std::to_string(id) + "]";
        h.mixText(p + ".attacker");
        h.mix(war.attacker);
        h.mixText(p + ".defender");
        h.mix(war.defender);
        h.mixText(p + ".cb");
        h.mix(static_cast<std::uint64_t>(war.cb.kind));
        h.mix(war.cb.region);
        h.mix(war.cb.origin);
        h.mixText(p + ".declaredAt");
        h.mix(war.declaredAt);
        h.mixText(p + ".noticeEndsAt");
        h.mix(war.noticeEndsAt);
        h.mixText(p + ".endedAt");
        h.mix(war.endedAt);
        h.mixText(p + ".outcome");
        h.mix(static_cast<std::uint64_t>(war.outcome));
        h.mixText(p + ".active");
        h.mixBool(war.active);
    }
    h.mixText("wars.massacreMarkers");
    h.mix(snap.massacreMarkers.size());
    for (const auto &[territory, marker] : snap.massacreMarkers)
    {
        h.mix(territory);
        h.mix(marker.first);
        h.mix(marker.second);
    }
    h.mixText("wars.nextWarId");
    h.mix(snap.nextWarId);
    h.mixText("wars.lastAutoDeclareMonth");
    h.mix(snap.lastAutoDeclareMonth);
    h.mixText("wars.nextProgressCheckAt");
    h.mix(snap.nextProgressCheckAt);
}

void hashClaims(
    StateHasher &h, const factions::ClaimStore &store
)
{
    const auto snap = store.snapshot();
    h.mixText("claims.rows");
    h.mix(snap.claims.size());
    for (const auto &[territory, claim] : snap.claims)
    {
        h.mix(territory);
        h.mix(claim.owner);
        h.mix(claim.stintRealSeconds);
        h.mix(claim.lastQualifyingRealTime);
        h.mix(claim.lastStampRealTime);
        h.mixText(claim.lawKey);
        h.mixBool(claim.abandoned);
    }
    h.mixText("claims.customLaws");
    h.mix(snap.customLaws.size());
    for (const auto &[key, set] : snap.customLaws)
    {
        h.mixText("customLaw[" + key + "]");
        for (const std::uint32_t threshold : set.convictionThreshold)
        {
            h.mix(threshold);
        }
        h.mix(set.suppression.size());
        for (const auto &[cls, thresholds] : set.suppression)
        {
            h.mix(static_cast<std::uint64_t>(cls));
            for (const std::uint32_t threshold : thresholds)
            {
                h.mix(threshold);
            }
        }
        h.mix(set.exemptions.size());
        for (const auto &[species, flags] : set.exemptions)
        {
            h.mix(species);
            for (const bool flag : flags)
            {
                h.mixBool(flag);
            }
        }
        h.mix(set.cultureBias.size());
        for (const auto &[species, bias] : set.cultureBias)
        {
            h.mix(species);
            h.mixSigned(bias);
        }
        h.mix(set.suspicion.size());
        for (const data::SuspicionTrigger &trigger : set.suspicion)
        {
            h.mix(static_cast<std::uint64_t>(trigger.act));
            h.mix(trigger.mask);
            h.mix(trigger.weight);
        }
        h.mixBool(set.safe);
    }
}

void hashItemsAndBags(
    StateHasher &h, const SaveSources &s
)
{
    h.mixText("items.instances");
    h.mix(s.itemInstances->all().size());
    for (const auto &[instance, def] : s.itemInstances->all())
    {
        h.mix(instance);
        h.mix(def);
    }
    h.mixText("items.bags");
    h.mix(s.bags->all().size());
    for (const auto &[who, bag] : s.bags->all())
    {
        h.mix(who);
        hashInventory(h, "bag", bag);
    }
}

void hashAppearance(
    StateHasher &h, const character::AppearanceHistory &appearance
)
{
    h.mixText("appearance.rows");
    h.mix(appearance.all().size());
    for (const auto &[who, stamps] : appearance.all())
    {
        h.mix(who);
        h.mix(stamps.size());
        for (const std::uint64_t at : stamps)
        {
            h.mix(at);
        }
    }
}

void hashRolesConsentsCombat(
    StateHasher &h, const SaveSources &s
)
{
    const auto roles = s.roles->snapshot();
    h.mixText("roles.openSeats");
    h.mix(roles.openSeats.size());
    for (const auto &[territory, seats] : roles.openSeats)
    {
        h.mix(territory);
        h.mix(seats.size());
        for (const std::string &seat : seats)
        {
            h.mixText(seat);
        }
    }
    h.mixText("roles.guardPower");
    h.mix(roles.guardPower.size());
    for (const auto &[territory, power] : roles.guardPower)
    {
        h.mix(territory);
        h.mix(power);
    }
    const auto consents = s.consents->snapshot();
    h.mixText("consents.pending");
    h.mix(consents.pending.size());
    for (const auto &[target, challengers] : consents.pending)
    {
        h.mix(target);
        h.mix(challengers.size());
        for (const auto &[challenger, at] : challengers)
        {
            h.mix(challenger);
            h.mix(at);
        }
    }
    h.mixText("consents.duels");
    h.mix(consents.duels.size());
    for (const auto &[key, duel] : consents.duels)
    {
        h.mix(key);
        h.mix(duel.a);
        h.mix(duel.b);
        h.mix(duel.startedAt);
        h.mix(duel.lastDamage);
    }
    h.mixText("combat.castCooldowns");
    h.mix(s.combatQueue->castCooldowns().size());
    for (const auto &[who, until] : s.combatQueue->castCooldowns())
    {
        h.mix(who);
        h.mix(until);
    }
}

} // namespace

std::uint64_t canonicalStateHash(
    const SaveSources &s
)
{
    // The same completeness contract as buildSave (every store present).
    if (s.clock == nullptr || s.rng == nullptr || s.characters == nullptr || s.npcs == nullptr || s.world == nullptr
        || s.justice == nullptr || s.events == nullptr || s.economy == nullptr || s.factions == nullptr
        || s.wars == nullptr || s.claims == nullptr || s.corpses == nullptr || s.itemInstances == nullptr
        || s.bags == nullptr || s.appearance == nullptr || s.roles == nullptr || s.consents == nullptr
        || s.combatQueue == nullptr)
    {
        return 0; // a wiring bug; the callers assert their bundles before hashing
    }

    StateHasher h;
    hashClock(h, *s.clock);
    hashRng(h, *s.rng);
    hashIds(h, s);
    hashWorld(h, *s.world);
    hashCharacters(h, s);
    hashCorpses(h, *s.corpses);
    hashJustice(h, *s.justice);
    hashEvents(h, *s.events);
    hashEconomy(h, *s.economy);
    hashFactions(h, *s.factions);
    hashWars(h, *s.wars);
    hashClaims(h, *s.claims);
    hashItemsAndBags(h, s);
    hashAppearance(h, *s.appearance);
    hashRolesConsentsCombat(h, s);
    return h.digest();
}

} // namespace harrowmere::persist
