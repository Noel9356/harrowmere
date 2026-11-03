# src/combat — the consent framework (Phase 2, task 2.7; combat core lands with 3.2/3.3)

The two consent channels the whole justice system depends on: **duels** and **arenas**, with the doc's broadcast rule — "Duels and arenas broadcast consent to nearby NPCs; otherwise every legal duel generates assault evidence."

| File | Owns |
|---|---|
| `consent.hpp/.cpp` | `ConsentRegistry` (pending requests + active duels), the duel flow (`requestDuel` / `acceptDuel` / `yieldDuel` / `endDuelOnDeath` / `sweepDuels`), `IDuelBroadcast` (the witness-registry seam), `ArenaTable` + `refreshArenaPresence` (the shared-id zone consent) |
| `justice_sink.hpp` | THE write-side justice seam (D-SINK-1): `recordCrime` / `noteAssault` / `hasWarrantIn` / `hasLethalWarrantIn` / `holdsGuardGearTheft`, with `NullJusticeSink` as the active implementation until 4.1. Card 3.3's pinned interface — landed by 2.4 (theft hook) and adopted unchanged by 3.3 |

## The pinned order (the card's pitfall)

**Broadcast → enable damage.** `acceptDuel` gathers the A-26 observers (`idsWithinRadius(midpoint, 50 m)`) and delivers `duelStarted` BEFORE the `duelOpponent` fields flip — the fields ARE the damage enable, and a witness who never saw the broadcast would turn the first lawful blow into assault evidence. `TestConsent_BroadcastReachesRadiusOnly` asserts the order from inside the broadcast callback.

## Consent is read in exactly one place

`character/status.hpp`'s `consentsToPvp` (2.1) — duel partner OR shared non-zero arena id. Nothing in this directory re-implements it; this directory only writes the fields the check reads.

## Wiring points (when their owners land)

- **3.1 / scheduler**: `sweepDuels` joins tick step 1 (Sweeps); the step-9 command handler maps `RequestDuel`/`YieldDuel` onto `requestDuel`/`yieldDuel`.
- **3.2 / applyDamage**: calls `ConsentRegistry::noteDuelDamage` after applying damage between duelists (feeds A-27's idle timeout).
- **3.3 / death**: calls `endDuelOnDeath` (a duel ends when either side dies; pending requests die with the dead).
- **3.4/3.5 + 3.10 / movement**: calls `refreshArenaPresence` after position changes (entering sets `arenaId = zoneId`, leaving clears it).
- **3.9 / perception**: implements `IDuelBroadcast` to store the consent window per observer.
- **6.5 / acceptance policy**: NPC duel acceptance (score proximity + disposition) calls `acceptDuel` on the NPC's behalf; this file keeps only the bookkeeping.
