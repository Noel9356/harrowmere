# Balance report — task 8.4

*The card's instrumented pass over the SAME real tick pipeline the soak drives. Nothing here was measured on a second sim, a shortcut loop, or a fixture rig: the read is `harrowmere_soak --balance` — the full soak loop with the (a)–(f) probes sampling the living stores at the month boundaries (the equilibrium pool daily).*

Run: seed `20261102`, 1 game-year (12 game-months, 5,256,000 ticks), the PR-soak window. In-suite backstops: `TestBalance_LevelCurve`, `TestBalance_EventEquilibrium`, `TestBalance_ConvictionRatesByLawSet` (generous bands, the same pinned seed).

**Verdict: GREEN — no Appendix A constant changes.** Every measured curve sits inside its target or traces to a data/content shape rather than a mis-tuned constant; four known-notes record where the intended experience curve depends on content the v1 data does not compose yet. Each is listed with the lever that owns it and the reason the balance pass left it alone (the card's rule: adjust only through Appendix A + data, never a code literal).

---

## (a) Time-to-level — the A-120 curve

The card's target curve: ~15 game-days per 10 levels early, doubling per band (A-120).

**Measured (cost side, the A-7/A-8 table at the four anchors):**

| anchor | XP over the next 10 levels | per-band cost growth |
|---|---|---|
| L1 → L11 | 2,298 | — |
| L25 → L35 | 64,949 | 28.3× |
| L50 → L60 | 2,136,930 | 32.9× |
| L75 → L85 | 70,344,431 | 32.9× |

**Measured (income side):** the playerless world's NPC XP income is structurally zero (no paying event spawns headless, nobody dies) — 0 XP/day across ~1.83 M early-band npc-days. The income side is the adventurer's faucet model, and the pass measured its one sim-side constant through the REAL death path: a player kill of a level-8 victim paid **480 XP — exactly A-35's 0.8 × the A-34 loss (1.6 × levelCosts[8])**. `MATCH`.

**The curve read.** The kill faucet scales with the victim's level cost, and the victim's level cost scales with the same `1.15^L` the cost table grows by — so under matching-level hunting the two cancel exactly and the curve reads **~12.7 matching-level kills per 10 levels at EVERY anchor** (the geometric identity: band sum 20.3 × level cost ÷ the 1.6 payout factor). At one hunt per game-day that is ~12.7 game-days per 10 levels, flat across all four bands. The early band therefore MEETS the A-120 target (~15 game-days); the **doubling does not emerge, and cannot be made to emerge from a constant**:

- under the self-scaling kill faucet, days-per-band stay flat (1.0× per band);
- under a level-flat income (the quest/event reward tables — data rows), days grow `1.15^10 = 4.05×` per band;
- the A-120 doubling (2×) sits between: **~a third of the late-band XP income must be level-flat** for the curve to double per band.

**Known-note (a).** The doubling is a content composition (how much flat XP the quest and event reward tables carry at late bands), not a tunable constant. A-7/A-8's cost table and A-34/A-35's faucet are the doc's own curves and measure exactly as designed; the pass made no change. The composition lands when the reward-table content does (per D-MOD-2, reward magnitudes are data rows).

## (b) Power distribution across the NPC population

The card's check: no runtime-reachable peak-stage NPCs before the age bands allow.

**Measured:** max NPC level **10** across all 12 months (= `STARTING_LEVEL_MAX`, the worldgen ceiling); peak-stage rows (76+) **0** in every monthly sample; the level histogram holds the entire population in the early band all year (4,753 → 6,154 living NPCs — the growth is the lifecycle's birth surplus, the soak's own invariant tracks it).

**Verdict: GREEN.** No runtime path lifted an NPC past the worldgen band inside the window; the peak stages stay behind the age bands as pinned.

## (c) Economy price stability + wealth concentration

**Prices.** 60 region-good price rows sampled monthly against the D-ECON-1 band. The soak's monthly invariant (every published price inside `[base/4, base×4]`) passed 12/12 months. The stability probe's stricter interior read — the share of prices inside `[base/2, 2×base]` — reads **0%** with median/p95 pinned at the band's floor.

The mechanism, traced: the headless supply side (each farmer adds `FARMER_OUTPUT_PER_GAME_DAY` = 8 grain × the season yield daily) outruns the consumption side (population × `perCapitaNeed` 2.0 grain/day) by enough to drive every region-good's demand−supply ratio to the full-pressure clamp, and the D-ECON-1 floor's integer division (`base/4`) bottoms at **0** for the low-base goods (grain's baseValue is 2 → floor 0). The band invariant holds trivially at the floor; the interior band is never exercised.

**Known-note (c-1).** This is a data-plane magnitude, not a constant: the levers are the farmer share of the role mix, `FARMER_OUTPUT_PER_GAME_DAY`'s data-adjacent value, and the goods' `perCapitaNeed`/`baseValue` rows — all data ("content breadth is data", the phase's own seam rule). Tuning them against a world with NO buyer agents (the trade paths are the player-facing 3.7 seams; NPC purchasers do not exist in v1) would optimize for an economy nobody plays. Documented, not adjusted; a `baseValue` rescale (×1000) would also ripple every fine/bounty/wage relationship and belongs to a content pass that can re-price the whole currency side.

**Wealth concentration.** The adult cohort's Gini reads **0.000** (n 4,292 → 4,327) — purses are flat. The headless wealth drivers (wages from territory wealth) pay uniformly; the spreading drivers (trade income, event payouts, fines) are player-facing seams with no headless call sites.

**Known-note (c-2).** Gini < 0.7 holds trivially; the spread the doc's wealth game wants arrives with the player-facing money paths. Nothing to tune — the measurement just cannot see the spread headless.

## (d) Crime rates — convictions per 100 player-crimes, by law set

Identical scripted trials in settlements of every shipped law set, walking the real pipeline (step-10 `applyDamage` → 4.1's real evidence → 4.2's case ticks → 4.3's surrender). Sightings ride the pinned 3.9 store seam under the A-55 rule (the NEAR-tier co-presence driver itself never shipped — known-note (d-3) below). Two arms:

**The crowd arm** (the market punch — the densest witness crowd within 40 m of the settlement center, ~60–300 qualifying NPCs):

| law set | assault | murder |
|---|---|---|
| tolerant_city | 24/24 convicted (100%) | 16/16 convicted (100%) |
| safe_zone_laws | 24/24 convicted (100%) | 16/16 convicted (100%) |
| default | 24/24 convicted (100%) | 16/16 convicted (100%) |

**The thin arm** (the thresholds' operating read — the victim plus exactly ONE witness, staged 150–400 m out of the center inside the same region cell):

| law set | assault conviction (1 witness) |
|---|---|
| tolerant_city (threshold 7000) | 9/16 = 56% (7 investigating) |
| default (threshold 6000) | 12/16 = 75% (4 investigating) |
| safe_zone_laws (threshold 6000) | 16/16 = 100% |

The law sets differentiate exactly where they should: the crowd arm saturates (the aggregate report weight clamps at SCALE ≥ every shipped threshold — a market punch convicts everywhere), while the single-witness band spreads 56%–100% across the 6000–7000 thresholds — tolerant_city's extra 1000 reads as 19 points of conviction rate. safe_zone_laws' 100% against default's 75% at the SAME threshold is witness-mix sensitivity, not law text: the safe-zone villages' cast drew heavier witnesses (vendor/guard role weights) — the case scores, not the thresholds, moved. The report keeps both readings honest rather than averaging the mix away.

**The species-conflict spot-check (the doc's flavor):** a beastkin murdered inside a xenophobic_village law set has its MURDER evidence row REFUSED outright (the hard gate fires before the evidence exists), while the same kill on a human victim records and convicts. **HELD.** (When the seed's 10%-per-kingdom variance roll draws no xenophobic settlement, the probe installs the shipped law set on one default village for the check and restores it after — the law SET is the subject, not the worldgen draw.)

**The repeat-offender ramp:** the same single-witness scene with the player's notoriety floor escalating in the assault's own notorietyGain steps (0 → 27,000; the damper is `threshold × SCALE/(SCALE+notoriety)`): convictions **3/5 → 5/5** (the two early misses read INVESTIGATING — cases below the face threshold — and cross as the floor climbs). The rate only rises, as the damper demands. **HELD.**

**Verdict: GREEN.** Convictions classify by law set, the hard gate and the damper hold, and the threshold band operates where the crowd weight does not saturate it.

**Known-note (d-3).** The NEAR-tier co-presence driver (the tick that would record sightings for co-present strangers) has no shipped call site — without it no witness can know a stranger's identity and every law set would measure the same structural zero. The probe scripts the sightings through 3.9's public store seam under the pinned A-55 rule (the same records the driver would write); the driver itself stays a deferred seam.

## (e) Bounty-vs-penalty invariant

Pure table math over the shipped crime rows (the D-BOUNTY-1 bounty `min(cap 50000, severity × 5)` against the conviction's currency leg `severity × 10` plus the XP debt and the jail term):

| crime | severity | bounty | fine |
|---|---|---|---|
| MURDER | 9000 | 45,000 | 90,000 |
| ASSAULT | 4000 | 20,000 | 40,000 |
| THEFT | 2000 | 10,000 | 20,000 |
| ARSON | 6000 | 30,000 | 60,000 |
| TRESPASS | 1000 | 5,000 | 10,000 |
| POACHING | 1500 | 7,500 | 15,000 |
| SMUGGLING | 3000 | 15,000 | 30,000 |
| OBSTRUCTION | 3000 | 15,000 | 30,000 |

**Verdict: GREEN — the bounty stays strictly below the conviction cost for every shipped severity** (the rate ratio is 2×; the XP debt and jail stack on top). No exploit window where surrendering to the bounty beats eating the conviction.

## (f) Event difficulty equilibrium

The card's target: successRate hovers 3000–7000 over time — the doc's regulation loop works. Two books sampled daily across the year (the drained per-attempt book is the honest population: a state's successRate moves at its attempt's conclusion and the event leaves the book the same tick):

- **drained (post-attempt): n = 2,336, p10 = p50 = p90 = 10000** — every concluded attempt in the year succeeded (declared transitions; the failure arm — the forced exit — never fired headless);
- **live book: n = 2,288, all 5000** — the template-seeded states, read before their first (and only) attempt concludes.

**Verdict: the regulation ARITHMETIC is verified; the headless EQUILIBRIUM is saturated.** The loop's math is the doc's own (signed delta ÷ attempts — R1) and is unit-pinned by the 5.2 battery; the in-suite `TestBalance_EventEquilibrium` additionally pins the equilibrium shape through the real `noteStateAttempt` + the 8.4 drain seam: a MIXED outcome stream converges into the doc's 3000–7000 band from the third attempt on (5000 → 10000 → 5000 → 6667 → 5000 …), while an all-success stream walks to SCALE and pins — the mechanism the headless read shows.

**Known-note (f).** The band is outcome-mix-dependent, not a tunable constant: the outcome mix is emergent sim shape (the headless templates resolve by declared transitions), and no Appendix A row governs it. The band's intent — the loop regulates toward the observed success fraction — holds by construction; the pass leaves it documented rather than injecting artificial failures.

---

## Constant changes

**None.** Each out-of-band reading was traced to its owner and left:

| candidate | traced to | decision |
|---|---|---|
| A-120 doubling absent | content composition (flat-XP share), not a constant | known-note (a) |
| price interior band unexercised | data magnitudes (role mix, farmer output, goods rows) + no buyer agents headless | known-note (c-1) |
| Gini 0.000 flat | wealth drivers are player-facing seams, no headless call site | known-note (c-2) |
| successRate saturated at SCALE | all-success headless event mix, emergent | known-note (f) |
| bounty-vs-penalty, peak-stage, exemption, damper, thresholds | — | GREEN as pinned |

## Two probe-found defects fixed en route (deviations rows 2026-11-02/8.4)

The instrumentation is only as good as the world it reads; the probe caught two live bugs that would have invalidated the (c)/(d) reads, fixed in this pass (both rows in DEVIATIONS.md):

1. **Bootstrap NPCs spawned with `integrity = 0`** (the worldgen draw never reached the bootstrap population) — every living witness's report weight (role × integrity × affinity span) was zero, which would have read as "no law set ever convicts" instead of measuring the thresholds. Fixed: the seed's integrity draw flows into the bootstrap.
2. **Village law sets were never seeded** (the stage-3 walk only visited settlements whose parent was the kingdom — villages hang off their CITY, so every regular village carried a null law set and inherited its parent city's `tolerant_city` rows through the `lawsAt` walk) — no generated world contained a single default-law settlement, and the (d) probe's law census would have had nothing to differentiate. Fixed: the walk seeds every settlement against its own template.

## How to reproduce

```
./build/runtime/harrowmere_soak --balance                 # the 1-game-year instrumented run
./build/runtime/harrowmere_soak --balance --game-months 3 # a shorter window
ctest --test-dir build -R harrowmere_core_tests           # the in-suite backstops
```
