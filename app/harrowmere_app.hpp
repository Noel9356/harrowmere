#ifndef HARROWMERE_APP_APP_HPP
#define HARROWMERE_APP_APP_HPP

#include "engine/kernel/engine_kernel.hpp"

namespace engine
{

// The composition root of Harrowmere: the engine's demo application rewritten
// in place (app/app.cpp is gone; this file took its target, its shape and its
// frame loop). The app embeds the kernel and owns the one concern the kernel
// deliberately does not - the frame loop. It is a consumer like any other: it
// acquires the capabilities the active mods have pulled in, and a backend
// capability no mod demanded is simply absent - the run degrades instead of
// assuming a backend exists (docs/architecture.md, "Capability demand").
//
// Two acquisitions are NOT degradable, and this is what makes the executable
// the game's own rather than a generic viewer:
//
//   - the simulation capability ("harrowmere.core", provided by the
//     harrowmere_core native mod) is a hard requirement. The game cannot run
//     without its sim; a missing provider means the assembly itself is broken
//     and the run exits 1 with a precise diagnostic.
//   - the window and renderer capabilities degrade exactly as the demo's
//     try-acquire pattern does: a headless CI run pumps itself at
//     [engine].idle_poll_ms and the gameplay layer stays renderer-free.
//
// The kernel never names the app, and the app names no module: the process
// side of the assembly only drives the engine. It configures the kernel (the
// assembly layer, config/harrowmere.toml by default), runs construct / start /
// the frame loop / stop, and turns every failure into the process exit code:
// 0 clean, 1 startup or frame failure, 2 command line usage error,
// 3 observed graphics backend validation errors.
//
// The command line is the app's invocation surface, mapped one-to-one onto
// hooks the kernel already exposes (docs/configuration.md, "Command line"):
// --config selects the assembly file, --set stages a "section.path=value"
// override (the highest configuration layer), --scene replaces the world
// content, --mods-dir re-points mod discovery, --validate dry-runs the whole
// startup without touching a backend (CI), --print-graph dry-runs and then
// prints the resolved component graph (mod development), --version prints
// the version. Every flag is optional; with no arguments the run is exactly
// what it was before the surface existed.
//
// The app also connects the kernel's trust gate to the terminal: a real run
// whose assembly carries flagged-but-untrusted mods or modules asks the user
// here before using any of them (docs/architecture.md, "Risk advisories and
// the trust gate"). Standing trust decisions belong in the assembly's [trust]
// section - config/harrowmere.toml trusts harrowmere_core and harrowmere_ui
// from day one, because our two native mods are risk-flagged by definition.
//
// main.cpp is the thin process entry point; everything a run does lives in
// this class, so another assembly could construct and drive it directly.
class HarrowmereApp final
{
public:
    HarrowmereApp()  = default;
    ~HarrowmereApp() = default;

    HarrowmereApp(const HarrowmereApp &)            = delete;
    HarrowmereApp &operator=(const HarrowmereApp &) = delete;

    // The whole run: command line, construct, start, the frame loop, the
    // opt-in latency report, stop. Returns the process exit code. An
    // embedder may pass no arguments (the no-arguments shape below is the
    // historical behavior: the default assembly configuration, no staged
    // overrides, full startup).
    int run(int argumentCount = 0, char *arguments[] = nullptr);

private:
    // The kernel the app embeds. Owned as a member so the app instance holds
    // the whole engine lifetime: the kernel destructor stops everything
    // exactly once, so HarrowmereApp going out of scope tears the run down.
    EngineKernel kernel;
};

} // namespace engine

#endif // HARROWMERE_APP_APP_HPP
