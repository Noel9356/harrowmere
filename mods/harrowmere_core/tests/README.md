# harrowmere_core tests

The unit-test target `harrowmere_core_tests` (doctest). The framework and
target name are pinned by task 0.6 (the full harness: test-count gate,
`scripts/ci.sh`); task 0.7 landed the harness nucleus - the doctest entry
point, the first pinned test files, and ctest registration - so its card's
tests exist and run.

## Layout

| File | Role |
|---|---|
| `main.cpp` | the doctest entry point (`DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`); test files never define it |
| `test_events.cpp` | task 0.7's two pinned tests over the sim event channels |
| `CMakeLists.txt` | the target: links the kernel (event bus headers + lib), compiles `../src/core/` headers, registers with ctest |

The target links the kernel and compiles the mod's headers directly - tests
run without the plugin scanner, exactly as the harness card requires ("link
objects, not the mod runtime"). The doctest single header is vendored at
`third_party/doctest/` (v2.4.12, the version task 0.6 pins).

## Running

```sh
ctest --test-dir build/mods/harrowmere_core/tests --output-on-failure
# or directly:
./build/runtime/harrowmere_core_tests
```

ctest discovery is directory-local until task 0.6 lifts `enable_testing()` to
the tree root (its `ci.sh` runs ctest from the configure root).

## The pinned tests (task 0.7)

- `TestEvents_PublishAfterMutationOrdering` - the publish-after-mutation
  rule: a handler dispatched by the engine observes the post-state (the
  culprit already inside the manhunt target set) and the event carries the
  post-mutation values; also pins that dispatch is synchronous on the
  caller's stack.
- `TestEvents_UnsubscribeDuringDispatchDeferred` - pins the kernel's exact
  contract for an `EventSubscription` released during a dispatch (verified
  against `kernel/include/engine/kernel/event_bus.hpp`): the structural
  removal is deferred (an already-scanned handler keeps its in-flight
  delivery) while the logical removal is immediate (a handler released before
  the scan reaches it is not invoked - no dangling dispatch), both silent
  from the next publish on, and a second `release()` is a no-op.

New test files follow the pinned naming `Test<System>_<Behavior>_<Expected>`
(common.md §3.5) and are added to the target's source list; `main.cpp` stays
untouched.
