#ifndef HARROWMERE_CORE_EVENTS_TEMPLATE_HPP
#define HARROWMERE_CORE_EVENTS_TEMPLATE_HPP

// template.hpp — task 5.1's data half: the event-template records, the
// event_templates.json loader, and the template validator (1.3's cross-ref
// stage — "event templates (5.1)" is the pinned load order's tail).
//
// The template schema (Appendix B, normative):
//   { "id", "key", "scale": "global"|"regional", "initialStateId",
//     "initialDifficulty", "initialSuccessRate",
//     "states": [ { "stateId", "entryActions", "exitActions",
//                   "objectives": [ {objectiveId, cap, actionType} ],
//                   "transitions": [ {to, conditions, onFire?} ] } ],
//     "actionCap": {...per ActionType}, "actionWeight": {...},
//     "rewardTable"?, "maxDurationDays", "weatherAffinity", "npcJoinPolicy",
//     "special"? }
//
// The v1 vocabularies are the card's pinned sets — with the one Appendix-B
// extension the shipped templates need (DEVIATIONS.md carries the row):
//   * conditions: Random, Flag, ActionThreshold, EventState, Timer,
//     MinParticipants (the pinned six) + TargetDead/TargetCaptured (B.6's
//     MANHUNT transitions; 5.4 owns their firing semantics — the engine
//     evaluates them against the event's target set).
//   * actions: the pinned eight (SpawnTargets, SetRegionFlag, PriceBias,
//     WeatherBias, SpawnNestedEvent, GrantRewardTier, EvacuateTerritory,
//     EndEvent) + DespawnTargets (B.6's raid cleanup — the transient
//     counterpart of SpawnTargets, per the B.6 note) + GarrisonTerritory
//     (5.5's reconquest exit — the GARRISON_RETURN template's re-garrison
//     action assigns guard roles to the event's NPC participants and
//     clears the territory's lawless flag; DEVIATIONS.md carries the row).
//
// Rewards are per template, OPTIONAL: a template without a rewardTable (or
// with an all-zero one) pays nothing at any payout point — payout is a
// no-op, not an error (5.4 reads the table; the loader only records it).
//
// Determinism: every container is std::map/std::vector in declaration order
// (R7); the loader validates acts/masks against the closed enums (1.1) and
// every state graph against the world-resolves-without-players rule.

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/enums.hpp" // ActionType (1.1's closed list)

namespace harrowmere::data
{
struct DataError; // data/json.hpp — the canonical error row
}

namespace harrowmere::events
{

// ---- conditions (the doc's vocabulary: "random, flags, NPC/actor actions,
// and other active states") ------------------------------------------------------

enum class ConditionKind : std::uint8_t
{
    Random,          // {pct} — the "events" stream roll
    Flag,            // {regionFlag, set} — 1.4's region flags
    ActionThreshold, // {actionType, total} — sum of participant actionAmounts
    EventState,      // {templateKey, stateId} — another live event's state
    Timer,           // {days} — time in the current state
    MinParticipants, // {n}
    TargetDead,      // B.6's MANHUNT: every target id dead (5.4's semantics)
    TargetCaptured   // B.6's MANHUNT: the target captured (5.4's semantics)
};

struct Condition
{
    ConditionKind kind = ConditionKind::Random;
    std::uint32_t pct  = 0;                     // Random
    std::string regionFlag;                     // Flag
    bool set              = true;               // Flag
    ActionType actionType = ActionType::DAMAGE; // ActionThreshold
    std::uint32_t total   = 0;                  // ActionThreshold
    std::string templateKey;                    // EventState
    std::uint64_t stateId = 0;                  // EventState
    std::uint32_t days    = 0;                  // Timer
    std::uint32_t n       = 0;                  // MinParticipants
};

// ---- actions (D-ACTION-1's pinned v1 set + B.6's DespawnTargets) ---------------

enum class ActionKind : std::uint8_t
{
    SpawnTargets,      // {speciesKey, roleKey, count, at?} — spawn the event's targets
    SetRegionFlag,     // {flag, set} — 1.4's persistent region flags (D-FLAGS-1)
    PriceBias,         // {good, pct, days} — 3.7's hook
    WeatherBias,       // {state, days} — 3.8's hook
    SpawnNestedEvent,  // {templateKey} — depth-checked
    GrantRewardTier,   // {tier} — 5.4's payout marker
    EvacuateTerritory, // {} — the manhunt's civilian evacuation (4.6's mechanic)
    EndEvent,          // {success}
    DespawnTargets,    // B.6 — remove the spawned targets (transient, D-FLAGS-1's split)
    GarrisonTerritory  // 5.5 — {roleKey} the reconquest exit: guard roles + clearLawless
};

struct Action
{
    ActionKind kind = ActionKind::SpawnTargets;
    std::string speciesKey;     // SpawnTargets
    std::string roleKey;        // SpawnTargets
    std::uint32_t count = 0;    // SpawnTargets
    std::string flag;           // SetRegionFlag
    bool set = true;            // SetRegionFlag
    std::string good;           // PriceBias
    std::uint32_t pct  = 0;     // PriceBias
    std::uint32_t days = 0;     // PriceBias/WeatherBias
    std::string state;          // WeatherBias (a weatherKey: clear/rain/storm/fog/snow)
    std::string templateKey;    // SpawnNestedEvent
    std::uint32_t tier = 0;     // GrantRewardTier
    bool success       = false; // EndEvent
};

// ---- the template -------------------------------------------------------------

struct Objective
{
    std::uint32_t objectiveId = 0;
    ActionType actionType     = ActionType::DAMAGE;
    std::uint32_t cap         = 0; // 0 = uncapped (A-104's default clamp applies at 5.3)
};

struct Transition
{
    std::uint64_t to = 0;
    std::vector<Condition> conditions; // ANY-of: one firing condition fires the transition
                                       // (B.6's TimerCondition note pins the reading)
    bool hasOnFire = false;
    Action onFire{};
};

struct TemplateState
{
    std::uint64_t stateId = 0;
    std::vector<Action> entryActions;
    std::vector<Action> exitActions;
    std::vector<Objective> objectives;
    std::vector<Transition> transitions; // evaluated in DECLARED order (determinism)
};

// The optional per-template reward table (5.4 pays it; a missing or all-zero
// table pays nothing — payout is a no-op, not an error).
struct RewardTable
{
    std::uint64_t xpPerContribution       = 0;
    std::uint64_t currencyPerContribution = 0;
    std::vector<std::string> items;

    bool allZero() const noexcept
    {
        return xpPerContribution == 0 && currencyPerContribution == 0 && items.empty();
    }
};

// npcJoinPolicy — role/species affinity weights so NPC actors join regional
// events by AI (5.2's difficulty loop accrues their statistical amounts;
// 5.1 only records the policy).
struct NpcJoinPolicy
{
    std::vector<std::string> roles;
    std::map<std::string, std::uint32_t> speciesBias; // speciesKey -> weight
};

struct EventTemplate
{
    std::uint64_t id = 0;
    std::string key;
    bool global                      = false; // scale: true = global, false = regional (regionId-bound)
    std::uint64_t initialStateId     = 0;
    std::uint32_t initialDifficulty  = 0; // 0..SCALE
    std::uint32_t initialSuccessRate = 0; // 0..SCALE
    std::vector<TemplateState> states;
    std::array<std::uint32_t, static_cast<std::size_t>(ActionType::COUNT)> actionCap{};
    std::array<std::uint32_t, static_cast<std::size_t>(ActionType::COUNT)> actionWeight{};
    bool hasRewardTable = false;
    RewardTable rewards{};
    std::uint32_t maxDurationDays = 0;
    std::map<std::string, std::uint32_t> weatherAffinity; // weatherKey -> weight (3.8's names)
    NpcJoinPolicy npcJoinPolicy;

    // B.6's `special` block (5.4's manhunt wiring reads it; the engine
    // applies targetFromWarrant at spawn time — the culprit enters
    // targetIds — and records culpritEarnsNothing as the reward exclusion).
    // 6.2's D-WAR-EVT-1 extension: dualTargetGroups — the template declares
    // two target groups; the war spawn joins each side's members as the
    // other side's targets (the engine books the two groups; DEVIATIONS.md
    // carries the row and src/factions/README.md documents the shape).
    bool specialTargetFromWarrant   = false;
    bool specialCulpritEarnsNothing = false;
    bool specialDualTargetGroups    = false;

    const TemplateState *state(std::uint64_t stateId) const;
};

// The registry: by id (deterministic order) + the key index. The DataRegistry
// loads it as the pinned order's tail stage; tests insert rows directly (the
// plain-struct seam the engine tests use).
class EventTemplateTable
{
public:
    void insert(EventTemplate tpl); // replaces by id; the key index follows

    const EventTemplate *byId(std::uint64_t id) const;
    const EventTemplate *byKey(const std::string &key) const;

    const std::map<std::uint64_t, EventTemplate> &all() const
    {
        return byId_;
    }
    std::size_t size() const
    {
        return byId_.size();
    }
    void clear()
    {
        byId_.clear();
        keyToId_.clear();
    }

private:
    std::map<std::uint64_t, EventTemplate> byId_;
    std::map<std::string, std::uint64_t> keyToId_;
};

// loadEventTemplates — the B.6 schema, strict (0.4's door): every act/mask
// tag validates against the closed enums, every SCALE value stays in range,
// every state graph validates (validateTemplate). On any error the loader
// appends DataError rows (file + JSON pointer + reason) and returns false;
// `out` is left partial and MUST NOT be consumed.
bool loadEventTemplates(const std::string &path, EventTemplateTable &out, std::vector<data::DataError> &errors);

// validateTemplate — the 1.3 cross-ref validator. Returns the fatal problems
// (empty = valid): unknown state references, duplicate state ids, and the
// world-resolves-without-players rule — every state's transitions must be
// reachable without player action: a TimerCondition fallback transition, or
// (the engine's guarantee) a declared maxDurationDays forcing the exit.
// `warnings` carries the non-fatal authoring bugs (a populated rewardTable a
// success-exit never fires).
void validateTemplate(const EventTemplate &tpl, std::vector<std::string> &problems, std::vector<std::string> &warnings);

} // namespace harrowmere::events

#endif // HARROWMERE_CORE_EVENTS_TEMPLATE_HPP
