# factions/ — the faction layer (task 6.1)

Factions are the social + resource substrate war (Phase 4's 6.2) reads: the doc-verbatim `Faction` record (members, per-member ranks 0..9, shared resources, per-pair relations, capital), the worldgen adoption (1.5's stage-5 seeds become the runtime records — kingdom courts by home-territory walk, the frontier wanderers the free company), the player-faction interactions (found at the A-108 cost, leader-only invite/rank/dissolve), and the lazy monthly drift sweep.

Territory ownership is NOT a faction concept — territories are claimed by characters (6.3, Phase 4); do not conflate the two (the card pitfall: ranks are per-faction integers, never SocialClassId — the titles live on territories and feed socialClassOf/suppression).

| File | Task | What lives here |
|---|---|---|
| `factions.{hpp,cpp}` | 6.1 | the `Faction`/`Relation` records, the store (membership lockstep across `Character.faction`, `Faction.members`, the index), the adoption, found/invite/rank/dissolve, SetRelation, the monthly drift, `FactionQueries` (the live `IFactionQueries`; `isAtWar` reads 6.2's war registry), the step-1 monthly system |

## As-built notes

* **Membership is bidirectional and lockstep** — every mutation goes through one store function (`attach`/`detach`) that writes all three sides (`Character.faction`, `Faction.members`, the `membership_` index); TestFaction_MembershipBidirectional pins it.
* **`sharedResources[goodId 0]` is the currency row** (R10 reserves 0 in every id space — the doc-verbatim struct carries no separate currency field). Dissolution distributes currency by rank share (weight = rank + 1, the flooring remainder to the top weight) into the members' i64 purses; goods rows disperse through 3.7's `addGoodsHeld` ledger.
* **The drift is lazy-gated** (the reputation-decay pattern): the first call in a new game-month applies the month once. Rivalry pairs harden or soften by the leaders' mean reputation (4.4's read); every other non-alliance pair drifts toward neutral. Trade volume and war memory are the dormant arms (no v1 driver; the war registry is Phase 4's).
* **The unpinned magnitudes** (seeded ±6000 scores, the 50/100 drift steps, the invite base/discount) are named constants in `core/constants.hpp` awaiting Appendix A promotion — DEVIATIONS.md carries the row.
* **`isAtWar` reads 6.2's war registry** through the context's `warStore` (the effectively-active gate — see the war/ section below); the null registry in a test context still answers false (D-SINK-1).

---

# war/ — the war layer (task 6.2)

Declared wars with casus belli, notice, treaty and outcome effects on regions and territorial control — NPC-driven by default, player-declarable against NPC factions (declareWar is the public entry; a player-declared war is one whose attacker faction the player leads).

| File | Task | What lives here |
|---|---|---|
| `war.{hpp,cpp}` | 6.2 | the doc-verbatim `War` record + `CasusBelliKind`, `WarStore` (the `IWarRegistry` face: `isCombatant`/`registerCasusBelli`), the five casus-belli validators, `declareWar` (notice + defender notification + `TerritoryChanged{War}`), the step-8 tick (battle progression + collapse/timeout resolution + the lazy monthly NPC auto-declaration), `endWar` (the outcome: region war-outcome bits, the claim rewiring through 6.3, the title transfers, the A-112 treaty) and `warEffectivelyActive` — the ONE flip instant |

## As-built notes

* **The one flip instant.** `warEffectivelyActive(war, now)` (active &&
  `now >= noticeEndsAt`) is the only notice gate: `IN_WAR` (2.1's derived
  status, read through `IWarRegistry::isCombatant`) and 3.3's `isCriminalAct`
  war exemption (read through `FactionStore::isAtWar` → the context's
  `warStore`) share it. A war in its notice window is not war: kills during
  notice are still crimes, nobody is a combatant (the card pitfall, pinned by
  TestWar_NoticeWindow_KillsDuringNoticeAreCrimes and
  TestWar_InWarFlipsAtNoticeEnd).
* **D-WAR-EVT-1 — the dual-targetGroups battle extension.** The doc's war
  paragraph: "war progresses by battle events (regional BATTLE event
  template — both factions' NPCs join as targets of each other)". As built:
  the shipped `BATTLE` template declares `special.dualTargetGroups` (the
  data half, Appendix-B shaped); the engine books the two groups
  (`EventEngine::targetGroups_`, group 0 = the seed's attacker side, 1 = the
  defender side) and inserts every member into the event's `targetIds` —
  each side IS the other's targets — through the new
  `IEventSpawner::spawnWarBattle` seam. The WAR MODULE computes the two
  member lists (faction membership is its data; the engine stays
  faction-blind — D-SINK-1 cuts both ways). One live BATTLE per contested
  region: the engine's scale-uniqueness gate is the spawn throttle and the
  template's `maxDurationDays: 1` is the cadence — no spawn book needed.
  TestWar_BattleSpawnsDualTargetGroups pins the shape.
* **The declaration rules, sentence by sentence** (the card's Done-when: the
  doc's war paragraph is sentence-by-sentence testable):
  | Doc sentence | Test |
  |---|---|
  | "a valid casus belli — BorderDispute (adjacent controlled territories)" | `TestWar_DeclareRequiresCbOrRatio` (the valid/invalid pair, the region-grid adjacency) |
  | "ResourceConflict (both factions extract the same good from contested region)" | `TestWar_DeclareRequiresCbOrRatio` (the produced-stock + co-resident reading) |
  | "Reputation (defender's leader reputation in attacker's capital < −4000)" | `TestWar_DeclareRequiresCbOrRatio` (below/above the floor) |
  | "Massacre (4.7's hook)" | `TestWar_DeclareRequiresCbOrRatio` (the marker consumed by the declaration) |
  | "or PowerRatio: attacker aggregate member power ≥ WAR_POWER_RATIO (1.5×)" | `TestWar_DeclareRequiresCbOrRatio` (over/under the band + the zero-member guard) |
  | "declareWar sets noticeEndsAt = now + WAR_NOTICE (24 game-hours)" | `TestWar_InWarFlipsAtNoticeEnd` (the flip instant) |
  | "defender auto-notified (PlayerSignal if player-side; NPC court otherwise)" | the notice rides `PlayerSignal::Kind::War` (D-WAR-SIGNAL-1); the NPC court's notice IS the war record |
  | "kills during notice are still crimes" | `TestWar_NoticeWindow_KillsDuringNoticeAreCrimes` |
  | "war progresses by battle events ... and by power attrition" | `TestWar_BattleSpawnsDualTargetGroups` (the events) + `TestWar_CollapseEndsAtThirtyPercent` (the attrition read) |
  | "ends when one side's aggregate power < 30% (A-111), or maxDurationDays = 14 timeout (attacker loses)" | `TestWar_CollapseEndsAtThirtyPercent` (both arms) |
  | "Outcomes modify region flags and territorial control" | `TestWar_OutcomeSetsRegionFlags` + `TestWar_TitlesTransferOnLoss` |
  | "a mandatory one-week peace treaty applies" | `TestWar_TreatyBlocksRedeclareSevenDays` |
* **NPC-driven by default**: the step-8 tick's lazy monthly sweep (the 6.1
  drift's pattern) declares for rival pairs (id-ascending) from the first
  valid casus belli in the pinned enum order; the treaty and the
  active-war gates hold the frequency. TestWar_NpcRivalsAutoDeclare pins it.
* **Aggregate power** reads the LIVING members' cached powers (cache
  freshness is the LOD tick's business — the 6.1 precedent); the PowerRatio
  comparison is a two-factor product (u64-safe, R3).

---

# claims/ — the territory-claim layer (task 6.3)

Player claims over territories with the doc's protection/abandonment rules and custom-law installation.

| File | Task | What lives here |
|---|---|---|
| `claims.{hpp,cpp}` | 6.3 | the claim interaction (unclaimed frontier / lawless / abandoned, the A-113 cost, the no-active-war gate, the kingdom tolerance), `installLaws` (the claim-store-owned custom sets; `lawsAt` resolves them), `dismantleAbandoned` (revert + keep the archive), the presence ledger (`notePresence`, validated minutes only) + the step-1 abandonment sweep (14 REAL days, the war pause on GAME time), the destruction-protection gate, `noteOwnershipTransfer` (6.2's rewiring) |

## As-built notes

* **D-TERR-OWNER-1**: `Territory.owner` (characterId, 0 = unowned) is the plan-pinned extension — DEVIATIONS.md carries the row. Worldgen leaves it 0; the claim writes it; the 6.2 outcome rewires it (the ledger row follows with a fresh timer, the installed laws stay); the dismantle clears it.
* **The two clocks** (the card's top misinterpretation risk, D-TIME-1): the 14 days are REAL (`ctx.realTimeSeconds`), the war-involvement pause and the protection suspension are GAME time (`warEffectivelyActive`'s read). The two never mix inside one expression.
* **The presence ledger**: real seconds accrue only while `!afk` (D-AFK-1); one stint of ≥ ACTIVITY_MINUTES (A-114 = 30 real minutes) resets the timer; a stamp gap longer than the AFK timeout (A-58) IS a logout and restarts the stint. The plugin stamps the player's claims at step 9 (the controller's fresh AFK verdict); the sweep stamps living NPC owners (never AFK — a dead leader's claim decays into abandonment).
* **The safe flag is worldgen-owned**: the install path never reads or writes `Territory.safe` — a claimed safe territory keeps its ward reports and its spawn point whatever the custom set carries (pinned both directions by TestClaims_SafeFlagNotRemovableByOwner).
* **Custom law sets are claim-store-owned** (stable `std::map` addresses — the registry-swap contract's test form); `lawsAt` resolves them through the untouched doc walk (D-JUR-1).
