# logging_stdout module

The foundation service of the engine: a leveled, tagged message sink that every other component depends on. The logging_stdout module provides the `LoggerCapability` (capability family `logging`, published in `logging_capability.hpp` next to this module — the single source of truth every consumer includes; the plugin descriptor and manifest carry the implementation-bound member name `logging.stdout`, the console-sink implementation) and requires nothing itself, so whenever a mod demands `logging` it is the first component constructed in the graph. Every module manifest declares `capability.logging` as a required permission, and mods that want to write log lines declare `logging` under `capabilities` in their `mod.toml` — the permission system therefore routes all logging through one auditable grant.

## Capability surface

`LoggerCapability` is deliberately the smallest capability surface in the tree: one method, `logMessage(LogLevel level, const std::string& message)`, plus one probe, `wouldLog(LogLevel level)` — the same minimum-level gate logMessage opens with, exposed so a caller that FORMATS its message first (a refusal path's number-to-string concatenation) can skip the formatting when the configured level would drop the line anyway. The `LogLevel` enum next to it defines the five levels the engine uses — `trace`, `debug`, `information`, `warning`, `error`. There is no formatting API, no sink selection, and no category argument on the surface; the module owns presentation, and a mod or module that wants structure prefixes the message itself. Keeping the surface this small means a replacement implementation (for example a networked or binary sink) can drop in without any consumer noticing more than the output format; the probe defaults to "yes" in the published header, so a replacement that ignores it never silences a call site.

## Configuration

The module reads its `[logging]` section during startup. Unknown keys are ignored, and a missing section means defaults.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `level` | string | `"information"` | minimum level that is emitted: `trace`, `debug`, `information`, `warning`, `error`; an unrecognized name falls back to `information` |
| `file` | string | (console only) | additional sink: log lines are appended to this file, resolved relative to the executable directory (e.g. `engine.log` writes next to the binary) |
| `flush` | string | `"always"` | when the sinks flush: `always` (the historical per-line flush, crash-accurate), `warning` (only warning-and-worse lines flush - durable diagnostics, fewer write syscalls on chatty runs), `never` (the streams decide; the file still flushes once at stop) |
| `timestamp_millis` | boolean | `false` | append `.mmm` sub-second precision to the timestamp (frame-loop diagnostics that must tell two messages in one second apart) |

An unrecognized level string is not a startup error — the module falls back to `information` so a typo degrades verbosity instead of failing the run. An unrecognized `flush` name IS a startup error (a misconfigured sink must not start silently); on a live reload the same typo keeps the previous policy instead, with one warning — a half-edited file must not take a running sink's durability down.

**Live configuration reload:** `level` and `flush` are retunable at runtime through the configuration watch hub — the same adopter pattern the screenshot and audio modules run. Free while `[engine].config_reload_poll_seconds` is 0 (nobody publishes the event the hub subscribes to); on a reload, a changed key applies without a restart — the classic dev-loop tuning story for verbosity. The file sink is deliberately restart-only: reopening files mid-run is a resource decision, not a value update.

## Implementation

The module is intentionally dependency-light: standard library only, no third-party code. Console output goes to standard output and is decorated based on what it is attached to — `standardOutputIsTerminal()` probes with `isatty`/`_isatty`, so interactive runs get human-oriented presentation while redirected or captured runs (CI logs) stay plain. Lines are time-stamped with wall-clock time (`std::chrono`/`std::ctime`), prefixed with the level tag, and the configured minimum level filters everything below it. When `[logging].file` is set, the file sink is opened at startup in append mode and every emitted line goes to both sinks; the file is flushed on each write so a crashed run keeps the lines it managed to emit.

The kernel's own startup diagnostics do not flow through this module — the kernel includes no capability header at all; it collects its report and the composition root prints it. What depends on this module being up early is everything else: mods and modules that log during their construct phase require `logging`, so the resolver orders it first among the demanded providers.

## Files

- `logging_capability.hpp` — the published capability surface every consumer includes
- `logging.hpp` / `logging.cpp` — the module and its capability implementation
- `module_plugin.cpp` — the static plugin descriptor (name, factory, provided capabilities)
- `CMakeLists.txt` — target and option wiring (`ENGINE_BUILD_MODULE_LOGGING_STDOUT`)
