# src/world — the world model (task 1.4)

The static world graph: **regions** (spatial cells with flags/season/weather/
mana), the **territory tree** (the legal hierarchy laws/titles/safety attach
to), the explicit **region → territory mapping**, and the three doc walks
implemented exactly once. Worldgen (1.5) populates the graph; everything
downstream (justice, events, war, UI) reads it through here.

| File | Role |
|---|---|
| `region.hpp` | `Region` (design reference §World, fields verbatim) + `SeasonState`/`WeatherState` (owned by task 3.8, declared here) + the `Climate` class drawn at worldgen (1.5) + the **D-WORLD-1 projection**: `regionIdAt`/`regionIdOf`/`regionIndicesInGrid`/`isGridRegion`/`regionCenter` — the ONE place world coordinates and region indices meet. |
| `territory.hpp/.cpp` | `Territory` (design reference §Territories, fields verbatim + the card's `region`/`lawless` pins), `CompactRecord` (the archive row 4.1 produces / 4.2 burns), and the `isSovereign`/`isUnclaimed` predicates (design rule, decided once). |
| `world.hpp/.cpp` | `WorldStore`: the graph storage (all `std::map`, R7), the generation surface (`addRegion`, `addTerritory`, `setRegionTerritory`, `clear`), the three doc walks, and the parent/child traversal helpers 4.x/6.x consume. |
| `worldgen.hpp/.cpp` | Task 1.5: `generateWorld(seed, DataRegistry, WorldStore&, NpcSeedOut&)` — the seven pinned generation stages, the `NpcSeed`/`FactionSeed` plan records, the safe-zone spawn derivation, and the FNV-1a summary hash. |

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
  advance them; the world model only owns the storage.
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

Tests: `tests/test_world_laws.cpp` (card 1.4's six pinned tests) and
`tests/test_worldgen.cpp` (card 1.5's five pinned tests + the structural
rules: tree shape, frontier ring, contiguous city blocks, title chain,
factions, enclave, rare-species bound, variance cap, the 2 s budget).
