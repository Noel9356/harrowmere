# src/world — the world model (task 1.4)

The static world graph: **regions** (spatial cells with flags/season/weather/
mana), the **territory tree** (the legal hierarchy laws/titles/safety attach
to), the explicit **region → territory mapping**, and the three doc walks
implemented exactly once. Worldgen (1.5) populates the graph; everything
downstream (justice, events, war, UI) reads it through here.

| File | Role |
|---|---|
| `region.hpp` | `Region` (design reference §World, fields verbatim) + `SeasonState`/`WeatherState` (owned by task 3.8, declared here) + the `Climate` class drawn at worldgen (1.5) + the **D-WORLD-1 projection**: `regionIdAt`/`regionIdOf`/`regionIndicesInGrid`/`isGridRegion`/`regionCenter`/`distanceSquaredM` — the ONE place world coordinates and region indices meet. |
| `territory.hpp/.cpp` | `Territory` (design reference §Territories, fields verbatim + the card's `region`/`lawless` pins), `CompactRecord` (the archive row 4.1 produces / 4.2 burns), and the `isSovereign`/`isUnclaimed` predicates (design rule, decided once). |
| `world.hpp/.cpp` | `WorldStore`: the graph storage (all `std::map`, R7), the generation surface (`addRegion`, `addTerritory`, `setRegionTerritory`, `clear`), the three doc walks, and the parent/child traversal helpers 4.x/6.x consume. |
| `worldgen.hpp/.cpp` | Task 1.5: `generateWorld(seed, DataRegistry, WorldStore&, NpcSeedOut&)` — the seven pinned generation stages, the `NpcSeed`/`FactionSeed` plan records, the safe-zone spawn derivation, and the FNV-1a summary hash. |
| `seasons.hpp/.cpp` | Task 3.8: the season clock + the weather markov machine that advance `Region`'s `SeasonState`/`WeatherState`, and the four frozen effect hooks (crop yields, migration pull, event weather-affinity, exposure). See the section below. |

## Worldgen (task 1.5)

`generateWorld` builds the entire static world deterministically, in the
card's pinned stage order: regions (climate + the D-LORE-3 mana band
9500..10000) → the territory tree (contiguous 3-5 region city blocks in
snake-ordered quadrants, one region per village, the outermost frontier ring
unclaimed) → laws + the per-kingdom variance roll + the NOBLE/ROYAL title
chain → one safe village per kingdom with its spawn point (the beginner
kingdom's safe village is the player spawn, `WorldGenResult::playerSpawn`) →
five faction seeds (four courts, one frontier free company, 2 rivalries + 1
alliance) → the population plan (`NpcSeed`: species by key, role, age,
integrity, level, home; consumed verbatim by 3.4) → the FNV-1a summary hash
(region flags/climate/mana + tree shape + plan counts; task 8.1 reuses it).

The determinism contract is the file's law: every roll goes through
SeedRng's pinned `worldgen` stream (R6) in the pinned stage/draw order — the
per-NPC order is **species → age → role → integrity**, fixed forever (card
pitfall) — and every container the output can observe is ordered (R7). Two
runs from one seed produce byte-identical output; no wall clock, no doubles
in the hash, no unordered containers anywhere in the file.

Plan-level outputs the graph itself does not carry live in
`WorldGenResult`: the faction seeds (6.1's records), the beginner kingdom +
player spawn, the single forest elven enclave, and `nextCharacterId` — the
character id space after the plan, which the runtime adopts so spawn-path
and player ids continue the same sequence (7.1 persists it; D-WGEN-1: the
outputs are fully persisted, loading a save never re-generates).

## Seasons & weather (task 3.8)

`seasons.hpp/.cpp` advances the `SeasonState`/`WeatherState` the region
carries, and freezes the four effect hooks the design reference names
("seasons and weather are regionally dynamic... crop yields, NPC migration,
event probabilities, and exposure risk"):

| Piece | Contract |
|---|---|
| Season clock | Pure worldTime math (no RNG): `seasonAt(t)` walks the pinned spring→summer→autumn→winter cycle at `SEASON_LENGTH_GAME_SECONDS` (A-51 = 30 game-days); `updateSeasons(store, worldTime)` writes every region's index + `startedAtWorldTime` (the exact A-51 boundary) in ascending id order, dirtying nothing on a no-transition tick. |
| Weather machine | A markov roll per region per game-hour on the pinned `weather` stream (R6/R8), one `pct()` draw per region per roll in ascending regionId order (R7). The cadence IS the worldTime pair of the slice — no roll-counter state to persist (7.1). Card table: clear 60 / rain 25 / storm 10 / fog 5; winter swaps rain→snow wholesale; swamp +fog 2500; mountain +snow 1500 (deltas 3.8-pinned, DEVIATIONS.md). `sinceWorldTime` stamps the crossed hour boundary on a kind change. |
| Phase-1 wiring | No Scheduler in Phase 1: `tickSeasonsAndWeather(clock, store, rng)` composes one fixed-update slice in the §4.4 order (clock advance → season roll → weather). 3.1 (Phase 2) re-registers `updateSeasons`/`updateWeather` into steps 0/2 — re-registration, not a rewrite. |
| Hook 1 — crop yields → 3.4 | `cropYieldMultiplier(season, weather)` (A-52 product; season 10000/12000/8000/3000 × weather storm 7000 / rain 11000, others unmodified). |
| Hook 2 — migration → 3.4 | `migrationClimatePull(season, climate)`: winter adds `MIGRATION_WINTER_PULL` (1000) for the warm climates (coastal, temperate — the 3.8-pinned warmth reading, DEVIATIONS.md). |
| Hook 3 — event probabilities → 5.1 | `weatherModifier(weather, weatherAffinity)`: the template's `"weatherAffinity": { "storm": 2000 }` map (keys = `weatherKey()`), SCALE-grid bonus for the spawn roll. |
| Hook 4 — exposure → 3.3/3.4 (Phase 2) | `exposureActive` (storm/snow) + `exposureDamagePerTick()` (A-53 = 2 hp/tick) + `isSheltered` (D-SHELTER-1, 50 m of the settlement center, inclusive) + the `applyExposure(region, near, anchor, applyDamage)` sweep — damage rides the one `applyDamage` path with source = 0 so deaths route to `onEnvironmentDeath` (3.3). Nothing calls it in Phase 1. |

Tests: `tests/test_seasons.cpp` — the two pinned cases
(`TestSeasons_CycleLengthAndOrder`, `TestWeather_MarkovStaysInTable`) plus
the cadence/determinism rules, the card's Done-when year fast-forward
(365 game-days over the full 256-region grid, 12 transitions exactly on the
A-51 boundaries), and the frozen hook signatures proven against fakes.

## The three doc walks (`D-JUR-1` — implemented once, here)

| Walk | Doc source | Semantics |
|---|---|---|
| `lawsAt(t)` | §Laws `lawsAt` | `t.laws` if set, else the parent chain; `DEFAULT_LAWS` at the root (`parent == 0`) or off any broken chain. |
| `socialClassOf(c, start)` | §Laws `socialClassOf` | Walk **up** from `start`; the **first** level whose `titles` carries `c` wins (the deepest, most local title — card pitfall: never "fix" this into breadth-search); `COMMONER` fallback. |
| `isAncestorOrSelf(a, t)` | §Justice `warrantActiveIn` | `a` is `t` or an ancestor of `t`; a warrant issued for `X` is active in `q` iff `isAncestorOrSelf(q, X)` — a village warrant covers its county/kingdom/empire, never a sibling village. |

`territoryAt(p)` is the fourth walk in spirit: region grid → the generation
mapping (`std::map<regionId, territoryId>`) → the territory covering the
point — **one map hit, no geometric recompute** (card 1.4). Positions
outside the A-40 plane and unmapped frontier regions resolve the **null
territory** (`kNoTerritory = 0`), whose walk semantics (DEFAULT_LAWS /
COMMONER / no jurisdiction / not safe) are exactly the docs' unclaimed-land
behavior — so call sites need no special cases.

## Region grid & the D-WORLD-1 seam

`REGION_GRID = 16`, `REGION_SIZE_M = 1024` (A-40, `core/constants.hpp`): 256
regions spanning `[0, 16384)²` metres. The id layout is
`regionId = zIndex * REGION_GRID + xIndex` (x fastest) — pinned in
`region.hpp` next to the projection. v1 is a flat plane (card 1.5's
`D-WORLD-1`); a spherical projection replaces the flat one by touching
`region.hpp` only — never raw coordinate math elsewhere.

## Ownership & swap contract

- All state is keyed by the opaque id aliases (R10); every map is
  `std::map` (R7) so iteration order is deterministic everywhere the sim or
  the worldgen hash observes it.
- `Territory.laws` points INTO a `DataRegistry`-owned `LawSet`; the store's
  `defaultLaws` (constructor) is the registry's DEFAULT_LAWS. A registry
  snapshot is immutable once committed; a future hot reload (card 1.3's
  optional path, OFF in v1) swaps snapshots **between sim ticks, never
  mid-tick** — systems may hold const refs across a tick, never across a
  swap. After a swap, the worldgen-built `laws` pointers must be re-wired
  with the same between-ticks rule.
- Populated once by worldgen (1.5) at new-game; persisted whole (7.1).

## Deliberate seams (do not build past them)

- `SeasonState`/`WeatherState`: task 3.8 owns the state machines that
  advance them — now landed in `seasons.hpp/.cpp` (the ordinal vocabularies,
  the two fixed-update slices, the four effect hooks); the world model only
  owns the storage.
- `Climate`: worldgen (1.5) assigns it; 3.8's winter migration pull and the
  enclave pick consume it. The consequence-flag BITS stay data-declared
  (territory_templates.json's regionFlags) — climate is an enum field, not a
  flag bit (DEVIATIONS.md carries the extension row).
- `CompactRecord`: task 4.1's `compactRecord(evidence)` defines the real
  payload (v1 carries the identification minimum — see the header doc and
  DEVIATIONS.md); task 4.2's burned-archive mechanic clears the vector.
- `Territory.owner`: task 6.3 adds it (`D-TERR-OWNER-1`).
- `lawless`: task 4.6 sets it (`A-96 LAWLESS_AFTER_DAYS`); the world model
  only carries the flag.

Tests: `tests/test_world_laws.cpp` (card 1.4's six pinned tests),
`tests/test_worldgen.cpp` (card 1.5's five pinned tests + the structural
rules: tree shape, frontier ring, contiguous city blocks, title chain,
factions, enclave, rare-species bound, variance cap, the 2 s budget), and
`tests/test_seasons.cpp` (card 3.8's two pinned tests + the cadence,
determinism, year fast-forward and frozen-hook cases).
