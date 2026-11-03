// quests.cpp — task 6.4: the quest layer over the event engine (the header
// carries the contract; this file is the loader, the checklist validator and
// the two play verbs). There is NO quest-specific payout path anywhere in
// this file — the bound template's rewardTable pays through 5.4's
// distributeRewards exactly like a world event's.

#include "quests/quests.hpp"

#include <algorithm>

#include "core/math.hpp"           // clampScalar (the R2 guards)
#include "character/character.hpp" // Character (the delivery's self-action)
#include "core/world_context.hpp"  // WorldContext (the verbs' context)
#include "data/json.hpp"           // the strict 0.4 door (D-DATA-1)
#include "events/contribution.hpp" // recordAction (the validated OBJECTIVE accrual)
#include "events/engine.hpp"       // the engine's startEventByKey + findMutable
#include "events/template.hpp"     // EventTemplate + the table

namespace harrowmere::quests
{

// ---- the registry -------------------------------------------------------------

void QuestTable::insert(
    QuestDef quest
)
{
    const std::uint32_t id = quest.id;
    const std::string key  = quest.key;
    byId_[id]              = std::move(quest);
    keyToId_[key]          = id;
}

const QuestDef *QuestTable::byId(
    std::uint32_t id
) const
{
    const auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : &it->second;
}

const QuestDef *QuestTable::byKey(
    const std::string &key
) const
{
    const auto it = keyToId_.find(key);
    return it == keyToId_.end() ? nullptr : byId(it->second);
}

// ---- the strict loader (the 0.4 door) ------------------------------------------

bool loadQuests(
    const std::string &path, QuestTable &out, std::vector<data::DataError> &errors
)
{
    const auto loaded = data::loadJsonFile(path);
    if (loaded.isErr())
    {
        errors.push_back({path, "", "quests.json failed to parse"});
        return false;
    }
    const data::Json &rows = loaded.value();
    if (!rows.is_array() || rows.empty())
    {
        errors.push_back({path, "", "quests.json must be a non-empty array of quest rows"});
        return false;
    }

    bool ok = true;
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        const data::Json &row     = rows[i];
        const std::string pointer = "/" + std::to_string(i);
        try
        {
            QuestDef quest;
            quest.id               = row.at("id").get<std::uint32_t>();
            quest.key              = row.at("key").get<std::string>();
            quest.giverRole        = row.at("giverRole").get<std::string>();
            quest.territoryScope   = row.value("territoryScope", std::string());
            quest.eventTemplateKey = row.value("eventTemplateKey", std::string());
            if (row.contains("eventStateBinding"))
            {
                const data::Json &binding = row.at("eventStateBinding");
                quest.bindingTemplateKey  = binding.at("templateKey").get<std::string>();
                quest.bindingStateId      = binding.at("stateId").get<std::uint64_t>();
            }
            quest.description = row.value("description", std::string());
            quest.repeatable  = row.value("repeatable", false);

            if (quest.key.empty())
                throw std::runtime_error("row missing 'key'");
            if (quest.eventTemplateKey.empty() && quest.bindingTemplateKey.empty())
                throw std::runtime_error("row needs 'eventTemplateKey' or 'eventStateBinding'");
            out.insert(std::move(quest));
        } catch (const std::exception &e)
        {
            errors.push_back({path, pointer, e.what()});
            ok = false;
        }
    }
    return ok;
}

// ---- the content checklist (warnings; editorial, never fatal) -------------------

void questChecklistWarnings(
    const QuestDef &quest, const events::EventTemplate *tpl, std::vector<std::string> &warnings
)
{
    const std::string tag = "quest '" + quest.key + "': ";
    if (tpl == nullptr)
    {
        // (d): the row binds no template — its rewards (if any were
        // authored) could never pay; the payout path does not exist.
        warnings.push_back(tag + "binds no event template (rewards would never pay through 5.4)");
        return;
    }

    // (a): at least one objective that is not damage-type — "slay 10 boars"
    // alone is the busywork the doc forbids.
    const bool hasNonDamage
        = std::any_of(tpl->states.begin(), tpl->states.end(), [](const events::TemplateState &state) {
              return std::any_of(
                  state.objectives.begin(), state.objectives.end(), [](const events::Objective &objective) {
                      return objective.actionType != ActionType::DAMAGE;
                  }
              );
          });
    if (!hasNonDamage)
    {
        warnings.push_back(tag + "declares no non-damage objective (the doc's busywork rule)");
    }

    // (b): the narrative — a description of at least 200 characters (the
    // named-NPC/territory half of the rule is editorial; the length check is
    // the mechanical guard).
    if (quest.description.size() < 200)
    {
        warnings.push_back(tag + "description shorter than 200 characters (the narrative rule)");
    }

    // (c): a non-violent resolution path — at least one success transition
    // whose conditions need no DAMAGE threshold (the Timer/objective/other-
    // event fallback family; the template validator's own playerless-exit
    // rule guarantees the reachability).
    const bool nonViolentExit
        = std::any_of(tpl->states.begin(), tpl->states.end(), [](const events::TemplateState &state) {
              return std::any_of(state.transitions.begin(), state.transitions.end(), [](const events::Transition &t) {
                  if (t.hasOnFire && t.onFire.kind == events::ActionKind::EndEvent && !t.onFire.success)
                      return false; // a failure exit is not a resolution
                  const bool damageOnly
                      = !t.conditions.empty()
                     && std::all_of(t.conditions.begin(), t.conditions.end(), [](const events::Condition &c) {
                            return c.kind == events::ConditionKind::ActionThreshold
                                && c.actionType == ActionType::DAMAGE;
                        });
                  return !damageOnly;
              });
          });
    if (!nonViolentExit)
    {
        warnings.push_back(tag + "offers no non-violent resolution path (the editorial resolution rule)");
    }

    // (d): the rewards scale via 5.4's difficulty mapping — structurally
    // true (the bound template's perContribution rates are the only pool);
    // the warning names the authoring bug: a quest whose bound template
    // pays nothing while the row advertises rewards.
    if (quest.bindingTemplateKey.empty() && (!tpl->hasRewardTable || tpl->rewards.allZero()))
    {
        warnings.push_back(
            tag + "bound template pays nothing (rewards must follow 5.4's pool, never a hand-pinned one)"
        );
    }
}

// ---- the play verbs --------------------------------------------------------------

std::uint64_t startQuest(
    events::EventEngine &engine,
    const QuestTable &quests,
    const events::EventTemplateTable &templates,
    const std::string &questKey,
    regionId region,
    WorldContext &ctx
)
{
    // The bound template must exist (the cross-check the loader leaves to
    // the boot order); the engine's own gates (scale uniqueness, depth)
    // stand — a quest is an event, refused like one.
    const QuestDef *quest = quests.byKey(questKey);
    if (quest == nullptr || quest->eventTemplateKey.empty())
        return 0;
    if (templates.byKey(quest->eventTemplateKey) == nullptr)
        return 0;
    return engine.startEventByKey(quest->eventTemplateKey, region, 0, nullptr, ctx);
}

std::uint32_t deliverQuestObjective(
    events::EventEngine &engine,
    const events::EventTemplateTable &templates,
    std::uint64_t eventId,
    characterId actor,
    std::uint32_t amount,
    WorldContext &ctx
)
{
    events::Event *ev = engine.findMutable(eventId);
    if (ev == nullptr)
        return 0; // no live instance: nothing accrues
    const events::EventTemplate *tpl = templates.byId(ev->templateId);
    if (tpl == nullptr)
        return 0;
    if (ctx.characters == nullptr || !ctx.characters->exists(actor))
        return 0;
    // The delivery is a participant's verb (the tracking UI joins first;
    // a non-participant's delivery accrues nothing — contribution is
    // participant state, 5.3).
    const auto participantIt = std::lower_bound(ev->participants.begin(), ev->participants.end(), actor);
    if (participantIt == ev->participants.end() || *participantIt != actor)
        return 0;
    if (engine.hasLeftEarly(eventId, actor))
        return 0; // the leave mark refuses later contribution (5.3's rule)

    // The delivery is a self-action against the quest's own books: the
    // OBJECTIVE clamp (getProgressDelta's remaining budget) is the guard —
    // over-delivery is a no-op, never a negative (R1). recordAction is the
    // ONLY path that writes the books (5.3's Done-when).
    const Character &self = ctx.characters->get(actor);
    return events::recordAction(*ev, ActionType::OBJECTIVE, amount, self, self, *tpl, ctx);
}

} // namespace harrowmere::quests
