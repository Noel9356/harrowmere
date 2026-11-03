// Task 5.1.1 - The capture rig & the image budget.
//
// Promotes the ad-hoc capture habit (the November evidence runs: spawn, walk,
// panels) into an in-repo suite with a pass/fail report, and a frame budget
// the whole phase is accountable to.
//
// What ships here:
//   * The scenario list — eight named setups (seed, time of day, weather,
//     camera pose, panels open) executed in a finite run, producing PNGs into
//     a per-scenario output directory. Pinned: spawn_day, spawn_night,
//     walk_village, walk_frontier, coast, rain, panels_open, corpse_aftermath.
//   * The checklist — for each scenario, what the reviewer must see (the 5.4.1
//     acceptance gate reads these). The checklists are TEXT (not pixel masks):
//     the suite renders under lavapipe in CI and under real GPUs locally, so
//     the pin is on WHAT IS VISIBLE (relief exists, borders present), never
//     on pixel equality across backends (the card's pitfall).
//   * The budget report — the run also records frame-time percentiles
//     (p50/p95) per scenario; the pinned budget is A-146 (16.6 ms render-side
//     on the reference machine at 1080p). 5.3.1's gate enforces it; this card
//     only measures and reports.
//   * The regression contract — the previous capture set's PNGs are kept
//     in-tree per scenario (docs/capture_suite.md lists them); a changed
//     image is legal only with a changed expectation line in that document.
//
// The suite runs through the engine's existing screenshot capability (kept
// in the §3.6 trim) — this module is the orchestration layer that names the
// scenarios, drives the run, and writes the report. It does NOT own the
// screenshot machinery itself (that is the engine's screenshot_png module).
//
// What this module exposes:
//   * ScenarioList    — the pinned eight scenarios, each with its setup,
//                       checklist, and budget row.
//   * runSuite(config) — executes the suite (or a single scenario) and writes
//                       the report + PNGs. Headless-safe: a scenario with
//                       quit_after_seconds produces its PNG and exits.
//   * BudgetReport    — the parsed percentile report, serializable to JSON
//                       for CI archival. The 5.3.1 gate reads this.
//   * parseBudgetReport(text) — parses a textual budget report (the shape the
//                       screenshot module writes) into BudgetReport. Pinned
//                       by TestCapture_BudgetReportParses.
//
// What this module does NOT ship (deferred to owner cards):
//   * The actual screenshot capture — the engine's screenshot_png module
//     owns the PNG write. This module orchestrates; the engine captures.
//   * The frame-time percentile measurement — the engine's profiler module
//     owns the timer. This module reads its report and parses it.
//   * The image-budget gate assertion — 5.3.1 owns the assertion. This
//     module measures and reports; 5.3.1 reads and asserts.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace harrowmere::capture
{

// ---- The pinned scenario list (card 5.1.1 spec) ---------------------------
//
// Eight scenarios, each a named setup (seed, time of day, weather, camera
// pose, panels open) executed in a finite run. The names are PINNED —
// TestCapture_ScenarioListComplete asserts the list matches the card's spec
// verbatim. Adding a scenario is a deviation (§1.4); changing a name is a
// deviation.

enum class ScenarioId : std::uint8_t
{
    spawn_day = 0,
    spawn_night,
    walk_village,
    walk_frontier,
    coast,
    rain,
    panels_open,
    corpse_aftermath,
    COUNT
};

inline constexpr std::uint8_t SCENARIO_COUNT = static_cast<std::uint8_t>(ScenarioId::COUNT);

// The name of a scenario (used as the PNG output subdirectory and the
// docs/capture_suite.md anchor). Stable across versions — the regression
// contract keys off the name.
const char *scenarioName(ScenarioId id);

// The seed a scenario runs with. PINNED: every scenario uses the same seed
// (the suite's deterministic anchor — the regression contract compares runs
// of the same seed). The seed is exposed so a reviewer can reproduce any
// capture locally.
inline constexpr std::uint64_t CAPTURE_SUITE_SEED = 20261103ull;

// The setup for one scenario: the inputs the run needs to reproduce the
// capture exactly. Every field is plain data so the suite is deterministic
// from the setup alone (no hidden state, no wall clock — R6/R8 apply to the
// suite just as to the sim).
struct ScenarioSetup
{
    ScenarioId id             = ScenarioId::spawn_day;
    std::uint64_t seed        = CAPTURE_SUITE_SEED;
    double quitAfterSeconds   = 1.0;  // finite run (TOML float spelling per DEVIATIONS.md 2026-10-29)
    double timeOfDayGameHours = 10.0; // 10:00 game time (A-129's world epoch)
    std::uint32_t weatherKind = 0;    // 0 = clear (the weather state machine's CLEAR ordinal)
    bool panelsOpen = false; // true for panels_open; the suite stages --set harrowmere_ui.visible_on_start accordingly
    // The camera pose: a world-space (x, z) the camera targets + a yaw/pitch.
    // PINNED per scenario so the regression contract compares like-for-like.
    double cameraTargetX  = 0.0;
    double cameraTargetZ  = 0.0;
    double cameraYawDeg   = 0.0;
    double cameraPitchDeg = 35.0;
};

// The checklist for one scenario: the observable features a reviewer must
// confirm in the PNG. TEXT (not pixel masks) — the suite renders under
// lavapipe in CI and under real GPUs locally, so the pin is on WHAT IS
// VISIBLE, never on pixel equality across backends (the card's pitfall).
//
// Each line cites the card that backs the feature (so a failed checklist
// line names the card that regressed).
struct ChecklistLine
{
    std::string feature; // e.g., "Terrain relief visible (no flat ground plane)"
    std::string backing; // the card/cite that backs it, e.g., "5.1.2 D-IMG-1"
};

// One scenario's full record: setup + checklist + budget row.
struct Scenario
{
    ScenarioSetup setup;
    std::vector<ChecklistLine> checklist;
    // The budget row for this scenario: the render-side p95 frame time the
    // 5.3.1 gate asserts against A-146 (16.6 ms). Heavier scenarios
    // (walk_village, coast, rain) are the 5.3.1 gate's anchors; the others
    // are measured but not gated.
    double budgetP95Ms = 16.6; // A-146 default; per-scenario overrides below
};

// The pinned scenario list — eight scenarios, in the order the card names
// them. TestCapture_ScenarioListComplete asserts the list matches the spec
// verbatim. The list is built once at first call and cached.
const std::vector<Scenario> &scenarioList();

// ---- The budget report -----------------------------------------------------
//
// The run records frame-time percentiles (p50/p95) per scenario. The report
// is serialized to JSON for CI archival; 5.3.1's gate reads it back and
// asserts the p95 against A-146.

struct ScenarioBudgetRow
{
    ScenarioId id = ScenarioId::spawn_day;
    std::string name; // matches scenarioName(id)
    double p50Ms               = 0.0;
    double p95Ms               = 0.0;
    std::uint64_t frameSamples = 0;     // how many frames the percentile is over
    bool gateAsserted          = false; // true iff this scenario is one of 5.3.1's gate anchors
};

struct BudgetReport
{
    std::vector<ScenarioBudgetRow> rows;
    double budgetGateMs = 16.6;      // A-146: the gate's threshold
    std::string referenceMachineTag; // e.g., "i5-8400/GTX1060" — the gate's anchor
    bool allGatesGreen = false;      // true iff every gateAsserted row's p95 ≤ budgetGateMs
};

// Serialize a BudgetReport to JSON (the shape CI archives). Stable across
// versions — the regression contract compares reports of the same suite run.
std::string serializeBudgetReport(const BudgetReport &report);

// Parse a textual budget report (the shape the screenshot module writes into
// the per-scenario output directory) into a BudgetReport. Pinned by
// TestCapture_BudgetReportParses: a known report text parses to a known
// BudgetReport. The parser is lenient (whitespace-tolerant, comment-skipping)
// so a hand-edited report still parses; strict on the numbers (a malformed
// percentile is a parse error, never a silent 0).
bool parseBudgetReport(const std::string &text, BudgetReport &out);

// ---- The run ---------------------------------------------------------------
//
// Run the suite (or a single scenario) and write the PNGs + the report into
// the output directory. Headless-safe: each scenario is a finite run
// (quit_after_seconds as a TOML float), so the suite runs under lavapipe in
// CI without a window. The function returns the parsed BudgetReport (the
// same shape 5.3.1's gate reads).
//
// `outputDirectory` is the per-suite root; each scenario writes into
// `<outputDirectory>/<scenarioName>/`. The screenshot module writes the
// PNGs; this module writes the report.json and the checklist.md (the
// regression contract's expectation file).
//
// `engineExecutable` is the path to the harrowmere executable (the run
// shells out to it with --set overrides per scenario). If empty, the run
// is "dry" — the scenarios are walked, the report is written with zeroed
// percentiles, and the PNGs are not produced. The dry mode is the CI smoke
// (TestCapture_RunProducesPngsPerScenario uses it to assert the suite's
// plumbing without a real GPU).
struct RunConfig
{
    std::filesystem::path outputDirectory;
    std::filesystem::path engineExecutable; // empty = dry mode
    std::string referenceMachineTag;        // empty = "unknown"
    bool dry = false;                       // true = walk scenarios, write report, no PNGs
};

// Run the suite. Returns the parsed BudgetReport. In dry mode the
// percentiles are zeroed (the suite's plumbing is exercised, not the GPU).
// In wet mode the run shells out to the engine executable per scenario,
// reads the per-scenario percentile report, and aggregates.
BudgetReport runSuite(const RunConfig &config);

// Convenience: run a single scenario (used by the dry-mode CI smoke and by
// a developer reproducing one capture locally).
bool runSingleScenario(const RunConfig &config, ScenarioId id, ScenarioBudgetRow &outRow);

} // namespace harrowmere::capture
