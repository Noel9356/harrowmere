# Harrowmere — Phase 4 acceptance walkthrough (task 8.5)

This is the player-chair acceptance script for the six design documents and the Phase 4 Reign exit gate. It is deliberately a manual runbook: unit tests are backstops, not substitutes for watching the UI, signals, map, and persisted campaign behave together.

## Run protocol

- **Build/runtime:** use a Release build from a clean checkout. Keep the game log visible, start with a new campaign, and do not edit save JSON by hand.
- **Reproducibility:** use the pinned campaign seed `20261102`. The headless harness now exposes deterministic spawn presets with `--scenario A01` … `--scenario A40`; `--list-scenarios` lists the IDs, and `--scenario-save` writes the prepared state through the real save API. Run, for example, `build/runtime/harrowmere_soak --scenario A33 --seed 20261102 --data-dir mods/harrowmere_core/data --scenario-save /tmp/acceptance-A33.json` for the 14-real-day abandonment/dismantle preset. Presets create state through the real stores and public subsystem entry points; do not hand-edit save JSON. A01–A18 and A37–A38 start from a clean deterministic campaign because the player performs the interaction during the walkthrough; the named automated fixtures are their rule backstops.
- **Evidence:** for each scenario, record the build ID, seed/preset, pass/fail, a short observation, and (for failures) the log excerpt plus a state hash if the harness produced one. Each scenario is intended to take 5–10 minutes; long-clock rules are exercised through their deterministic fixture instead of waiting in real time.
- **Automated backstops:** run `scripts/ci.sh` before the manual walkthrough. `scripts/acceptance.sh --ci` validates the scenario registry and executes the A36 three-contributor reward payout and A40 10-day/save/load/10-day/ territory-transfer/save/load campaign fixtures. The named unit test remains the subsystem regression backstop, not proof that the visual observation was made.
- **Sign-off:** one engineer and one person who has never worked on Harrowmere must each execute all 40 scenarios independently. Do not mark a scenario green based only on this document or on a unit test.

## Scenario record

For every scenario, tick the result only after the steps have been performed in a running build. “Expected” describes the observable result; “Proof” names the design source and automated regression where one exists.

### A01 — A consented duel leaves no criminal evidence

- **Setup:** Start a fresh campaign; meet an NPC willing to duel and send the consent request before either side attacks.
- **Steps:** Accept the duel; land one non-lethal and one lethal-capable hit; end the duel normally; open the Justice panel and inspect the evidence list.
- **Expected:** Both participants are consented for the duel; combat does not create a criminal record or witness report merely because a duel hit landed.
- **Proof:** `common.md` Design reference §F.1 “Duels & arenas” and §F.4 “Crime & Evidence”; Phase 4 Task 8.5; `TestCriminalAct_DuelFalse` and `TestConsent_DuelSetsBothSides`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A02 — An unconsented villager assault starts the suspicion trail

- **Setup:** Approach a village resident who is not hostile and has not consented to combat. Keep the Justice panel available.
- **Steps:** Punch the villager once; stop attacking; remain in sight long enough for the resident or a witness to react; follow the resulting wanted state through investigation and enforcement.
- **Expected:** The assault is not treated as a duel. Suspicion/shadowing can precede a case; a poster/warrant appears only after the evidence and identity thresholds are met. A sufficiently strong player may be hunted rather than simply arrested.
- **Proof:** `common.md` Design reference §F.4 “Suspicion”, “Evidence”, and “Enforcement”; Tasks 4.1, 4.2, 4.5, 4.6; `TestWitness_WeightFormulaDocExact`, `TestCase_InvestigatingBelowFullThreshold`, `TestSuspicion_NeverCreatesEvidence`, `TestEnforcement_WeakCulpritArrested`, `TestEnforcement_StrongCulpritTriggersEvacuationAndManhunt`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A03 — A protected species exception suppresses the murder report

- **Setup:** Use the xenophobic-village fixture and the species marked exempt by that village's law data; make the victim an ordinary villager.
- **Steps:** Commit the murder in the fixture; inspect the Justice panel and the evidence store before and after the normal report sweep.
- **Expected:** The shipped `xenophobic_village` exemption for beastkin murder suppresses the evidence object entirely (including its ID increment). Do not generalize this to every species or every village.
- **Proof:** `common.md` Design reference §F.4 “Laws” and “Crime & Evidence”; Task 8.5; `TestEvidence_ExemptSpeciesNoRecord`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A04 — Elf and human witnesses contribute their correct weights

- **Setup:** Use a witness fixture with one eligible elf and one eligible human observing the same crime from equivalent positions and with equal integrity/affinity inputs.
- **Steps:** Trigger the same witnessed crime in the fixture twice, changing only which witness is present; compare the report weights and case score.
- **Expected:** Species/role/integrity/affinity weighting follows the pinned witness formula; no visual intuition or species label substitutes for the computed score.
- **Proof:** `common.md` Design reference §F.4 “Witnesses”; Task 8.5; `TestWitness_WeightFormulaDocExact` backstops the formula and `TestBias_XenophobicVillageLowersAffinity` backstops species/culture bias; the elf-versus-human comparison remains a manual observation.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A05 — Children and non-sapients do not qualify as witnesses

- **Setup:** Put an eligible adult witness, a child, and a non-sapient NPC in the same witness fixture.
- **Steps:** Trigger one crime in view of all three; inspect report entries and weights.
- **Expected:** Only the qualifying adult contributes a witness report; child and non-sapient exclusions are respected.
- **Proof:** `common.md` Design reference §F.4 “Witnesses”; `TestWitness_ChildAndNonSapientSkip`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A06 — A crime with no surviving witness does not invent a testimony

- **Setup:** Use the no-surviving-witness fixture; keep the victim and any physical evidence state visible in Justice.
- **Steps:** Commit the crime; remove/resolve all potential witnesses before investigation; advance the report sweep; inspect identity and case state.
- **Expected:** No witness identity is fabricated. Physical evidence and later corpse discovery may still create a report through their own paths.
- **Proof:** `common.md` Design reference §F.1 “Death” and §F.4 “Evidence”; `TestIdentity_OnlyNpcReportsCount`, `TestDelayed_DiscoveredBodyAppendsReport`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A07 — Corpse discovery produces a delayed report

- **Setup:** Use a crime fixture in which the body is initially undiscovered and a later NPC discovery is scheduled.
- **Steps:** Commit the crime; verify no immediate discovery report; advance to the discovery; inspect the evidence report timestamp and case score.
- **Expected:** Discovery appends a report at discovery time; it does not backdate a fabricated eyewitness report to the moment of death.
- **Proof:** `common.md` Design reference §F.4 “Delayed reports”; Task 8.5; `TestDelayed_DiscoveredBodyAppendsReport`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A08 — A lawful lethal action is not automatically a crime

- **Setup:** Arrange a duel or a valid lethal-warrant context from the matching fixture; verify the current consent/warrant state before acting.
- **Steps:** Perform the permitted lethal action; inspect the evidence store, warrant state, and Justice panel.
- **Expected:** `isCriminalAct` applies the explicit duel/warrant exemptions; a lawful kill does not create an ordinary murder record.
- **Proof:** `common.md` Design reference §F.4 “Crime & Evidence”; Task 3.3; `TestCriminalAct_DuelFalse`, `TestCriminalAct_LethalWarrantExonerates_NonLethalDoesNot`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A09 — Selling a guard's gear is treated as theft

- **Setup:** Use the guard-equipment fixture with an item traceable to a guard inventory and a vendor who accepts the item type.
- **Steps:** Take the guard's gear without permission; sell it through the normal vendor interaction; open Justice and follow the evidence to the case.
- **Expected:** Selling does not launder ownership: the transfer is recorded as theft evidence and can feed the normal case pipeline.
- **Proof:** `common.md` Design reference §F.4 “Crime & Evidence”; Tasks 2.4, 4.1, 4.2; `TestGuardGear_SellEmitsTheftSignal` (signal emission) and the normal evidence/case pipeline tests in `test_evidence.cpp` / `test_cases_warrants.cpp`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A10 — The first successful bribe halves the score exactly once

- **Setup:** Use the bribery fixture with a live case and a bribeable report.
- **Steps:** Attempt the permitted bribe; inspect the case score; attempt the same bribe again against the same report.
- **Expected:** The first successful bribe applies one halving. A second attempt does not halve the already-adjusted score again.
- **Proof:** `common.md` Design reference §F.4 “Bribery”; Task 4.1; `TestBribery_HalvesOnceThenFails`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A11 — Failed repeat bribery creates obstruction evidence

- **Setup:** Continue from A10 after the one permitted successful bribe.
- **Steps:** Try to bribe the same witness/report again; inspect the new evidence and the case progression.
- **Expected:** The repeat attempt fails and produces obstruction evidence; it does not grant a second score reduction.
- **Proof:** `common.md` Design reference §F.4 “Bribery”; Task 4.1; `TestBribery_FailureCreatesObstructionEvidence`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A12 — Healing a wanted target does not create a reward loop

- **Setup:** Use the wanted-target/manhunt fixture with a healer and a target whose status can be restored repeatedly.
- **Steps:** Heal the target, allow the event/attempt to observe the heal, and repeat the heal without adding qualifying hunter damage or a new rewardable contribution.
- **Expected:** Healing alone does not generate an unbounded payout; reward eligibility remains tied to the pinned contribution rules.
- **Proof:** `common.md` Design reference §F.5 “Contribution” and “Rewards”; Tasks 5.3, 5.4; `TestValidation_HealCappedByHpLost` (the exact no-HP-loss / no-heal-loop assertion) and `TestRewards_IneligibleDoNotShrinkShares`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A13 — Capturing a manhunt target pays once

- **Setup:** Use a live manhunt with a valid hunter contribution and an uncaptured wanted target.
- **Steps:** Capture the target alive; inspect the reward record; attempt to process the same completion again through the fixture's event-end path.
- **Expected:** The completion resolves once and the bounty is paid once; duplicate event-end handling cannot duplicate currency.
- **Proof:** `common.md` Design reference §F.4 “Bounty hunters” and §F.5 “Rewards”; Tasks 5.4, 4.7; `TestCapture_PaidOnce`, `TestManhunt_EndsOnCapturePublishesResolved`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A14 — Surrender ends the manhunt and proceeds to conviction

- **Setup:** Start with an active manhunt and the player as the wanted culprit.
- **Steps:** Use the surrender interaction; inspect the manhunt event, warrant, jail/sentence state, and player control state.
- **Expected:** Surrender resolves the manhunt and invokes the conviction path; it does not leave a second active hunt targeting the same culprit.
- **Proof:** `common.md` Design reference §F.4 “Surrender” and §F.5 “Manhunt”; Tasks 4.3, 5.4; `TestSurrender_ConvictsAndEndsManhunt`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A15 — A successful escape clears the old sentence and creates fresh crime

- **Setup:** Use the jailed-player fixture with an active sentence and an escape route available.
- **Steps:** Attempt the escape; on success inspect jail records, warrant/case state, and new evidence; re-enter the settlement.
- **Expected:** The old jail record is cleared as specified, and escape creates a fresh crime rather than silently erasing the legal history.
- **Proof:** `common.md` Design reference §F.4 “Jail” and “Escape”; Task 4.3; `TestEscape_SuccessClearsJailRecordsFreshCrime`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A16 — A failed escape extends the sentence

- **Setup:** Use the pinned failed-escape fixture.
- **Steps:** Attempt the escape; inspect the resulting remaining sentence and jail map.
- **Expected:** Failure applies the documented sentence increase exactly once and leaves a consistent jail record.
- **Proof:** `common.md` Design reference §F.4 “Escape”; Task 4.3; `TestEscape_FailureDoublesSentence`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A17 — A prison breakout makes the rescuer accountable

- **Setup:** Use the breakout fixture with a jailed prisoner, a rescuer, and one qualifying witness/guard.
- **Steps:** Trigger the breakout; inspect the rescuer's evidence and any resulting case while verifying the released prisoner state.
- **Expected:** The rescuer receives the pinned evidence; the release does not silently attribute the action to the prisoner alone.
- **Proof:** `common.md` Design reference §F.4 “Breakout”; Task 4.3; `TestBreakout_RescuerGetsEvidence`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A18 — The bounty distinguishes alive and dead capture

- **Setup:** Use two otherwise identical manhunt fixtures, one with an alive capture and one with a dead target.
- **Steps:** Resolve each capture; compare the payout and the final event state.
- **Expected:** Alive capture pays the full pinned amount; dead capture pays only the documented partial amount; neither path can pay twice.
- **Proof:** `common.md` Design reference §F.4 “Bounty hunters”; Tasks 4.7, 5.4; `TestCapture_AliveFullDeadPartial`, `TestCapture_PaidOnce`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A19 — Massacre produces a faction casus belli

- **Setup:** Use a settlement with a known ruling faction and at least one faction leader able to receive the massacre signal.
- **Steps:** Execute the massacre fixture; inspect the relation/war book and the notification delivered to the offended faction.
- **Expected:** The massacre hook produces the appropriate casus belli; the NPC faction can act on it without player input.
- **Proof:** `common.md` Design reference §F.1 “Factions”; Phase 4 Tasks 4.7, 6.2; `TestMassacre_ZeroLootAndCasusBelli`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A20 — All five war declaration routes are enforced

- **Setup:** Run `harrowmere_soak --scenario A20` to stage the deterministic NPC faction/power-ratio declaration. The full five-route matrix is exercised by the named WarStore fixture.
- **Steps:** Inspect the NPC war book and notice notification; then run the `TestWar_DeclareRequiresCbOrRatio` regression fixture to inspect each of the five legal routes plus the negative case.
- **Expected:** The acceptance preset creates the PowerRatio war; the automated fixture proves BorderDispute, ResourceConflict, Reputation, Massacre, and PowerRatio each permit declaration and the invalid case refuses.
- **Proof:** Phase 4 Task 6.2; `TestWar_DeclareRequiresCbOrRatio`, `TestMassacre_ZeroLootAndCasusBelli`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A21 — The notice window remains criminal until war begins

- **Setup:** Declare a war and keep the game clock before `noticeEndsAt`.
- **Steps:** Have the attacker and defender members exchange a lethal attack during notice; inspect `IN_WAR` and the crime record; advance exactly to the notice boundary and inspect again.
- **Expected:** During notice, `IN_WAR` is still off and kills remain crimes. At `noticeEndsAt`, both factions flip to `IN_WAR` together and the war crime exemption becomes active for faction members only.
- **Proof:** Phase 4 Task 6.2 and decision `D-WAR-EVT-1`; `TestWar_NoticeWindow_KillsDuringNoticeAreCrimes`, `TestWar_InWarFlipsAtNoticeEnd`, `TestCriminalAct_WarExempt`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A22 — Player territory protection suspends when the war activates

- **Setup:** Claim a player-owned territory, then arrange an incoming war against its owner faction; ensure the declaration notice is still running.
- **Steps:** Attempt the same destruction/siege action before notice ends and after `noticeEndsAt`; inspect ownership, archive, and event results.
- **Expected:** Offline protection remains through notice and is suspended from the effective-war instant while the war is active; the real-time abandonment clock is not substituted for game-time war state.
- **Proof:** Phase 4 Tasks 6.2–6.3, decisions `D-SP-WAR-1` and `D-TIME-1`; `TestClaims_IndestructibleOfflineExceptWar` and the notice tests in `test_war.cpp`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A23 — War outcome changes contested region flags

- **Setup:** Use a wartime fixture with one contested region and a known winning faction.
- **Steps:** Resolve the war; open the Map panel; compare the region flag overlay before and after resolution.
- **Expected:** The winning faction's war-outcome bits are applied to the contested region and are visible in the Map panel; unrelated regions are not changed.
- **Proof:** Phase 4 Task 6.2; `TestWar_OutcomeSetsRegionFlags` and `TestEffects_RegionFlagPersistsAfterEnd`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A24 — Lost player territory transfers to the winner's leading member

- **Setup:** Use a war fixture with a player-owned territory in the contested region and a known winning faction leader.
- **Steps:** Resolve the war; inspect the territory owner, claim book, garrison, and Map overlay; save and reload once.
- **Expected:** The territory owner and claim are rewired to the winning faction's leading member; garrison state follows the outcome contract; the change survives save/load.
- **Proof:** Phase 4 Tasks 6.2, 6.3, 7.1; `TestWar_TitlesTransferOnLoss`, `TestClaims_IndestructibleOfflineExceptWar`, `TestSave_RoundtripHashEqual`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A25 — NPC titles transfer when a faction loses

- **Setup:** Use a war fixture containing an NPC-ruled territory with a noble/royal title and an appointed winner-side replacement.
- **Steps:** Resolve the war; inspect the territory title and a character's derived social class.
- **Expected:** The old title is replaced by the winner's appointee; the ordinary `socialClassOf` read reflects the new title without a special-case UI patch.
- **Proof:** Phase 4 Task 6.2; `TestWar_TitlesTransferOnLoss`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A26 — The seven-day peace treaty blocks immediate re-declaration

- **Setup:** Use two factions that have just ended a war.
- **Steps:** Attempt declaration in both directions before seven game-days; advance to the treaty boundary and attempt again.
- **Expected:** Both directions are blocked while `treatyUntil` is in the future; declaration becomes eligible after the seven-day treaty expires.
- **Proof:** Phase 4 Task 6.2; `TestWar_TreatyBlocksRedeclareSevenDays`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A27 — Only an unclaimed frontier or lawless territory can be claimed

- **Setup:** Visit one unclaimed frontier territory, one lawless territory, one already-owned territory, and one ordinary child territory.
- **Steps:** Attempt the claim interaction at each territory center; inspect currency and owner after every attempt.
- **Expected:** Eligible frontier/lawless claims cost the pinned amount and set `owner`; ineligible owned/parented territories are refused without charging the player.
- **Proof:** Phase 4 Task 6.3 and decision `D-TERR-OWNER-1`; `TestClaims_UnclaimedOnlyClaimable`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A28 — A claimed territory installs custom laws used by `lawsAt`

- **Setup:** Claim an eligible territory and author a custom law set that conforms to the `LawSet` schema.
- **Steps:** Install the set; inspect the territory panel; query a crime whose result differs under the old and custom law rows.
- **Expected:** The installed law set is stored on the territory and the normal `lawsAt` resolver returns it for subsequent justice decisions.
- **Proof:** Phase 4 Task 6.3; `TestClaims_InstallCustomLawsLawsAtReturnsThem`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A29 — A claimed safe zone cannot have its safe flag removed by the owner

- **Setup:** Claim a territory that already carries the `safe` flag and verify its ward-report behavior.
- **Steps:** As owner, attempt to remove the safe flag; trigger a ward report after the attempted change.
- **Expected:** The owner cannot remove `safe`; ward reports continue to be appended for the protected zone.
- **Proof:** Phase 4 Task 6.3; `TestClaims_SafeFlagNotRemovableByOwner`, `TestEvidence_SafeZoneWardReportAppended`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A30 — Twenty-nine validated active minutes do not reset abandonment

- **Setup:** Use an owned test claim with a running real-time presence accumulator and no active war.
- **Steps:** Accumulate 29 validated active minutes; inspect the abandonment timer; start a fresh qualifying session and continue to 30 minutes.
- **Expected:** The 29-minute session does not reset the timer; one qualifying session of at least 30 validated active minutes does.
- **Proof:** Phase 4 Task 6.3 and decision `D-TIME-1`; `TestAbandon_TimerNeedsValidatedMinutes`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A31 — AFK time is excluded from owner activity

- **Setup:** Use an owned test claim with a new presence ledger.
- **Steps:** Accumulate 20 validated active minutes, then 40 AFK minutes; inspect `ownerPresence` and the abandonment reset eligibility. The preset deliberately makes the wall-clock session exceed 30 minutes while the validated active portion remains below the threshold.
- **Expected:** AFK seconds do not count toward the validated 30-minute threshold; only non-AFK validated activity contributes.
- **Proof:** Phase 4 Task 6.3 and decision `D-AFK-1`; `TestAbandon_AfkTimeNotCounted`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A32 — Fourteen real days without a qualifying owner session flags abandonment

- **Setup:** Use the abandonment fixture with `realTime` separated from `worldTime`; no qualifying owner session and no active war involvement.
- **Steps:** Advance the real clock through the fourteen-day threshold while keeping game-time war state inactive; inspect the `abandoned` flag and claim eligibility.
- **Expected:** The territory is flagged abandoned at fourteen real days; advancing game time alone does not substitute for the real-time rule.
- **Proof:** Phase 4 Task 6.3 and decision `D-TIME-1`; `TestAbandon_FourteenRealDaysFlags`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A33 — Dismantling reverts ownership but preserves the archive

- **Setup:** Use an abandoned territory with a custom law set and a non-empty world-memory archive.
- **Steps:** Dismantle the territory; inspect ownership, laws, archive, and whether a new claim can be made.
- **Expected:** The territory returns to unclaimed state; custom laws are discarded; archive/world memory is preserved.
- **Proof:** Phase 4 Task 6.3; `TestDismantle_RevertsAndKeepsArchive`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A34 — A world event resolves with zero player input

- **Setup:** Start a fresh NPC-populated world and wait for an eligible autonomous event; do not issue player commands during the event.
- **Steps:** Observe the event in the Events panel; leave the player idle and let the event reach its normal resolution condition; inspect its history.
- **Expected:** The event progresses and ends through NPC/system actions; no player contribution is required for lifecycle completion.
- **Proof:** `common.md` Design reference §F.5 “Events”; Tasks 5.1–5.2; `test_event_engine.cpp` lifecycle cases and `test_event_difficulty.cpp`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A35 — A regional battle event exposes both factions as target groups

- **Setup:** Use a battle fixture with two NPC factions and at least one eligible member on each side.
- **Steps:** Let the regional `BATTLE` event spawn; inspect the event target groups and participants from both factions; advance to resolution.
- **Expected:** Each faction's participants are valid targets of the opposing side through the dual-target-group extension; no one-sided event is shown.
- **Proof:** Phase 4 Task 6.2 decision `D-WAR-EVT-1`; `TestWar_BattleSpawnsDualTargetGroups`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A36 — Event rewards conserve the pool and break ties deterministically

- **Setup:** Run `harrowmere_soak --scenario A36`. The preset starts the shipped `bandit_raid` event and joins three live NPCs with equal validated damage contribution. Its test difficulty yields a 12,001-point pool.
- **Steps:** Resolve the event through `EventEngine::finishEvent`; compare each currency delta and the total against the template payout rate. Repeat with the same seed.
- **Expected:** Each contributor receives an equal base share, the one-point integer remainder goes to the lowest character ID, and total payout equals the pool multiplied by the currency-per-contribution rate.
- **Proof:** `common.md` Design reference §F.5 “Contribution” and “Rewards”; Tasks 5.3–5.4; `TestRewards_TotalComputedOnce`, `TestRewards_RemainderToTopContributor`, `TestRewards_TieBreakByLowerId`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A37 — Appearance-cost griefing is blocked by cooldown

- **Setup:** Use a player fixture with enough XP for one appearance change and a target cooldown state.
- **Steps:** Make one appearance change; immediately attempt another change that would evade the intended cost/cooldown; inspect XP and suspicion state.
- **Expected:** The cooldown refuses the repeated change; no free repeated appearance changes or power exploit is introduced.
- **Proof:** `common.md` Design reference §F.2 “Appearance”; Task 2.6; `TestAppearance_CooldownBlocks`, `TestAppearance_PowerUnchangedAfterChange`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A38 — Respec is refused while the player is in combat

- **Setup:** Use a combat fixture with a player whose progression can be respecced outside combat.
- **Steps:** Enter combat; request respec; verify stats/skills/XP are unchanged; leave combat and retry under the fixture's valid conditions.
- **Expected:** The combat-time request is refused without partial mutation; the non-combat control case follows its ordinary validation path.
- **Proof:** `common.md` Design reference §F.3 “Progression”; Phase 4 Task 8.5; `TestRespec_BlockedInCombat` in `test_progression.cpp`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A39 — A mid-manhunt save/load resumes the exact campaign state

- **Setup:** Use a live manhunt with active event targets, a warrant, and non-zero RNG stream state. Save to a normal campaign slot.
- **Steps:** Record the canonical hash; quit; reload the slot; compare the visible event/warrant state and canonical hash; advance both original and loaded fixtures through the same command sequence in the test harness.
- **Expected:** The loaded campaign restores the active manhunt and continues with identical state/RNG progression; no target, payout, or warrant is duplicated or lost.
- **Proof:** Phase 4 Tasks 7.1, 8.1, 8.2; `TestSave_RngContinuationIdentical`, `TestRoundtrip_MilestoneMidJustice`, `TestRoundtrip_MilestoneWartime`, and `TestRoundtrip_DoubleRoundtripStable` backstop continuation/serialization. Preset `A39` stages an active warrant and MANHUNT event through the justice and event stores and can write it with `--scenario-save`; the save/load comparison remains part of the player-chair run and the automated roundtrip/RNG tests remain the serialization backstop.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

### A40 — Full campaign acceptance: ten days, reload, ten more, war, transfer

- **Setup:** Fresh install and new campaign with the pinned seed; use a scenario fixture only where a reliable NPC war setup is required.
- **Steps:** Run `harrowmere_soak --scenario A40` with the acceptance seed. The preset advances ten game-days, saves and reloads, advances ten more, creates a player frontier claim and faction, crosses a war notice boundary, resolves the war, verifies the losing player's territory and claim owner move to the winning faction, then saves/reloads and compares canonical state hashes. During the manual walkthrough, inspect the Map overlay too.
- **Expected:** The full sequence completes without a crash; the post-load campaign is continuous; the war transfers the player-owned territory and rewires its claim; the final save/load preserves the new world state.
- **Proof:** Phase 4 “Playable when”; Tasks 6.2, 6.3, 7.1, 8.1–8.5; `TestWar_OutcomeSetsRegionFlags`, `TestWar_TitlesTransferOnLoss`, `TestSave_RoundtripHashEqual`, `TestSave_RngContinuationIdentical`.
- **Result:** [ ] Pass [ ] Fail [ ] Blocked

## Sign-off

Both signatories must execute all 40 scenarios from a fresh build and attach an evidence bundle (logs, screenshots where useful, seed/preset names, and save hashes for the save/load scenarios). A blank sign-off is not a pass.

| Role | Name | Build / commit | Date | Result | Evidence bundle |
|---|---|---|---|---|---|
| Engineer |  |  |  | [ ] Green [ ] Failed |  |
| Independent tester (never worked on Harrowmere) |  |  |  | [ ] Green [ ] Failed |  |

## Execution boundaries

1. The deterministic `--scenario A01` through `--scenario A40` registry is implemented in `harrowmere_soak`. Fixture presets use the real stores and save API; A01–A18 and A37–A38 intentionally start a fresh seeded campaign because the player performs the interaction during the walkthrough.
2. The Options screen/settings integration and the sample species/recipe data mod plus compiled native fixture consumer are implemented in this tree. The options UI persists its quality preference; the current renderer has no runtime quality setter, so a graphics-quality change applies to renderer configuration on a subsequent startup rather than being claimed as an immediate visual change.
3. The automated preflight and tests do not substitute for manual player-chair execution. The sign-off table remains blank until one engineer and one independent tester each complete all 40 scenarios on a built runtime and attach their evidence bundle.
4. The complete automated gate HAS RUN green in this tree (2026-11-03, two-core Release build): configure → build (warnings-as-errors) → core CTest (400 cases + the one-year PR soak) → UI CTest → bootstrap smoke → graph sanity → the 8.4 balance measurement → this preflight (40 presets, A36, A40) → the nightly three-game-year soak (36/36 months, 15,768,000 ticks, RSS flat at 26 MiB). The first real run surfaced and fixed two 7.1/8.2 defects (the loader discarded the justice id counters into a temporary; the canonical hasher hashed the raw counter map instead of the persisted peek pair) and the missing trust wiring for the sample mod and the settings/hot_reload modules — recorded in DEVIATIONS.md (2026-11-03, task 8.5 rows). That automated battery does not substitute for manual player-chair execution. The sign-off table remains blank until one engineer and one independent tester each complete all 40 scenarios on a built runtime and attach their evidence bundle.
