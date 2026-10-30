# harrowmere_ui tests

The unit-test target `harrowmere_ui_tests` (doctest v2.4.12, the same vendored
single header the core harness pins at `third_party/doctest/`). Task 7.4P1
landed the harness with the input mapper's pinned tests; the 7.3P1 test
(`test_debug_panel.cpp`, shipped at the mod root) joins a binary of its own
when that task's ImGui dependency lands - see the mod's `CMakeLists.txt` for
the source map.

## Layout

| File | Role |
|---|---|
| `main.cpp` | the doctest entry point (`DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`); test files never define it |
| `test_input_mapper.cpp` | task 7.4P1's three pinned tests (`TestInput_P1BindingsComplete`, `TestInput_NoDirectSimMutation`, `TestInput_IntentRoundTrip`) over the input mapper's seams, with fakes for the overlay gate, the camera rig, the input capability and the sim capability |
| `CMakeLists.txt` | the target: links `harrowmere_ui_objects` (the mod's objects - never the runtime mod library, card 0.6's rule verbatim) plus `harrowmere_core_objects` for the roundtrip's drain side, registers with ctest |

## Running

```sh
ctest --test-dir build/mods/harrowmere_ui/tests --output-on-failure
# or directly:
./build/runtime/mods/harrowmere_ui/harrowmere_ui_tests
```

ctest discovery is directory-local (the same shape the core harness documents);
`scripts/ci.sh` runs both test binaries in one command.

## The roundtrip's drain side

`TestInput_IntentRoundTrip` is the only place a UI-mod test file includes
harrowmere_core internals (`player/controller.hpp`): the card pins "headless:
inject key state -> Move intent applied on the fixed-update subscription", and
the fixed-update application is task 3.10's `player::applyIntents` (step 9's
home, common.md §4.4). The mod's own sources keep the §4.2 include policy -
only `harrowmere_capability.hpp` crosses the mod boundary there; the policy's
review grep reads `src/`, not `tests/`.

New test files follow the pinned naming `Test<System>_<Behavior>_<Expected>`
(common.md §3.5) and are added to the target's source list; `main.cpp` stays
untouched.
