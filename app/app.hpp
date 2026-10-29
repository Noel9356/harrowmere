#ifndef ENGINE_APP_APP_HPP
#define ENGINE_APP_APP_HPP

#include "engine/kernel/engine_kernel.hpp"

namespace engine
{

// The composition root: the demo application the engine tree ships. The app
// embeds the kernel and owns the one concern the kernel deliberately does
// not - the frame loop. It is a consumer like any other: it acquires the
// capabilities the active mods have pulled in, and a capability no mod
// demanded is simply absent - the run degrades instead of assuming a backend
// exists (docs/architecture.md, "Capability demand").
//
// The kernel never names the app, and the app names no module: the process
// side of the assembly only drives the engine. It configures the kernel (the
// assembly layer, config/engine.toml by default), runs construct / start /
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
// the trust gate"). Standing trust decisions belong in the configuration's
// [trust] section, not in prompt answers.
//
// main.cpp is the thin process entry point; everything a run does lives in
// this class, so another assembly could construct and drive it directly.
class App final
{
public:
    App()  = default;
    ~App() = default;

    App(const App &)            = delete;
    App &operator=(const App &) = delete;

    // The whole run: command line, construct, start, the frame loop, the
    // opt-in latency report, stop. Returns the process exit code. An
    // embedder may pass no arguments (the no-arguments shape below is the
    // historical behavior: the default assembly configuration, no staged
    // overrides, full startup).
    int run(int argumentCount = 0, char *arguments[] = nullptr);

private:
    // The kernel the app embeds. Owned as a member so the app instance holds
    // the whole engine lifetime: the kernel destructor stops everything
    // exactly once, so App going out of scope tears the run down.
    EngineKernel kernel;
};

} // namespace engine

#endif // ENGINE_APP_APP_HPP
