// persist/load.cpp — task 7.1's load half: read a save, validate EVERYTHING,
// then apply. The refusal rule is the 8.2 pitfall's own: a corrupted-but-
// parseable file is refused, never silently accepted — every section
// validates into typed snapshots BEFORE the first store write, so a refusal
// leaves every target store untouched (no partial load).
//
// Validation surface: format tag, version (the 7.1 version policy — refuse
// with a clear message, no migration in v1), per-section presence + types,
// cross-section identity (a warrant whose evidence id is unknown, an event
// participant id with no character row, a territory whose parent id is
// unknown), and the id-space sanity the stores assert on (counter >= every
// live id). Content-level mismatches that a content update can legitimately
// produce (a law-set key whose set no longer exists) degrade
// deterministically and land in LoadTargets::warnings instead.

#include "persist/save.hpp"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <utility>

#include "character/appearance.hpp"
#include "character/store.hpp"
#include "combat/attacks.hpp"
#include "combat/consent.hpp"
#include "combat/death.hpp"
#include "core/clock.hpp"
#include "core/constants.hpp"
#include "core/enums.hpp"
#include "core/rng.hpp"
#include "data/registry.hpp"
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

// The validation accumulator: a false LoaderState refuses the load with the
// FIRST error (the file + pointer + reason line the plan's hard rule asks
// for).
struct LoaderState
{
    const std::string &file;
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.empty();
    }

    bool fail(
        const std::string &pointer, const std::string &reason
    )
    {
        if (error.empty())
        {
            error = file + ": " + pointer + ": " + reason;
        }
        return false;
    }
};

[[nodiscard]] bool isUint(
    const Json &v
)
{
    return v.is_number_unsigned();
}

[[nodiscard]] bool isInt(
    const Json &v
)
{
    return v.is_number_integer(); // signed or unsigned both integer
}

bool readUint(
    LoaderState &st, const Json &parent, const char *key, std::uint64_t &out, const std::string &pointer
)
{
    if (!parent.contains(key) || !isUint(parent[key]))
    {
        return st.fail(pointer + "/" + key, "expected an unsigned integer");
    }
    out = parent[key].get<std::uint64_t>();
    return true;
}

bool readInt(
    LoaderState &st, const Json &parent, const char *key, std::int64_t &out, const std::string &pointer
)
{
    if (!parent.contains(key) || !isInt(parent[key]))
    {
        return st.fail(pointer + "/" + key, "expected an integer");
    }
    out = parent[key].get<std::int64_t>();
    return true;
}

bool readBool(
    LoaderState &st, const Json &parent, const char *key, bool &out, const std::string &pointer
)
{
    if (!parent.contains(key) || !parent[key].is_boolean())
    {
        return st.fail(pointer + "/" + key, "expected a boolean");
    }
    out = parent[key].get<bool>();
    return true;
}

bool readString(
    LoaderState &st, const Json &parent, const char *key, std::string &out, const std::string &pointer
)
{
    if (!parent.contains(key) || !parent[key].is_string())
    {
        return st.fail(pointer + "/" + key, "expected a string");
    }
    out = parent[key].get<std::string>();
    return true;
}

bool readU32(
    LoaderState &st, const Json &parent, const char *key, std::uint32_t &out, const std::string &pointer
)
{
    std::uint64_t value = 0;
    if (!readUint(st, parent, key, value, pointer))
    {
        return false;
    }
    if (value > std::numeric_limits<std::uint32_t>::max())
    {
        return st.fail(pointer + "/" + key, "value exceeds u32");
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

bool readI32(
    LoaderState &st, const Json &parent, const char *key, std::int32_t &out, const std::string &pointer
)
{
    std::int64_t value = 0;
    if (!readInt(st, parent, key, value, pointer))
    {
        return false;
    }
    if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max())
    {
        return st.fail(pointer + "/" + key, "value exceeds i32");
    }
    out = static_cast<std::int32_t>(value);
    return true;
}

bool readPosition(
    LoaderState &st, const Json &parent, const char *key, Position &out, const std::string &pointer
)
{
    if (!parent.contains(key) || !parent[key].is_array() || parent[key].size() != 3)
    {
        return st.fail(pointer + "/" + key, "expected a [x, y, z] array");
    }
    const Json &coords = parent[key];
    double x = 0, y = 0, z = 0;
    if (!coords[0].is_number() || !coords[1].is_number() || !coords[2].is_number())
    {
        return st.fail(pointer + "/" + key, "expected numeric coordinates");
    }
    x   = coords[0].get<double>();
    y   = coords[1].get<double>();
    z   = coords[2].get<double>();
    out = Position{x, y, z};
    return true;
}

bool readUintArray(
    LoaderState &st, const Json &parent, const char *key, std::vector<std::uint64_t> &out, const std::string &pointer
)
{
    if (!parent.contains(key) || !parent[key].is_array())
    {
        return st.fail(pointer + "/" + key, "expected an array");
    }
    for (const Json &entry : parent[key])
    {
        if (!isUint(entry))
        {
            return st.fail(pointer + "/" + key, "expected unsigned integer entries");
        }
        out.push_back(entry.get<std::uint64_t>());
    }
    return true;
}

bool readInventory(
    LoaderState &st, const Json &parent, const char *key, items::Inventory &out, const std::string &pointer
)
{
    if (!parent.contains(key) || !parent[key].is_object())
    {
        return st.fail(pointer + "/" + key, "expected an inventory object");
    }
    const Json &bag     = parent[key];
    const std::string p = pointer + "/" + key;
    if (!bag.contains("stacks") || !bag["stacks"].is_array())
    {
        return st.fail(p + "/stacks", "expected an array");
    }
    for (const Json &row : bag["stacks"])
    {
        if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !isUint(row[1]))
        {
            return st.fail(p + "/stacks", "expected [itemKey, count] pairs");
        }
        if (row[1].get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
        {
            return st.fail(p + "/stacks", "count exceeds u32");
        }
        out.stacks.emplace(row[0].get<std::uint64_t>(), static_cast<std::uint32_t>(row[1].get<std::uint64_t>()));
    }
    if (!readUintArray(st, bag, "equipped", out.equipped, p))
    {
        return false;
    }
    std::vector<std::uint64_t> loose;
    if (!readUintArray(st, bag, "instances", loose, p))
    {
        return false;
    }
    out.instances = loose;
    return true;
}

bool readLawSet(
    LoaderState &st, const Json &row, data::LawSet &out, const std::string &pointer
)
{
    if (!row.is_object())
    {
        return st.fail(pointer, "expected a law-set object");
    }
    if (!readString(st, row, "key", out.key, pointer))
    {
        return false;
    }
    if (!row.contains("convictionThreshold") || !row["convictionThreshold"].is_array()
        || row["convictionThreshold"].size() != static_cast<std::size_t>(CrimeType::COUNT))
    {
        return st.fail(pointer + "/convictionThreshold", "expected an array of CrimeType::COUNT thresholds");
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(CrimeType::COUNT); ++i)
    {
        const Json &value = row["convictionThreshold"][i];
        if (!isUint(value) || value.get<std::uint64_t>() > SCALE)
        {
            return st.fail(pointer + "/convictionThreshold", "expected thresholds on [0, SCALE]");
        }
        out.convictionThreshold[i] = static_cast<std::uint32_t>(value.get<std::uint64_t>());
    }
    if (!row.contains("suppression") || !row["suppression"].is_array())
    {
        return st.fail(pointer + "/suppression", "expected an array");
    }
    for (const Json &entry : row["suppression"])
    {
        if (!entry.is_array() || entry.size() != static_cast<std::size_t>(CrimeType::COUNT) + 1 || !isUint(entry[0]))
        {
            return st.fail(pointer + "/suppression", "expected [classOrdinal, ...COUNT values] rows");
        }
        const auto cls = static_cast<std::uint8_t>(entry[0].get<std::uint64_t>());
        if (cls >= static_cast<std::uint8_t>(SocialClassId::COUNT))
        {
            return st.fail(pointer + "/suppression", "class ordinal out of range");
        }
        auto &values = out.suppression[static_cast<SocialClassId>(cls)];
        for (std::size_t i = 0; i < static_cast<std::size_t>(CrimeType::COUNT); ++i)
        {
            const Json &value = entry[i + 1];
            if (!isUint(value) || value.get<std::uint64_t>() > SCALE)
            {
                return st.fail(pointer + "/suppression", "expected values on [0, SCALE]");
            }
            values[i] = static_cast<std::uint32_t>(value.get<std::uint64_t>());
        }
    }
    if (!row.contains("exemptions") || !row["exemptions"].is_array())
    {
        return st.fail(pointer + "/exemptions", "expected an array");
    }
    for (const Json &entry : row["exemptions"])
    {
        if (!entry.is_array() || entry.size() != static_cast<std::size_t>(CrimeType::COUNT) + 1 || !isUint(entry[0]))
        {
            return st.fail(pointer + "/exemptions", "expected [speciesId, ...COUNT flags] rows");
        }
        std::uint64_t species = entry[0].get<std::uint64_t>();
        if (species > std::numeric_limits<std::uint16_t>::max())
        {
            return st.fail(pointer + "/exemptions", "species id exceeds u16");
        }
        auto &flags = out.exemptions[static_cast<std::uint16_t>(species)];
        for (std::size_t i = 0; i < static_cast<std::size_t>(CrimeType::COUNT); ++i)
        {
            if (!entry[i + 1].is_boolean())
            {
                return st.fail(pointer + "/exemptions", "expected boolean flags");
            }
            flags[i] = entry[i + 1].get<bool>();
        }
    }
    if (!row.contains("cultureBias") || !row["cultureBias"].is_array())
    {
        return st.fail(pointer + "/cultureBias", "expected an array");
    }
    for (const Json &entry : row["cultureBias"])
    {
        if (!entry.is_array() || entry.size() != 2 || !isUint(entry[0]) || !isInt(entry[1]))
        {
            return st.fail(pointer + "/cultureBias", "expected [speciesId, bias] rows");
        }
        std::uint64_t species = entry[0].get<std::uint64_t>();
        if (species > std::numeric_limits<std::uint16_t>::max())
        {
            return st.fail(pointer + "/cultureBias", "species id exceeds u16");
        }
        const std::int64_t rawBias = entry[1].get<std::int64_t>();
        if (rawBias < -static_cast<std::int64_t>(SCALE) || rawBias > static_cast<std::int64_t>(SCALE))
        {
            return st.fail(pointer + "/cultureBias", "bias outside [-SCALE, SCALE]");
        }
        out.cultureBias[static_cast<std::uint16_t>(species)] = static_cast<std::int32_t>(rawBias);
    }
    if (!row.contains("suspicion") || !row["suspicion"].is_array())
    {
        return st.fail(pointer + "/suspicion", "expected an array");
    }
    for (const Json &entry : row["suspicion"])
    {
        if (!entry.is_object())
        {
            return st.fail(pointer + "/suspicion", "expected trigger objects");
        }
        data::SuspicionTrigger trigger;
        std::uint64_t act = 0;
        if (!readUint(st, entry, "act", act, pointer + "/suspicion"))
        {
            return false;
        }
        if (act > 5) // the six pinned SuspicionAct values (1.1's closed list)
        {
            return st.fail(pointer + "/suspicion/act", "unknown SuspicionAct ordinal");
        }
        trigger.act = static_cast<SuspicionAct>(act);
        if (!readU32(st, entry, "mask", trigger.mask, pointer + "/suspicion"))
        {
            return false;
        }
        if (!readU32(st, entry, "weight", trigger.weight, pointer + "/suspicion"))
        {
            return false;
        }
        if (trigger.weight > SCALE)
        {
            return st.fail(pointer + "/suspicion/weight", "expected weight on [0, SCALE]");
        }
        out.suspicion.push_back(trigger);
    }
    if (!readBool(st, row, "safe", out.safe, pointer))
    {
        return false;
    }
    return true;
}

bool readNpcState(
    LoaderState &st, const Json &row, NpcState &npc, const std::string &pointer
)
{
    if (!row.is_object())
    {
        return st.fail(pointer, "expected an npc object");
    }
    if (!readString(st, row, "name", npc.name, pointer))
    {
        return false;
    }
    if (!readUint(st, row, "birthWorldTime", npc.birthWorldTime, pointer))
    {
        return false;
    }
    if (!readString(st, row, "roleKey", npc.roleKey, pointer))
    {
        return false;
    }
    if (!row.contains("needs") || !row["needs"].is_object())
    {
        return st.fail(pointer + "/needs", "expected an object");
    }
    const Json &needs = row["needs"];
    if (!readUint(st, needs, "hunger", npc.needs.hunger, pointer + "/needs")
        || !readUint(st, needs, "rest", npc.needs.rest, pointer + "/needs")
        || !readUint(st, needs, "wealth", npc.needs.wealth, pointer + "/needs")
        || !readUint(st, needs, "social", npc.needs.social, pointer + "/needs"))
    {
        return false;
    }
    if (!readUint(st, row, "integrity", npc.integrity, pointer))
    {
        return false;
    }
    if (!row.contains("trust") || !row["trust"].is_object())
    {
        return st.fail(pointer + "/trust", "expected an object");
    }
    const Json &trust            = row["trust"];
    const std::string tp         = pointer + "/trust";
    std::uint64_t suspicionScore = 0;
    if (!readUint(st, trust, "suspicionScore", suspicionScore, tp)
        || !readUint(st, trust, "lastSuspicionDecay", npc.trust.lastSuspicionDecay, tp))
    {
        return false;
    }
    npc.trust.suspicionScore = static_cast<std::uint32_t>(suspicionScore);
    if (false)
    {
        return false;
    }
    if (!trust.contains("perCharacterScore") || !trust["perCharacterScore"].is_array())
    {
        return st.fail(tp + "/perCharacterScore", "expected an array");
    }
    for (const Json &entry : trust["perCharacterScore"])
    {
        if (!entry.is_array() || entry.size() != 2 || !isUint(entry[0]) || !isUint(entry[1]))
        {
            return st.fail(tp + "/perCharacterScore", "expected [who, score] pairs");
        }
        npc.trust.perCharacterScore.emplace(entry[0].get<characterId>(), entry[1].get<std::uint64_t>());
    }
    if (!trust.contains("lastSeen") || !trust["lastSeen"].is_array())
    {
        return st.fail(tp + "/lastSeen", "expected an array");
    }
    for (const Json &entry : trust["lastSeen"])
    {
        if (!entry.is_array() || entry.size() != 2 || !isUint(entry[0]) || !isUint(entry[1]))
        {
            return st.fail(tp + "/lastSeen", "expected [who, at] pairs");
        }
        npc.trust.lastSeen.emplace(entry[0].get<characterId>(), entry[1].get<std::uint64_t>());
    }
    if (!row.contains("affinity") || !row["affinity"].is_array())
    {
        return st.fail(pointer + "/affinity", "expected an array");
    }
    for (const Json &entry : row["affinity"])
    {
        if (!entry.is_array() || entry.size() != 2 || !isUint(entry[0]) || !isInt(entry[1]))
        {
            return st.fail(pointer + "/affinity", "expected [who, value] pairs");
        }
        npc.affinity.emplace(entry[0].get<characterId>(), entry[1].get<std::int64_t>());
    }
    if (!row.contains("actionCounters") || !row["actionCounters"].is_array())
    {
        return st.fail(pointer + "/actionCounters", "expected an array");
    }
    for (const Json &entry : row["actionCounters"])
    {
        if (!entry.is_array() || entry.size() != 2 || !entry[0].is_string() || !isUint(entry[1]))
        {
            return st.fail(pointer + "/actionCounters", "expected [key, value] pairs");
        }
        npc.actionCounters.emplace(entry[0].get<std::string>(), entry[1].get<std::uint64_t>());
    }
    if (!readUint(st, row, "homeTerritory", npc.homeTerritory, pointer))
    {
        return false;
    }
    std::uint64_t currentRegion = 0;
    if (!readUint(st, row, "currentRegion", currentRegion, pointer))
    {
        return false;
    }
    npc.currentRegion = static_cast<regionId>(currentRegion);
    if (!readInventory(st, row, "inventory", npc.inventory, pointer))
    {
        return false;
    }
    if (!readBool(st, row, "invulnerable", npc.invulnerable, pointer))
    {
        return false;
    }
    if (!readBool(st, row, "migrated", npc.migrated, pointer))
    {
        return false;
    }
    if (!readBool(st, row, "fleeing", npc.fleeing, pointer))
    {
        return false;
    }
    if (!readUint(st, row, "fleeingUntil", npc.fleeingUntil, pointer))
    {
        return false;
    }
    if (row.contains("perTick"))
    {
        // The 8.3 as-built's per-tick block (optional: pre-8.3 saves load the
        // do-not-save defaults — the tier/pass re-derivation the original
        // reading relied on). Required rows fail loudly through the strict
        // door; absent rows take the historical defaults.
        const Json &perTick = row["perTick"];
        if (!perTick.is_object())
        {
            return st.fail(pointer + "/perTick", "expected an object");
        }
        const std::string tp = pointer + "/perTick";
        std::uint64_t tier   = 0;
        if (!readUint(st, perTick, "tier", tier, tp) || !readUint(st, perTick, "needsCarry", npc.needsCarry, tp)
            || !readUint(st, perTick, "nextDutyWorldTime", npc.nextDutyWorldTime, tp)
            || !readUint(st, perTick, "lastMigrationRollWorldTime", npc.lastMigrationRollWorldTime, tp))
        {
            return false;
        }
        npc.tier           = static_cast<LodTier>(tier);
        const Json &intent = perTick.contains("intent") ? perTick["intent"] : Json::object();
        if (!intent.is_object())
        {
            return st.fail(tp + "/intent", "expected an object");
        }
        std::uint64_t intentKind = 0;
        if (!readUint(st, intent, "kind", intentKind, tp + "/intent")
            || !readUint(st, intent, "target", npc.currentIntent.target, tp + "/intent"))
        {
            return false;
        }
        npc.currentIntent.kind = static_cast<NpcIntentKind>(intentKind);
        if (!readBool(st, perTick, "migrating", npc.migrating, tp))
        {
            return false;
        }
        readPosition(st, perTick, "migrationTarget", npc.migrationTarget, tp);
    }
    return true;
}

bool readEventRow(
    LoaderState &st, const Json &row, std::uint64_t &id, events::Event &ev, std::string &pointerOut
)
{
    if (!row.is_object())
    {
        return st.fail(pointerOut, "expected an event object");
    }
    if (!readUint(st, row, "id", id, pointerOut))
    {
        return false;
    }
    pointerOut += "/" + std::to_string(id);
    if (!readUint(st, row, "templateId", ev.templateId, pointerOut))
    {
        return false;
    }
    std::uint64_t region = 0;
    if (!readUint(st, row, "region", region, pointerOut))
    {
        return false;
    }
    if (region > std::numeric_limits<std::uint16_t>::max())
    {
        return st.fail(pointerOut + "/region", "region exceeds u16");
    }
    ev.region           = static_cast<regionId>(region);
    std::uint64_t depth = 0;
    if (!readUint(st, row, "depth", depth, pointerOut))
    {
        return false;
    }
    if (depth > std::numeric_limits<std::uint8_t>::max())
    {
        return st.fail(pointerOut + "/depth", "depth exceeds u8");
    }
    ev.depth = static_cast<std::uint8_t>(depth);
    if (!row.contains("state") || !row["state"].is_object())
    {
        return st.fail(pointerOut + "/state", "expected an object");
    }
    const Json &state    = row["state"];
    const std::string sp = pointerOut + "/state";
    if (!readUint(st, state, "stateId", ev.currentState.stateId, sp)
        || !readU32(st, state, "difficulty", ev.currentState.difficulty, sp)
        || !readU32(st, state, "attempts", ev.currentState.attempts, sp)
        || !readU32(st, state, "successRate", ev.currentState.successRate, sp))
    {
        return false;
    }
    if (!state.contains("powerSnapshots") || !state["powerSnapshots"].is_array())
    {
        return st.fail(sp + "/powerSnapshots", "expected an array");
    }
    for (const Json &entry : state["powerSnapshots"])
    {
        if (!entry.is_array() || entry.size() != 2 || !isUint(entry[0]) || !isUint(entry[1]))
        {
            return st.fail(sp + "/powerSnapshots", "expected [who, power] pairs");
        }
        ev.currentState.powerSnapshots.emplace(entry[0].get<characterId>(), entry[1].get<std::uint64_t>());
    }
    if (!readUintArray(st, row, "participants", ev.participants, pointerOut))
    {
        return false;
    }
    if (!row.contains("actionAmounts") || !row["actionAmounts"].is_array())
    {
        return st.fail(pointerOut + "/actionAmounts", "expected an array");
    }
    for (const Json &entry : row["actionAmounts"])
    {
        if (!entry.is_array() || entry.size() != 4 || !isUint(entry[0]))
        {
            return st.fail(pointerOut + "/actionAmounts", "expected [who, damage, healing, objective] rows");
        }
        std::array<std::uint32_t, static_cast<std::size_t>(ActionType::COUNT)> amounts{};
        for (std::size_t i = 0; i < amounts.size(); ++i)
        {
            if (!isUint(entry[i + 1]) || entry[i + 1].get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
            {
                return st.fail(pointerOut + "/actionAmounts", "amounts exceed u32");
            }
            amounts[i] = static_cast<std::uint32_t>(entry[i + 1].get<std::uint64_t>());
        }
        ev.actionAmounts.emplace(entry[0].get<characterId>(), amounts);
    }
    if (!row.contains("hpLost") || !row["hpLost"].is_array())
    {
        return st.fail(pointerOut + "/hpLost", "expected an array");
    }
    for (const Json &entry : row["hpLost"])
    {
        if (!entry.is_array() || entry.size() != 2 || !isUint(entry[0]) || !isUint(entry[1]))
        {
            return st.fail(pointerOut + "/hpLost", "expected [who, lost] pairs");
        }
        ev.hpLost.emplace(entry[0].get<characterId>(), entry[1].get<std::uint64_t>());
    }
    std::vector<std::uint64_t> targetIds;
    if (!readUintArray(st, row, "targetIds", targetIds, pointerOut))
    {
        return false;
    }
    ev.targetIds.insert(targetIds.begin(), targetIds.end());
    std::vector<std::uint64_t> excluded;
    if (!readUintArray(st, row, "excludedFromRewards", excluded, pointerOut))
    {
        return false;
    }
    ev.excludedFromRewards.insert(excluded.begin(), excluded.end());
    if (!readUint(st, row, "startedAt", ev.startedAt, pointerOut))
    {
        return false;
    }
    if (!readUint(st, row, "lastTransitionAt", ev.lastTransitionAt, pointerOut))
    {
        return false;
    }
    return true;
}

// A side-book parse helper (all optional keys).
void readEventSide(
    LoaderState &st, const Json &row, events::EventEngine::Snapshot &book, std::uint64_t id, const std::string &pointer
)
{
    if (!row.contains("side") || !row["side"].is_object())
    {
        return;
    }
    const Json &side    = row["side"];
    const std::string p = pointer + "/side";
    if (side.contains("leftEarly") && side["leftEarly"].is_array())
    {
        std::vector<std::uint64_t> rows;
        if (readUintArray(st, side, "leftEarly", rows, p))
        {
            book.leftEarly[id].insert(rows.begin(), rows.end());
        }
    }
    if (side.contains("spawnedTargets") && side["spawnedTargets"].is_array())
    {
        std::vector<std::uint64_t> rows;
        if (readUintArray(st, side, "spawnedTargets", rows, p))
        {
            book.spawnedTargets[id] = std::vector<characterId>(rows.begin(), rows.end());
        }
    }
    if (side.contains("grantedTiers") && side["grantedTiers"].is_array())
    {
        for (const Json &tier : side["grantedTiers"])
        {
            if (isUint(tier) && tier.get<std::uint64_t>() <= std::numeric_limits<std::uint32_t>::max())
            {
                book.grantedTiers[id].push_back(static_cast<std::uint32_t>(tier.get<std::uint64_t>()));
            } else
            {
                st.fail(p + "/grantedTiers", "expected u32 tiers");
                return;
            }
        }
    }
    if (side.contains("seed") && side["seed"].is_object())
    {
        const Json &seed = side["seed"];
        ManhuntSeed parsed;
        if (!readUint(st, seed, "warrantId", parsed.warrantId, p + "/seed")
            || !readUint(st, seed, "culprit", parsed.culprit, p + "/seed")
            || !readUint(st, seed, "territory", parsed.territory, p + "/seed"))
        {
            return;
        }
        std::uint64_t region = 0;
        if (!readUint(st, seed, "region", region, p + "/seed"))
        {
            return;
        }
        parsed.region = static_cast<regionId>(region);
        book.seeds.emplace(id, parsed);
    }
    if (side.contains("targetGroups") && side["targetGroups"].is_array() && side["targetGroups"].size() == 2)
    {
        std::array<std::set<characterId>, 2> groups;
        for (std::size_t g = 0; g < 2; ++g)
        {
            for (const Json &who : side["targetGroups"][g])
            {
                if (!isUint(who))
                {
                    st.fail(p + "/targetGroups", "expected character id entries");
                    return;
                }
                groups[g].insert(who.get<characterId>());
            }
        }
        book.targetGroups.emplace(id, groups);
    }
}

} // namespace

bool readSaveFile(
    const std::string &path, LoadTargets &t, std::string &error
)
{
    // -- read + strict parse (the 0.4 door: duplicate keys rejected) --
    const auto loaded = data::loadJsonFile(path);
    if (loaded.isErr())
    {
        error = loaded.error().toString();
        return false;
    }
    const Json &doc = loaded.value();
    if (!doc.is_object())
    {
        error = path + ": : expected a JSON object";
        return false;
    }

    LoaderState st{path, {}};
    const std::string root = "";

    // -- format + version first (the 7.1 version policy) --
    std::string format;
    if (!readString(st, doc, "format", format, root) || format != SAVE_FORMAT_ID)
    {
        st.fail(root + "/format", "expected the \"" + std::string(SAVE_FORMAT_ID) + "\" format tag");
        error = st.error;
        return false;
    }
    std::uint64_t version = 0;
    if (!readUint(st, doc, "version", version, root) || version != SAVE_FORMAT_VERSION)
    {
        std::ostringstream message;
        message << "unsupported save version " << version << " (this build reads version " << SAVE_FORMAT_VERSION
                << "); the file is left untouched (no migration in v1)";
        st.fail(root + "/version", message.str());
        error = st.error;
        return false;
    }
    if (!readUint(st, doc, "seed", t.seed, root))
    {
        error = st.error;
        return false;
    }

    // Missing targets are wiring bugs — refuse before touching anything.
    if (t.clock == nullptr || t.rng == nullptr || t.characters == nullptr || t.npcs == nullptr || t.world == nullptr
        || t.justice == nullptr || t.events == nullptr || t.economy == nullptr || t.factions == nullptr
        || t.wars == nullptr || t.claims == nullptr || t.corpses == nullptr || t.itemInstances == nullptr
        || t.bags == nullptr || t.appearance == nullptr || t.roles == nullptr || t.consents == nullptr
        || t.combatQueue == nullptr || t.registry == nullptr)
    {
        error = path + ": : a LoadTargets store pointer is null (wiring bug)";
        return false;
    }

    // ============================ parse phase ================================
    // Every section parses into local typed state; no store is touched until
    // the whole document has validated.

    // -- clocks --
    std::uint64_t worldTime = 0, realTime = 0, worldCarry = 0, realCarry = 0;
    std::uint32_t timeScale = 60, tickRate = 10;
    if (!doc.contains("clocks") || !doc["clocks"].is_object())
    {
        error = path + ": /clocks: expected an object";
        return false;
    }
    const Json &clocks = doc["clocks"];
    if (!readUint(st, clocks, "worldTime", worldTime, "/clocks")
        || !readUint(st, clocks, "realTime", realTime, "/clocks")
        || !readU32(st, clocks, "timeScale", timeScale, "/clocks")
        || !readU32(st, clocks, "tickRate", tickRate, "/clocks")
        || !readUint(st, clocks, "worldCarry", worldCarry, "/clocks")
        || !readUint(st, clocks, "realCarry", realCarry, "/clocks"))
    {
        error = st.error;
        return false;
    }

    // -- rng streams --
    std::vector<std::pair<std::string, std::uint64_t>> rngPairs;
    if (!doc.contains("rng") || !doc["rng"].is_array())
    {
        error = path + ": /rng: expected an array";
        return false;
    }
    for (const Json &row : doc["rng"])
    {
        if (!row.is_object() || !row.contains("stream") || !row["stream"].is_string()
            || !isUint(row.value("hi", Json(0))) || !isUint(row.value("lo", Json(0))))
        {
            error = path + ": /rng: expected {stream, hi, lo} rows";
            return false;
        }
        const std::string stream = row["stream"].get<std::string>();
        rngPairs.emplace_back(stream + ":hi", row["hi"].get<std::uint64_t>());
        rngPairs.emplace_back(stream + ":lo", row["lo"].get<std::uint64_t>());
    }

    // -- ids --
    std::uint64_t characterCounter = 0, evidenceCounter = 0, warrantCounter = 0, eventCounter = 0, instanceCounter = 0;
    if (!doc.contains("ids") || !doc["ids"].is_object())
    {
        error = path + ": /ids: expected an object";
        return false;
    }
    const Json &ids = doc["ids"];
    if (!readUint(st, ids, "character", characterCounter, "/ids")
        || !readUint(st, ids, "evidence", evidenceCounter, "/ids")
        || !readUint(st, ids, "warrant", warrantCounter, "/ids") || !readUint(st, ids, "event", eventCounter, "/ids")
        || !readUint(st, ids, "itemInstance", instanceCounter, "/ids"))
    {
        error = st.error;
        return false;
    }

    // -- world --
    if (!doc.contains("world") || !doc["world"].is_object())
    {
        error = path + ": /world: expected an object";
        return false;
    }
    const Json &worldDoc = doc["world"];
    std::vector<world::Region> regions;
    struct TerritoryRow
    {
        world::Territory territory;
        std::string lawKey;
    };
    std::vector<TerritoryRow> territoryRows;
    std::vector<std::pair<regionId, territoryId>> territoryIndex;
    std::vector<territoryId> lawless;
    std::vector<std::pair<territoryId, std::uint64_t>> lawlessWatch;
    Position playerSpawn{};
    if (!worldDoc.contains("regions") || !worldDoc["regions"].is_array() || !worldDoc.contains("territories")
        || !worldDoc["territories"].is_array() || !worldDoc.contains("territoryIndex")
        || !worldDoc["territoryIndex"].is_array() || !worldDoc.contains("lawless") || !worldDoc["lawless"].is_array()
        || !worldDoc.contains("lawlessWatch") || !worldDoc["lawlessWatch"].is_array())
    {
        error = path + ": /world: expected regions/territories/territoryIndex/lawless/lawlessWatch arrays";
        return false;
    }
    for (const Json &row : worldDoc["regions"])
    {
        if (!row.is_object())
        {
            error = path + ": /world/regions: expected region objects";
            return false;
        }
        world::Region region;
        std::uint64_t id      = 0;
        std::uint64_t climate = 0;
        if (!readUint(st, row, "id", id, "/world/regions") || !readU32(st, row, "flags", region.flags, "/world/regions")
            || !readU32(st, row, "manaRichness", region.manaRichness, "/world/regions")
            || !readUint(st, row, "climate", climate, "/world/regions"))
        {
            error = st.error;
            return false;
        }
        if (climate >= static_cast<std::uint64_t>(world::Climate::COUNT))
        {
            error = path + ": /world/regions: climate ordinal out of range";
            return false;
        }
        region.climate = static_cast<world::Climate>(climate);
        if (!row.contains("season") || !row["season"].is_object() || !row.contains("weather")
            || !row["weather"].is_object())
        {
            error = path + ": /world/regions: expected season/weather objects";
            return false;
        }
        if (!readU32(st, row["season"], "index", region.season.index, "/world/regions/season")
            || !readUint(st, row["season"], "startedAt", region.season.startedAtWorldTime, "/world/regions/season")
            || !readU32(st, row["weather"], "kind", region.weather.kind, "/world/regions/weather")
            || !readUint(st, row["weather"], "since", region.weather.sinceWorldTime, "/world/regions/weather"))
        {
            error = st.error;
            return false;
        }
        region.id = static_cast<regionId>(id);
        regions.push_back(region);
    }
    for (const Json &row : worldDoc["territories"])
    {
        if (!row.is_object())
        {
            error = path + ": /world/territories: expected territory objects";
            return false;
        }
        TerritoryRow parsed;
        world::Territory &territory = parsed.territory;
        std::uint64_t id = 0, parent = 0, type = 0, region = 0;
        if (!readUint(st, row, "id", id, "/world/territories")
            || !readUint(st, row, "parent", parent, "/world/territories")
            || !readUint(st, row, "type", type, "/world/territories")
            || !readUint(st, row, "region", region, "/world/territories"))
        {
            error = st.error;
            return false;
        }
        if (type >= static_cast<std::uint64_t>(TerritoryType::COUNT))
        {
            error = path + ": /world/territories: territory type ordinal out of range";
            return false;
        }
        if (region > std::numeric_limits<std::uint16_t>::max())
        {
            error = path + ": /world/territories: region exceeds u16";
            return false;
        }
        territory.id     = static_cast<territoryId>(id);
        territory.parent = static_cast<territoryId>(parent);
        territory.type   = static_cast<TerritoryType>(type);
        territory.region = static_cast<regionId>(region);
        if (!readBool(st, row, "safe", territory.safe, "/world/territories")
            || !readBool(st, row, "lawless", territory.lawless, "/world/territories")
            || !readUint(st, row, "owner", territory.owner, "/world/territories"))
        {
            error = st.error;
            return false;
        }
        if (!readString(st, row, "lawKey", parsed.lawKey, "/world/territories"))
        {
            error = st.error;
            return false;
        }
        if (!row.contains("titles") || !row["titles"].is_array() || !row.contains("archive")
            || !row["archive"].is_array())
        {
            error = path + ": /world/territories: expected titles/archive arrays";
            return false;
        }
        for (const Json &title : row["titles"])
        {
            if (!title.is_array() || title.size() != 2 || !isUint(title[0]) || !isUint(title[1]))
            {
                error = path + ": /world/territories: expected [holder, class] title rows";
                return false;
            }
            const auto cls = static_cast<std::uint8_t>(title[1].get<std::uint64_t>());
            if (cls >= static_cast<std::uint8_t>(SocialClassId::COUNT))
            {
                error = path + ": /world/territories: social class ordinal out of range";
                return false;
            }
            territory.titles.emplace(title[0].get<characterId>(), static_cast<SocialClassId>(cls));
        }
        for (const Json &archiveRow : row["archive"])
        {
            if (!archiveRow.is_object())
            {
                error = path + ": /world/territories: expected archive row objects";
                return false;
            }
            world::CompactRecord record;
            if (!readUint(st, archiveRow, "evidenceId", record.evidenceId, "/world/territories/archive")
                || !readUint(st, archiveRow, "worldTime", record.worldTime, "/world/territories/archive")
                || !readU32(st, archiveRow, "crimeType", record.crimeType, "/world/territories/archive")
                || !readUint(st, archiveRow, "culprit", record.culprit, "/world/territories/archive")
                || !readUint(st, archiveRow, "victim", record.victim, "/world/territories/archive"))
            {
                error = st.error;
                return false;
            }
            territory.archive.push_back(record);
        }
        territoryRows.push_back(std::move(parsed));
    }
    for (const Json &row : worldDoc["territoryIndex"])
    {
        if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !isUint(row[1]))
        {
            error = path + ": /world/territoryIndex: expected [region, territory] pairs";
            return false;
        }
        territoryIndex.emplace_back(
            static_cast<regionId>(row[0].get<std::uint64_t>()), static_cast<territoryId>(row[1].get<std::uint64_t>())
        );
    }
    for (const Json &row : worldDoc["lawless"])
    {
        if (!isUint(row))
        {
            error = path + ": /world/lawless: expected territory ids";
            return false;
        }
        lawless.push_back(row.get<territoryId>());
    }
    for (const Json &row : worldDoc["lawlessWatch"])
    {
        if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !isUint(row[1]))
        {
            error = path + ": /world/lawlessWatch: expected [territory, at] pairs";
            return false;
        }
        lawlessWatch.emplace_back(row[0].get<territoryId>(), row[1].get<std::uint64_t>());
    }
    if (!readPosition(st, worldDoc, "playerSpawn", playerSpawn, "/world"))
    {
        error = st.error;
        return false;
    }

    // -- claims (custom laws first — the territory law keys may reference
    // them) --
    factions::ClaimStore::Snapshot claimBook;
    if (!doc.contains("claims") || !doc["claims"].is_object())
    {
        error = path + ": /claims: expected an object";
        return false;
    }
    const Json &claimsDoc = doc["claims"];
    if (!claimsDoc.contains("rows") || !claimsDoc["rows"].is_array() || !claimsDoc.contains("customLaws")
        || !claimsDoc["customLaws"].is_array())
    {
        error = path + ": /claims: expected rows/customLaws arrays";
        return false;
    }
    for (const Json &row : claimsDoc["customLaws"])
    {
        data::LawSet set;
        if (!readLawSet(st, row, set, "/claims/customLaws"))
        {
            error = st.error;
            return false;
        }
        claimBook.customLaws.emplace(set.key, std::move(set));
    }
    for (const Json &row : claimsDoc["rows"])
    {
        if (!row.is_object())
        {
            error = path + ": /claims/rows: expected claim row objects";
            return false;
        }
        factions::ClaimState claim;
        std::uint64_t territory = 0;
        if (!readUint(st, row, "territory", territory, "/claims/rows")
            || !readUint(st, row, "owner", claim.owner, "/claims/rows")
            || !readUint(st, row, "stintRealSeconds", claim.stintRealSeconds, "/claims/rows")
            || !readUint(st, row, "lastQualifyingRealTime", claim.lastQualifyingRealTime, "/claims/rows")
            || !readUint(st, row, "lastStampRealTime", claim.lastStampRealTime, "/claims/rows")
            || !readString(st, row, "lawKey", claim.lawKey, "/claims/rows")
            || !readBool(st, row, "abandoned", claim.abandoned, "/claims/rows"))
        {
            error = st.error;
            return false;
        }
        claimBook.claims.emplace(static_cast<territoryId>(territory), std::move(claim));
    }

    // -- characters + npcs --
    std::vector<Character> characterRows;
    std::vector<NpcState> npcRows;
    std::vector<bool> playerFlags;
    std::vector<PlayerCreation> creations;
    if (!doc.contains("characters") || !doc["characters"].is_array())
    {
        error = path + ": /characters: expected an array";
        return false;
    }
    for (const Json &row : doc["characters"])
    {
        if (!row.is_object() || !row.contains("character") || !row["character"].is_object())
        {
            error = path + ": /characters: expected rows with a character object";
            return false;
        }
        const Json &cDoc = row["character"];
        Character c;
        std::uint64_t id = 0, species = 0;
        if (!readUint(st, row, "id", id, "/characters"))
        {
            error = st.error;
            return false;
        }
        c.id = static_cast<characterId>(id);
        if (!readUint(st, cDoc, "species", species, "/characters"))
        {
            error = st.error;
            return false;
        }
        c.species = static_cast<speciesId>(species);
        if (!readU32(st, cDoc, "statusFlags", c.statusFlags, "/characters")
            || !readUint(st, cDoc, "duelOpponent", c.duelOpponent, "/characters")
            || !readUint(st, cDoc, "arenaId", c.arenaId, "/characters"))
        {
            error = st.error;
            return false;
        }
        if (!readUintArray(st, cDoc, "appearanceComponents", c.appearanceComponents, "/characters")
            || !readUintArray(st, cDoc, "equippedItems", c.equippedItems, "/characters"))
        {
            error = st.error;
            return false;
        }
        if (!cDoc.contains("body") || !cDoc["body"].is_object())
        {
            error = path + ": /characters: expected a body object";
            return false;
        }
        if (!readUint(st, cDoc["body"], "currentHp", c.body.currentHp, "/characters/body")
            || !readUint(st, cDoc["body"], "maxHp", c.body.maxHp, "/characters/body")
            || !readPosition(st, cDoc["body"], "position", c.body.position, "/characters/body"))
        {
            error = st.error;
            return false;
        }
        if (!cDoc.contains("progression") || !cDoc["progression"].is_object())
        {
            error = path + ": /characters: expected a progression object";
            return false;
        }
        const Json &pDoc        = cDoc["progression"];
        std::uint64_t levelTemp = 0;
        if (!readUint(st, pDoc, "totalXp", c.progression.totalXp, "/characters/progression")
            || !readInt(st, pDoc, "availableXp", c.progression.availableXp, "/characters/progression")
            || !readUint(st, pDoc, "level", levelTemp, "/characters/progression")
            || !readInt(st, pDoc, "currency", c.progression.currency, "/characters/progression"))
        {
            error = st.error;
            return false;
        }
        c.progression.level = static_cast<std::uint16_t>(
            levelTemp == 0 ? 1
                           : std::min(levelTemp, static_cast<std::uint64_t>(std::numeric_limits<std::uint16_t>::max()))
        );
        if (!pDoc.contains("stats") || !pDoc["stats"].is_array()
            || pDoc["stats"].size() != static_cast<std::size_t>(StatType::COUNT) || !pDoc.contains("skills")
            || !pDoc["skills"].is_array() || pDoc["skills"].size() != static_cast<std::size_t>(SkillType::COUNT))
        {
            error = path + ": /characters/progression: expected stats/skills arrays of COUNT entries";
            return false;
        }
        for (std::size_t i = 0; i < static_cast<std::size_t>(StatType::COUNT); ++i)
        {
            if (!isUint(pDoc["stats"][i]))
            {
                error = path + ": /characters/progression/stats: expected unsigned entries";
                return false;
            }
            c.progression.stats[i] = pDoc["stats"][i].get<std::uint64_t>();
        }
        for (std::size_t i = 0; i < static_cast<std::size_t>(SkillType::COUNT); ++i)
        {
            if (!isUint(pDoc["skills"][i]))
            {
                error = path + ": /characters/progression/skills: expected unsigned entries";
                return false;
            }
            c.progression.skills[i] = pDoc["skills"][i].get<std::uint64_t>();
        }
        if (!cDoc.contains("cooldowns") || !cDoc["cooldowns"].is_array()
            || cDoc["cooldowns"].size() != static_cast<std::size_t>(CooldownType::COUNT))
        {
            error = path + ": /characters: expected a cooldowns array of CooldownType::COUNT entries";
            return false;
        }
        for (std::size_t i = 0; i < static_cast<std::size_t>(CooldownType::COUNT); ++i)
        {
            if (!isUint(cDoc["cooldowns"][i]))
            {
                error = path + ": /characters/cooldowns: expected unsigned entries";
                return false;
            }
            c.cooldowns[i] = cDoc["cooldowns"][i].get<std::uint64_t>();
        }
        std::uint64_t faction = 0, eventId = 0, lastCombatTime = 0, peakPower = 0;
        if (!readUint(st, cDoc, "faction", faction, "/characters")
            || !readUint(st, cDoc, "eventId", eventId, "/characters")
            || !readUint(st, cDoc, "lastCombatTime", lastCombatTime, "/characters")
            || !readUint(st, cDoc, "peakPower", peakPower, "/characters"))
        {
            error = st.error;
            return false;
        }
        c.faction        = static_cast<factionId>(faction);
        c.eventId        = eventId;
        c.lastCombatTime = lastCombatTime;
        c.peakPower      = peakPower;
        // cachedPower/powerDirty are NOT loaded: the design reference's
        // contract — dirty flags set true for all on load, power recomputes.
        c.cachedPower = 0;
        c.powerDirty  = true;

        bool isPlayer = false;
        if (!readBool(st, row, "isPlayer", isPlayer, "/characters"))
        {
            error = st.error;
            return false;
        }
        PlayerCreation creation;
        if (isPlayer)
        {
            if (!row.contains("creation") || !row["creation"].is_object())
            {
                error = path + ": /characters: player rows carry a creation object";
                return false;
            }
            if (!readString(st, row["creation"], "name", creation.name, "/characters/creation")
                || !readString(st, row["creation"], "classKey", creation.classKey, "/characters/creation")
                || !readUintArray(
                    st, row["creation"], "appearanceComponents", creation.appearanceComponents, "/characters/creation"
                ))
            {
                error = st.error;
                return false;
            }
        }
        NpcState npc;
        const bool hasNpc = row.contains("npc") && row["npc"].is_object() && !row["npc"].empty();
        if (hasNpc && !readNpcState(st, row["npc"], npc, "/characters/npc"))
        {
            error = st.error;
            return false;
        }
        characterRows.push_back(c);
        npcRows.push_back(npc);
        playerFlags.push_back(isPlayer);
        creations.push_back(std::move(creation));
    }

    // -- corpses --
    std::vector<Corpse> corpseRows;
    if (!doc.contains("corpses") || !doc["corpses"].is_array())
    {
        error = path + ": /corpses: expected an array";
        return false;
    }
    for (const Json &row : doc["corpses"])
    {
        if (!row.is_object())
        {
            error = path + ": /corpses: expected corpse row objects";
            return false;
        }
        Corpse corpse;
        if (!readUint(st, row, "victim", corpse.victim, "/corpses")
            || !readPosition(st, row, "position", corpse.pos, "/corpses")
            || !readUint(st, row, "deathTime", corpse.deathTime, "/corpses")
            || !readBool(st, row, "discovered", corpse.discovered, "/corpses")
            || !readUint(st, row, "discoveredBy", corpse.discoveredBy, "/corpses")
            || !readInt(st, row, "lootCurrency", corpse.lootCurrency, "/corpses"))
        {
            error = st.error;
            return false;
        }
        if (!row.contains("loot") || !row["loot"].is_array())
        {
            error = path + ": /corpses: expected a loot array";
            return false;
        }
        for (const Json &stack : row["loot"])
        {
            if (!stack.is_array() || stack.size() != 2 || !isUint(stack[0]) || !isUint(stack[1]))
            {
                error = path + ": /corpses: expected [itemKey, count] loot rows";
                return false;
            }
            const std::uint64_t count = stack[1].get<std::uint64_t>();
            if (count > std::numeric_limits<std::uint32_t>::max())
            {
                error = path + ": /corpses: loot count exceeds u32";
                return false;
            }
            corpse.loot.push_back(items::ItemStack{stack[0].get<std::uint64_t>(), static_cast<std::uint32_t>(count)});
        }
        corpseRows.push_back(std::move(corpse));
    }

    // -- justice --
    if (!doc.contains("justice") || !doc["justice"].is_object())
    {
        error = path + ": /justice: expected an object";
        return false;
    }
    const Json &justiceDoc = doc["justice"];
    justice::JusticeStore justiceBook; // validated into a real store first, then moved
    if (!justiceDoc.contains("evidence") || !justiceDoc["evidence"].is_array())
    {
        error = path + ": /justice: expected an evidence array";
        return false;
    }
    for (const Json &row : justiceDoc["evidence"])
    {
        if (!row.is_object())
        {
            error = path + ": /justice/evidence: expected evidence row objects";
            return false;
        }
        justice::Evidence e;
        std::uint64_t territory = 0, type = 0, status = 0;
        if (!readUint(st, row, "id", e.id, "/justice/evidence")
            || !readUint(st, row, "culpritId", e.culpritId, "/justice/evidence")
            || !readUint(st, row, "victimId", e.victimId, "/justice/evidence")
            || !readUint(st, row, "territory", territory, "/justice/evidence")
            || !readUint(st, row, "worldTime", e.worldTime, "/justice/evidence")
            || !readUint(st, row, "type", type, "/justice/evidence")
            || !readUint(st, row, "status", status, "/justice/evidence")
            || !readU32(st, row, "physicalScore", e.physicalScore, "/justice/evidence")
            || !readU32(st, row, "aggregateReportWeight", e.aggregateReportWeight, "/justice/evidence"))
        {
            error = st.error;
            return false;
        }
        if (type >= static_cast<std::uint64_t>(CrimeType::COUNT)
            || status > static_cast<std::uint64_t>(EvidenceStatus::ARCHIVED))
        {
            error = path + ": /justice/evidence: crime type/status ordinal out of range";
            return false;
        }
        e.type      = static_cast<CrimeType>(type);
        e.status    = static_cast<EvidenceStatus>(status);
        e.territory = static_cast<territoryId>(territory);
        if (!row.contains("reports") || !row["reports"].is_array())
        {
            error = path + ": /justice/evidence: expected a reports array";
            return false;
        }
        for (const Json &reportRow : row["reports"])
        {
            if (!reportRow.is_object())
            {
                error = path + ": /justice/evidence: expected report objects";
                return false;
            }
            justice::Report r;
            if (!readUint(st, reportRow, "source", r.source, "/justice/evidence/reports")
                || !readU32(st, reportRow, "weight", r.weight, "/justice/evidence/reports")
                || !readBool(st, reportRow, "knowsIdentity", r.knowsIdentity, "/justice/evidence/reports")
                || !readBool(st, reportRow, "bribed", r.bribed, "/justice/evidence/reports"))
            {
                error = st.error;
                return false;
            }
            e.reports.push_back(r);
        }
        justiceBook.evidenceStore[e.territory].push_back(std::move(e));
    }
    if (!justiceDoc.contains("warrants") || !justiceDoc["warrants"].is_array())
    {
        error = path + ": /justice: expected a warrants array";
        return false;
    }
    for (const Json &row : justiceDoc["warrants"])
    {
        if (!row.is_object())
        {
            error = path + ": /justice/warrants: expected warrant row objects";
            return false;
        }
        justice::Warrant w;
        std::uint64_t id = 0, territory = 0;
        if (!readUint(st, row, "id", id, "/justice/warrants")
            || !readUint(st, row, "evidenceId", w.evidenceId, "/justice/warrants")
            || !readUint(st, row, "culpritId", w.culpritId, "/justice/warrants")
            || !readUint(st, row, "territory", territory, "/justice/warrants")
            || !readBool(st, row, "lethal", w.lethal, "/justice/warrants")
            || !readUint(st, row, "bounty", w.bounty, "/justice/warrants")
            || !readUint(st, row, "expiresTime", w.expiresTime, "/justice/warrants")
            || !readBool(st, row, "paid", w.paid, "/justice/warrants"))
        {
            error = st.error;
            return false;
        }
        w.territory = static_cast<territoryId>(territory);
        justiceBook.warrants.emplace(id, w);
    }
    if (!justiceDoc.contains("activeWarrantsByCulprit") || !justiceDoc["activeWarrantsByCulprit"].is_array())
    {
        error = path + ": /justice: expected an activeWarrantsByCulprit array";
        return false;
    }
    for (const Json &row : justiceDoc["activeWarrantsByCulprit"])
    {
        if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !row[1].is_array())
        {
            error = path + ": /justice/activeWarrantsByCulprit: expected [culprit, [warrantIds]] pairs";
            return false;
        }
        std::set<std::uint64_t> &warrantIds = justiceBook.activeWarrantsByCulprit[row[0].get<characterId>()];
        for (const Json &wid : row[1])
        {
            if (!isUint(wid))
            {
                error = path + ": /justice/activeWarrantsByCulprit: expected warrant ids";
                return false;
            }
            warrantIds.insert(wid.get<std::uint64_t>());
        }
    }
    if (!justiceDoc.contains("reputations") || !justiceDoc["reputations"].is_array())
    {
        error = path + ": /justice: expected a reputations array";
        return false;
    }
    for (const Json &row : justiceDoc["reputations"])
    {
        if (!row.is_object() || !row.contains("who") || !isUint(row["who"]))
        {
            error = path + ": /justice/reputations: expected rows with a who id";
            return false;
        }
        justice::Reputation rep;
        if (!row.contains("perTerritory") || !row["perTerritory"].is_array())
        {
            error = path + ": /justice/reputations: expected a perTerritory array";
            return false;
        }
        for (const Json &entry : row["perTerritory"])
        {
            if (!entry.is_array() || entry.size() != 2 || !isUint(entry[0]) || !isInt(entry[1]))
            {
                error = path + ": /justice/reputations: expected [territory, score] pairs";
                return false;
            }
            rep.perTerritory.emplace(
                entry[0].get<territoryId>(), static_cast<std::int32_t>(entry[1].get<std::int64_t>())
            );
        }
        if (!readI32(st, row, "notoriety", rep.notoriety, "/justice/reputations")
            || !readUint(st, row, "lastDecayTime", rep.lastDecayTime, "/justice/reputations"))
        {
            error = st.error;
            return false;
        }
        justiceBook.reputations.emplace(row["who"].get<characterId>(), std::move(rep));
    }
    auto readUintPairs =
        [&st](
            const Json &parent, const char *key, const std::string &pointer, std::map<std::uint64_t, std::uint64_t> &out
        ) {
            if (!parent.contains(key) || !parent[key].is_array())
            {
                return st.fail(pointer + "/" + key, "expected an array");
            }
            for (const Json &row : parent[key])
            {
                if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !isUint(row[1]))
                {
                    return st.fail(pointer + "/" + key, "expected [id, value] pairs");
                }
                out.emplace(row[0].get<std::uint64_t>(), row[1].get<std::uint64_t>());
            }
            return true;
        };
    if (!readUintPairs(justiceDoc, "jailUntil", "/justice", justiceBook.jailUntil))
    {
        error = st.error;
        return false;
    }
    if (!readUintPairs(justiceDoc, "posters", "/justice", justiceBook.posters))
    {
        error = st.error;
        return false;
    }
    if (!readUintPairs(justiceDoc, "caseLastTick", "/justice", justiceBook.caseLastTick))
    {
        error = st.error;
        return false;
    }
    if (!justiceDoc.contains("assaultDamage") || !justiceDoc["assaultDamage"].is_array())
    {
        error = path + ": /justice: expected an assaultDamage array";
        return false;
    }
    for (const Json &row : justiceDoc["assaultDamage"])
    {
        if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !row[1].is_array())
        {
            error = path + ": /justice/assaultDamage: expected [culprit, [[victim, dmg]]] rows";
            return false;
        }
        std::map<characterId, std::uint64_t> &victims = justiceBook.assaultDamage[row[0].get<characterId>()];
        for (const Json &entry : row[1])
        {
            if (!entry.is_array() || entry.size() != 2 || !isUint(entry[0]) || !isUint(entry[1]))
            {
                error = path + ": /justice/assaultDamage: expected [victim, dmg] pairs";
                return false;
            }
            victims.emplace(entry[0].get<characterId>(), entry[1].get<std::uint64_t>());
        }
    }
    if (!justiceDoc.contains("missingPersons") || !justiceDoc["missingPersons"].is_array())
    {
        error = path + ": /justice: expected a missingPersons array";
        return false;
    }
    for (const Json &row : justiceDoc["missingPersons"])
    {
        if (!row.is_array() || row.size() != 3 || !isUint(row[0]) || !isUint(row[1]) || !isUint(row[2]))
        {
            error = path + ": /justice/missingPersons: expected [victim, territory, markedAt] rows";
            return false;
        }
        justice::MissingPerson missing;
        missing.territory = row[1].get<territoryId>();
        missing.markedAt  = row[2].get<std::uint64_t>();
        justiceBook.missingPersons.emplace(row[0].get<characterId>(), missing);
    }
    if (!justiceDoc.contains("signalledCulprits") || !justiceDoc["signalledCulprits"].is_array())
    {
        error = path + ": /justice: expected a signalledCulprits array";
        return false;
    }
    for (const Json &row : justiceDoc["signalledCulprits"])
    {
        if (!isUint(row))
        {
            error = path + ": /justice/signalledCulprits: expected character ids";
            return false;
        }
        justiceBook.signalledCulprits.insert(row.get<characterId>());
    }
    auto readStampsMap = [&st](
                             const Json &parent,
                             const char *key,
                             const std::string &pointer,
                             std::map<characterId, std::vector<std::uint64_t>> &out
                         ) {
        if (!parent.contains(key) || !parent[key].is_array())
        {
            return st.fail(pointer + "/" + key, "expected an array");
        }
        for (const Json &row : parent[key])
        {
            if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !row[1].is_array())
            {
                return st.fail(pointer + "/" + key, "expected [culprit, [stamps]] rows");
            }
            std::vector<std::uint64_t> &stamps = out[row[0].get<characterId>()];
            for (const Json &at : row[1])
            {
                if (!isUint(at))
                {
                    return st.fail(pointer + "/" + key, "expected u64 stamps");
                }
                stamps.push_back(at.get<std::uint64_t>());
            }
        }
        return true;
    };
    if (!readStampsMap(justiceDoc, "captureStamps", "/justice", justiceBook.captureStamps))
    {
        error = st.error;
        return false;
    }
    auto readTerritoryStampMap = [&st](
                                     const Json &parent,
                                     const char *key,
                                     const std::string &pointer,
                                     std::map<characterId, std::map<territoryId, std::uint64_t>> &out
                                 ) {
        if (!parent.contains(key) || !parent[key].is_array())
        {
            return st.fail(pointer + "/" + key, "expected an array");
        }
        for (const Json &row : parent[key])
        {
            if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !row[1].is_array())
            {
                return st.fail(pointer + "/" + key, "expected [culprit, [[territory, at]]] rows");
            }
            std::map<territoryId, std::uint64_t> &rows = out[row[0].get<characterId>()];
            for (const Json &entry : row[1])
            {
                if (!entry.is_array() || entry.size() != 2 || !isUint(entry[0]) || !isUint(entry[1]))
                {
                    return st.fail(pointer + "/" + key, "expected [territory, at] pairs");
                }
                rows.emplace(entry[0].get<territoryId>(), entry[1].get<std::uint64_t>());
            }
        }
        return true;
    };
    if (!readTerritoryStampMap(justiceDoc, "massacreMarkers", "/justice", justiceBook.massacreMarkers))
    {
        error = st.error;
        return false;
    }
    if (!justiceDoc.contains("killTally") || !justiceDoc["killTally"].is_array())
    {
        error = path + ": /justice: expected a killTally array";
        return false;
    }
    for (const Json &row : justiceDoc["killTally"])
    {
        if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !row[1].is_array())
        {
            error = path + ": /justice/killTally: expected [culprit, [[territory, at]]] rows";
            return false;
        }
        std::vector<std::pair<territoryId, std::uint64_t>> &rows = justiceBook.killTally[row[0].get<characterId>()];
        for (const Json &entry : row[1])
        {
            if (!entry.is_array() || entry.size() != 2 || !isUint(entry[0]) || !isUint(entry[1]))
            {
                error = path + ": /justice/killTally: expected [territory, at] pairs";
                return false;
            }
            rows.emplace_back(entry[0].get<territoryId>(), entry[1].get<std::uint64_t>());
        }
    }

    // -- events --
    events::EventEngine::Snapshot eventBook;
    if (!doc.contains("events") || !doc["events"].is_object())
    {
        error = path + ": /events: expected an object";
        return false;
    }
    const Json &eventsDoc = doc["events"];
    for (const char *section : {"active", "history"})
    {
        if (!eventsDoc.contains(section) || !eventsDoc[section].is_array())
        {
            error = std::string(path) + ": /events/" + section + ": expected an array";
            return false;
        }
        for (const Json &row : eventsDoc[section])
        {
            std::uint64_t id = 0;
            events::Event ev;
            std::string pointer = std::string("/events/") + section;
            if (!readEventRow(st, row, id, ev, pointer))
            {
                error = st.error;
                return false;
            }
            if (std::string(section) == "active")
            {
                readEventSide(st, row, eventBook, id, pointer);
                if (!st.ok())
                {
                    error = st.error;
                    return false;
                }
                eventBook.active.emplace(id, std::move(ev));
            } else
            {
                eventBook.ended.emplace(id, std::move(ev));
            }
        }
    }
    if (!eventsDoc.contains("priceBias") || !eventsDoc["priceBias"].is_array() || !eventsDoc.contains("weatherBias")
        || !eventsDoc["weatherBias"].is_array())
    {
        error = path + ": /events: expected priceBias/weatherBias arrays";
        return false;
    }
    for (const Json &row : eventsDoc["priceBias"])
    {
        if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !row[1].is_array())
        {
            error = path + ": /events/priceBias: expected [region, [[good, pct, until]]] rows";
            return false;
        }
        std::map<goodId, std::pair<std::uint32_t, std::uint64_t>> &goods
            = eventBook.priceBias[static_cast<regionId>(row[0].get<std::uint64_t>())];
        for (const Json &entry : row[1])
        {
            if (!entry.is_array() || entry.size() != 3 || !isUint(entry[0]) || !isUint(entry[1]) || !isUint(entry[2]))
            {
                error = path + ": /events/priceBias: expected [good, pct, until] rows";
                return false;
            }
            const std::uint64_t pct = entry[1].get<std::uint64_t>();
            if (pct > std::numeric_limits<std::uint32_t>::max())
            {
                error = path + ": /events/priceBias: pct exceeds u32";
                return false;
            }
            goods.emplace(
                entry[0].get<goodId>(), std::make_pair(static_cast<std::uint32_t>(pct), entry[2].get<std::uint64_t>())
            );
        }
    }
    for (const Json &row : eventsDoc["weatherBias"])
    {
        if (!row.is_array() || row.size() != 3 || !isUint(row[0]) || !row[1].is_string() || !isUint(row[2]))
        {
            error = path + ": /events/weatherBias: expected [region, state, until] rows";
            return false;
        }
        eventBook.weatherBias.emplace(
            static_cast<regionId>(row[0].get<std::uint64_t>()),
            std::make_pair(row[1].get<std::string>(), row[2].get<std::uint64_t>())
        );
    }

    // -- economy --
    EconomyStore::Snapshot econBook;
    if (!doc.contains("economy") || !doc["economy"].is_object())
    {
        error = path + ": /economy: expected an object";
        return false;
    }
    const Json &econDoc = doc["economy"];
    for (const char *key : {"stocks", "prices", "pendingTaxes", "territoryWealth", "population", "lastRecompute"})
    {
        if (!econDoc.contains(key) || !econDoc[key].is_array())
        {
            error = std::string(path) + ": /economy/" + key + ": expected an array";
            return false;
        }
    }
    for (const Json &row : econDoc["stocks"])
    {
        if (!row.is_array() || row.size() != 4 || !isUint(row[0]) || !isUint(row[1]) || !isInt(row[2])
            || !isInt(row[3]))
        {
            error = path + ": /economy/stocks: expected [region, good, supply, demand] rows";
            return false;
        }
        econBook.stocks[static_cast<regionId>(row[0].get<std::uint64_t>())][row[1].get<goodId>()]
            = RegionStock{row[2].get<std::int64_t>(), row[3].get<std::int64_t>()};
    }
    for (const Json &row : econDoc["prices"])
    {
        if (!row.is_array() || row.size() != 3 || !isUint(row[0]) || !isUint(row[1]) || !isUint(row[2]))
        {
            error = path + ": /economy/prices: expected [region, good, price] rows";
            return false;
        }
        econBook.prices[static_cast<regionId>(row[0].get<std::uint64_t>())][row[1].get<goodId>()]
            = row[2].get<std::uint64_t>();
    }
    for (const Json &row : econDoc["pendingTaxes"])
    {
        if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !isUint(row[1]))
        {
            error = path + ": /economy/pendingTaxes: expected [territory, value] rows";
            return false;
        }
        econBook.pendingTaxes.emplace(row[0].get<territoryId>(), row[1].get<std::uint64_t>());
    }
    for (const Json &row : econDoc["territoryWealth"])
    {
        if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !isUint(row[1]))
        {
            error = path + ": /economy/territoryWealth: expected [territory, value] rows";
            return false;
        }
        econBook.territoryWealth.emplace(row[0].get<territoryId>(), row[1].get<std::uint64_t>());
    }
    for (const Json &row : econDoc["population"])
    {
        if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !isUint(row[1]))
        {
            error = path + ": /economy/population: expected [region, count] rows";
            return false;
        }
        const std::uint64_t count = row[1].get<std::uint64_t>();
        if (count > std::numeric_limits<std::uint32_t>::max())
        {
            error = path + ": /economy/population: count exceeds u32";
            return false;
        }
        econBook.population.emplace(
            static_cast<regionId>(row[0].get<std::uint64_t>()), static_cast<std::uint32_t>(count)
        );
    }
    for (const Json &row : econDoc["lastRecompute"])
    {
        if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !isUint(row[1]))
        {
            error = path + ": /economy/lastRecompute: expected [region, at] rows";
            return false;
        }
        econBook.lastRecompute.emplace(static_cast<regionId>(row[0].get<std::uint64_t>()), row[1].get<std::uint64_t>());
    }
    if (!econDoc.contains("goodsHeld") || !econDoc["goodsHeld"].is_array())
    {
        error = path + ": /economy/goodsHeld: expected an array";
        return false;
    }
    for (const Json &row : econDoc["goodsHeld"])
    {
        if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !row[1].is_array())
        {
            error = path + ": /economy/goodsHeld: expected [who, [[good, value]]] rows";
            return false;
        }
        std::map<goodId, std::uint64_t> &goods = econBook.goodsHeld[row[0].get<characterId>()];
        for (const Json &entry : row[1])
        {
            if (!entry.is_array() || entry.size() != 2 || !isUint(entry[0]) || !isUint(entry[1]))
            {
                error = path + ": /economy/goodsHeld: expected [good, value] pairs";
                return false;
            }
            goods.emplace(entry[0].get<goodId>(), entry[1].get<std::uint64_t>());
        }
    }

    // -- factions + wars --
    factions::FactionStore::Snapshot factionBook;
    if (!doc.contains("factions") || !doc["factions"].is_array() || !doc.contains("factionIndex")
        || !doc["factionIndex"].is_object())
    {
        error = path + ": /factions: expected a factions array + a factionIndex object";
        return false;
    }
    for (const Json &row : doc["factions"])
    {
        if (!row.is_object())
        {
            error = path + ": /factions: expected faction row objects";
            return false;
        }
        factions::Faction faction;
        std::uint64_t id = 0, capital = 0;
        if (!readUint(st, row, "id", id, "/factions") || !readString(st, row, "name", faction.name, "/factions")
            || !readUint(st, row, "capital", capital, "/factions")
            || !readBool(st, row, "playerFounded", faction.playerFounded, "/factions"))
        {
            error = st.error;
            return false;
        }
        faction.capital = static_cast<territoryId>(capital);
        std::vector<std::uint64_t> memberRows;
        if (!readUintArray(st, row, "members", memberRows, "/factions"))
        {
            error = st.error;
            return false;
        }
        faction.members.insert(memberRows.begin(), memberRows.end());
        if (!row.contains("ranks") || !row["ranks"].is_array())
        {
            error = path + ": /factions: expected a ranks array";
            return false;
        }
        for (const Json &entry : row["ranks"])
        {
            if (!entry.is_array() || entry.size() != 2 || !isUint(entry[0]) || !isUint(entry[1]))
            {
                error = path + ": /factions: expected [who, rank] rows";
                return false;
            }
            faction.ranks.emplace(
                entry[0].get<characterId>(), static_cast<std::uint8_t>(entry[1].get<std::uint64_t>())
            );
        }
        if (!row.contains("sharedResources") || !row["sharedResources"].is_array())
        {
            error = path + ": /factions: expected a sharedResources array";
            return false;
        }
        for (const Json &entry : row["sharedResources"])
        {
            if (!entry.is_array() || entry.size() != 2 || !isUint(entry[0]) || !isUint(entry[1]))
            {
                error = path + ": /factions: expected [good, value] rows";
                return false;
            }
            faction.sharedResources.emplace(entry[0].get<goodId>(), entry[1].get<std::uint64_t>());
        }
        if (!row.contains("relations") || !row["relations"].is_array())
        {
            error = path + ": /factions: expected a relations array";
            return false;
        }
        for (const Json &entry : row["relations"])
        {
            if (!entry.is_object())
            {
                error = path + ": /factions: expected relation objects";
                return false;
            }
            factions::Relation relation;
            std::uint64_t other = 0;
            if (!readUint(st, entry, "faction", other, "/factions/relations")
                || !readI32(st, entry, "score", relation.score, "/factions/relations")
                || !readBool(st, entry, "alliance", relation.alliance, "/factions/relations")
                || !readBool(st, entry, "rivalry", relation.rivalry, "/factions/relations")
                || !readUint(st, entry, "treatyUntil", relation.treatyUntil, "/factions/relations"))
            {
                error = st.error;
                return false;
            }
            faction.relations.emplace(static_cast<factionId>(other), relation);
        }
        factionBook.factions.emplace(static_cast<factionId>(id), std::move(faction));
    }
    const Json &factionIndex = doc["factionIndex"];
    if (!factionIndex.contains("membership") || !factionIndex["membership"].is_array()
        || !factionIndex.contains("tradeVolume") || !factionIndex["tradeVolume"].is_array())
    {
        error = path + ": /factionIndex: expected membership/tradeVolume arrays";
        return false;
    }
    for (const Json &row : factionIndex["membership"])
    {
        if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !isUint(row[1]))
        {
            error = path + ": /factionIndex/membership: expected [who, faction] pairs";
            return false;
        }
        factionBook.membership.emplace(row[0].get<characterId>(), static_cast<factionId>(row[1].get<std::uint64_t>()));
    }
    for (const Json &row : factionIndex["tradeVolume"])
    {
        if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !isUint(row[1]))
        {
            error = path + ": /factionIndex/tradeVolume: expected [key, value] pairs";
            return false;
        }
        factionBook.tradeVolume.emplace(row[0].get<std::uint64_t>(), row[1].get<std::uint64_t>());
    }
    std::uint64_t nextFactionId = 0;
    if (!readUint(st, factionIndex, "lastDriftMonth", factionBook.lastDriftMonth, "/factionIndex")
        || !readUint(st, factionIndex, "nextFactionId", nextFactionId, "/factionIndex"))
    {
        error = st.error;
        return false;
    }
    factionBook.nextFactionId = static_cast<factionId>(nextFactionId);

    factions::WarStore::Snapshot warBook;
    if (!doc.contains("wars") || !doc["wars"].is_array() || !doc.contains("warState") || !doc["warState"].is_object())
    {
        error = path + ": /wars: expected a wars array + a warState object";
        return false;
    }
    for (const Json &row : doc["wars"])
    {
        if (!row.is_object())
        {
            error = path + ": /wars: expected war row objects";
            return false;
        }
        factions::War war;
        std::uint64_t id = 0, attacker = 0, defender = 0, outcome = 0;
        if (!readUint(st, row, "id", id, "/wars") || !readUint(st, row, "attacker", attacker, "/wars")
            || !readUint(st, row, "defender", defender, "/wars"))
        {
            error = st.error;
            return false;
        }
        war.attacker = static_cast<factionId>(attacker);
        war.defender = static_cast<factionId>(defender);
        if (!row.contains("cb") || !row["cb"].is_object())
        {
            error = path + ": /wars: expected a cb object";
            return false;
        }
        std::uint64_t kind = 0, cbRegion = 0, cbOrigin = 0;
        if (!readUint(st, row["cb"], "kind", kind, "/wars/cb")
            || !readUint(st, row["cb"], "region", cbRegion, "/wars/cb")
            || !readUint(st, row["cb"], "origin", cbOrigin, "/wars/cb"))
        {
            error = st.error;
            return false;
        }
        if (kind >= 5) // CasusBelliKind's five pinned values (6.2)
        {
            error = path + ": /wars/cb: casus belli kind ordinal out of range";
            return false;
        }
        war.cb.kind   = static_cast<factions::CasusBelliKind>(kind);
        war.cb.region = static_cast<regionId>(cbRegion);
        war.cb.origin = static_cast<territoryId>(cbOrigin);
        if (!readUint(st, row, "declaredAt", war.declaredAt, "/wars")
            || !readUint(st, row, "noticeEndsAt", war.noticeEndsAt, "/wars")
            || !readUint(st, row, "endedAt", war.endedAt, "/wars") || !readUint(st, row, "outcome", outcome, "/wars")
            || !readBool(st, row, "active", war.active, "/wars"))
        {
            error = st.error;
            return false;
        }
        if (outcome > static_cast<std::uint64_t>(factions::WarOutcome::DefenderWon))
        {
            error = path + ": /wars: outcome ordinal out of range";
            return false;
        }
        war.outcome = static_cast<factions::WarOutcome>(outcome);
        warBook.wars.emplace(id, war);
    }
    const Json &warState = doc["warState"];
    if (!readUint(st, warState, "nextWarId", warBook.nextWarId, "/warState")
        || !readUint(st, warState, "lastAutoDeclareMonth", warBook.lastAutoDeclareMonth, "/warState")
        || !readUint(st, warState, "nextProgressCheckAt", warBook.nextProgressCheckAt, "/warState"))
    {
        error = st.error;
        return false;
    }
    if (!warState.contains("massacreMarkers") || !warState["massacreMarkers"].is_array())
    {
        error = path + ": /warState: expected a massacreMarkers array";
        return false;
    }
    for (const Json &row : warState["massacreMarkers"])
    {
        if (!row.is_array() || row.size() != 3 || !isUint(row[0]) || !isUint(row[1]) || !isUint(row[2]))
        {
            error = path + ": /warState/massacreMarkers: expected [territory, faction, at] rows";
            return false;
        }
        warBook.massacreMarkers.emplace(
            row[0].get<territoryId>(),
            std::make_pair(static_cast<factionId>(row[1].get<std::uint64_t>()), row[2].get<std::uint64_t>())
        );
    }

    // -- items (instances + bags), appearance, roles, consents, combat --
    if (!doc.contains("items") || !doc["items"].is_object() || !doc["items"].contains("instances")
        || !doc["items"]["instances"].is_array() || !doc["items"].contains("bags") || !doc["items"]["bags"].is_array())
    {
        error = path + ": /items: expected instances/bags arrays";
        return false;
    }
    std::vector<std::pair<std::uint64_t, std::uint32_t>> instanceRows;
    for (const Json &row : doc["items"]["instances"])
    {
        if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !isUint(row[1]))
        {
            error = path + ": /items/instances: expected [instanceId, defId] pairs";
            return false;
        }
        const std::uint64_t def = row[1].get<std::uint64_t>();
        if (def > std::numeric_limits<std::uint32_t>::max())
        {
            error = path + ": /items/instances: def id exceeds u32";
            return false;
        }
        instanceRows.emplace_back(row[0].get<std::uint64_t>(), static_cast<std::uint32_t>(def));
    }
    std::vector<std::pair<characterId, items::Inventory>> bagRows;
    for (const Json &row : doc["items"]["bags"])
    {
        if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !row[1].is_object())
        {
            error = path + ": /items/bags: expected [who, inventory] rows";
            return false;
        }
        // (the inventory object is positional — row[1] IS the bag)
        const Json &bagDoc = row[1];
        items::Inventory bag;
        if (!bagDoc.contains("stacks") || !bagDoc["stacks"].is_array())
        {
            error = path + ": /items/bags: expected a stacks array";
            return false;
        }
        for (const Json &stackRow : bagDoc["stacks"])
        {
            if (!stackRow.is_array() || stackRow.size() != 2 || !isUint(stackRow[0]) || !isUint(stackRow[1]))
            {
                error = path + ": /items/bags: expected [itemKey, count] pairs";
                return false;
            }
            const std::uint64_t count = stackRow[1].get<std::uint64_t>();
            if (count > std::numeric_limits<std::uint32_t>::max())
            {
                error = path + ": /items/bags: count exceeds u32";
                return false;
            }
            bag.stacks.emplace(stackRow[0].get<std::uint64_t>(), static_cast<std::uint32_t>(count));
        }
        if (!readUintArray(st, bagDoc, "equipped", bag.equipped, "/items/bags"))
        {
            error = st.error;
            return false;
        }
        std::vector<std::uint64_t> loose;
        if (!readUintArray(st, bagDoc, "instances", loose, "/items/bags"))
        {
            error = st.error;
            return false;
        }
        bag.instances = loose;
        bagRows.emplace_back(row[0].get<characterId>(), std::move(bag));
    }
    std::map<characterId, std::vector<std::uint64_t>> appearanceRows;
    if (!doc.contains("appearance") || !doc["appearance"].is_array())
    {
        error = path + ": /appearance: expected an array";
        return false;
    }
    for (const Json &row : doc["appearance"])
    {
        if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !row[1].is_array())
        {
            error = path + ": /appearance: expected [who, [stamps]] rows";
            return false;
        }
        std::vector<std::uint64_t> stamps;
        for (const Json &at : row[1])
        {
            if (!isUint(at))
            {
                error = path + ": /appearance: expected u64 stamps";
                return false;
            }
            stamps.push_back(at.get<std::uint64_t>());
        }
        appearanceRows.emplace(row[0].get<characterId>(), std::move(stamps));
    }
    RoleRegistry::Snapshot roleBook;
    if (!doc.contains("roles") || !doc["roles"].is_object())
    {
        error = path + ": /roles: expected an object";
        return false;
    }
    const Json &rolesDoc = doc["roles"];
    if (!rolesDoc.contains("openSeats") || !rolesDoc["openSeats"].is_array() || !rolesDoc.contains("guardPower")
        || !rolesDoc["guardPower"].is_array())
    {
        error = path + ": /roles: expected openSeats/guardPower arrays";
        return false;
    }
    for (const Json &row : rolesDoc["openSeats"])
    {
        if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !row[1].is_array())
        {
            error = path + ": /roles/openSeats: expected [territory, [keys]] rows";
            return false;
        }
        std::set<std::string> &seats = roleBook.openSeats[row[0].get<territoryId>()];
        for (const Json &seat : row[1])
        {
            if (!seat.is_string())
            {
                error = path + ": /roles/openSeats: expected role key strings";
                return false;
            }
            seats.insert(seat.get<std::string>());
        }
    }
    for (const Json &row : rolesDoc["guardPower"])
    {
        if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !isUint(row[1]))
        {
            error = path + ": /roles/guardPower: expected [territory, power] rows";
            return false;
        }
        roleBook.guardPower.emplace(row[0].get<territoryId>(), row[1].get<std::uint64_t>());
    }
    combat::ConsentRegistry::Snapshot consentBook;
    if (!doc.contains("consents") || !doc["consents"].is_object())
    {
        error = path + ": /consents: expected an object";
        return false;
    }
    const Json &consentsDoc = doc["consents"];
    if (!consentsDoc.contains("pending") || !consentsDoc["pending"].is_array() || !consentsDoc.contains("duels")
        || !consentsDoc["duels"].is_array())
    {
        error = path + ": /consents: expected pending/duels arrays";
        return false;
    }
    for (const Json &row : consentsDoc["pending"])
    {
        if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !row[1].is_array())
        {
            error = path + ": /consents/pending: expected [target, [[challenger, at]]] rows";
            return false;
        }
        std::map<characterId, std::uint64_t> &challengers = consentBook.pending[row[0].get<characterId>()];
        for (const Json &entry : row[1])
        {
            if (!entry.is_array() || entry.size() != 2 || !isUint(entry[0]) || !isUint(entry[1]))
            {
                error = path + ": /consents/pending: expected [challenger, at] pairs";
                return false;
            }
            challengers.emplace(entry[0].get<characterId>(), entry[1].get<std::uint64_t>());
        }
    }
    for (const Json &row : consentsDoc["duels"])
    {
        if (!row.is_object())
        {
            error = path + ": /consents/duels: expected duel row objects";
            return false;
        }
        combat::ActiveDuel duel;
        std::uint64_t key = 0;
        if (!readUint(st, row, "key", key, "/consents/duels") || !readUint(st, row, "a", duel.a, "/consents/duels")
            || !readUint(st, row, "b", duel.b, "/consents/duels")
            || !readUint(st, row, "startedAt", duel.startedAt, "/consents/duels")
            || !readUint(st, row, "lastDamage", duel.lastDamage, "/consents/duels"))
        {
            error = st.error;
            return false;
        }
        consentBook.duels.emplace(key, duel);
    }
    std::map<characterId, std::uint64_t> castCooldowns;
    if (!doc.contains("combat") || !doc["combat"].is_object() || !doc["combat"].contains("castCooldowns")
        || !doc["combat"]["castCooldowns"].is_array())
    {
        error = path + ": /combat: expected a castCooldowns array";
        return false;
    }
    for (const Json &row : doc["combat"]["castCooldowns"])
    {
        if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !isUint(row[1]))
        {
            error = path + ": /combat/castCooldowns: expected [who, until] pairs";
            return false;
        }
        castCooldowns.emplace(row[0].get<characterId>(), row[1].get<std::uint64_t>());
    }

    // -- quests: the reserved key; an empty array is the pinned reading --
    if (!doc.contains("quests") || !doc["quests"].is_array())
    {
        error = path + ": /quests: expected an array";
        return false;
    }

    // -- runtime (optional) --
    std::map<std::uint64_t, std::map<characterId, std::uint64_t>> contributionBook;
    std::set<std::uint64_t> enforcementResponded;
    RuntimeState runtimeParsed;
    bool hasRuntime = doc.contains("runtime") && doc["runtime"].is_object();
    if (hasRuntime)
    {
        const Json &rt = doc["runtime"];
        // /runtime/economy is the nested gate object the save side writes
        // (README: `economy {started, lastHour, lastDay}`).
        if (!rt.contains("economy") || !rt["economy"].is_object())
        {
            error = path + ": /runtime/economy: expected an object";
            return false;
        }
        const Json &economyGates = rt["economy"];
        if (!readUint(st, rt, "tickNumber", runtimeParsed.tickNumber, "/runtime")
            || !readUint(st, rt, "gridLastSweepDay", runtimeParsed.gridLastSweepDay, "/runtime")
            || !readUint(st, rt, "justiceSweepLastDay", runtimeParsed.justiceSweepLastDay, "/runtime")
            || !readUint(st, rt, "weatherPrevWorldTime", runtimeParsed.weatherPrevWorldTime, "/runtime")
            || !readBool(st, economyGates, "started", runtimeParsed.economyStarted, "/runtime/economy")
            || !readUint(st, economyGates, "lastHour", runtimeParsed.economyLastHour, "/runtime/economy")
            || !readUint(st, economyGates, "lastDay", runtimeParsed.economyLastDay, "/runtime/economy")
            || !readUint(st, rt, "suspicionLastRunHour", runtimeParsed.suspicionLastRunHour, "/runtime"))
        {
            error = st.error;
            return false;
        }
        if (rt.contains("contributionSnapshotAt") && rt["contributionSnapshotAt"].is_array())
        {
            for (const Json &row : rt["contributionSnapshotAt"])
            {
                if (!row.is_array() || row.size() != 2 || !isUint(row[0]) || !row[1].is_array())
                {
                    error = path + ": /runtime/contributionSnapshotAt: expected [eventId, [[actor, at]]] rows";
                    return false;
                }
                std::map<characterId, std::uint64_t> &actors = contributionBook[row[0].get<std::uint64_t>()];
                for (const Json &entry : row[1])
                {
                    if (!entry.is_array() || entry.size() != 2 || !isUint(entry[0]) || !isUint(entry[1]))
                    {
                        error = path + ": /runtime/contributionSnapshotAt: expected [actor, at] pairs";
                        return false;
                    }
                    actors.emplace(entry[0].get<characterId>(), entry[1].get<std::uint64_t>());
                }
            }
        }
        if (rt.contains("contributionNextWalkAt"))
        {
            // The 8.3 as-built's cadence gate rides its books; older saves
            // without the row load a fresh gate (the pre-gate saves' books
            // were tick-stamped, so the first post-load walk is a no-op for
            // them — the row is optional by design).
            if (!isUint(rt["contributionNextWalkAt"]))
            {
                error = path + ": /runtime/contributionNextWalkAt: expected an unsigned integer";
                return false;
            }
            runtimeParsed.contributionNextWalkAt = rt["contributionNextWalkAt"].get<std::uint64_t>();
        }
        if (rt.contains("enforcementResponded") && rt["enforcementResponded"].is_array())
        {
            for (const Json &wid : rt["enforcementResponded"])
            {
                if (!isUint(wid))
                {
                    error = path + ": /runtime/enforcementResponded: expected warrant ids";
                    return false;
                }
                enforcementResponded.insert(wid.get<std::uint64_t>());
            }
        }
        if (rt.contains("player") && rt["player"].is_object())
        {
            readPosition(st, rt["player"], "position", runtimeParsed.playerPosition, "/runtime/player");
            readBool(st, rt["player"], "valid", runtimeParsed.playerPositionValid, "/runtime/player");
        }
    }

    // -- cross-section identity checks (refuse the incoherent) --
    {
        std::set<std::uint64_t> knownEvidence;
        for (const auto &[territory, rows] : justiceBook.evidenceStore)
        {
            for (const justice::Evidence &e : rows)
            {
                knownEvidence.insert(e.id);
            }
        }
        for (const auto &[id, w] : justiceBook.warrants)
        {
            if (knownEvidence.count(w.evidenceId) == 0)
            {
                st.fail("/justice/warrants", "warrant " + std::to_string(id) + " references unknown evidence");
                error = st.error;
                return false;
            }
        }
        std::set<characterId> knownCharacters;
        for (const Character &c : characterRows)
        {
            if (!knownCharacters.insert(c.id).second)
            {
                st.fail("/characters", "duplicate character id " + std::to_string(c.id));
                error = st.error;
                return false;
            }
        }
        for (const auto &[culprit, warrantIds] : justiceBook.activeWarrantsByCulprit)
        {
            if (knownCharacters.count(culprit) == 0)
            {
                st.fail(
                    "/justice/activeWarrantsByCulprit", "culprit " + std::to_string(culprit) + " has no character row"
                );
                error = st.error;
                return false;
            }
            for (const std::uint64_t wid : warrantIds)
            {
                if (justiceBook.warrants.count(wid) == 0)
                {
                    st.fail(
                        "/justice/activeWarrantsByCulprit", "index references unknown warrant " + std::to_string(wid)
                    );
                    error = st.error;
                    return false;
                }
            }
        }
        std::set<territoryId> knownTerritories;
        for (const TerritoryRow &row : territoryRows)
        {
            if (!knownTerritories.insert(row.territory.id).second)
            {
                st.fail("/world/territories", "duplicate territory id " + std::to_string(row.territory.id));
                error = st.error;
                return false;
            }
        }
        for (const TerritoryRow &row : territoryRows)
        {
            if (row.territory.parent != 0 && knownTerritories.count(row.territory.parent) == 0)
            {
                st.fail(
                    "/world/territories", "territory " + std::to_string(row.territory.id) + " references unknown parent"
                );
                error = st.error;
                return false;
            }
        }
        // The id counters can never sit below a live id (R10: ids are never
        // reused; a rewound counter would reissue).
        std::uint64_t maxCharacterId = 0;
        for (const Character &c : characterRows)
        {
            maxCharacterId = std::max(maxCharacterId, c.id);
        }
        if (characterCounter <= maxCharacterId)
        {
            st.fail("/ids/character", "counter rewound below a live character id");
            error = st.error;
            return false;
        }
        std::uint64_t maxInstanceId = 0;
        for (const auto &[instance, def] : instanceRows)
        {
            (void)def;
            maxInstanceId = std::max(maxInstanceId, instance);
        }
        if (instanceCounter <= maxInstanceId)
        {
            st.fail("/ids/itemInstance", "counter rewound below a live instance id");
            error = st.error;
            return false;
        }
        std::uint64_t maxEventId = 0;
        for (const auto &[id, ev] : eventBook.active)
        {
            (void)ev;
            maxEventId = std::max(maxEventId, id);
        }
        for (const auto &[id, ev] : eventBook.ended)
        {
            (void)ev;
            maxEventId = std::max(maxEventId, id);
        }
        if (eventCounter <= maxEventId)
        {
            st.fail("/ids/event", "counter rewound below a live event id");
            error = st.error;
            return false;
        }
    }

    if (!st.ok())
    {
        error = st.error;
        return false;
    }

    // ============================ apply phase ================================
    // The whole document validated; the stores take the snapshots in the
    // dependency order (clocks -> rng -> ids -> world -> claims -> law
    // pointers -> characters -> ... -> runtime gates).

    t.clock->worldTime  = worldTime;
    t.clock->realTime   = realTime;
    t.clock->timeScale  = timeScale;
    t.clock->tickRate   = tickRate;
    t.clock->worldCarry = worldCarry;
    t.clock->realCarry  = realCarry;

    t.rng->seedAll(t.seed);
    t.rng->loadState(rngPairs);

    t.world->clear();
    for (const world::Region &region : regions)
    {
        t.world->addRegion(region);
    }
    // Territory insert order: parents before children rebuilds the tree
    // through addTerritory's wiring (children vectors are derived, never
    // applied — a saved children list would double-attach). Id order is
    // parent order for every worldgen tree; sort defensively anyway.
    std::vector<TerritoryRow> orderedTerritories = territoryRows;
    std::sort(orderedTerritories.begin(), orderedTerritories.end(), [](const TerritoryRow &a, const TerritoryRow &b) {
        return a.territory.id < b.territory.id;
    });
    for (TerritoryRow &row : orderedTerritories)
    {
        world::Territory territory = row.territory;
        territory.children.clear(); // derived from parent on insert
        territory.laws = nullptr;   // resolved below (claims first, then registry)
        t.world->addTerritory(territory);
    }
    for (const auto &[region, territory] : territoryIndex)
    {
        t.world->setRegionTerritory(region, territory);
    }
    for (const territoryId id : lawless)
    {
        t.world->markLawless(id);
    }
    for (const auto &[id, at] : lawlessWatch)
    {
        t.world->watchGarrisonLess(id, at);
    }

    t.claims->restore(claimBook);
    for (TerritoryRow &row : territoryRows)
    {
        if (row.lawKey.empty())
        {
            continue; // nullptr: the inherit walk (lawsAt's rule)
        }
        world::Territory *territory = t.world->territory(row.territory.id);
        if (territory == nullptr)
        {
            continue; // validated above; unreachable
        }
        if (const data::LawSet *custom = t.claims->customLawByKey(row.lawKey); custom != nullptr)
        {
            territory->laws = custom; // the store-owned stable address
        } else if (const data::LawSet *fromRegistry = t.registry->lawSet(row.lawKey); fromRegistry != nullptr)
        {
            territory->laws = fromRegistry;
        } else
        {
            // A content mismatch (the set's key no longer exists — e.g. a mod
            // row removed between sessions): the inherit walk stands, the
            // reason is recorded, the load is not refused.
            territory->laws = nullptr;
            t.warnings.push_back(
                "law set '" + row.lawKey + "' (territory " + std::to_string(row.territory.id)
                + ") not in content; the territory inherits"
            );
        }
    }

    // Characters: adopt the counter BEFORE the first restore (the store's own
    // contract — adoption happens before the first create, never mid-run).
    t.characters->adoptWorldGen(characterCounter, playerSpawn);
    for (std::size_t i = 0; i < characterRows.size(); ++i)
    {
        t.characters->restoreCharacter(characterRows[i], playerFlags[i], playerFlags[i] ? &creations[i] : nullptr);
        if (!playerFlags[i])
        {
            // Every NPC carries a sidecar record (the 3.4 pairing); the parsed
            // defaults stand when the save's npc object was empty.
            t.npcs->create(characterRows[i].id) = npcRows[i];
        }
    }

    for (const Corpse &corpse : corpseRows)
    {
        t.corpses->restoreCorpse(corpse);
    }

    t.itemInstances->adoptCounter(instanceCounter);
    for (const auto &[instance, def] : instanceRows)
    {
        t.itemInstances->restore(instance, def);
    }
    for (const auto &[who, bag] : bagRows)
    {
        t.bags->bag(who) = bag; // auto-insert on first touch (the store's contract)
    }
    t.appearance->restore(std::move(appearanceRows));

    // The counters live in the REAL store (JusticeStore::ids_): loadIdState on
    // the local justiceBook would restore counters that are then thrown away
    // with the book (the maps below are all it contributes) — the loaded world
    // would re-issue evidence/warrant ids from FIRST_ID and fail the 8.2
    // double-roundtrip property. The real store is fresh here, so the
    // IdGenerator::loadState "never rewinds a live generator" contract holds.
    t.justice->loadIdState(
        std::map<std::string, std::uint64_t>{{"evidence", evidenceCounter}, {"warrant", warrantCounter}}
    );
    // JusticeStore is copy/move-deleted by design (one storage truth): the
    // maps apply member-by-member — the store's own public surface.
    t.justice->evidenceStore           = std::move(justiceBook.evidenceStore);
    t.justice->warrants                = std::move(justiceBook.warrants);
    t.justice->activeWarrantsByCulprit = std::move(justiceBook.activeWarrantsByCulprit);
    t.justice->reputations             = std::move(justiceBook.reputations);
    t.justice->jailUntil               = std::move(justiceBook.jailUntil);
    t.justice->assaultDamage           = std::move(justiceBook.assaultDamage);
    t.justice->missingPersons          = std::move(justiceBook.missingPersons);
    t.justice->caseLastTick            = std::move(justiceBook.caseLastTick);
    t.justice->signalledCulprits       = std::move(justiceBook.signalledCulprits);
    t.justice->posters                 = std::move(justiceBook.posters);
    t.justice->captureStamps           = std::move(justiceBook.captureStamps);
    t.justice->massacreMarkers         = std::move(justiceBook.massacreMarkers);
    t.justice->killTally               = std::move(justiceBook.killTally);

    eventBook.idCounters = std::map<std::string, std::uint64_t>{{"event", eventCounter}};
    t.events->restore(eventBook);

    t.economy->restore(econBook);
    t.factions->restore(factionBook);
    t.wars->restore(warBook);
    t.roles->restore(roleBook);
    t.consents->restore(consentBook);
    t.combatQueue->restoreCastCooldowns(std::move(castCooldowns));

    if (t.runtime != nullptr)
    {
        runtimeParsed.contributionSnapshotAt = std::move(contributionBook);
        runtimeParsed.enforcementResponded   = std::move(enforcementResponded);
        *t.runtime                           = std::move(runtimeParsed);
    }

    return true;
}

} // namespace harrowmere::persist
