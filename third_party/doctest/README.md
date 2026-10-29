# doctest (vendored, pinned)

| | |
|---|---|
| Version | v2.4.12 (the version task 0.6 pins for the game's test harness) |
| Source | https://github.com/doctest/doctest at tag `v2.4.12`, file `doctest/doctest.h` |
| SHA256 | `94029a7d32da24a56249658147dbd2b33ff0b9ed665295cbbaf19aafff5b0ced` |
| License | MIT (embedded in the header's preamble) |
| Consumed by | `mods/harrowmere_core/tests/CMakeLists.txt` (SYSTEM include, `harrowmere_doctest_headers` INTERFACE target) |

Single-header drop-in, the same vendoring pattern the engine uses for pinned
single headers (see `third_party/miniaudio/`). Do not modify `doctest.h` by
hand - re-vendor the pinned tag instead and update the hash above.

Provenance: task 0.6 owns the test harness and pins this dependency; task 0.7
vendored it ahead of that card because 0.7's pinned tests (its card's Tests
block and the §1.5 Definition of Done) must exist and pass when 0.7 lands -
see `docs/DEVIATIONS.md` in the harrowmere_core package.
