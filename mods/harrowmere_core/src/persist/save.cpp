// persist/save.cpp — task 7.1's serialize + atomic-write half.
//
// buildSave assembles the whole document (the schema lives in
// persist/README.md — the Done-when's "self-describing" home). writeSaveFile
// rounds it through <path>.tmp and a std::filesystem::rename (crash-safe on
// POSIX/NTFS — the card's rule 1; needs the mod's `filesystem.write` grant).
//
// Determinism notes (R7): nlohmann's object type is std::map, so object keys
// serialize sorted; every array is filled in map order or stable insertion
// order. Two runs over the same state produce byte-identical files.

#include "persist/save.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include "character/appearance.hpp"
#include "character/store.hpp"
#include "combat/attacks.hpp"
#include "combat/consent.hpp"
#include "combat/death.hpp"
#include "core/clock.hpp"
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

using data::Json;

// ---- small typed writers ------------------------------------------------------

Json positionOf(
    const Position &p
)
{
    return Json::array({p.x, p.y, p.z});
}

Json inventoryOf(
    const items::Inventory &bag
)
{
    Json stacks = Json::array();
    for (const auto &[itemKey, count] : bag.stacks)
    {
        stacks.push_back(Json::array({itemKey, count}));
    }
    Json equipped = Json::array();
    for (const std::uint64_t id : bag.equipped)
    {
        equipped.push_back(id);
    }
    Json loose = Json::array();
    for (const std::uint64_t id : bag.instances)
    {
        loose.push_back(id);
    }
    return Json{{"stacks", std::move(stacks)}, {"equipped", std::move(equipped)}, {"instances", std::move(loose)}};
}

Json npcStateOf(
    const NpcState &npc
)
{
    Json perCharacterScore = Json::array();
    for (const auto &[who, score] : npc.trust.perCharacterScore)
    {
        perCharacterScore.push_back(Json::array({who, score}));
    }
    Json lastSeen = Json::array();
    for (const auto &[who, at] : npc.trust.lastSeen)
    {
        lastSeen.push_back(Json::array({who, at}));
    }
    Json affinity = Json::array();
    for (const auto &[who, value] : npc.affinity)
    {
        affinity.push_back(Json::array({who, value}));
    }
    Json actionCounters = Json::array();
    for (const auto &[key, value] : npc.actionCounters)
    {
        actionCounters.push_back(Json::array({key, value}));
    }
    return Json{
        {"name", npc.name},
        {"birthWorldTime", npc.birthWorldTime},
        {"roleKey", npc.roleKey},
        {"needs",
            Json{
                {"hunger", npc.needs.hunger},
                {"rest", npc.needs.rest},
                {"wealth", npc.needs.wealth},
                {"social", npc.needs.social}
            }},
        {"integrity", npc.integrity},
        {"trust",
            Json{
                {"suspicionScore", npc.trust.suspicionScore},
                {"perCharacterScore", std::move(perCharacterScore)},
                {"lastSeen", std::move(lastSeen)},
                {"lastSuspicionDecay", npc.trust.lastSuspicionDecay}
            }},
        {"affinity", std::move(affinity)},
        {"actionCounters", std::move(actionCounters)},
        {"homeTerritory", npc.homeTerritory},
        {"currentRegion", npc.currentRegion},
        {"inventory", inventoryOf(npc.inventory)},
        {"invulnerable", npc.invulnerable},
        {"migrated", npc.migrated},
        {"fleeing", npc.fleeing},
        {"fleeingUntil", npc.fleeingUntil},
        // The per-tick block (the 8.3 as-built): the intent/migration-walk
        // state and the pacing stamps shape what the next ticks DO — the
        // 7.1 continuation contract (TestSave_RngContinuationIdentical)
        // needs them on the save once NEAR-tier work runs from tick 1 (the
        // 8.3 spawn placement made it so; the soak's roundtrip caught the
        // fresh-intent re-pick consuming a different RNG slice than the
        // kept-alive world's live intents).
        {"perTick",
            Json{
                {"tier", static_cast<std::uint64_t>(npc.tier)},
                {"needsCarry", npc.needsCarry},
                {"nextDutyWorldTime", npc.nextDutyWorldTime},
                {"lastMigrationRollWorldTime", npc.lastMigrationRollWorldTime},
                {"intent",
                    Json{
                        {"kind", static_cast<std::uint64_t>(npc.currentIntent.kind)},
                        {"target", npc.currentIntent.target}
                    }},
                {"migrating", npc.migrating},
                {"migrationTarget", positionOf(npc.migrationTarget)}
            }},
    };
}

Json lawSetOf(
    const data::LawSet &set
)
{
    Json thresholds = Json::array();
    for (const std::uint32_t value : set.convictionThreshold)
    {
        thresholds.push_back(value);
    }
    Json suppression = Json::array();
    for (const auto &[cls, values] : set.suppression)
    {
        Json row = Json::array({static_cast<std::uint64_t>(cls)});
        for (const std::uint32_t value : values)
        {
            row.push_back(value);
        }
        suppression.push_back(std::move(row));
    }
    Json exemptions = Json::array();
    for (const auto &[species, flags] : set.exemptions)
    {
        Json row = Json::array({static_cast<std::uint64_t>(species)});
        for (const bool flag : flags)
        {
            row.push_back(flag);
        }
        exemptions.push_back(std::move(row));
    }
    Json cultureBias = Json::array();
    for (const auto &[species, bias] : set.cultureBias)
    {
        cultureBias.push_back(Json::array({static_cast<std::uint64_t>(species), bias}));
    }
    Json suspicion = Json::array();
    for (const data::SuspicionTrigger &trigger : set.suspicion)
    {
        suspicion.push_back(
            Json{{"act", static_cast<std::uint64_t>(trigger.act)}, {"mask", trigger.mask}, {"weight", trigger.weight}}
        );
    }
    return Json{
        {"key", set.key},
        {"convictionThreshold", std::move(thresholds)},
        {"suppression", std::move(suppression)},
        {"exemptions", std::move(exemptions)},
        {"cultureBias", std::move(cultureBias)},
        {"suspicion", std::move(suspicion)},
        {"safe", set.safe},
    };
}

Json eventRowOf(
    std::uint64_t id, const events::Event &ev, const events::EventEngine::Snapshot &book
)
{
    Json powerSnapshots = Json::array();
    for (const auto &[who, power] : ev.currentState.powerSnapshots)
    {
        powerSnapshots.push_back(Json::array({who, power}));
    }
    Json participants = Json::array();
    for (const characterId who : ev.participants)
    {
        participants.push_back(who);
    }
    Json actionAmounts = Json::array();
    for (const auto &[who, amounts] : ev.actionAmounts)
    {
        actionAmounts.push_back(Json::array({who, amounts[0], amounts[1], amounts[2]}));
    }
    Json hpLost = Json::array();
    for (const auto &[who, lost] : ev.hpLost)
    {
        hpLost.push_back(Json::array({who, lost}));
    }
    Json targetIds = Json::array();
    for (const characterId who : ev.targetIds)
    {
        targetIds.push_back(who);
    }
    Json excluded = Json::array();
    for (const characterId who : ev.excludedFromRewards)
    {
        excluded.push_back(who);
    }

    Json row = Json{
        {"id", id},
        {"templateId", ev.templateId},
        {"region", ev.region},
        {"depth", ev.depth},
        {"state",
            Json{
                {"stateId", ev.currentState.stateId},
                {"difficulty", ev.currentState.difficulty},
                {"attempts", ev.currentState.attempts},
                {"successRate", ev.currentState.successRate},
                {"powerSnapshots", std::move(powerSnapshots)}
            }},
        {"participants", std::move(participants)},
        {"actionAmounts", std::move(actionAmounts)},
        {"hpLost", std::move(hpLost)},
        {"targetIds", std::move(targetIds)},
        {"excludedFromRewards", std::move(excluded)},
        {"startedAt", ev.startedAt},
        {"lastTransitionAt", ev.lastTransitionAt},
    };

    // Active events carry their per-event side books (the 5.1/5.3/5.4/6.2
    // state the card pins as world state). Ended events carry none: their
    // books were cleared by finishEvent and the step-1 sweep retires them.
    if (const auto side = book.leftEarly.find(id); side != book.leftEarly.end())
    {
        Json leftEarly = Json::array();
        for (const characterId who : side->second)
        {
            leftEarly.push_back(who);
        }
        row["side"] = Json{{"leftEarly", std::move(leftEarly)}};
    }
    if (const auto spawned = book.spawnedTargets.find(id); spawned != book.spawnedTargets.end())
    {
        Json rows = Json::array();
        for (const characterId who : spawned->second)
        {
            rows.push_back(who);
        }
        row["side"]["spawnedTargets"] = std::move(rows);
    }
    if (const auto tiers = book.grantedTiers.find(id); tiers != book.grantedTiers.end())
    {
        Json rows = Json::array();
        for (const std::uint32_t tier : tiers->second)
        {
            rows.push_back(tier);
        }
        row["side"]["grantedTiers"] = std::move(rows);
    }
    if (const auto seed = book.seeds.find(id); seed != book.seeds.end())
    {
        row["side"]["seed"] = Json{
            {"warrantId", seed->second.warrantId},
            {"culprit", seed->second.culprit},
            {"territory", seed->second.territory},
            {"region", seed->second.region}
        };
    }
    if (const auto groups = book.targetGroups.find(id); groups != book.targetGroups.end())
    {
        Json group0 = Json::array();
        for (const characterId who : groups->second[0])
        {
            group0.push_back(who);
        }
        Json group1 = Json::array();
        for (const characterId who : groups->second[1])
        {
            group1.push_back(who);
        }
        row["side"]["targetGroups"] = Json::array({std::move(group0), std::move(group1)});
    }
    return row;
}

} // namespace

// ---- buildSave -----------------------------------------------------------------

data::Json buildSave(
    const SaveSources &s
)
{
    // The same completeness contract the hasher enforces: every store bound.
    if (s.clock == nullptr || s.rng == nullptr || s.characters == nullptr || s.npcs == nullptr || s.world == nullptr
        || s.justice == nullptr || s.events == nullptr || s.economy == nullptr || s.factions == nullptr
        || s.wars == nullptr || s.claims == nullptr || s.corpses == nullptr || s.itemInstances == nullptr
        || s.bags == nullptr || s.appearance == nullptr || s.roles == nullptr || s.consents == nullptr
        || s.combatQueue == nullptr)
    {
        return Json{
            {"format", SAVE_FORMAT_ID},
            {"version", 0},
            {"error", "buildSave: a SaveSources store pointer is null (wiring bug)"}
        };
    }

    Json out;
    out["format"]  = SAVE_FORMAT_ID;
    out["version"] = SAVE_FORMAT_VERSION;
    out["seed"]    = s.seed;

    // -- clocks (0.3; the carries are persisted per clock.hpp's own note) --
    out["clocks"] = Json{
        {"worldTime", s.clock->worldTime},
        {"realTime", s.clock->realTime},
        {"timeScale", s.clock->timeScale},
        {"tickRate", s.clock->tickRate},
        {"worldCarry", s.clock->worldCarry},
        {"realCarry", s.clock->realCarry}
    };

    // -- RNG streams (0.3): 2 entries per pinned stream, alphabetical --
    Json rng               = Json::array();
    const auto streamState = s.rng->state();
    for (std::size_t i = 0; i + 1 < streamState.size(); i += 2)
    {
        // state() lays out ("<stream>:hi", "<stream>:lo") per stream in order.
        const std::string &hiKey = streamState[i].first;
        const std::string stream = hiKey.substr(0, hiKey.size() - 3); // strip ":hi"
        rng.push_back(Json{{"stream", stream}, {"hi", streamState[i].second}, {"lo", streamState[i + 1].second}});
    }
    out["rng"] = std::move(rng);

    // -- ids (R10: one counter per named space; the item-instance counter is
    // the ItemInstanceRegistry's own space) --
    out["ids"] = Json{
        {"character", s.characters->nextCharacterId()},
        {"evidence", s.justice->peekEvidenceId()},
        {"warrant", s.justice->peekWarrantId()},
        {"event", s.events->peekEventId()},
        {"itemInstance", s.itemInstances->counter()}
    };

    // -- world: regions, territories (children derived on load), the
    // generation mapping, the lawless sets, the spawn point --
    Json regions = Json::array();
    for (const auto &[id, region] : s.world->regions())
    {
        regions.push_back(
            Json{
                {"id", id},
                {"flags", region.flags},
                {"manaRichness", region.manaRichness},
                {"climate", static_cast<std::uint64_t>(region.climate)},
                {"season", Json{{"index", region.season.index}, {"startedAt", region.season.startedAtWorldTime}}},
                {"weather", Json{{"kind", region.weather.kind}, {"since", region.weather.sinceWorldTime}}}
            }
        );
    }
    Json territories = Json::array();
    for (const auto &[id, territory] : s.world->territories())
    {
        Json titles = Json::array();
        for (const auto &[holder, cls] : territory.titles)
        {
            titles.push_back(Json::array({holder, static_cast<std::uint64_t>(cls)}));
        }
        Json archive = Json::array();
        for (const world::CompactRecord &row : territory.archive)
        {
            archive.push_back(
                Json{
                    {"evidenceId", row.evidenceId},
                    {"worldTime", row.worldTime},
                    {"crimeType", row.crimeType},
                    {"culprit", row.culprit},
                    {"victim", row.victim}
                }
            );
        }
        territories.push_back(
            Json{
                {"id", id},
                {"parent", territory.parent},
                {"type", static_cast<std::uint64_t>(territory.type)},
                {"safe", territory.safe},
                {"lawless", territory.lawless},
                {"owner", territory.owner}, // D-TERR-OWNER-1: persisted with the territory
                {"region", territory.region},
                {"lawKey", territory.laws != nullptr ? territory.laws->key : std::string()},
                {"titles", std::move(titles)},
                {"archive", std::move(archive)}
            }
        );
    }
    Json territoryIndex = Json::array();
    for (const auto &[region, territory] : s.world->regionTerritories())
    {
        territoryIndex.push_back(Json::array({region, territory}));
    }
    Json lawless      = Json::array();
    Json lawlessWatch = Json::array();
    for (const auto &[id, territory] : s.world->territories())
    {
        (void)territory;
        if (s.world->isLawless(id))
        {
            lawless.push_back(id);
        }
        if (s.world->isGarrisonLessWatched(id))
        {
            lawlessWatch.push_back(Json::array({id, s.world->lawlessWatchStart(id)}));
        }
    }
    const Position spawn = s.characters->playerSpawn();
    out["world"]         = Json{
        {"regions", std::move(regions)},
        {"territories", std::move(territories)},
        {"territoryIndex", std::move(territoryIndex)},
        {"lawless", std::move(lawless)},
        {"lawlessWatch", std::move(lawlessWatch)},
        {"playerSpawn", positionOf(spawn)}
    };

    // -- characters: the doc struct verbatim + the NpcState sidecar (the
    // player's row carries {} for the npc object, D-STORE-1) --
    Json characters = Json::array();
    for (const auto &[id, c] : s.characters->all())
    {
        Json stats = Json::array();
        for (const std::uint64_t v : c.progression.stats)
        {
            stats.push_back(v);
        }
        Json skills = Json::array();
        for (const std::uint64_t v : c.progression.skills)
        {
            skills.push_back(v);
        }
        Json cooldowns = Json::array();
        for (const std::uint64_t v : c.cooldowns)
        {
            cooldowns.push_back(v);
        }
        Json appearanceComponents = Json::array();
        for (const std::uint64_t v : c.appearanceComponents)
        {
            appearanceComponents.push_back(v);
        }
        Json equippedItems = Json::array();
        for (const std::uint64_t v : c.equippedItems)
        {
            equippedItems.push_back(v);
        }
        Json row = Json{
            {"id", id},
            {"isPlayer", s.characters->isPlayer(id)},
            {"character",
                Json{
                    {"species", c.species},
                    {"statusFlags", c.statusFlags},
                    {"duelOpponent", c.duelOpponent},
                    {"arenaId", c.arenaId},
                    {"appearanceComponents", std::move(appearanceComponents)},
                    {"equippedItems", std::move(equippedItems)},
                    {"body",
                        Json{
                            {"currentHp", c.body.currentHp},
                            {"maxHp", c.body.maxHp},
                            {"position", positionOf(c.body.position)}
                        }},
                    {"progression",
                        Json{
                            {"totalXp", c.progression.totalXp},
                            {"availableXp", c.progression.availableXp},
                            {"level", c.progression.level},
                            {"stats", std::move(stats)},
                            {"skills", std::move(skills)},
                            {"currency", c.progression.currency}
                        }},
                    {"cooldowns", std::move(cooldowns)},
                    {"faction", c.faction},
                    {"eventId", c.eventId},
                    {"lastCombatTime", c.lastCombatTime},
                    // cachedPower/powerDirty are NOT saved (recomputed on load);
                    // peakPower is the anti-abuse watermark and persists.
                    {"peakPower", c.peakPower}
                }},
            {"npc", s.npcs->contains(id) ? npcStateOf(s.npcs->get(id)) : Json::object()}
        };
        if (s.characters->isPlayer(id))
        {
            const PlayerCreation &creation = s.characters->playerCreation();
            Json components                = Json::array();
            for (const std::uint64_t v : creation.appearanceComponents)
            {
                components.push_back(v);
            }
            row["creation"] = Json{
                {"name", creation.name},
                {"appearanceComponents", std::move(components)},
                {"classKey", creation.classKey}
            };
        }
        characters.push_back(std::move(row));
    }
    out["characters"] = std::move(characters);

    // -- corpses (they matter for the delayed reports) --
    Json corpses = Json::array();
    for (const auto &[victim, corpse] : s.corpses->all())
    {
        Json loot = Json::array();
        for (const items::ItemStack &stack : corpse.loot)
        {
            loot.push_back(Json::array({stack.itemKey, stack.count}));
        }
        corpses.push_back(
            Json{
                {"victim", corpse.victim},
                {"position", positionOf(corpse.pos)},
                {"deathTime", corpse.deathTime},
                {"loot", std::move(loot)},
                {"discovered", corpse.discovered},
                {"discoveredBy", corpse.discoveredBy},
                {"lootCurrency", corpse.lootCurrency}
            }
        );
    }
    out["corpses"] = std::move(corpses);

    // -- justice (§4.5's store + the 4.x side state; the suspicion memory
    // rides the owning NpcState — D-STORE-1 — so the key stays reserved) --
    Json evidence = Json::array();
    for (const auto &[territory, rows] : s.justice->evidenceStore)
    {
        for (const justice::Evidence &e : rows)
        {
            Json reports = Json::array();
            for (const justice::Report &r : e.reports)
            {
                reports.push_back(
                    Json{
                        {"source", r.source},
                        {"weight", r.weight},
                        {"knowsIdentity", r.knowsIdentity},
                        {"bribed", r.bribed}
                    }
                );
            }
            evidence.push_back(
                Json{
                    {"id", e.id},
                    {"culpritId", e.culpritId},
                    {"victimId", e.victimId},
                    {"territory", territory},
                    {"worldTime", e.worldTime},
                    {"type", static_cast<std::uint64_t>(e.type)},
                    {"status", static_cast<std::uint64_t>(e.status)},
                    {"physicalScore", e.physicalScore},
                    {"aggregateReportWeight", e.aggregateReportWeight},
                    {"reports", std::move(reports)}
                }
            );
        }
    }
    Json warrants = Json::array();
    for (const auto &[id, w] : s.justice->warrants)
    {
        warrants.push_back(
            Json{
                {"id", id},
                {"evidenceId", w.evidenceId},
                {"culpritId", w.culpritId},
                {"territory", w.territory},
                {"lethal", w.lethal},
                {"bounty", w.bounty},
                {"expiresTime", w.expiresTime},
                {"paid", w.paid}
            }
        );
    }
    Json activeWarrantsByCulprit = Json::array();
    for (const auto &[culprit, ids] : s.justice->activeWarrantsByCulprit)
    {
        Json warrantIds = Json::array();
        for (const std::uint64_t wid : ids)
        {
            warrantIds.push_back(wid);
        }
        activeWarrantsByCulprit.push_back(Json::array({culprit, std::move(warrantIds)}));
    }
    Json reputations = Json::array();
    for (const auto &[who, rep] : s.justice->reputations)
    {
        Json perTerritory = Json::array();
        for (const auto &[territory, score] : rep.perTerritory)
        {
            perTerritory.push_back(Json::array({territory, score}));
        }
        reputations.push_back(
            Json{
                {"who", who},
                {"perTerritory", std::move(perTerritory)},
                {"notoriety", rep.notoriety},
                {"lastDecayTime", rep.lastDecayTime}
            }
        );
    }
    Json jailUntil = Json::array();
    for (const auto &[who, until] : s.justice->jailUntil)
    {
        jailUntil.push_back(Json::array({who, until}));
    }
    Json assaultDamage = Json::array();
    for (const auto &[culprit, victims] : s.justice->assaultDamage)
    {
        Json rows = Json::array();
        for (const auto &[victim, dmg] : victims)
        {
            rows.push_back(Json::array({victim, dmg}));
        }
        assaultDamage.push_back(Json::array({culprit, std::move(rows)}));
    }
    Json missingPersons = Json::array();
    for (const auto &[victim, row] : s.justice->missingPersons)
    {
        missingPersons.push_back(Json::array({victim, row.territory, row.markedAt}));
    }
    Json caseLastTick = Json::array();
    for (const auto &[evidenceId, at] : s.justice->caseLastTick)
    {
        caseLastTick.push_back(Json::array({evidenceId, at}));
    }
    Json signalledCulprits = Json::array();
    for (const characterId who : s.justice->signalledCulprits)
    {
        signalledCulprits.push_back(who);
    }
    Json posters = Json::array();
    for (const auto &[territory, at] : s.justice->posters)
    {
        posters.push_back(Json::array({territory, at}));
    }
    Json captureStamps = Json::array();
    for (const auto &[culprit, stamps] : s.justice->captureStamps)
    {
        Json rows = Json::array();
        for (const std::uint64_t at : stamps)
        {
            rows.push_back(at);
        }
        captureStamps.push_back(Json::array({culprit, std::move(rows)}));
    }
    Json massacreMarkers = Json::array();
    for (const auto &[culprit, territories] : s.justice->massacreMarkers)
    {
        Json rows = Json::array();
        for (const auto &[territory, at] : territories)
        {
            rows.push_back(Json::array({territory, at}));
        }
        massacreMarkers.push_back(Json::array({culprit, std::move(rows)}));
    }
    Json killTally = Json::array();
    for (const auto &[culprit, rows] : s.justice->killTally)
    {
        Json entries = Json::array();
        for (const auto &[territory, at] : rows)
        {
            entries.push_back(Json::array({territory, at}));
        }
        killTally.push_back(Json::array({culprit, std::move(entries)}));
    }
    out["justice"] = Json{
        {"evidence", std::move(evidence)},
        {"warrants", std::move(warrants)},
        {"activeWarrantsByCulprit", std::move(activeWarrantsByCulprit)},
        {"reputations", std::move(reputations)},
        {"jailUntil", std::move(jailUntil)},
        {"assaultDamage", std::move(assaultDamage)},
        {"missingPersons", std::move(missingPersons)},
        {"caseLastTick", std::move(caseLastTick)},
        {"signalledCulprits", std::move(signalledCulprits)},
        {"posters", std::move(posters)},
        {"captureStamps", std::move(captureStamps)},
        {"massacreMarkers", std::move(massacreMarkers)},
        {"killTally", std::move(killTally)},
        {"suspicionMemory", Json::array()}
    };

    // -- events (active + the ended retention window; the side books ride the
    // active rows) --
    const auto eventBook = s.events->snapshot();
    Json activeEvents    = Json::array();
    for (const auto &[id, ev] : eventBook.active)
    {
        activeEvents.push_back(eventRowOf(id, ev, eventBook));
    }
    Json endedEvents = Json::array();
    for (const auto &[id, ev] : eventBook.ended)
    {
        endedEvents.push_back(eventRowOf(id, ev, eventBook));
    }
    Json priceBias = Json::array();
    for (const auto &[region, goods] : eventBook.priceBias)
    {
        Json rows = Json::array();
        for (const auto &[good, bias] : goods)
        {
            rows.push_back(Json::array({good, bias.first, bias.second}));
        }
        priceBias.push_back(Json::array({region, std::move(rows)}));
    }
    Json weatherBias = Json::array();
    for (const auto &[region, bias] : eventBook.weatherBias)
    {
        weatherBias.push_back(Json::array({region, bias.first, bias.second}));
    }
    out["events"] = Json{
        {"active", std::move(activeEvents)},
        {"history", std::move(endedEvents)},
        {"priceBias", std::move(priceBias)},
        {"weatherBias", std::move(weatherBias)}
    };

    // -- economy (the goods table is boot content, not state) --
    const auto econ = s.economy->snapshot();
    Json stocks     = Json::array();
    for (const auto &[region, goods] : econ.stocks)
    {
        for (const auto &[good, stock] : goods)
        {
            stocks.push_back(Json::array({region, good, stock.supply, stock.demand}));
        }
    }
    Json prices = Json::array();
    for (const auto &[region, goods] : econ.prices)
    {
        for (const auto &[good, price] : goods)
        {
            prices.push_back(Json::array({region, good, price}));
        }
    }
    Json pendingTaxes = Json::array();
    for (const auto &[territory, value] : econ.pendingTaxes)
    {
        pendingTaxes.push_back(Json::array({territory, value}));
    }
    Json territoryWealth = Json::array();
    for (const auto &[territory, value] : econ.territoryWealth)
    {
        territoryWealth.push_back(Json::array({territory, value}));
    }
    Json goodsHeld = Json::array();
    for (const auto &[who, goods] : econ.goodsHeld)
    {
        Json rows = Json::array();
        for (const auto &[good, value] : goods)
        {
            rows.push_back(Json::array({good, value}));
        }
        goodsHeld.push_back(Json::array({who, std::move(rows)}));
    }
    Json population = Json::array();
    for (const auto &[region, count] : econ.population)
    {
        population.push_back(Json::array({region, count}));
    }
    Json lastRecompute = Json::array();
    for (const auto &[region, at] : econ.lastRecompute)
    {
        lastRecompute.push_back(Json::array({region, at}));
    }
    out["economy"] = Json{
        {"stocks", std::move(stocks)},
        {"prices", std::move(prices)},
        {"pendingTaxes", std::move(pendingTaxes)},
        {"territoryWealth", std::move(territoryWealth)},
        {"goodsHeld", std::move(goodsHeld)},
        {"population", std::move(population)},
        {"lastRecompute", std::move(lastRecompute)}
    };

    // -- factions (the card's array) + the store-level index --
    const auto factionBook = s.factions->snapshot();
    Json factionRows       = Json::array();
    for (const auto &[id, faction] : factionBook.factions)
    {
        Json members = Json::array();
        for (const characterId who : faction.members)
        {
            members.push_back(who);
        }
        Json ranks = Json::array();
        for (const auto &[who, rank] : faction.ranks)
        {
            ranks.push_back(Json::array({who, rank}));
        }
        Json sharedResources = Json::array();
        for (const auto &[good, value] : faction.sharedResources)
        {
            sharedResources.push_back(Json::array({good, value}));
        }
        Json relations = Json::array();
        for (const auto &[other, relation] : faction.relations)
        {
            relations.push_back(
                Json{
                    {"faction", other},
                    {"score", relation.score},
                    {"alliance", relation.alliance},
                    {"rivalry", relation.rivalry},
                    {"treatyUntil", relation.treatyUntil}
                }
            );
        }
        factionRows.push_back(
            Json{
                {"id", id},
                {"name", faction.name},
                {"members", std::move(members)},
                {"ranks", std::move(ranks)},
                {"sharedResources", std::move(sharedResources)},
                {"relations", std::move(relations)},
                {"capital", faction.capital},
                {"playerFounded", faction.playerFounded}
            }
        );
    }
    out["factions"] = std::move(factionRows);
    Json membership = Json::array();
    for (const auto &[who, faction] : factionBook.membership)
    {
        membership.push_back(Json::array({who, faction}));
    }
    Json tradeVolume = Json::array();
    for (const auto &[key, value] : factionBook.tradeVolume)
    {
        tradeVolume.push_back(Json::array({key, value}));
    }
    out["factionIndex"] = Json{
        {"membership", std::move(membership)},
        {"tradeVolume", std::move(tradeVolume)},
        {"lastDriftMonth", factionBook.lastDriftMonth},
        {"nextFactionId", factionBook.nextFactionId}
    };

    // -- wars (the card's array) + the store-level state --
    const auto warBook = s.wars->snapshot();
    Json warRows       = Json::array();
    for (const auto &[id, war] : warBook.wars)
    {
        warRows.push_back(
            Json{
                {"id", id},
                {"attacker", war.attacker},
                {"defender", war.defender},
                {"cb",
                    Json{
                        {"kind", static_cast<std::uint64_t>(war.cb.kind)},
                        {"region", war.cb.region},
                        {"origin", war.cb.origin}
                    }},
                {"declaredAt", war.declaredAt},
                {"noticeEndsAt", war.noticeEndsAt},
                {"endedAt", war.endedAt},
                {"outcome", static_cast<std::uint64_t>(war.outcome)},
                {"active", war.active}
            }
        );
    }
    out["wars"]     = std::move(warRows);
    Json warMarkers = Json::array();
    for (const auto &[territory, marker] : warBook.massacreMarkers)
    {
        warMarkers.push_back(Json::array({territory, marker.first, marker.second}));
    }
    out["warState"] = Json{
        {"nextWarId", warBook.nextWarId},
        {"lastAutoDeclareMonth", warBook.lastAutoDeclareMonth},
        {"nextProgressCheckAt", warBook.nextProgressCheckAt},
        {"massacreMarkers", std::move(warMarkers)}
    };

    // -- claims (the ledger + the store-owned custom law sets) --
    const auto claimBook = s.claims->snapshot();
    Json claimRows       = Json::array();
    for (const auto &[territory, claim] : claimBook.claims)
    {
        claimRows.push_back(
            Json{
                {"territory", territory},
                {"owner", claim.owner},
                {"stintRealSeconds", claim.stintRealSeconds},
                {"lastQualifyingRealTime", claim.lastQualifyingRealTime},
                {"lastStampRealTime", claim.lastStampRealTime},
                {"lawKey", claim.lawKey},
                {"abandoned", claim.abandoned}
            }
        );
    }
    Json customLaws = Json::array();
    for (const auto &[key, set] : claimBook.customLaws)
    {
        customLaws.push_back(lawSetOf(set));
    }
    out["claims"] = Json{{"rows", std::move(claimRows)}, {"customLaws", std::move(customLaws)}};

    // -- item instances + the bag store --
    Json instances = Json::array();
    for (const auto &[instance, def] : s.itemInstances->all())
    {
        instances.push_back(Json::array({instance, def}));
    }
    Json bagRows = Json::array();
    for (const auto &[who, bag] : s.bags->all())
    {
        bagRows.push_back(Json::array({who, inventoryOf(bag)}));
    }
    out["items"] = Json{{"instances", std::move(instances)}, {"bags", std::move(bagRows)}};

    // -- appearance stamps (2.6's history; the 3.9 version source) --
    Json appearanceRows = Json::array();
    for (const auto &[who, stamps] : s.appearance->all())
    {
        Json rows = Json::array();
        for (const std::uint64_t at : stamps)
        {
            rows.push_back(at);
        }
        appearanceRows.push_back(Json::array({who, std::move(rows)}));
    }
    out["appearance"] = std::move(appearanceRows);

    // -- roles (open seats + guard power) --
    const auto roleBook = s.roles->snapshot();
    Json openSeats      = Json::array();
    for (const auto &[territory, seats] : roleBook.openSeats)
    {
        Json rows = Json::array();
        for (const std::string &seat : seats)
        {
            rows.push_back(seat);
        }
        openSeats.push_back(Json::array({territory, std::move(rows)}));
    }
    Json guardPower = Json::array();
    for (const auto &[territory, power] : roleBook.guardPower)
    {
        guardPower.push_back(Json::array({territory, power}));
    }
    out["roles"] = Json{{"openSeats", std::move(openSeats)}, {"guardPower", std::move(guardPower)}};

    // -- consents (pending duels + the live duels) --
    const auto consentBook = s.consents->snapshot();
    Json pendingRequests   = Json::array();
    for (const auto &[target, challengers] : consentBook.pending)
    {
        Json rows = Json::array();
        for (const auto &[challenger, at] : challengers)
        {
            rows.push_back(Json::array({challenger, at}));
        }
        pendingRequests.push_back(Json::array({target, std::move(rows)}));
    }
    Json duelRows = Json::array();
    for (const auto &[key, duel] : consentBook.duels)
    {
        duelRows.push_back(
            Json{
                {"key", key},
                {"a", duel.a},
                {"b", duel.b},
                {"startedAt", duel.startedAt},
                {"lastDamage", duel.lastDamage}
            }
        );
    }
    out["consents"] = Json{{"pending", std::move(pendingRequests)}, {"duels", std::move(duelRows)}};

    // -- combat (the A-31 casting cadence) --
    Json castCooldowns = Json::array();
    for (const auto &[who, until] : s.combatQueue->castCooldowns())
    {
        castCooldowns.push_back(Json::array({who, until}));
    }
    out["combat"] = Json{{"castCooldowns", std::move(castCooldowns)}};

    // -- quests: reserved. The 6.4 as-built carries NO quest state outside the
    // event engine (a quest IS an event instance; quests.json is content) —
    // the array stays empty and the README documents the reading.
    out["quests"] = Json::array();

    // -- the runtime pacing gates (optional rows; the canonical hash reads
    // the stores, never this block — but the gates are load-bearing for the
    // 7.1 continuation contract: a resumed world must pace exactly like the
    // kept-alive one, so a gate that shapes what the next ticks WRITE rides
    // here with the state it paces) --
    if (s.runtime != nullptr)
    {
        const RuntimeState &rt = *s.runtime;
        Json snapshotAt        = Json::array();
        for (const auto &[eventId, actors] : rt.contributionSnapshotAt)
        {
            Json rows = Json::array();
            for (const auto &[actor, at] : actors)
            {
                rows.push_back(Json::array({actor, at}));
            }
            snapshotAt.push_back(Json::array({eventId, std::move(rows)}));
        }
        Json responded = Json::array();
        for (const std::uint64_t warrantId : rt.enforcementResponded)
        {
            responded.push_back(warrantId);
        }
        out["runtime"] = Json{
            {"tickNumber", rt.tickNumber},
            {"gridLastSweepDay", rt.gridLastSweepDay},
            {"justiceSweepLastDay", rt.justiceSweepLastDay},
            {"weatherPrevWorldTime", rt.weatherPrevWorldTime},
            {"economy",
                Json{{"started", rt.economyStarted}, {"lastHour", rt.economyLastHour}, {"lastDay", rt.economyLastDay}}},
            {"suspicionLastRunHour", rt.suspicionLastRunHour},
            {"contributionSnapshotAt", std::move(snapshotAt)},
            {"contributionNextWalkAt", rt.contributionNextWalkAt},
            {"enforcementResponded", std::move(responded)},
            {"player", Json{{"position", positionOf(rt.playerPosition)}, {"valid", rt.playerPositionValid}}}
        };
    }

    return out;
}

// ---- writeSaveFile -------------------------------------------------------------

bool writeSaveFile(
    const std::string &path, const SaveSources &sources, std::string &error
)
{
    const Json document = buildSave(sources);
    if (document.contains("error"))
    {
        error = document["error"].get<std::string>();
        return false;
    }

    // Atomic write (the card's rule 1): serialize to <path>.tmp, then rename
    // over the target. POSIX/NTFS rename is atomic over an existing file, so
    // a crash mid-write leaves the previous save intact and readable.
    const std::string tmpPath = path + ".tmp";

    std::error_code ec;
    const std::filesystem::path targetDirectory = std::filesystem::path(path).parent_path();
    if (!targetDirectory.empty() && !std::filesystem::exists(targetDirectory))
    {
        std::filesystem::create_directories(targetDirectory, ec);
        if (ec)
        {
            error = "writeSaveFile: cannot create directory '" + targetDirectory.string() + "': " + ec.message();
            return false;
        }
    }

    {
        std::ofstream tmp(tmpPath, std::ios::binary | std::ios::trunc);
        if (!tmp.is_open())
        {
            error = "writeSaveFile: cannot open '" + tmpPath + "' for writing";
            return false;
        }
        const std::string serialized = document.dump();
        tmp.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        tmp.flush();
        if (!tmp.good())
        {
            error = "writeSaveFile: write to '" + tmpPath + "' failed";
            tmp.close();
            std::filesystem::remove(tmpPath, ec);
            return false;
        }
    }

    std::filesystem::rename(tmpPath, path, ec);
    if (ec)
    {
        error = "writeSaveFile: rename '" + tmpPath + "' -> '" + path + "' failed: " + ec.message();
        std::filesystem::remove(tmpPath, ec); // never leave the tmp behind
        return false;
    }
    return true;
}

bool saveFileExists(
    const std::string &path
)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec) && std::filesystem::is_regular_file(path, ec);
}

} // namespace harrowmere::persist
