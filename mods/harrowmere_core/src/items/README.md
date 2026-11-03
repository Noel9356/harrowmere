# src/items — bags, gear, crafting (Phase 2, task 2.4)

The item layer over `data/items.json` + `data/recipes.json`: the bag record, the item-instance id space, the doc-verbatim gear/power score math, and the craft action. No item state lives on the doc `Character` beyond `equippedItems` (instance ids — the doc field that makes `getGearScore` well-defined).

| File | Owns |
|---|---|
| `inventory.hpp/.cpp` | `Inventory` (stacks + equipped mirror + loose instances), `ItemInstanceRegistry` (the `itemIdCounter` 7.1 serializes), `InventoryStore` (the player-side bag store; NPC bags join `NpcState` in 3.4 with the same record), the stack/instance helpers |
| `gear.hpp/.cpp` | `getItemScore` / `getGearScore` (A-17..A-19, design reference verbatim), `equip` / `unequip` (fires `markPowerDirty`), `consumeItem` (heal clamped to maxHp; XP through `applyXpDelta` + `totalXp +=`; fires `markPowerDirty`), the guard-gear entitlement read + `onGuardGearSold` theft hook |
| `crafting.hpp/.cpp` | `RecipeData` + `RecipeTable` (loads + cross-references `recipes.json`: every input/output key must resolve to an items.json row), `craft` (reqSkill gate → input consume → output spawn → craft counter) |

## Wiring points (when their owners land)

- **3.3 / loot**: corpse loot spawns instances through `ItemInstanceRegistry::spawn`; the acquire path calls the NpcState `holdsGuardGear` flag (3.4).
- **3.7 / vendor sell**: the sell path calls `onGuardGearSold` per guardMarked stack — the hook calls into the `JusticeSink` (`combat/justice_sink.hpp`, D-SINK-1; null default until 4.1).
- **7.1 / saves**: the instance counter + instance map + bags serialize; `adoptCounter`/`restore` are the load path's merge points.
- **7.3 / Inventory panel**: reads the bag through the capability views (step-13 snapshot).

## Rules this layer keeps

- Equipping has **no stat requirements** in v1 (none in the docs).
- Consumption never bypasses `applyXpDelta` (R4); heals clamp at `maxHp`.
- Crafting grants **no XP** (the docs' only XP sources: kills, consumed items) and never dirties power (bag-only change; 2.5's audited list does not name crafting).
- Craft counters increment through `PlayerActions::recordCraft` (player) / `NpcState::actionCounters` (3.4) — 2.3's `reqActions` inputs.
