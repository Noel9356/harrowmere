# quests/ — the quest layer (task 6.4)

Quests are thin wrappers over the event engine — the doc's structure with no parallel mechanic anywhere: **a quest IS a regional event instance** of its bound template (`npcJoinPolicy = [player]`), running on the SAME contribution math (5.3), the SAME payout (5.4's difficulty-scaled pool) and the SAME no-time-exclusive rule. There is no quest-specific payout path in this tree — the reviewer's grep for one finds nothing.

The three doc types map onto the event vocabulary:

| Type | Mechanism |
|---|---|
| gathering | the `object_delivered` objective — `deliverQuestObjective` records validated OBJECTIVE amounts; the ActionThreshold transition completes the stage |
| story | chained templates — the exit actions carry `SpawnNestedEvent` before the `EndEvent` (the engine's as-built transition sequence skips the onFire once the exits end the event), the next stage joins through `EventStateCondition` reads |
| event | a tie-in on a published world event — the quest's own instance completes when the bound world event (`eventStateBinding`) reaches its bound state |

| File | Task | What lives here |
|---|---|---|
| `quests.{hpp,cpp}` | 6.4 | the `QuestDef` rows + the strict `quests.json` loader (the 0.4 door), the content-checklist validator (warnings, never fatal), `startQuest` and `deliverQuestObjective` |

## The content checklist (the doc's busywork rule, made procedural)

`questChecklistWarnings` appends one warning per violation — editorial, never fatal: (a) at least one objective that is NOT damage-type, (b) a narrative description of at least 200 characters, (c) a non-violent resolution path (a success transition whose conditions need no DAMAGE threshold), (d) rewards that follow 5.4's pool (structurally true; the warning names the authoring bug of a quest whose bound template pays nothing). The plugin runs it at boot and logs each warning; the shipped quests (`data/quests.json` — two gathering, one story chain, one event-tie-in) warn nothing.

## As-built notes

* **The quest template's `actionCap[OBJECTIVE]` is the quest's own work total** (the objective's cap): 5.3's `getContribution` normalizes the accumulated amount against the action cap — a cap of 10000 with a cap-3 objective would price three deliveries as 3 contribution points. The shipped templates carry cap = the objective cap; DEVIATIONS.md carries the row.
* **The giver dialog is Phase 4's** (the card's own Parallel row): the rows load loud at boot (unknown bound template = a startup error), the quest layer is callable headless (the pinned tests play all four), and the claim surface arrives with the claim dialog.
* The giver may front a small advance from their own purse at the claim site (a 3.7 transaction) — never from the reward pool.
