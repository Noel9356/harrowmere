# The game's authoritative-simulation native mod: a shared library linking the
# kernel, packaged into its own runtime package directory (mod.toml, defaults,
# world claim, data/) by the engine's own runtime-layout target. This file
# copies the build pattern of a module's CMakeLists (the same plugin scanner
# loads both); the mod publishes its capability vocabulary from src/bridge/
# exactly the way a module does.
#
# Object-library shape (task 0.6's contract): every game translation unit
# compiles exactly once, into the OBJECT library `harrowmere_core_objects`;
# the runtime SHARED library and the unit-test binary are both built from
# those objects. Tests therefore link compiled objects, not the mod runtime,
# and run without the engine's plugin scanner (card 0.6's pitfall verbatim).
add_library(harrowmere_core_objects OBJECT
    src/harrowmere_plugin.cpp
    # task 0.2 - fixed-point math, Result, id generation
    src/core/math.cpp
    # task 0.3 - WorldClock + SeedRng
    src/core/clock.cpp
    src/core/rng.cpp
    # task 0.4 - strict JSON layer, schema DSL, config seam
    src/data/json.cpp
    src/data/schema.cpp
    src/data/config.cpp
    src/data/mod_overrides.cpp
    # task 1.1 - species / items / classes loaders
    src/data/species.cpp
    src/data/items.cpp
    src/data/classes.cpp
    # task 1.2 - crime / laws / roles / territory templates loaders
    src/data/crime.cpp
    src/data/laws.cpp
    src/data/roles.cpp
    src/data/territory_templates.cpp
    # task 1.3 - the DataRegistry over all data files
    src/data/registry.cpp
    # task 1.4 - the world model: regions, territory tree, doc walks
    src/world/territory.cpp
    src/world/world.cpp
    # task 1.5 - world generation from seed
    src/world/worldgen.cpp
    # task 1.6 - the spatial index (A-39 uniform grid)
    src/world/spatial_grid.cpp
    # task 3.8 - the regional season clock + weather state machine
    src/world/seasons.cpp
    # task 7.2P1 - the static scene bridge (frame-coupled presentation)
    src/bridge/scene_bridge.cpp
    src/bridge/character_lab.cpp
    # rev 24 - the per-character randomized presentation look (the tunic
    # dyes, jittered tones and crowd silhouettes)
    src/bridge/character_look.cpp
    # rev 24 - the procedural prop-mesh vocabulary (the proper tree models)
    src/bridge/prop_meshes.cpp
    # tasks 7.3/7.4 - the published capability surface's step-9 command
    # application: the Phase-2 command set (card 7.3's pinned list) routes
    # to its owner systems; the 3.10 intents forward to applyIntents
    src/core/command_apply.cpp
    # task 3.10 - player control & interaction intents: the Interact gate,
    # handler registry and resolver sink (interactions), then the intent
    # queue, kinematic transform, AFK clock and action log (controller)
    src/player/interactions.cpp
    src/player/controller.cpp
    # task 2.0 - character records (the doc struct verbatim) + the unified store
    src/character/store.cpp
    # task 2.1 - derived statuses, PvP consent, cooldowns
    src/character/status.cpp
    # task 2.2 - species application (multipliers, sapience, bias, lifespan)
    src/character/species.cpp
    # task 2.3 - the progression engine (tables, purchases, respec, mana)
    src/progression/progression.cpp
    # task 2.3 - the skill-side block (effectiveness, requirements, buySkill)
    src/progression/skills.cpp
    # task 2.4 - items: the bag store + instance registry, gear scores and
    # equip/consume, the recipe table and craft action
    src/items/inventory.cpp
    src/items/gear.cpp
    src/items/crafting.cpp
    # task 2.5 - the cached power pipeline & the matchmaking score
    src/progression/power.cpp
    # task 2.6 - appearance changes (cost, cooldown, A-25 history + seams)
    src/character/appearance.cpp
    # task 2.7 - duels & arenas (the consent framework + broadcast order)
    src/combat/consent.cpp
    # task 3.1 - the 14-step tick pipeline (registrable systems + timers)
    src/core/scheduler.cpp
    # task 3.2 - the damage pipeline & attack resolution
    src/combat/damage.cpp
    src/combat/attacks.cpp
    # task 3.3 - death paths, XP transfer, corpses (D-SINK-1 sinks)
    src/combat/death.cpp
    # task 3.4 - NPC lifecycle: birth, aging, needs, migration
    src/npc/lifecycle.cpp
    # task 3.5 - the LOD tier hysteresis & the step-3 active set
    src/npc/lod.cpp
    # task 3.6 - roles & critical replacement
    src/npc/roles.cpp
    # task 3.7 - regional economy (D-ECON-1 prices, trades, taxes)
    src/economy/economy.cpp
    # task 3.9 - relationships, perception & witness qualification: the
    # affinity sources + bias adapter + the doc witness-weight formula
    # (relationships), the sighting memories + the live IPerceptionSink
    # (perception); 4.1's witnessReports reads both through this API
    src/npc/relationships.cpp
    src/npc/perception.cpp
    # tasks 4.1-4.4 - the justice layer: the shared JusticeStore, the
    # reputation/notoriety records with lazy decay (4.4), the crime-commit
    # pipeline + reports (4.1), the case state machine + warrants (4.2) and
    # conviction/sentencing/jail (4.3)
    src/justice/reputation.cpp
    src/justice/evidence.cpp
    src/justice/reports.cpp
    src/justice/cases.cpp
    src/justice/warrants.cpp
    src/justice/conviction.cpp
    # task 4.5 - behavioral suspicion & trust: the capped per-NPC memory,
    # the (act, context) trigger evaluation, the decay/relief rules and the
    # price/service/greeting effects (never evidence, never violence)
    src/justice/suspicion.cpp
    # task 4.6 - law enforcement AI: guard-power odds, the signal-before-
    # force invariant, evacuation + the manhunt spawn (via the event-spawner
    # seam) and the lawless-territory driver
    src/justice/enforcement.cpp
    # task 4.7 - citizen justice: the violence gate, the militia posse,
    # bounty claims with pay-once + capture fatigue, smuggling exclusion and
    # the village-slaughter marker
    src/justice/citizen_justice.cpp
    # task 5.1 - the event engine: the event_templates.json loader +
    # validator (template) and the data-driven state machine lifecycle with
    # scale uniqueness, the depth cap and the no-player resolution guarantee
    # (engine)
    src/events/template.cpp
    src/events/engine.cpp
    # tasks 5.2-5.5 - the event loop's organs: the self-balancing difficulty
    # regulation (difficulty), contribution/snapshot/validation with the
    # runtime damage+heal router (contribution), the single-pass reward
    # distribution (rewards), and the world-effect consumer wiring with the
    # D-FLAGS-1 transient/persistent split (world_effects)
    src/events/difficulty.cpp
    src/events/contribution.cpp
    src/events/rewards.cpp
    src/events/world_effects.cpp
    # task 6.1 - factions: the Faction records (membership, ranks, shared
    # resources, relations) with the worldgen adoption, the player-faction
    # interactions (found/invite/rank/dissolve), the lazy monthly drift and
    # the live IFactionQueries adapter (isAtWar reads 6.2's war registry)
    src/factions/factions.cpp
    # task 6.2 - war: the casus belli validation, the notice window, the
    # battle-event progression (D-WAR-EVT-1), the collapse/timeout resolution
    # and the outcome effects (region flags, claim rewiring, title transfers)
    src/factions/war.cpp
    # task 6.3 - territory claims: the claim interaction, custom-law
    # installation, the presence ledger + abandonment sweep, the
    # destruction-protection gate
    src/factions/claims.cpp
    # task 6.5 - matchmaking & scaling (SP-adapted): the score's three SP
    # consumers (duel opponent selection, manhunt difficulty normalization,
    # event difficulty calibration) through the IMatchmakingSink seam
    src/progression/matchmaking.cpp
    # task 7.3b - the society view fills: the territory tree, the JusticeView
    # whitelist and the EventsView (the tested free functions; rebuildViews
    # is the thin caller)
    src/bridge/society_view.cpp
    # task 5.1.2 - terrain relief from the world model: the deterministic
    # heightfield (D-IMG-1: height is presentation; heightAt is the ONE
    # height door). The bridge (5.1.2's consumer) and 5.2.1 scatter / 5.2.2
    # water / 6.1.1 camera floor all read through this one function.
    src/bridge/terrain.cpp
    # task 5.1.3 - characters beyond the token: per-species silhouette
    # primitives (A-153), yaw from movement direction, walk bob from
    # traveled distance (A-154). Pure-data derivation; the bridge composes
    # the visual AFTER reading the sim's position, never before writing it.
    src/bridge/character_visuals.cpp
    # task 5.1.4 - sky, sun & weather on screen: sun direction/intensity
    # from the day/night phase, sky gradient keyed to that phase (A-155
    # anchors interpolated), fog density/tint from weather state, exposure
    # tone target (A-156 floor/ceiling), bloom strength (A-157). D-IMG-3:
    # no new engine module, no kernel edit — the renderer's existing path
    # carries it.
    src/bridge/atmosphere.cpp
    # task 5.2.1 - scatter: vegetation, rock & settlement props placed
    # deterministically from (seed, climate, terrain) via a splitmix64
    # hash (D-IMG-2: NOT the worldgen RNG stream). Per-climate density
    # tables, A-150 settlement footprints, A-152 per-view cap.
    src/bridge/scatter.cpp
    # task 5.2.2 - water, coasts & borders on the land: the sea-level
    # plane (A-151), territory border ribbons projected onto the heightfield
    # (A-159 lift), owner-colored from the faction palette (A-158),
    # event-driven recolor on TerritoryChanged (no polling).
    src/bridge/borders.cpp
    # task 6.4 - quests: the quest rows (quests.json loader), the content
    # checklist validator and the two play verbs (start + deliver) over the
    # event engine — a quest IS an event instance, no separate payout path
    src/quests/quests.cpp
    # task 7.1 - save/load: the versioned JSON snapshot of the entire
    # simulation (atomic write, deterministic order, roundtrip-exact load)
    # plus the canonical state hasher the roundtrip test and the 8.3 soak
    # failure dumps share (state_hash is 8.1's file, pulled forward by its
    # two consumers)
    src/persist/save.cpp
    src/persist/load.cpp
    src/persist/state_hash.cpp
)

# The game source tree: "core/..." and "data/..." include paths, the
# published capability header in src/bridge/, and the internal core headers
# that name "rng/pcg64.hpp" relative to src/core/.
target_include_directories(harrowmere_core_objects PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
)

# The required capabilities' published headers (include path only): the mod
# compiles against the providers' types and stays free of their binaries -
# the implementations arrive at runtime as plugin libraries.
target_link_libraries(harrowmere_core_objects PUBLIC
    engine_kernel
    engine_capability_logging
    engine_capability_scene
    engine_capability_input
    engine_capability_pbr # the lighting family's published header (optional probe, 7.2P1 sun seam)
    engine_capability_character_customisation # rev 34: the parts-library capability the mod acquires
)

# nlohmann_json v3.11.3 (vendored at third_party/nlohmann_json, task 0.4):
# include path only, marked SYSTEM so the game's warnings-as-errors never
# fires inside the vendor header. Game code includes it only through
# src/data/json.hpp - the review-enforced single door (D-DATA-1).
target_include_directories(harrowmere_core_objects SYSTEM PUBLIC
    "${CMAKE_SOURCE_DIR}/third_party/nlohmann_json/single_include"
)

# The objects join a SHARED library; every object must be position independent
# (the tree sets PIC globally, pinned here so the object library stands alone).
set_target_properties(harrowmere_core_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# The global Definition of Done (common.md section 1.5): game targets compile
# with warnings-as-errors. The kernel's PUBLIC warning set (-Wall -Wextra
# -Wpedantic) is inherited through engine_kernel; -Werror upgrades it on the
# game's own translation units (the object library compiles them all).
if(MSVC)
    target_compile_options(harrowmere_core_objects PRIVATE /W4 /WX)
else()
    target_compile_options(harrowmere_core_objects PRIVATE -Werror)
endif()

# The runtime mod library: one exported entry table, built from the shared
# objects (exactly one compilation of each TU).
add_library(harrowmere_core SHARED
    $<TARGET_OBJECTS:harrowmere_core_objects>
)

# $<TARGET_OBJECTS> carries the object files and nothing else: an object
# library's link dependencies do NOT flow through it (only the
# target_link_libraries form propagates them, which is why the test binary
# below resolves kernel symbols and this library, before the fix, did not).
# The runtime library therefore links the kernel itself - the objects call
# non-inline kernel functions (ComponentContext::configuration*InSection,
# executableDirectory, prettifyChannelName). On ELF systems those references
# used to survive the link undefined and resolve at load time through the
# plugin RPATH ($ORIGIN/.. -> runtime/, where libengine_kernel.so sits);
# MinGW resolves every symbol at link time and needs the kernel's import
# library on the link line, or the DLL fails with undefined references
# (found on Windows: harrowmere_plugin.cpp.obj, ld exit 1). PUBLIC mirrors
# the module plugins' link pattern (engine_kernel is this tree's one shared
# library every plugin binds to).
target_link_libraries(harrowmere_core PUBLIC engine_kernel)

# Plugin deployment: the library lands in the mod's runtime package directory
# (runtime/mods/harrowmere_core/), where the kernel's mod discovery looks for
# it (mod.toml entry = "harrowmere_core" -> libharrowmere_core.so / .dll).
# PREFIX keeps one naming scheme on every platform (the module convention);
# the ORIGIN entries resolve sibling packages and their parent (the kernel and
# GLFW runtimes) in runtime/; WINDOWS_EXPORT_ALL_SYMBOLS publishes the mod's
# C++ surface across the library boundary.
set_target_properties(harrowmere_core PROPERTIES
    PREFIX "lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime/mods/harrowmere_core"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime/mods/harrowmere_core"
    BUILD_RPATH "${ENGINE_PLUGIN_RPATH_BUILD}"
    INSTALL_RPATH "${ENGINE_PLUGIN_RPATH_INSTALL}"
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)
# MinGW: one explicit exported symbol (the mod entry table) disables the
# linker's all-symbol auto-export; this flag publishes the rest of the C++
# surface the same way the modules do.
if(MINGW)
    target_link_options(harrowmere_core PRIVATE -Wl,--export-all-symbols)
endif()

# ---- The published capability surface ---------------------------------------
# harrowmere_capability.hpp is the ONLY header any consumer (the app,
# harrowmere_ui, third-party native mods) compiles against. The INTERFACE
# target carries the include path only - no consumer ever links this mod's
# implementation; acquisitions are resolved at runtime by the kernel.
add_library(harrowmere_capability_headers INTERFACE)
target_include_directories(harrowmere_capability_headers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge")

# The portability guard: the published header must stay implementation-,
# platform- and vendor-agnostic (see cmake/capability_header_guard.cmake) -
# the same mechanical rule the engine modules keep for their headers.
engine_guard_capability_header("${CMAKE_CURRENT_SOURCE_DIR}/src/bridge/harrowmere_capability.hpp")

# ---- Game data ships with the package ---------------------------------------
# All base content lives in data/ as JSON (D-DATA-1): the loaders of tasks
# 1.1/1.2 consume exactly these files at startup (1.3's DataRegistry wraps
# them). The build copies the directory into the package so a run sees the
# same content the repo ships.
add_custom_command(TARGET harrowmere_core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/data"
            "${CMAKE_BINARY_DIR}/runtime/mods/harrowmere_core/data"
    COMMENT "packaging harrowmere_core game data"
)

# ---- Unit tests ---------------------------------------------------------------
# The harness (task 0.6): one doctest binary linking harrowmere_core_objects;
# every card's test file is registered there. The switch keeps a packager's
# default path identical when the test binary is unwanted.
option(HARROWMERE_CORE_BUILD_TESTS "Build the harrowmere_core unit tests" ON)
if(HARROWMERE_CORE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
