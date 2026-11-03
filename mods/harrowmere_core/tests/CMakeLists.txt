# mods/harrowmere_core/tests/CMakeLists.txt - the test harness (task 0.6).
#
# Builds `harrowmere_core_tests` (doctest v2.4.12, vendored single header at
# third_party/doctest) and registers it with CTest. The target links the
# mod's compiled OBJECTS - never the runtime mod library - so tests execute
# without the engine's plugin scanner (card 0.6's pitfall, verbatim:
# "Don't link tests against the *mod runtime*; link objects").
#
# One test binary for the whole phase: every card's test file is listed here
# exactly once, and main.cpp carries the SINGLE doctest implement define. A
# test file defining its own main is a link error once all files join this
# binary - keep new test files free of DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN.
#
# ctest discovery is directory-local (the stock root CMakeLists does not call
# enable_testing; see DEVIATIONS.md task 0.7 row): run
#     ctest --test-dir build/mods/harrowmere_core/tests --output-on-failure
# scripts/ci.sh (task 0.6) wires exactly that into the one-command gate.

set(HARROWMERE_TEST_SOURCES
    main.cpp
    test_math.cpp                # task 0.2 - math helpers, IdGenerator, Result
    test_clock_rng.cpp           # task 0.3 - WorldClock + SeedRng
    test_data_loader.cpp         # task 0.4 - strict JSON, schema DSL, merge, config
    test_mod_overrides.cpp       # task 7.5 - deterministic package discovery order + D-MOD-2 keyed merges
    test_determinism.cpp         # task 8.1 - worldgen and scripted tick state hashes
    test_save_fuzz.cpp           # task 8.2 - milestone roundtrips, truncated/corrupt save refusal
    test_native_mod_consumer.cpp # task 7.5 - fixture third-party consumer of published harrowmere.core views
    fixtures/native_consumer.cpp # the fixture mod code, compiled against the published header only
    test_events.cpp              # task 0.7 - sim event channels
    test_enums_data_loaders.cpp  # task 1.1 - closed enums + species/items/classes
    test_data_loaders_2.cpp      # task 1.2 - crime/laws/roles/territory/suspicion
    test_registry.cpp            # task 1.3 - DataRegistry load order + cross-refs
    test_world_laws.cpp          # task 1.4 - world model: lawsAt/socialClassOf/jurisdiction
    test_worldgen.cpp            # task 1.5 - world generation from seed
    test_spatial_grid.cpp        # task 1.6 - the A-39 spatial index
    test_seasons.cpp             # task 3.8 - the season clock + weather machine
    test_player_control.cpp      # task 3.10 - intents, kinematic transform, AFK, action log
    test_scene_bridge.cpp        # task 7.2P1 - the static scene bridge
    test_character_store.cpp     # task 2.0 - the doc-verbatim Character + CharacterStore
    test_status.cpp              # task 2.1 - derived statuses, consent, cooldowns
    test_species.cpp             # task 2.2 - species application
    test_progression.cpp         # task 2.3 - the progression engine
    test_items.cpp               # task 2.4 - inventory, gear scores, equip/consume, crafting
    test_power.cpp               # task 2.5 - the power pipeline, the audit, matchmaking
    test_appearance.cpp          # task 2.6 - appearance cost/cooldown/A-25 suspicion
    test_consent.cpp             # task 2.7 - duels & arenas (the consent framework)
    test_scheduler.cpp           # task 3.1 - the 14-step tick pipeline
    test_combat.cpp              # task 3.2 - the damage pipeline & attack resolution
    test_death.cpp               # task 3.3 - death paths, XP transfer, isCriminalAct
    test_npc_lifecycle.cpp       # task 3.4 - birth/aging/needs/migration
    test_lod.cpp                 # task 3.5 - the LOD hysteresis & tier workloads
    test_roles.cpp               # task 3.6 - roles & critical replacement
    test_economy.cpp             # task 3.7 - regional economy (D-ECON-1 prices)
    test_player_view_fill.cpp    # task 2.8 - the runtime adoption + the PlayerView fill
    test_relationships.cpp       # task 3.9 - affinity, sightings, witness qualification
    test_reputation.cpp          # task 4.4 - reputation/notoriety: lazy decay + propagation
    test_evidence.cpp            # task 4.1 - evidence, witnesses, delayed reports, bribery
    test_cases_warrants.cpp      # task 4.2 - case state machine, warrants, jurisdiction, sweeps
    test_conviction.cpp          # task 4.3 - conviction, sentencing, jail, surrender, escape
    test_suspicion.cpp           # task 4.5 - suspicion memory, tiers, decay, effects
    test_event_engine.cpp        # task 5.1 - the event engine lifecycle + rules
    test_event_difficulty.cpp    # task 5.2 - the difficulty loop (signed delta, D-DIFF-1 cadence)
    test_event_contribution.cpp  # task 5.3 - validation guards, contribution math, snapshots
    test_event_rewards.cpp       # task 5.4 - the single-pass payout + the MANHUNT completion
    test_event_effects.cpp       # task 5.5 - the D-FLAGS-1 split + the reconquest template
    test_enforcement.cpp         # task 4.6 - guard odds, signals, lawless territories
    test_citizen_justice.cpp     # task 4.7 - the gate, militias, bounties, massacres
    test_factions.cpp            # task 6.1 - membership, relations, founding, dissolution, drift
    test_war.cpp                 # task 6.2 - the five casus belli, the notice window, collapse, treaty, outcomes
    test_claims.cpp              # task 6.3 - claiming, custom laws, protections, abandonment, dismantling
    test_matchmaking.cpp         # task 6.5 - duel proposal, anti-abuse regressions, event seeding
    test_quests.cpp              # task 6.4 - quests as event instances, payouts, the checklist
    test_society_views.cpp       # task 7.3b - the society view fills (the live whitelist)
    test_save_load.cpp           # task 7.1 - the roundtrip, dirty flags, RNG continuation, atomic tmp, version refusal
    test_terrain.cpp             # task 5.1.2 - terrain relief: heightAt purity, determinism, region-edge gradient cap
    test_capture_scenarios.cpp   # task 5.1.1 - the capture suite: scenario list, PNG smoke, budget report parser
    capture_scenarios.cpp        # task 5.1.1 - the capture suite implementation (scenario list, run, report parse)
    test_character_visuals.cpp   # task 5.1.3 - characters beyond the token: silhouette, yaw, bob, corpse, no-sim-state
    test_atmosphere.cpp          # task 5.1.4 - sky/sun/weather: sun tracks phase, anchors interpolate, fog, exposure, season
    test_scatter.cpp             # task 5.2.1 - scatter: determinism, density, water/slope rejection, footprints, cap, save/load
    test_borders.cpp             # task 5.2.2 - water/coasts/borders: ribbon projection, owner color, recolor, safe style, no-sim-writes
    test_image_budget.cpp        # task 5.3.1 - the image budget gate: percentiles, escalation ladder, profiler citations
    test_soak.cpp                # task 8.3 - the invariants hold (1-game-year smoke; the nightly run drives soak_main)
    test_balance.cpp             # task 8.4 - the balance verdicts (level curve, event equilibrium, conviction rates by law set)
    soak_main.cpp                # task 8.3 - the headless harness (library mode here: HARROWMERE_SOAK_NO_MAIN)
)

add_executable(harrowmere_core_tests ${HARROWMERE_TEST_SOURCES})

# Objects, not the runtime library (see the header comment).
target_link_libraries(harrowmere_core_tests PRIVATE harrowmere_core_objects)

# doctest v2.4.12 (vendored): SYSTEM so warnings-as-errors never fires inside
# the vendor header. Both include roots are exported because the test files
# use both spellings (<doctest.h> and "doctest/doctest.h").
target_include_directories(harrowmere_core_tests SYSTEM PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/doctest"
    "${CMAKE_SOURCE_DIR}/third_party"
)

# Warnings-as-errors on the game's own test translation units (global DoD,
# common.md section 1.5).
if(MSVC)
    target_compile_options(harrowmere_core_tests PRIVATE /W4 /WX)
else()
    target_compile_options(harrowmere_core_tests PRIVATE -Werror)
endif()

# Compile-time marker so game code can gate test-only seams (reserved for
# later tasks; none of the Phase-1 cards consume it yet).
target_compile_definitions(harrowmere_core_tests PRIVATE HARROWMERE_TEST=1)

# soak_main.cpp compiles as a LIBRARY inside the test binary (its main() is
# for the standalone nightly binary below).
target_compile_definitions(harrowmere_core_tests PRIVATE HARROWMERE_SOAK_NO_MAIN=1)

# The shipped data directory (task 0.4's pinned decision), baked as an
# absolute path so the loader-facing tests pass regardless of the working
# directory ctest chooses.
target_compile_definitions(harrowmere_core_tests PRIVATE
    HARROWMERE_CORE_DATA_DIR="${CMAKE_SOURCE_DIR}/mods/harrowmere_core/data"
)

# CTest registration. The doctest binary is its own runner
# (DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN lives in main.cpp only). The working
# directory keeps the repo-root relative paths in test fixtures honest even
# without the baked define.
include(CTest)
add_test(
    NAME harrowmere_core_tests
    COMMAND harrowmere_core_tests
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# Friendly alias: cmake --build build --target harrowmere_tests
add_custom_target(harrowmere_tests
    DEPENDS harrowmere_core_tests
    COMMENT "Build harrowmere_core_tests (doctest harness, task 0.6)"
)

# ---- The standalone soak harness (task 8.3) ----------------------------------
# The nightly full run drives THIS binary (scripts opt in by label); the PR
# gate is TestSoak_OneGameYear_Smoke inside the doctest binary above.
add_executable(harrowmere_soak
    soak_main.cpp
)
target_link_libraries(harrowmere_soak PRIVATE harrowmere_core_objects)
target_include_directories(harrowmere_soak SYSTEM PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/doctest"
    "${CMAKE_SOURCE_DIR}/third_party"
)
if(MSVC)
    target_compile_options(harrowmere_soak PRIVATE /W4 /WX)
else()
    target_compile_options(harrowmere_soak PRIVATE -Werror)
endif()
target_compile_definitions(harrowmere_soak PRIVATE
    HARROWMERE_CORE_DATA_DIR="${CMAKE_SOURCE_DIR}/mods/harrowmere_core/data"
)

add_test(
    NAME soak_nightly_three_game_years
    COMMAND harrowmere_soak --game-years 3
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(soak_nightly_three_game_years PROPERTIES LABELS "nightly;soak")

add_test(
    NAME soak_pr_one_game_year
    COMMAND harrowmere_soak --game-years 1
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(soak_pr_one_game_year PROPERTIES LABELS "soak")
