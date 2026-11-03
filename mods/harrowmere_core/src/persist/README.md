# persist/ — the save/load layer (task 7.1)

One versioned JSON snapshot of the entire simulation: atomic on write, deterministic in field order, roundtrip-exact on load, refusing anything that does not validate cleanly.

| File | Role |
|---|---|
| `save.hpp` | the public surface: `SaveSources` / `LoadTargets`, `buildSave`, `writeSaveFile`, `readSaveFile`, `RuntimeState` |
| `save.cpp` | serialization (buildSave) + the atomic tmp/rename write |
| `load.cpp` | strict validation into typed snapshots, then the ordered apply |
| `state_hash.{hpp,cpp}` | the canonical FNV-1a state hasher (8.1's file, shared with the 8.3 soak dumps) |

## The format

```json
{ "format": "game-save", "version": 1, "seed": 123456789, ... }
```

`version` mismatch refuses with a clear message (no migration in v1 — the Appendix E seam is the field itself). Objects key-sort (nlohmann's map type); every array below is written in map/insertion order (R7). Two saves of the same state are byte-identical.

### Top-level keys

| Key | Shape | Notes |
|---|---|---|
| `format` | string | always `"game-save"` |
| `version` | u32 | `SAVE_FORMAT_VERSION` (save.hpp) |
| `seed` | u64 | the world seed; `readSaveFile` reseeds the RNG from it before applying the stream states |
| `clocks` | object | `worldTime`, `realTime`, `timeScale`, `tickRate`, `worldCarry`, `realCarry` (0.3; the carries persist per clock.hpp's own note) |
| `rng` | array of `{stream, hi, lo}` | one row per pinned SeedRng stream (0.3), alphabetical; `readSaveFile` reconstitutes the `(<stream>:hi`, `<stream>:lo)` pairs SeedRng::state() emits |
| `ids` | object | one key per id space (R10): `character` (CharacterStore), `evidence` + `warrant` (JusticeStore), `event` (EventEngine), `itemInstance` (ItemInstanceRegistry) |
| `world` | object | see below |
| `characters` | array | see below |
| `corpses` | array | see below |
| `justice` | object | see below |
| `events` | object | see below |
| `economy` | object | see below |
| `factions` / `factionIndex` | array / object | the Faction rows / the store-level maps |
| `wars` / `warState` | array / object | the War rows / the store-level state |
| `claims` | object | the 6.3 claim ledger + custom law sets |
| `items` | object | the item-instance map + the bag store rows |
| `appearance` | array | 2.6's per-character change stamps |
| `roles` | object | open replacement seats + guard power (3.6) |
| `consents` | object | pending duel requests + live duels (2.7) |
| `combat` | object | the A-31 casting-cadence book (CombatQueue) |
| `quests` | array | **reserved, always empty** — the 6.4 as-built carries no quest state outside the event engine (a quest IS an event instance, serialized under `events`; quests.json is content). The key exists so the schema stays stable. |
| `runtime` | object | optional: the pacing gates below (not part of the canonical hash) |

### `world`

- `regions`: array of `{id, flags, manaRichness, climate, season:{index, startedAt}, weather:{kind, since}}`.
- `territories`: array of `{id, parent, type, safe, lawless, owner, region, lawKey, titles:[[holder, class]], archive:[{evidenceId, worldTime, crimeType, culprit, victim}]}`.
  - `owner` is D-TERR-OWNER-1's extension (persisted with the territory).
  - `children` is deliberately NOT serialized: derived from `parent` on load (addTerritory rewires the tree; a saved children list would double-attach).
  - `lawKey` = the resolved law set's key; empty = inherit (lawsAt's walk). On load the pointer re-resolves: ClaimStore custom set first, then the DataRegistry set, else inherit + a `LoadTargets::warnings` row.
- `territoryIndex`: the generation mapping, `[region, territory]` pairs.
- `lawless`: the lawless territory ids; `lawlessWatch`: `[territory, watchStart]` pairs (4.6).
- `playerSpawn`: worldgen stage-4's safe-zone spawn point (D-WGEN-1: a load never re-generates, so the point rides the save).

### `characters` rows

```json
{ "id": 2501, "isPlayer": false,
  "character": { "species", "statusFlags", "duelOpponent", "arenaId",
                 "appearanceComponents": [...], "equippedItems": [...],
                 "body": {"currentHp", "maxHp", "position": [x, y, z]},
                 "progression": {"totalXp", "availableXp", "level", "stats": [6], "skills": [8], "currency"},
                 "cooldowns": [3], "faction", "eventId", "lastCombatTime", "peakPower" },
  "npc": { ... } | {},
  "creation": { "name", "appearanceComponents": [...], "classKey" } }   // player rows only
```

- `cachedPower` / `powerDirty` are NOT serialized (7.1's do-not-save list): every restored character has `powerDirty = true` and power recomputes (TestSave_DirtyFlagsSetOnLoad pins it). `peakPower` persists bit-exact.
- `npc` is the NpcState sidecar (D-STORE-1), `{}` for the player. Persisted fields: `name`, `birthWorldTime`, `roleKey`, `needs` (hunger/rest/wealth/social), `integrity`, `trust` (suspicionScore, perCharacterScore, lastSeen, lastSuspicionDecay), `affinity`, `actionCounters`, `homeTerritory`, `currentRegion`, `inventory`, `invulnerable`, `migrated`, `fleeing`, `fleeingUntil`. Do-not-save: `tier` (LOD recomputes — saving it lets stale tiers survive anchors) and the per-tick fields (`needsCarry`, `nextDutyWorldTime`, `lastMigrationRollWorldTime`, `currentIntent`, `migrating`, `migrationTarget`).

### `corpses` rows

`{victim, position: [x,y,z], deathTime, loot: [[itemKey, count]], discovered, discoveredBy, lootCurrency}`. The staged `pending_` list is not serializable (same-tick state — deaths stage at step 10 and materialize at step 11; empty at every save point).

### `justice`

`evidence` (rows carry the snapshot `territory` + full report list), `warrants`, `activeWarrantsByCulprit` (`[culprit, [warrantIds]]`), `reputations` (`{who, perTerritory, notoriety, lastDecayTime}` — the anti-abuse fields ride the row), `jailUntil`, `assaultDamage`, `missingPersons`, `caseLastTick`, `signalledCulprits`, `posters`, `captureStamps`, `massacreMarkers`, `killTally`, `suspicionMemory` (reserved-empty: the suspicion memory is per-observer state on the owning NpcState's `trust` record — D-STORE-1 — and serializes with the characters).

### `events`

- `active`: full Event rows (`id`, `templateId`, `region`, `depth`, `state` with powerSnapshots, sorted `participants`, `actionAmounts` (per actor, one entry per ActionType), `hpLost`, `targetIds`, `excludedFromRewards`, `startedAt`, `lastTransitionAt`) plus the per-event side books as `side`: `leftEarly`, `spawnedTargets`, `grantedTiers`, `seed` (the manhunt's warrant seed), `targetGroups` (the 6.2 D-WAR-EVT-1 dual-group book).
- `history`: the ended retention window, same row shape minus side books (finishEvent clears the books; the step-1 sweep retires the rows).
- `priceBias` / `weatherBias`: the 5.5 world-effect records — transient event state that IS saved (the card's own list).

### `economy`

`stocks` `[region, good, supply, demand]`, `prices` `[region, good, price]`, `pendingTaxes`, `territoryWealth`, `goodsHeld`, `population`, `lastRecompute`. The goods table is boot content (goods.json) and is not part of the snapshot.

### `factions` / `factionIndex`, `wars` / `warState`

`factions` rows: `{id, name, members, ranks, sharedResources, relations (incl. treatyUntil, D-WAR-TREATY-1), capital, playerFounded}`; `factionIndex`: the lockstep membership index, the dormant trade volume, `lastDriftMonth`, `nextFactionId`. `wars` rows are the doc-verbatim War struct (cb kind/region/origin included); `warState`: `nextWarId`, `lastAutoDeclareMonth`, `massacreMarkers` (`[territory, culpritFaction, at]`).

### `claims`

`rows`: `{territory, owner, stintRealSeconds, lastQualifyingRealTime, lastStampRealTime, lawKey, abandoned}`; `customLaws`: full LawSet rows (key, convictionThreshold × CrimeType::COUNT, suppression rows, exemption rows, cultureBias, suspicion trigger rows, safe).

### `items`

`instances`: `[instanceId, itemDefId]` (the ItemInstanceRegistry map — the card's "must serialize in saves" note); `bags`: `[who, {stacks, equipped, instances}]` (the InventoryStore rows).

### `runtime` (optional, not hashed)

`tickNumber`, `gridLastSweepDay`, `justiceSweepLastDay`, `weatherPrevWorldTime`, `economy {started, lastHour, lastDay}`, `suspicionLastRunHour`, `contributionSnapshotAt`, `enforcementResponded` (4.6's signal-before-force memory — without it a resumed world re-responds to warrants guards already engaged), `player {position, valid}`. These are the step systems' lazy cadence stamps and the plugin's day gates — pacing, not world state; they exist so a loaded world resumes every cadence exactly.

## Guarantees

- **Atomic write**: serialize → `<path>.tmp` → `std::filesystem::rename`. A crash mid-write leaves the previous save intact; the tmp never survives a completed call (TestSave_AtomicTmpRemovedOnCrash).
- **No partial load**: the whole document validates into typed snapshots before the first store write; cross-section identity is checked (warrant → evidence, culprit index → characters, territory → parent, counters ≥ every live id). A refusal leaves every store untouched.
- **RNG continuation**: the saved stream states + the saved seed reproduce the exact random sequence (TestSave_RngContinuationIdentical; the soak and 8.1 replay depend on it).
- **Roundtrip**: `canonicalStateHash` (state_hash.hpp) walks exactly the persisted field set — hash(save) == hash(after load) whenever the roundtrip is exact (TestSave_RoundtripHashEqual, on a 30-game-day soak world).
- **Floats excluded** (8.1): positions hash as `llround(x*1000)`; no other f64 exists in persisted state.
- **Version policy**: wrong `version` refuses; the file is left untouched.
