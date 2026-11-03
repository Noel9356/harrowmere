# src/progression — the XP economy engine

Task 2.3's card (with 2.2's species side living in `src/character/`). The design reference's (progression.md) math, line for line: precomputed cost/cap tables, the soft-cap effectiveness function, the literal refund (D-REFUND-1), peaceful/respec gating, level/stat/skill purchases, the D-LORE-1 mana rule, and class-gated skill requirements.

| File | Owner | Content |
|---|---|---|
| `progression.hpp` | task 2.3 | the pinned API: `precomputeCosts` / `precomputeCaps` (constexpr tables), `levelCosts` / `statCosts` / `skillCosts` / `statCaps` / `skillCaps` (const-ref views), `getEffectiveness`, `getRefund` (D-REFUND-1, literal), `isPeaceful` / `canRespec` / `canRespecStat` / `canRespecSkill`, `buyLevel` / `buyStat` / `buySkill`, `respecStat` / `respecSkill`, and the D-LORE-1 mana helpers |
| `progression.cpp` | task 2.3 | the §Progression / §Levels / §Stats blocks: tables, effectiveness, refunds, peaceful gating, `buyLevel`, `buyStat`, the mana rule, `respecStat` |
| `skills.cpp` | task 2.3 | the §Skills block: `getSkillEffectiveness`, the requirement checks (reqStats / reqSkills / reqActions from classes.json), `buySkill`, `respecSkill` |
| `power.hpp` | task 2.5 | the cached power pipeline's seam: `markPowerDirty` (the dirty-flag call every purchase, respec and equip path fires) plus the doc-verbatim `getCurrentPower` (NON-const character — the doc's own pseudocode mutates the cache fields; no const overload exists, by the card's pitfall), `getPotentialPower` (isqrt of totalXp) and `getMatchmakingScore` (the 8:2 peak/potential blend) |
| `power.cpp` | task 2.5 | the pipeline itself: the A-20/A-21 weight arrays built from constants.hpp's named rows (static_assert-tied), the cache/watermark write-back in the doc's order, and the audit-backed call-site discipline (TestPower_DirtyCallSites_Complete) |

## The pinned decisions this code encodes

- **D-REFUND-1**: `getRefund` is LITERAL — `costs[point-1]`, the last point's cost, not the sum. Suspected design gap, implemented as written, flagged in `docs/DEVIATIONS.md`, pinned by `TestRespec_RefundsLiteralLastCost`.
- **Level indexing**: `levelCosts[i]` is the XP to advance from level `i` to `i+1` (design reference §Levels; the first purchase, 1→2, costs `levelCosts[1]` — see the DEVIATIONS.md row on A-7's "why" paraphrase).
- **D-LORE-1**: stat points above `STAT_THRESHOLD` (A-12) require ARCANA effectiveness ≥ `point - STAT_THRESHOLD`; a shortfall rolls a chance of `min(SCALE, shortfall × 2000)` on the pinned `combat` stream, and failure consumes the XP and deals `maxHp × (5 + shortfall/500) / 100` pain (death routes through 3.3). Skills never fail.
- **Caps and the knee**: the v1 caps (A-13..A-16: 10 + level/5, max 30) bind below the threshold (50), so the sqrt branch and the mana rule are unreachable through normal v1 progression — both are implemented verbatim (the design source's own arithmetic) and re-enter via data (raise the caps). DEVIATIONS.md 2026-10-31 carries the row.
- **Level purchases never dirty the power cache** (design reference: "level changes do not enter the formula"; 2.5's audit test pins the negative).
- **R1/R4**: `availableXp` is i64; every cost is widened signed before negation and every delta flows through `applyXpDelta` (the −MAX_XP floor). `totalXp` never decreases.

## The test harness

`tests/test_progression.cpp` pins the card's eleven named tests; the species side of the math lives in `tests/test_species.cpp` (task 2.2).
