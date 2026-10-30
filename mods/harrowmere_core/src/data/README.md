# src/data — the JSON data layer (task 0.4)

The game's one door for game **content** (D-DATA-1, common.md §4.3): species,
items, crime data, laws, roles, territory templates, suspicion triggers —
later, event templates, quests, recipes, names. Content is JSON under
`data/`; *configuration* is TOML and belongs to the engine — the single
configuration read this layer owns is wrapped in `config.hpp` (below).

A data error fails **startup** with file + JSON pointer + what was expected —
never a mid-sim crash (phase-1 exit checklist).

| File | Role |
|---|---|
| `json.hpp/.cpp` | strict loader (`loadJsonFile` / `parseJsonText`), `scaleToGrid` (THE scale conversion), `scaledValue`, `mergeData`, `utf8ErrorOffset`. The only place nlohmann headers appear. |
| `schema.hpp/.cpp` | the schema DSL and validator (`Schema::validate` → `std::vector<DataError>`). |
| `config.hpp/.cpp` | the configuration wrapper: `GameConfig` (pinned defaults, `[engine].mod_directory`, base data-directory resolution), `ConfigSource` (kernel-free seam), `isDeclaredConfigKey`. |
| `registry.hpp/.cpp` | the DataRegistry (task 1.3): runs the pinned load order (enums → species → items → classes → crime → laws → roles → territory templates → suspicion), cross-references the table set, commits an all-or-nothing snapshot, and is the ONLY query surface later systems use (`species(id)` O(1) array, `defaultLaws()`, `lawSet`, `role`, `itemById`, `territoryTemplate`, `suspicionTriggers`). Hot reload is optional and OFF in v1. Cross-ref errors use logical set-key pointers (`/sets/<key>/exemptions/42`) — see DEVIATIONS.md. |
| `config_engine.hpp` | the engine-side half of the seam: `EngineConfigSource` over the kernel's `ConfigurationStore`. The ONLY file here that includes an engine header. |

## The strictness contract

`loadJsonFile` / `parseJsonText` reject, with file + pointer + reason:

1. **Invalid UTF-8** (first invalid byte reported; a single leading BOM is
   tolerated and skipped).
2. **Trailing garbage** — nothing but whitespace may follow the top-level
   value.
3. **Duplicate object keys** — nlohmann's default parse silently keeps the
   last; we do not (task 0.4's pitfall: choose strict). Strictness is
   implemented as a SAX DOM builder that rejects the duplicate the moment it
   is read, carrying the offender's pointer.
4. **Syntax errors** — byte position + innermost pointer.
5. **Unopenable/unreadable files.**

Schema validation additionally rejects **unknown fields** (a typo'd
`"raritty"` is an error, not a silently ignored key), wrong types, out-of-range
values, unknown enum values, unknown map keys — every violation is reported
(the report is capped, see *Engineering constants*).

## Numeric policy (the SCALE grid)

Design rule: *"Data can be non-integer but will be scaled and casted into
integer to be use."* Concrete pins (task 0.4):

- `JsonType::Integer` fields take JSON integers only; a fractional value is
  an error (never a silent cast).
- `JsonType::Scaled` fields take the **dual form** (Appendix B authors both):
  - integer number → used **as-is**, already on the grid
    (`"severity": 9000`, `"xenophobiaBias": 2000`, `"rarity": 2000`);
  - fractional number → **`scaleToGrid(value)`**: ×SCALE (A-1, 10'000), round
    half away from zero (`"statMultiplier": 2.0` → 20'000,
    `"perCapitaNeed": 0.5` → 5'000, `"damageTakenMultiplier": 0.25` → 2'500).
- `scaleToGrid` is the **single** conversion point in the game (task 0.4
  pitfall); there is no second rounding path to drift from it. Bounds are
  checked **on the grid**, after conversion.

## The Schema DSL

```cpp
using namespace harrowmere::data;

// A record: object with named fields (unknown fields are errors).
// Bounded overload covers FieldSpec's min/max.
Schema speciesRow;
speciesRow.field("id", JsonType::Integer, 0, 1'000'000)
    .field("key", JsonType::String)
    .field("sapient", JsonType::Boolean)
    .field("lifespan_years", JsonType::Integer, 0, 2'000'000)
    .map("statMultiplier", JsonType::Scaled, kStatNames)   // keys from a closed set
    .field("xenophobiaBias", JsonType::Scaled, 0,
           static_cast<std::int64_t>(SCALE))
    .field("notes", JsonType::String, false);              // optional

// Root forms: whole documents.
Schema names = Schema::arrayOf(Schema::scalar(JsonType::String));
Schema masks = Schema::enumeration(kMaskNames);            // leaf string, closed set

// Validate → every violation one DataError (file + pointer + expected).
std::vector<DataError> errors = speciesRow.validate(row, "species.json");
```

The card's pinned signatures (`field(name, type[, required])`,
`array(name, element[, required])`, `enumerate(name, values)`) are unchanged;
the bounded `field` overload, the static leaf forms (`scalar`, `enumeration`,
`arrayOf`) and `map(name, valueType[, keys][, required])` extend the skeleton
because the Appendix B starter files need them — DEVIATIONS.md has the row.
Closed sets are passed as arrays of `const char *` (a braced list does not
implicitly convert to the pinned `std::span` parameter).

## Mod merge policy

`mergeData(base, override)` — objects deep-merge (override wins, new keys
added, other keys kept); **arrays and scalars replace wholesale**, never
concatenate. This mirrors the engine's TOML layer philosophy ("rows authored
wholesale and replaced wholesale") and is the seam task 7.5's data-mod surface
stands on: the base package (`harrowmere_core`) is the first element of the
package enumeration; other active packages apply afterwards in manifest-id
order.

## Configuration wrapper

```cpp
// where the kernel is visible (the mod's construct(), the app):
const harrowmere::data::EngineConfigSource source(kernel.configuration());
const auto config = harrowmere::data::GameConfig::load(source);
config.tickRate();            // [harrowmere].tick_rate, default 10 (D-TICK-1)
config.timeScale();           // [harrowmere].time_scale, default 60 (D-TIME-1)
config.autosaveGameHours();   // default 6 (task 7.1 consumes)
config.seed();                // default 0 = random, persisted by 7.1
config.coreDataDirectory(exeDir);
// → <exeDir>/<mod_directory>/harrowmere_core/data  (task 0.4 pinned decision)
```

Missing keys fall back to the pinned defaults `mods/harrowmere_core/config.toml`
ships; out-of-range or negative values fall back too (configuration garbage
never reaches the sim as a plausible number). The wrapper reads only keys
`mod.toml`'s `config_keys` declares — `isDeclaredConfigKey()` is the guard,
`kDeclaredConfigKeys` the list; keep them in lockstep with the manifest and
the compiled `knownConfigKeys`.

## Build integration (merge note)

This layer is **not yet on the mod's source list**: `json.cpp` includes
`core/constants.hpp` (SCALE, A-1), which is task 0.2's parallel deliverable —
wiring the sources before that file exists would break the mod build. The
merge snippet, for when 0.2 lands (or together with 0.6's test harness):

```cmake
# mods/harrowmere_core/CMakeLists.txt — additions only
add_library(harrowmere_core SHARED
    src/harrowmere_plugin.cpp
    src/data/json.cpp
    src/data/schema.cpp
    src/data/config.cpp
)
target_include_directories(harrowmere_core PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"   # "data/..." and "core/..." includes
)

# root CMakeLists.txt (or the mod's) — the vendored dependency, SYSTEM so its
# warnings stay out of our warning-clean build (the engine's miniaudio pattern):
add_library(harrowmere_nlohmann_json INTERFACE)
target_include_directories(harrowmere_nlohmann_json SYSTEM INTERFACE
    "${CMAKE_SOURCE_DIR}/third_party/nlohmann_json/single_include")
target_link_libraries(harrowmere_core PRIVATE harrowmere_nlohmann_json)

# task 0.6: tests/test_data_loader.cpp joins harrowmere_core_tests with the
# same include paths (plus third_party/ for doctest). The test file defines
# no main — tests/main.cpp owns it.
```

Until then, `scripts/task04_verify/` (outside the repo) compiles and runs the
layer standalone against a stub `core/constants.hpp`.

## Engineering constants (not Appendix A rows)

Three named constants here are *engineering* bounds of the loader, not game
rules, so they live next to their code with comments rather than in the
constants registry (flagged for the lead in DEVIATIONS.md; move them to
`core/constants.hpp` if the DoD is read strictly):

- `kMaxNestingDepth` (200) — hostile-input depth cap; the SAX builder is
  iterative, this keeps error bookkeeping bounded.
- `kMaxValidationErrors` (100) — report cap so a wrong file cannot flood the
  startup log; the cap note tells the author to fix and re-validate.
- `kInt64MagnitudeLimit` (2^63 as a double) — the clamp guard behind
  `scaleToGrid`; absurd inputs clamp, then the schema bounds reject them.

## Not here (ownership map)

- Per-content-type loaders + their schemas → tasks 1.1, 1.2 (species, items,
  classes, names / crime, laws, roles, territory templates, suspicion).
- The cross-referencing registry over all files → task 1.3 (`DataRegistry`,
  landed in `registry.hpp/.cpp` above; the optional hot-reload path
  re-validates via this layer between ticks).
- The world model over the loaded content (regions, territory tree, the
  three doc walks) → task 1.4 (`src/world/`, documented there).
- Event templates / quests / recipes schemas → tasks 5.1, 6.4, 2.4. The
  `kind`-discriminated rows in `event_templates.json` (heterogeneous
  condition/action objects) are validated by their owning loader; this DSL
  covers the structural floor they build on.
- Other packages' data enumeration and override application → task 7.5
  (consumes `mergeData`).
- The test harness and `harrowmere_core_tests` wiring → task 0.6.
