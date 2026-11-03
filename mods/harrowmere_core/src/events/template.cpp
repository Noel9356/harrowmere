// template.cpp — the event_templates.json loader + the template validator
// (task 5.1's data half; see template.hpp for the pinned reading). Strict
// 0.4 door: every error carries file + JSON pointer + reason, and a failed
// load leaves `out` partial (the registry commits nothing on errors).

#include "events/template.hpp"

#include "core/constants.hpp" // SCALE (A-1)
#include "data/json.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace harrowmere::events
{

namespace
{

std::optional<std::string> readFile(
    const std::string &path
)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// ---- closed tag tables (lockstep with enums.hpp; R9) ---------------------------

struct ActionTypeRow
{
    ActionType type;
    const char *tag;
};
constexpr ActionTypeRow kActionTypeTags[] = {
    {ActionType::DAMAGE, "DAMAGE"},
    {ActionType::HEALING, "HEALING"},
    {ActionType::OBJECTIVE, "OBJECTIVE"},
};
static_assert(
    sizeof(kActionTypeTags) / sizeof(kActionTypeTags[0]) == static_cast<std::size_t>(ActionType::COUNT),
    "action-type tag table must match ActionType::COUNT"
);

std::optional<ActionType> actionTypeFromKey(
    std::string_view tag
)
{
    for (const auto &row : kActionTypeTags)
        if (tag == row.tag)
            return row.type;
    return std::nullopt;
}

struct ConditionKindRow
{
    ConditionKind kind;
    const char *tag;
};
constexpr ConditionKindRow kConditionTags[] = {
    {ConditionKind::Random, "Random"},
    {ConditionKind::Flag, "Flag"},
    {ConditionKind::ActionThreshold, "ActionThreshold"},
    {ConditionKind::EventState, "EventState"},
    {ConditionKind::Timer, "Timer"},
    {ConditionKind::MinParticipants, "MinParticipants"},
    {ConditionKind::TargetDead, "TargetDead"},
    {ConditionKind::TargetCaptured, "TargetCaptured"},
};

struct ActionKindRow
{
    ActionKind kind;
    const char *tag;
};
constexpr ActionKindRow kActionKindTags[] = {
    {ActionKind::SpawnTargets, "SpawnTargets"},
    {ActionKind::SetRegionFlag, "SetRegionFlag"},
    {ActionKind::PriceBias, "PriceBias"},
    {ActionKind::WeatherBias, "WeatherBias"},
    {ActionKind::SpawnNestedEvent, "SpawnNestedEvent"},
    {ActionKind::GrantRewardTier, "GrantRewardTier"},
    {ActionKind::EvacuateTerritory, "EvacuateTerritory"},
    {ActionKind::EndEvent, "EndEvent"},
    {ActionKind::DespawnTargets, "DespawnTargets"},
    {ActionKind::GarrisonTerritory, "GarrisonTerritory"},
};

std::optional<ConditionKind> lookupCondition(
    std::string_view tag
)
{
    // The card names the six pinned conditions with the Condition suffix
    // (TimerCondition{days}, ActionThresholdCondition{...}); B.6's data
    // mixes the bare form (ActionThreshold) and the suffixed form
    // (TimerCondition) on the same transition. Both spellings resolve.
    std::string_view bare = tag;
    if (bare.size() > 9 && bare.substr(bare.size() - 9) == "Condition")
    {
        bare.remove_suffix(9);
    }
    for (const auto &row : kConditionTags)
        if (bare == row.tag)
            return row.kind;
    return std::nullopt;
}

std::optional<ActionKind> lookupAction(
    std::string_view tag
)
{
    for (const auto &row : kActionKindTags)
        if (tag == row.tag)
            return row.kind;
    return std::nullopt;
}

[[noreturn]] void throwRow(
    const std::string &pointer, const std::string &message
)
{
    throw std::runtime_error(pointer + ": " + message);
}

std::uint32_t readU32(
    const nlohmann::json &row, const char *field, const std::string &pointer
)
{
    if (!row.contains(field) || !row[field].is_number_unsigned())
    {
        throwRow(pointer, std::string("missing/invalid unsigned field '") + field + "'");
    }
    const std::uint64_t value = row[field].get<std::uint64_t>();
    if (value > SCALE)
    {
        throwRow(pointer + "/" + field, "value out of the 0..SCALE range");
    }
    return static_cast<std::uint32_t>(value);
}

std::string readString(
    const nlohmann::json &row, const char *field, const std::string &pointer
)
{
    if (!row.contains(field) || !row[field].is_string())
    {
        throwRow(pointer, std::string("missing/invalid string field '") + field + "'");
    }
    return row[field].get<std::string>();
}

Condition parseCondition(
    const nlohmann::json &row, const std::string &pointer
)
{
    Condition c;
    const std::string kind = readString(row, "kind", pointer);
    const auto parsed      = lookupCondition(kind);
    if (!parsed)
    {
        throwRow(pointer + "/kind", "unknown condition kind '" + kind + "'");
    }
    c.kind = *parsed;
    switch (c.kind)
    {
    case ConditionKind::Random:
        c.pct = readU32(row, "pct", pointer);
        break;
    case ConditionKind::Flag:
        c.regionFlag = readString(row, "flag", pointer);
        if (row.contains("set"))
        {
            c.set = row["set"].get<bool>();
        }
        break;
    case ConditionKind::ActionThreshold: {
        const std::string at = readString(row, "actionType", pointer);
        const auto type      = actionTypeFromKey(at);
        if (!type)
        {
            throwRow(pointer + "/actionType", "unknown action type '" + at + "'");
        }
        c.actionType = *type;
        c.total      = readU32(row, "total", pointer);
        break;
    }
    case ConditionKind::EventState:
        c.templateKey = readString(row, "templateKey", pointer);
        if (!row.contains("stateId") || !row["stateId"].is_number_unsigned())
        {
            throwRow(pointer, "EventState condition missing unsigned 'stateId'");
        }
        c.stateId = row["stateId"].get<std::uint64_t>();
        break;
    case ConditionKind::Timer:
        if (!row.contains("days") || !row["days"].is_number_unsigned())
        {
            throwRow(pointer, "Timer condition missing unsigned 'days'");
        }
        c.days = row["days"].get<std::uint32_t>();
        break;
    case ConditionKind::MinParticipants:
        if (!row.contains("n") || !row["n"].is_number_unsigned())
        {
            throwRow(pointer, "MinParticipants condition missing unsigned 'n'");
        }
        c.n = row["n"].get<std::uint32_t>();
        break;
    case ConditionKind::TargetDead:
    case ConditionKind::TargetCaptured:
        break; // B.6's target conditions carry no payload (5.4 reads the targets)
    }
    return c;
}

Action parseAction(
    const nlohmann::json &row, const std::string &pointer
)
{
    Action a;
    const std::string kind = readString(row, "kind", pointer);
    const auto parsed      = lookupAction(kind);
    if (!parsed)
    {
        throwRow(pointer + "/kind", "unknown action kind '" + kind + "'");
    }
    a.kind = *parsed;
    switch (a.kind)
    {
    case ActionKind::SpawnTargets:
        a.speciesKey = readString(row, "speciesKey", pointer);
        if (row.contains("roleKey"))
        {
            a.roleKey = row["roleKey"].get<std::string>();
        }
        if (!row.contains("count") || !row["count"].is_number_unsigned())
        {
            throwRow(pointer, "SpawnTargets missing unsigned 'count'");
        }
        a.count = row["count"].get<std::uint32_t>();
        break;
    case ActionKind::SetRegionFlag:
        a.flag = readString(row, "flag", pointer);
        if (row.contains("set"))
        {
            a.set = row["set"].get<bool>();
        }
        break;
    case ActionKind::PriceBias:
        a.good = readString(row, "good", pointer);
        a.pct  = readU32(row, "pct", pointer);
        if (!row.contains("days") || !row["days"].is_number_unsigned())
        {
            throwRow(pointer, "PriceBias missing unsigned 'days'");
        }
        a.days = row["days"].get<std::uint32_t>();
        break;
    case ActionKind::WeatherBias:
        a.state = readString(row, "state", pointer);
        if (!row.contains("days") || !row["days"].is_number_unsigned())
        {
            throwRow(pointer, "WeatherBias missing unsigned 'days'");
        }
        a.days = row["days"].get<std::uint32_t>();
        break;
    case ActionKind::SpawnNestedEvent:
        a.templateKey = readString(row, "templateKey", pointer);
        break;
    case ActionKind::GrantRewardTier:
        if (!row.contains("tier") || !row["tier"].is_number_unsigned())
        {
            throwRow(pointer, "GrantRewardTier missing unsigned 'tier'");
        }
        a.tier = row["tier"].get<std::uint32_t>();
        break;
    case ActionKind::EvacuateTerritory:
        break; // the manhunt's evacuation needs no payload (the event's region)
    case ActionKind::EndEvent:
        if (row.contains("success"))
        {
            a.success = row["success"].get<bool>();
        }
        break;
    case ActionKind::DespawnTargets:
        break; // B.6's transient cleanup needs no payload
    case ActionKind::GarrisonTerritory:
        // 5.5's reconquest exit: the role the participants garrison into is
        // data (the roles.json key — "guard" in the shipped template).
        a.roleKey = readString(row, "roleKey", pointer);
        break;
    }
    return a;
}

void parseActionList(
    const nlohmann::json &row, const char *field, const std::string &pointer, std::vector<Action> &out
)
{
    if (!row.contains(field))
    {
        return; // optional
    }
    const nlohmann::json &list = row[field];
    if (!list.is_array())
    {
        throwRow(pointer + "/" + field, "must be an array of actions");
    }
    std::size_t i = 0;
    for (const auto &entry : list)
    {
        out.push_back(parseAction(entry, pointer + "/" + field + "/" + std::to_string(i++)));
    }
}

TemplateState parseState(
    const nlohmann::json &row, const std::string &pointer
)
{
    TemplateState s;
    if (!row.contains("stateId") || !row["stateId"].is_number_unsigned())
    {
        throwRow(pointer, "state missing unsigned 'stateId'");
    }
    s.stateId = row["stateId"].get<std::uint64_t>();

    parseActionList(row, "entryActions", pointer, s.entryActions);
    parseActionList(row, "exitActions", pointer, s.exitActions);

    if (row.contains("objectives"))
    {
        const nlohmann::json &list = row["objectives"];
        if (!list.is_array())
        {
            throwRow(pointer + "/objectives", "must be an array");
        }
        std::size_t i = 0;
        for (const auto &entry : list)
        {
            const std::string objPointer = pointer + "/objectives/" + std::to_string(i++);
            Objective o;
            if (!entry.contains("objectiveId") || !entry["objectiveId"].is_number_unsigned())
            {
                throwRow(objPointer, "objective missing unsigned 'objectiveId'");
            }
            o.objectiveId        = entry["objectiveId"].get<std::uint32_t>();
            const std::string at = readString(entry, "actionType", objPointer);
            const auto type      = actionTypeFromKey(at);
            if (!type)
            {
                throwRow(objPointer + "/actionType", "unknown action type '" + at + "'");
            }
            o.actionType = *type;
            o.cap        = readU32(entry, "cap", objPointer);
            s.objectives.push_back(o);
        }
    }

    if (row.contains("transitions"))
    {
        const nlohmann::json &list = row["transitions"];
        if (!list.is_array())
        {
            throwRow(pointer + "/transitions", "must be an array");
        }
        std::size_t i = 0;
        for (const auto &entry : list)
        {
            const std::string trPointer = pointer + "/transitions/" + std::to_string(i++);
            Transition t;
            if (!entry.contains("to") || !entry["to"].is_number_unsigned())
            {
                throwRow(trPointer, "transition missing unsigned 'to'");
            }
            t.to = entry["to"].get<std::uint64_t>();
            if (!entry.contains("conditions") || !entry["conditions"].is_array())
            {
                throwRow(trPointer, "transition missing 'conditions' array");
            }
            std::size_t j = 0;
            for (const auto &cond : entry["conditions"])
            {
                t.conditions.push_back(parseCondition(cond, trPointer + "/conditions/" + std::to_string(j++)));
            }
            if (entry.contains("onFire"))
            {
                t.hasOnFire = true;
                t.onFire    = parseAction(entry["onFire"], trPointer + "/onFire");
            }
            s.transitions.push_back(t);
        }
    }
    return s;
}

void parseWeightMap(
    const nlohmann::json &row,
    const char *field,
    const std::string &pointer,
    std::array<std::uint32_t, static_cast<std::size_t>(ActionType::COUNT)> &out
)
{
    if (!row.contains(field))
    {
        return; // optional — the zero default stands
    }
    const nlohmann::json &map = row[field];
    if (!map.is_object())
    {
        throwRow(pointer + "/" + field, "must be an object keyed by ActionType");
    }
    for (auto it = map.begin(); it != map.end(); ++it)
    {
        const auto type = actionTypeFromKey(it.key());
        if (!type)
        {
            throwRow(pointer + "/" + field, "unknown action type '" + it.key() + "'");
        }
        if (!it.value().is_number_unsigned())
        {
            throwRow(pointer + "/" + field + "/" + it.key(), "must be unsigned");
        }
        const std::uint64_t value = it.value().get<std::uint64_t>();
        if (value > SCALE)
        {
            throwRow(pointer + "/" + field + "/" + it.key(), "value out of the 0..SCALE range");
        }
        out[static_cast<std::size_t>(*type)] = static_cast<std::uint32_t>(value);
    }
}

} // namespace

// ---- the table -----------------------------------------------------------------

const TemplateState *EventTemplate::state(
    std::uint64_t stateId
) const
{
    for (const TemplateState &s : states)
        if (s.stateId == stateId)
            return &s;
    return nullptr;
}

void EventTemplateTable::insert(
    EventTemplate tpl
)
{
    const std::uint64_t id = tpl.id;
    keyToId_[tpl.key]      = id;
    byId_[id]              = std::move(tpl);
}

const EventTemplate *EventTemplateTable::byId(
    std::uint64_t id
) const
{
    const auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : &it->second;
}

const EventTemplate *EventTemplateTable::byKey(
    const std::string &key
) const
{
    const auto it = keyToId_.find(key);
    return it == keyToId_.end() ? nullptr : &byId_.at(it->second);
}

// ---- the validator -------------------------------------------------------------

void validateTemplate(
    const EventTemplate &tpl, std::vector<std::string> &problems, std::vector<std::string> &warnings
)
{
    if (tpl.states.empty())
    {
        problems.push_back("template '" + tpl.key + "' has no states");
        return;
    }
    // Duplicate state ids are a graph corruption (the state() lookup answers
    // the first; the author meant two distinct states).
    for (std::size_t i = 0; i < tpl.states.size(); ++i)
    {
        for (std::size_t j = i + 1; j < tpl.states.size(); ++j)
        {
            if (tpl.states[i].stateId == tpl.states[j].stateId)
            {
                problems.push_back(
                    "template '" + tpl.key + "' declares state id " + std::to_string(tpl.states[i].stateId) + " twice"
                );
            }
        }
    }
    if (tpl.state(tpl.initialStateId) == nullptr)
    {
        problems.push_back(
            "template '" + tpl.key + "' initial state " + std::to_string(tpl.initialStateId) + " does not exist"
        );
    }
    // Every transition's target must exist.
    for (const TemplateState &s : tpl.states)
    {
        for (const Transition &t : s.transitions)
        {
            if (tpl.state(t.to) == nullptr)
            {
                problems.push_back(
                    "template '" + tpl.key + "' state " + std::to_string(s.stateId) + " transitions to unknown state "
                    + std::to_string(t.to)
                );
            }
        }
    }

    // The world-resolves-without-players rule (5.1's guarantee): every state
    // with transitions must carry a player-independent exit — a
    // TimerCondition fallback (the doc's own mechanism) — or the template
    // declares maxDurationDays and the engine's forced exit resolves it.
    for (const TemplateState &s : tpl.states)
    {
        if (s.transitions.empty())
        {
            continue; // terminal state: the maxDuration rule owns the exit
        }
        bool timerReachable = false;
        for (const Transition &t : s.transitions)
        {
            for (const Condition &c : t.conditions)
            {
                if (c.kind == ConditionKind::Timer)
                {
                    timerReachable = true;
                    break;
                }
            }
            if (timerReachable)
            {
                break;
            }
        }
        if (!timerReachable && tpl.maxDurationDays == 0)
        {
            problems.push_back(
                "template '" + tpl.key + "' state " + std::to_string(s.stateId)
                + " has no non-player-reachable exit (no TimerCondition fallback and no maxDurationDays)"
            );
        }
    }

    // The rewardless-template silence / populated-table warning: a template
    // WITH a populated rewardTable whose success-exit state never fires a
    // DistributeRewards (5.4's payout transition) is a probable authoring
    // bug. v1's templates fire rewards through 5.4's payout transitions;
    // here the check flags a populated table on a template whose every
    // success path is the engine's forced exit (no reward-carrying state).
    if (tpl.hasRewardTable && !tpl.rewards.allZero())
    {
        bool anySuccessState = false;
        for (const TemplateState &s : tpl.states)
        {
            for (const Action &a : s.entryActions)
            {
                if (a.kind == ActionKind::EndEvent && a.success)
                {
                    anySuccessState = true;
                }
            }
            for (const Transition &t : s.transitions)
            {
                if (t.hasOnFire && t.onFire.kind == ActionKind::EndEvent && t.onFire.success)
                {
                    anySuccessState = true;
                }
            }
            for (const Action &a : s.exitActions)
            {
                if (a.kind == ActionKind::EndEvent && a.success)
                {
                    anySuccessState = true;
                }
            }
        }
        if (!anySuccessState)
        {
            warnings.push_back(
                "template '" + tpl.key + "' carries a populated rewardTable but no success-exit state fires it"
            );
        }
    }
}

// ---- the loader ----------------------------------------------------------------

bool loadEventTemplates(
    const std::string &path, EventTemplateTable &out, std::vector<data::DataError> &errors
)
{
    const auto content = readFile(path);
    if (!content)
    {
        errors.push_back({path, "", "file not found"});
        return false;
    }
    nlohmann::json doc;
    try
    {
        doc = nlohmann::json::parse(*content);
    } catch (const nlohmann::json::parse_error &e)
    {
        errors.push_back({path, "", std::string("parse error: ") + e.what()});
        return false;
    }
    if (!doc.is_array())
    {
        errors.push_back({path, "", "event_templates.json must be an array of templates"});
        return false;
    }

    EventTemplateTable staged;
    std::size_t rowIndex = 0;
    for (const auto &row : doc)
    {
        const std::string pointer = "/" + std::to_string(rowIndex++);
        try
        {
            EventTemplate tpl;
            tpl.id                  = readU32(row, "id", pointer);
            tpl.key                 = readString(row, "key", pointer);
            const std::string scale = readString(row, "scale", pointer);
            if (scale != "global" && scale != "regional")
            {
                throwRow(pointer + "/scale", "must be \"global\" or \"regional\"");
            }
            tpl.global = scale == "global";
            if (!row.contains("initialStateId") || !row["initialStateId"].is_number_unsigned())
            {
                throwRow(pointer, "missing unsigned 'initialStateId'");
            }
            tpl.initialStateId     = row["initialStateId"].get<std::uint64_t>();
            tpl.initialDifficulty  = readU32(row, "initialDifficulty", pointer);
            tpl.initialSuccessRate = readU32(row, "initialSuccessRate", pointer);

            if (!row.contains("states") || !row["states"].is_array() || row["states"].empty())
            {
                throwRow(pointer + "/states", "must be a non-empty array of states");
            }
            std::size_t i = 0;
            for (const auto &stateRow : row["states"])
            {
                tpl.states.push_back(parseState(stateRow, pointer + "/states/" + std::to_string(i++)));
            }

            parseWeightMap(row, "actionCap", pointer, tpl.actionCap);
            parseWeightMap(row, "actionWeight", pointer, tpl.actionWeight);

            if (row.contains("rewardTable"))
            {
                const nlohmann::json &rt = row["rewardTable"];
                if (!rt.is_object())
                {
                    throwRow(pointer + "/rewardTable", "must be an object");
                }
                tpl.hasRewardTable = true;
                if (rt.contains("xpPerContribution") && rt["xpPerContribution"].is_number_unsigned())
                {
                    tpl.rewards.xpPerContribution = rt["xpPerContribution"].get<std::uint64_t>();
                }
                if (rt.contains("currencyPerContribution") && rt["currencyPerContribution"].is_number_unsigned())
                {
                    tpl.rewards.currencyPerContribution = rt["currencyPerContribution"].get<std::uint64_t>();
                }
                if (rt.contains("items") && rt["items"].is_array())
                {
                    for (const auto &item : rt["items"])
                    {
                        if (item.is_string())
                        {
                            tpl.rewards.items.push_back(item.get<std::string>());
                        }
                    }
                }
            }

            if (!row.contains("maxDurationDays") || !row["maxDurationDays"].is_number_unsigned())
            {
                throwRow(pointer, "missing unsigned 'maxDurationDays' (the world-must-resolve guarantee)");
            }
            tpl.maxDurationDays = row["maxDurationDays"].get<std::uint32_t>();

            if (row.contains("weatherAffinity") && row["weatherAffinity"].is_object())
            {
                for (auto it = row["weatherAffinity"].begin(); it != row["weatherAffinity"].end(); ++it)
                {
                    if (it.value().is_number_unsigned())
                    {
                        tpl.weatherAffinity[it.key()] = it.value().get<std::uint32_t>();
                    }
                }
            }

            if (row.contains("npcJoinPolicy") && row["npcJoinPolicy"].is_object())
            {
                const nlohmann::json &policy = row["npcJoinPolicy"];
                if (policy.contains("roles") && policy["roles"].is_array())
                {
                    for (const auto &role : policy["roles"])
                    {
                        if (role.is_string())
                        {
                            tpl.npcJoinPolicy.roles.push_back(role.get<std::string>());
                        }
                    }
                }
                if (policy.contains("speciesBias") && policy["speciesBias"].is_object())
                {
                    for (auto it = policy["speciesBias"].begin(); it != policy["speciesBias"].end(); ++it)
                    {
                        if (it.value().is_number_unsigned())
                        {
                            tpl.npcJoinPolicy.speciesBias[it.key()] = it.value().get<std::uint32_t>();
                        }
                    }
                }
            }

            if (row.contains("special") && row["special"].is_object())
            {
                const nlohmann::json &special = row["special"];
                if (special.contains("targetFromWarrant"))
                {
                    tpl.specialTargetFromWarrant = special["targetFromWarrant"].get<bool>();
                }
                if (special.contains("culpritEarnsNothing"))
                {
                    tpl.specialCulpritEarnsNothing = special["culpritEarnsNothing"].get<bool>();
                }
                if (special.contains("dualTargetGroups"))
                {
                    tpl.specialDualTargetGroups = special["dualTargetGroups"].get<bool>(); // D-WAR-EVT-1 (6.2)
                }
            }

            std::vector<std::string> problems;
            std::vector<std::string> warnings;
            validateTemplate(tpl, problems, warnings);
            if (!problems.empty())
            {
                throwRow(pointer, problems.front());
            }
            // Warnings are authoring-bug notices, not load failures — the
            // registry logs them at commit; the template still loads.
            (void)warnings;

            staged.insert(std::move(tpl));
        } catch (const std::runtime_error &e)
        {
            errors.push_back({path, pointer, e.what()});
        }
    }

    if (!errors.empty())
    {
        return false;
    }
    out = std::move(staged);
    return true;
}

} // namespace harrowmere::events
