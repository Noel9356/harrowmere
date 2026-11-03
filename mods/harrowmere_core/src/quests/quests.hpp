// quests.hpp — task 6.4: quests as thin wrappers over the event engine.
//
// The doc's structure ("There is no quest log spam of 'slay 10 boars'; quests
// are built from the same event templates as world events"): a quest IS a
// regional event instance — `npcJoinPolicy = [player]` and quest-specific
// entry actions — running on the SAME contribution math (5.3), the SAME
// payout (5.4's difficulty-scaled pool, never a hand-pinned one) and the
// SAME no-time-exclusive rule. There is no quest-specific payout path: a
// quest reward rides distributeRewards like every event reward (the card
// pitfall; the giver may front a small advance from their own purse — a 3.7
// transaction, never from the reward pool).
//
// The three doc types map to the event vocabulary:
//   * gathering — the `object_delivered` objective: the delivery verb
//     (deliverQuestObjective) records validated OBJECTIVE amounts; the
//     ActionThreshold transition fires when the cap is met.
//   * story — chained templates: an onFire SpawnNestedEvent starts the next
//     stage; EventStateCondition reads another live event's state.
//   * event — a tie-in on a published world event: the quest's own instance
//     completes when the bound world event (eventStateBinding) reaches its
//     bound state.
//
// quests.json rows (D-DATA-1, the strict 0.4 door):
//   { "id", "key", "giverRole", "territoryScope", "eventTemplateKey",
//     "eventStateBinding"? : { "templateKey", "stateId" },
//     "description", "repeatable" }
// The row is the AUTHORING view; the bound template is the mechanical truth
// (the tracking UI reads EventsView, §4.2's view — the quest's event shows
// up there like any world event).
//
// The content checklist (the doc's design rule made procedural — a content
// PR's checklist, encoded as template-validator-style WARNINGS, never
// fatal): every quest template must (a) declare at least one objective that
// is NOT damage-type, (b) carry a narrative description of at least 200
// characters, (c) offer a non-violent resolution path (a success transition
// whose conditions need no DAMAGE threshold — the Timer/objective fallback
// family), (d) scale rewards via 5.4's difficulty mapping (structurally
// true: the bound template's perContribution rates are the only pool; a
// row without a paying bound template is the authoring bug the warning
// names). questChecklistWarnings appends one warning per violation; the
// rule is editorial — the loader loads, the warning lists.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/types.hpp" // characterId, regionId

namespace harrowmere::data
{
struct DataError; // data/json.hpp — the canonical error row
}

namespace harrowmere::events
{
class EventEngine;         // events/engine.hpp — pointer only here
struct EventTemplate;      // events/template.hpp — the complete type below
struct EventTemplateTable; // events/template.hpp — pointer only here
} // namespace harrowmere::events

namespace harrowmere
{
struct WorldContext; // core/world_context.hpp — reference only here
}

namespace harrowmere::quests
{

// One quest row (quests.json), the authoring view over its bound template.
struct QuestDef
{
    std::uint32_t id = 0;
    std::string key;
    std::string giverRole;            // roles.json key of the giver NPC (D-SP-1: givers are NPCs)
    std::string territoryScope;       // "" = anywhere; else the territory key the quest is offered in
    std::string eventTemplateKey;     // the quest's own event template (the instance 6.4 starts)
    std::string bindingTemplateKey;   // event-tie-in: the world event the quest watches ("" = none)
    std::uint64_t bindingStateId = 0; // event-tie-in: the watched event's completion state
    std::string description;          // the narrative (checklist (b): >= 200 chars, named places)
    bool repeatable = false;
};

// The registry: by id + the key index (R7's deterministic orders).
class QuestTable
{
public:
    void insert(QuestDef quest); // replaces by id; the key index follows

    const QuestDef *byId(std::uint32_t id) const;
    const QuestDef *byKey(const std::string &key) const;
    const std::map<std::uint32_t, QuestDef> &rows() const
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
    std::map<std::uint32_t, QuestDef> byId_;
    std::map<std::string, std::uint32_t> keyToId_;
};

// loadQuests — the strict 0.4 door: schema-checked rows (the card's field
// list; an unknown template key is NOT checked here — the bound template
// lives in the event-templates stage and the cross-check is the caller's
// boot order). On any error the loader appends DataError rows (file + JSON
// pointer + reason) and returns false; `out` is left partial and MUST NOT
// be consumed.
bool loadQuests(const std::string &path, QuestTable &out, std::vector<data::DataError> &errors);

// ---- the content checklist (the validator warnings; editorial, not fatal) ----

// questChecklistWarnings — the doc's design rule made procedural: one
// warning per violation, the quest's key prefixed. `tpl` is the bound
// template (nullptr = the row binds nothing — warning (d) fires alone).
void questChecklistWarnings(
    const QuestDef &quest, const events::EventTemplate *tpl, std::vector<std::string> &warnings
);

// ---- the play verbs -----------------------------------------------------------

// startQuest — start the quest's event instance: the regional event the
// engine builds from the bound template (the same scale-uniqueness, depth
// and validator rules as any world event). Returns the event id (0 =
// refused: unknown quest or template, or the engine refused the start).
// The giver may front a small advance from their own purse at the claim
// site (a 3.7 transaction; Phase 4's claim dialog) — never from the pool.
std::uint64_t startQuest(
    events::EventEngine &engine,
    const QuestTable &quests,
    const events::EventTemplateTable &templates,
    const std::string &questKey,
    regionId region,
    WorldContext &ctx
);

// deliverQuestObjective — the gathering verb: the actor delivers `amount`
// progress units of the quest's `object_delivered` objective (items already
// left their bag by the caller's trade/interact path — this is the event
// bookkeeping). The amount validates through 5.3's recordAction (the
// OBJECTIVE clamp: only the remaining budget accrues — over-delivery is a
// no-op, never a negative). Returns the validated amount.
std::uint32_t deliverQuestObjective(
    events::EventEngine &engine,
    const events::EventTemplateTable &templates,
    std::uint64_t eventId,
    characterId actor,
    std::uint32_t amount,
    WorldContext &ctx
);

} // namespace harrowmere::quests
