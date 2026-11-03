# Phase 4 — Reign hand-over

Date: 2026-11-03 Repository baseline: the git remote (github.com/Noel9356/harrowmere) cloned and re-verified in this environment; upstream Noel9356/engine HEAD has since moved past the pinned commit (see "Upstream sync boundary"). Plan basis: `common.md` revision 16 + `phase-4-reign.md`.

## Phase 4 implementation status

| Card / gate | Implementation in this tree | Verification status |
|---|---|---|
| 6.2 War | `src/factions/war.{hpp,cpp}`; declaration, notice/active boundary, outcomes, title/owner transfer, collapse and treaty | `test_war.cpp` green in the full CTest run (2026-11-03) |
| 6.3 Claims | `src/factions/claims.{hpp,cpp}`; owner, custom laws, safe-zone protection, two-clock abandonment and dismantling | `test_claims.cpp` green in the full CTest run (2026-11-03) |
| 7.1 Save/load | Versioned full-state save/load and deterministic hash seams; load-path id-counter restoration fixed this pass | `test_save_load.cpp` green in the full CTest run (2026-11-03) |
| 7.5 Data/native mod surface | `src/data/mod_overrides.{hpp,cpp}`; manifest discovery; sorted package-id overlays; D-MOD-2 keyed entity merging; merged registry/recipe/quest validation; sample `mods/example_species_mod/`; compiled native view-consumer fixture | `test_mod_overrides.cpp` + `test_native_mod_consumer.cpp` green in the full CTest run (2026-11-03) |
| 7.6 Options | Persistent settings-backed volume, quality preference, UI scale/verbosity and key remaps; `settings` + `hot_reload` capabilities; input mapper reads remaps at startup and applies hand-edited settings reloads | `test_options.cpp` green in the full CTest run (2026-11-03); trust wiring for the newly active modules added this pass |
| 8.1 Determinism | Canonical hasher and deterministic tick/worldgen tests | `test_determinism.cpp` green in the full CTest run (2026-11-03); hasher's ids block aligned with the persisted field set (see DEVIATIONS.md 2026-11-03) |
| 8.2 Save/load fuzz | Truncation/corruption refusal and double-roundtrip property tests | `test_save_fuzz.cpp` green in the full CTest run (2026-11-03) — the first real run exposed the loader's discarded justice id counters; fixed this pass |
| 8.3 Headless soak | Real tick-pipeline soak executable; PR one-year and nightly three-year CTest entries | One-year soak green (174 s, ~30k ticks/s); nightly three-game-year soak green (36/36 months, 15,768,000 ticks in 436 s, RSS flat 26 MiB, 2026-11-03) |
| 8.4 Balance | `docs/balance_report.md`, pinned statistical tests and unchanged Appendix A constants | `--balance` probe green end to end (2026-11-03); its headless structural reads reproduce the report's known-notes |
| 8.5 Acceptance | `docs/acceptance_walkthrough.md` (40 scenarios), `harrowmere_soak --scenario A01..A40`, and `scripts/acceptance.sh` | `scripts/acceptance.sh --ci` green (2026-11-03): 40-record preflight, registry validation, A36 + A40 fixtures; all 40 presets executed headless rc=0; both human sign-offs still required |

## Task 7.5 details

`harrowmere_core` discovers package directories under the configured mods folder using the kernel's `discoverModPackages` rule. It skips its own package and packages without `data/`, then passes the other active packages to `materializeMergedData`. The merger sorts by manifest id, applies core data first, and materializes a staging directory consumed by the existing typed loaders. The registry validates species/items/laws/roles/crime/event data; startup then runs the goods, recipes and quests loaders against that same merged directory, including their cross references. Source mod directories are never modified.

D-MOD-2 arrays merge by stable identity (`key`; `crime_data.json` uses its actual schema discriminator `type`, recorded in `DEVIATIONS.md`). Matched rows deep-merge, new rows are added deterministically, and non-entity arrays retain the ordinary replace-array behavior. The sample package adds the `mossling` species and `craft_mossling_wrap` recipe; because recipes are not a D-MOD-2 entity array, its sample `recipes.json` contains the complete resulting recipe table. The fixture consumer compiles against the published `harrowmere_capability.hpp` and reads world/player views without including simulation stores.

## Task 7.6 details

`harrowmere_ui` requires the engine `settings` and `hot_reload` capabilities. Its package `config.toml` contains defaults for every exposed option and its manifest declares each consumed configuration key. On first run, defaults seed the independent `settings.toml` only when the user has no saved value. The Options window is opened with F2. Volume is applied through the optional audio capability when present; key remaps feed the input mapper and are refreshed on settings-file changes; UI scale is applied to ImGui; verbosity controls additional panel guidance. The graphics quality preset is persisted and survives restart, but the current renderer capability has no runtime quality setter, so the panel labels it as a startup/configuration preference rather than claiming an immediate renderer change. Campaign save data is not used for options.

## Task 8.5 acceptance walkthrough

The walkthrough has 40 scenario records with setup, steps, expected observations, design proof, automated backstops and result fields. The headless harness exposes all 40 identifiers through `--list-scenarios` and accepts `--scenario A01` through `--scenario A40`. State-specific presets use simulation stores and subsystem APIs; `--scenario-save` writes the setup via the ordinary save API. A40 executes ten game-days, save/load, ten more game-days, a declared and resolved war, and a final save/load hash check.

The sign-off table is deliberately blank. Task 8.5 requires one engineer and one independent person who has never worked on the codebase to execute the full player-chair script. A script or unit test cannot truthfully sign for those people.

The AUTOMATED half of the gate has run green in this tree (2026-11-03): `scripts/acceptance.sh --ci` validates the 40-record document, the assembly validate/graph shape, the 40-preset registry, and executes the A36 and A40 fixtures; each of the 40 presets was additionally run headless (rc=0) and A39's `--scenario-save` wrote a valid versioned save through the real save API. The first real build also surfaced three integration defects that the static checks could not see — fixed this pass and recorded in DEVIATIONS.md (2026-11-03, task 8.5 rows): the load path restored the justice id counters into a temporary and discarded them; the canonical hasher hashed the raw counter map instead of the persisted peek pair; and the assembly's `[trust]` section had not covered the sample mod or the settings/hot_reload modules the options panel activates, so every headless boot failed the trust gate.

## One-command CI gate

`scripts/ci.sh [build-dir]` runs configure, build, core CTest (including the newly registered 8.1/8.2 test files and the one-year soak), UI CTest, bootstrap smoke, graph/validation checks, balance measurement, and the A40 acceptance campaign. `.github/workflows/ci.yml` (added this pass — the README and this document already named it) invokes this gate on pushes and pull requests and schedules the full three-game-year soak daily. Set `HARROWMERE_FULL_NIGHTLY=1` to include the three-game-year soak locally. `scripts/acceptance.sh --check-only` validates the 40 scenario records without a build; `scripts/acceptance.sh --ci [build-dir]` performs runtime validation, graph sanity, scenario-registry validation and A40.

## Verification in this environment

The full gate ran green in this environment on 2026-11-03 (two-core Release build, GitHub reachable for the pinned FetchContent dependencies — Vulkan-Headers v1.4.365 + Vulkan-Hpp, GLFW 3.4, Lua 5.4.8 hash-verified, sol2, toml++, glm): configure → build (warnings-as-errors, zero warnings) → core CTest (400 test cases, 291k assertions) + the one-year PR soak (174 s) → UI CTest → bootstrap smoke (headless boot exit 0, pinned startup line, graph shape, one world claimant) → `--validate` → `--print-graph` sanity → the 8.4 balance probe → `scripts/acceptance.sh --ci` (40-record preflight, registry validation, A36 + A40) → the nightly three-game-year soak (36/36 months, 15,768,000 ticks in 436 s, RSS flat at 26 MiB).

The first real compile-and-run surfaced three integration defects the previous pass's syntax-only checks could not see. All three are fixed in this tree and recorded as DEVIATIONS.md rows (2026-11-03, task 8.5):

1. `src/persist/load.cpp` restored the justice evidence/warrant id counters into a local `JusticeStore` and then moved only the doc maps into the real store — every loaded world re-issued ids from FIRST_ID and failed the 8.2 double-roundtrip property (the milestone fixture with live evidence diverged). The counters now load into the real store.
2. `src/persist/state_hash.cpp` hashed the raw counter map; the save schema persists the peek pair, and "never drawn" (no map entry) versus "next id = start_" (an entry at start_) hash differently while being behaviorally identical and indistinguishable in the file. The hasher now hashes the persisted peek pair — the 8.2 property still catches a lost counter (values differ) but no longer flakes on never-drawn spaces.
3. `config/harrowmere.toml`'s `[trust]` section covered only the two original game mods. The 7.5 sample mod (a native package) and the settings + hot_reload modules (active since 7.6 required their capabilities) also draw trust advisories, so every headless run failed startup. All three are trusted standing in the assembly config, with the rationale in place.

Static checks previously recorded still hold: shell syntax on the scripts, the 40×5 acceptance document preflight, every walkthrough-cited test name re-verified against the source declarations (55 names), JSON/TOML parsing, and ZIP integrity on the delivery archive.

## Balance and performance notes (8.4 / 8.3)

`mods/harrowmere_core/docs/balance_report.md` records seed `20261102`, one game-year, 12 months / 5,256,000 ticks, and no Appendix A constant changes. It records these limitations instead of tuning misleading fixture outputs:

- Matching-level kill XP produces about 12.7 kills per ten levels at every anchor; the A-120 doubling curve needs reward-table composition changes, not a magic numeric constant.
- The headless economy pins most prices at the floor and yields Gini 0.000 because buyer/trade/event-payout drivers are player-facing and absent from that fixture.
- Mixed event outcomes exercise the regulation band, while the all-success headless mix saturates the rate at SCALE.

The existing 8.3 deviation notes report approximately 168 seconds per one-game-year run (~31k ticks/s) and 379 seconds per three-game-year run (~42k ticks/s) on the recorded two-core CI machine after the performance fixes. A per-step profile report is not checked in; capture `--ticks` output on the release/reference machine before certifying per-step budgets.

## Deviations digest and deferred register

`mods/harrowmere_core/docs/DEVIATIONS.md` has 128 dated rows in this snapshot (125 from the previous pass + three 2026-11-03 task-8.5 rows). The 8.3 entries describe the real-pipeline performance changes and persisted per-NPC continuation state; the 8.4 entry explains the balance probes and why no constant was changed; the 2026-11-03 rows record the three integration defects the first real build/run surfaced and fixed (loader id-counter restore, hasher alignment with the persisted ids block, trust wiring for the sample mod and the settings/hot_reload modules). The task 7.5 identity-field exception for `crime_data.json` is also recorded there.

Appendix E was reviewed against the checked-in seams. The register remains intact: server/networking remains outside the single-player boundary; further quests/events/goods/recipes remain data extensions; infection, mana pools, realms, gamepad/localization and save migration remain deferred. Re-entry anchors include `SimCapability` for authoritative-sim consumers, `src/quests/` and event templates for content breadth, `src/factions/claims.*` and `war.*` for territorial systems, and `src/persist/load.cpp` for the version-refusal/migration boundary. No deferred feature was silently promoted.

## Upstream sync boundary

The handout records upstream `Noel9356/engine` HEAD `94108c0` (2026-10-29); the README pins that commit reference. GitHub was reachable this pass and the finding is: the engine remote's current HEAD (e5c475e, 2026-11-03, "update") carries a refactored kernel (game_mod_api / module_descriptor / trust_gate extraction and a different kernel header set), and the pinned `94108c0` tree is no longer fetchable from that remote ("couldn't find remote ref") — the upstream history was replaced, so a live byte-for-byte diff against the pinned engine commit is no longer possible. The surviving anchor is this repository's own initial import (8b89ab0, the specialize-in-place starting point): against it, the root `CMakeLists.txt` is byte-identical and `kernel/` differs ONLY by the documented 2026-10-31 `floatingValue` integer-coercion fix (the DEVIATIONS.md task-`engine` row mirrored upstream). A stray whitespace-only reflow in `kernel/include/engine/kernel/parallel_for.hpp` was reverted this pass so the stock zone carries exactly that one documented delta. The upstream refactor at the new engine HEAD is a future sync decision for the owner — it is NOT this tree's pin, and this pass did not chase it.
