# nlohmann_json (vendored, pinned)

Single-header JSON library, **v3.11.3**, vendored per task 0.4 spec item 1
("Vendor nlohmann_json v3.11.3 per the engine's dependency tiers ... or
third_party/nlohmann_json/").

- Source: https://github.com/nlohmann/json, tag **v3.11.3**,
  `single_include/nlohmann/json.hpp` (919,975 bytes; the version macros in
  the header pin 3.11.3).
- License: `LICENSE.MIT` in this directory.
- Consumed **only** through `mods/harrowmere_core/src/data/json.hpp`
  (D-DATA-1): no other game file may include nlohmann headers directly —
  review-enforced, grep `#include.*nlohmann` outside `src/data/`.
- Treat the include path as SYSTEM (the engine's vendored-dependency pattern,
  e.g. `third_party/miniaudio` for `audio_miniaudio`) so the library's
  warnings stay out of the game's warnings-as-errors build. The CMake snippet
  lives in `mods/harrowmere_core/src/data/README.md` ("Build integration").
