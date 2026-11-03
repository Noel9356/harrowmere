# character/ — Character records, the store, statuses, species, appearance (tasks 2.0–2.2, 2.6)

The character record layer of `harrowmere_core`: the design reference's `Character` struct **verbatim** (players and NPCs share the structure — design rule: "NPC actors share the same structure"), the one unified store every later system reads and writes characters through, the derived statuses / PvP consent / cooldowns (2.1), and the species application (2.2).

## Contents

| File | Contents |
|---|---|
| `character.hpp` | `Body`, `Progression`, `Character` (doc fields, doc order, doc defaults), `PlayerCreation` (the creation-flow input), `maxHpForLevel` (A-42 + A-43, the single home of the HP formula) |
| `store.hpp` / `store.cpp` | `CharacterStore` — one `std::map<characterId, Character>` + the `playerIds_` set (D-STORE-1), `createPlayer` / `createNpc` / `get` / `isPlayer` / `all` |
| `status.hpp` / `status.cpp` (task 2.1) | `hasStatus` (the four DERIVED statuses, computed from their owning state through the `WorldContext` seams — D-CHAR-1), `setStatus` (non-derived flags only; none exist in v1 — asserts), `consentsToPvp` (the doc's duel/arena consent, verbatim — THE only consent check in the codebase), `cooldownActive` / `setCooldown` (slots hold the expiry worldTime) |
| `species.hpp` / `species.cpp` (task 2.2) | `isSapient`, `getStatEffectiveness` (the doc formula: soft-capped point x species statMultiplier / SCALE, all data-driven), `effectiveLifespan` (A-5's level growth), `speciesBiasToward` (the data-driven bias core 3.9 wraps) |
| `appearance.hpp` / `appearance.cpp` (task 2.6) | `AppearanceHistory` (the per-character change-timestamp store; the A-25 window count and the last-change read 3.9/4.5 use for "disguises defeat strangers"), `appearanceChangeCost` (A-22/A-23), `canChangeAppearance` (cooldown slot + availableXp — the doc's comparison, never currency), `changeAppearance` (pay from XP -> replace components -> arm the A-24 cooldown -> record -> fire the A-25 suspicion seam -> publish AppearanceChanged). Appearance NEVER touches stats/skills/equipment — the power cache is not even dirtied |

## The contracts that live here

* **D-STORE-1** — one unified store; NPC-only state (name, age, role, needs, brain, LOD tier, relationships) lives in `NpcState` (task 3.4, `src/npc/state.hpp`), **never** on `Character`. Jail state lives in justice. The compile-time reflection table in `tests/test_character_store.cpp` pins the negatives.
* **Id continuity** — plan NPCs carry their pre-assigned `NpcSeed::id` (worldgen 1.5, strict plan order); post-plan spawns (births 3.4, backup NPCs 3.6) seed with `id = 0` and the store draws from the counter adopted via `adoptWorldGen(WorldGenResult::nextCharacterId, WorldGenResult::playerSpawn)`. Ids are never reused (R10).
* **Pinned starting state** — player: level 1, stats/skills at the doc floor 0, `totalXp = 0`, `availableXp = 0`, currency 100 (A-41), maxHp by `maxHpForLevel` (A-42/A-43), spawned at the safe-zone spawn point. NPC: the seed's species + level, otherwise the same floor, currency 0 (no NPC purse is pinned; earnings arrive with roles/economy, 3.4/3.7).
* **Recovery posture** (§3.4) — `get()` on an unknown id asserts in debug; release serves the deterministic zero character (the `DataRegistry::species()` pattern). Creation collisions no-op deterministically with a logged warning.

## What does NOT live here (do not build ahead)

Purchases & respec (2.3 — `src/progression/`), items/inventory (2.4), power caching (2.5), appearance changes (2.6), duels (2.7), the `NpcState` record and all lifecycle logic (3.4), and any save/load (7.1 — will mirror this struct verbatim). The two 2.1/2.2 rules this directory pins:

* **statusFlags stays untouched** — no code writes bits for the four derived statuses (grep review; the runtime half is pinned by `TestStatus_DerivedStatusesNeverStored`).
* **No species lists in code** — every species read goes through the DataRegistry's rows (the one sanctioned comparison is the A.2 "species ids" row: `PLAYER_SPECIES_ID` 0 = player).
