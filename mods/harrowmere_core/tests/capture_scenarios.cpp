// Task 5.1.1 - The capture rig & the image budget (implementation).
//
// See capture_scenarios.hpp for the contract. This file owns:
//   * The pinned scenario list (eight scenarios, each with setup + checklist
//     + budget row).
//   * The budget-report serializer/parser (JSON shape, stable across versions).
//   * The run orchestrator (shells out to the engine executable per scenario
//     in wet mode; walks the scenarios and writes a zeroed report in dry mode).
//
// What this file does NOT own:
//   * The screenshot capture (the engine's screenshot_png module owns the
//     PNG write — this module orchestrates).
//   * The frame-time percentile measurement (the engine's profiler module
//     owns the timer — this module reads its report and parses it).
//   * The image-budget gate assertion (5.3.1 owns the assertion — this
//     module measures and reports).
//
// The run is HEADLESS-SAFE: each scenario is a finite run (quit_after_seconds
// as a TOML float, the spelling DEVIATIONS.md 2026-10-29 pinned), so the
// suite runs under lavapipe in CI without a window. The dry mode (no engine
// executable) walks the scenarios and writes a zeroed report so the suite's
// plumbing is exercised in CI without a GPU (TestCapture_RunProducesPngsPerScenario
// uses this).

#include "capture_scenarios.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>

#include "core/constants_phase5.hpp" // RENDER_FRAME_BUDGET_MS (A-146)

namespace harrowmere::capture
{

namespace
{

// The pinned scenario names — exactly the card's list, in order. Adding or
// renaming a scenario is a deviation (§1.4); the TestCapture_ScenarioListComplete
// test asserts the list matches the spec verbatim.
constexpr std::array<const char *, SCENARIO_COUNT> kScenarioNames = {
    "spawn_day",
    "spawn_night",
    "walk_village",
    "walk_frontier",
    "coast",
    "rain",
    "panels_open",
    "corpse_aftermath",
};

// The 5.3.1 gate anchors: the heaviest scenarios the budget gate asserts.
// The card (5.3.1) names walk_village, coast, rain as the gate's anchors;
// the other five are measured but not gated. Stored as a lookup so the
// BudgetReport's `gateAsserted` flag is set correctly.
constexpr std::array<ScenarioId, 3> kGateAnchors = {
    ScenarioId::walk_village,
    ScenarioId::coast,
    ScenarioId::rain,
};

bool isGateAnchor(
    ScenarioId id
) noexcept
{
    for (const auto a : kGateAnchors)
        if (a == id)
            return true;
    return false;
}

// Build the pinned checklist for one scenario. Each line cites the card that
// backs the feature (so a failed checklist line names the card that regressed).
// The checklists are TEXT (not pixel masks) — the suite renders under lavapipe
// in CI and under real GPUs locally, so the pin is on WHAT IS VISIBLE, never
// on pixel equality across backends (the card's pitfall).
std::vector<ChecklistLine> checklistFor(
    ScenarioId id
)
{
    switch (id)
    {
    case ScenarioId::spawn_day:
        return {
            {"Terrain relief visible (no flat ground plane)", "5.1.2 D-IMG-1"},
            {"Sun light from above (day phase ~10:00 game time)", "5.1.4 / A-129"},
            {"Sky gradient readable as day (not night)", "5.1.4 A-155"},
            {"Safe-zone disc visible at the spawn village", "2.5.3 / D-SHELTER-1"},
        };
    case ScenarioId::spawn_night:
        return {
            {"Sky gradient readable as night (dark, not black)", "5.1.4 A-155/A-156"},
            {"Sun light absent or low (night phase)", "5.1.4"},
            {"Exposure floor holds (night is dark but playable)", "5.1.4 A-156"},
            {"Terrain relief still visible under low light", "5.1.2"},
        };
    case ScenarioId::walk_village:
        return {
            {"NPCs present (species-colored tokens, faction rings)", "2.5.3"},
            {"Vegetation dense near the village (scatter)", "5.2.1 A-150"},
            {"Territory marker visible (village height 5 m)", "2.5.3 / 1.6.4"},
            {"Walking motion legible (camera follows)", "1.8.1 / 6.1.1 (future)"},
        };
    case ScenarioId::walk_frontier:
        return {
            {"Vegetation sparse on the frontier (scatter density gradient)", "5.2.1"},
            {"No settlement props in the frontier ring", "5.2.1 / 1.6.1 frontier"},
            {"Terrain relief varied (mountains/swamp if present)", "5.1.2"},
            {"No FAR NPCs rendered (presentation radius cut)", "2.5.3 A-118"},
        };
    case ScenarioId::coast:
        return {
            {"Water plane visible at sea level (A-151 = 0 m)", "5.2.2 A-151"},
            {"Coastal terrain clamped low (≤ 8 m within 2 km)", "5.1.2 A-147.e"},
            {"Coastal climate reads as coast (flat, near water)", "5.1.2 / 1.6.1"},
            {"Borders visible on the terrain (coastal territory)", "5.2.2 A-158"},
        };
    case ScenarioId::rain:
        return {
            {"Weather state is rain (sky tint desaturated)", "5.1.4 / A-117"},
            {"Fog density raised (rain raises fog)", "5.1.4"},
            {"Terrain relief still visible through rain", "5.1.2"},
            {"NPCs present (if near a settlement)", "2.5.3"},
        };
    case ScenarioId::panels_open:
        return {
            {"ImGui panels visible (HUD, Character, Inventory)", "2.6.2"},
            {"Panels do not obscure the world (composite position)", "2.6.2 D-UI-1"},
            {"F1 toggle works (panels close on second press)", "1.7.2 / 2.7.1"},
            {"HUD reads live vitals (HP/level/coin non-zero)", "3.1.1 / 2.6.2"},
        };
    case ScenarioId::corpse_aftermath:
        return {
            {"Corpse visible (flattened gray box)", "2.5.3 / A-143"},
            {"Corpse decays after CORPSE_DECAY_DAYS (3 game-days)", "2.6.1 A-36"},
            {"Loot is interactable (if within INTERACT_RANGE_M)", "1.7.1 A-121"},
            {"No living NPC at the corpse's position", "2.5.3"},
        };
    default:
        return {};
    }
}

// The pinned setup for one scenario. The setups are deterministic (every
// field is plain data) so two runs of the same scenario produce identical
// captures (the regression contract).
ScenarioSetup setupFor(
    ScenarioId id
)
{
    ScenarioSetup s;
    s.id               = id;
    s.seed             = CAPTURE_SUITE_SEED;
    s.quitAfterSeconds = 1.0; // finite run (TOML float spelling per DEVIATIONS.md)
    s.panelsOpen       = false;
    // The camera targets the beginner safe-zone spawn (1.6.1's playerSpawn)
    // by default; per-scenario overrides below. The exact spawn position is
    // seed-derived, so the suite uses a fixed camera target near the world
    // center (8 km into the 16 km world) — the camera looks at the spawn
    // village's approximate location.
    s.cameraTargetX  = 8000.0; // near world center (A-40: 16 km world)
    s.cameraTargetZ  = 8000.0;
    s.cameraYawDeg   = 0.0;
    s.cameraPitchDeg = 35.0;

    switch (id)
    {
    case ScenarioId::spawn_day:
        s.timeOfDayGameHours = 10.0; // A-129: world epoch opens at 10:00
        s.weatherKind        = 0;    // clear
        break;
    case ScenarioId::spawn_night:
        s.timeOfDayGameHours = 2.0; // 02:00 game time (deep night)
        s.weatherKind        = 0;
        break;
    case ScenarioId::walk_village:
        s.timeOfDayGameHours = 12.0; // noon (bright)
        s.weatherKind        = 0;
        // Camera at a village offset (the suite's seed plants a village near
        // the spawn; the camera targets the same village).
        s.cameraTargetX = 7500.0;
        s.cameraTargetZ = 7500.0;
        break;
    case ScenarioId::walk_frontier:
        s.timeOfDayGameHours = 14.0; // afternoon
        s.weatherKind        = 0;
        // Camera at the frontier ring (the outermost region row) — sparse
        // scatter, no settlement props.
        s.cameraTargetX = 15000.0;
        s.cameraTargetZ = 15000.0;
        break;
    case ScenarioId::coast:
        s.timeOfDayGameHours = 11.0;
        s.weatherKind        = 0;
        // Camera at the west coast (synthetic climate grid puts COASTAL at x=0;
        // the real worldgen's coast is seed-derived, but the suite's seed
        // produces a coast on the world's edge).
        s.cameraTargetX = 500.0;
        s.cameraTargetZ = 8000.0;
        break;
    case ScenarioId::rain:
        s.timeOfDayGameHours = 13.0;
        s.weatherKind        = 2; // rain (the weather state machine's RAIN ordinal)
        break;
    case ScenarioId::panels_open:
        s.timeOfDayGameHours = 10.0;
        s.weatherKind        = 0;
        s.panelsOpen         = true; // stages --set harrowmere_ui.visible_on_start=1
        break;
    case ScenarioId::corpse_aftermath:
        s.timeOfDayGameHours = 18.0; // dusk (aftermath mood)
        s.weatherKind        = 0;
        // Camera at the spawn village (the suite's seed produces a corpse
        // near the spawn after the scripted combat tick).
        s.cameraTargetX = 8000.0;
        s.cameraTargetZ = 8000.0;
        break;
    default:
        break;
    }
    return s;
}

// The budget row for one scenario. The 5.3.1 gate anchors (walk_village,
// coast, rain) use the A-146 default (16.6 ms p95); the others are measured
// but not gated, so their budget row is informational (still defaults to
// A-146 so a reviewer sees the gate's threshold in the report).
double budgetP95For(
    ScenarioId id
)
{
    (void)id;
    return harrowmere::RENDER_FRAME_BUDGET_MS; // A-146
}

// Write a string to a file (creating parent directories as needed). Returns
// true on success. Used by the run to write report.json and checklist.md.
bool writeFile(
    const std::filesystem::path &path, const std::string &content
)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
        return false;
    std::ofstream out(path);
    if (!out)
        return false;
    out << content;
    return out.good();
}

// Read a file's contents into a string. Returns empty on failure. Used by
// the run to read the per-scenario percentile report the engine writes.
std::string readFile(
    const std::filesystem::path &path
)
{
    std::ifstream in(path);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

// ---- public API ------------------------------------------------------------

const char *scenarioName(
    ScenarioId id
)
{
    const auto i = static_cast<std::size_t>(id);
    if (i >= SCENARIO_COUNT)
        return "<unknown>";
    return kScenarioNames[i];
}

const std::vector<Scenario> &scenarioList()
{
    static const std::vector<Scenario> list = []() {
        std::vector<Scenario> v;
        v.reserve(SCENARIO_COUNT);
        for (std::uint8_t i = 0; i < SCENARIO_COUNT; ++i)
        {
            const auto id = static_cast<ScenarioId>(i);
            Scenario s;
            s.setup       = setupFor(id);
            s.checklist   = checklistFor(id);
            s.budgetP95Ms = budgetP95For(id);
            v.push_back(std::move(s));
        }
        return v;
    }();
    return list;
}

// ---- budget report serialize / parse --------------------------------------

std::string serializeBudgetReport(
    const BudgetReport &report
)
{
    // JSON shape (stable across versions — the regression contract compares
    // reports of the same suite run). Indented for readability; the parser
    // is whitespace-tolerant so a minified report still parses.
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "{\n";
    out << "  \"budgetGateMs\": " << report.budgetGateMs << ",\n";
    out << "  \"referenceMachineTag\": \"" << report.referenceMachineTag << "\",\n";
    out << "  \"allGatesGreen\": " << (report.allGatesGreen ? "true" : "false") << ",\n";
    out << "  \"rows\": [\n";
    for (std::size_t i = 0; i < report.rows.size(); ++i)
    {
        const auto &r = report.rows[i];
        out << "    {\n";
        out << "      \"name\": \"" << r.name << "\",\n";
        out << "      \"p50Ms\": " << r.p50Ms << ",\n";
        out << "      \"p95Ms\": " << r.p95Ms << ",\n";
        out << "      \"frameSamples\": " << r.frameSamples << ",\n";
        out << "      \"gateAsserted\": " << (r.gateAsserted ? "true" : "false") << "\n";
        out << "    }" << (i + 1 < report.rows.size() ? "," : "") << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

namespace
{

// A minimal JSON value extractor: finds `"key": value` and parses value as
// a double, a uint64, a bool, or a quoted string. The parser is lenient
// (whitespace-tolerant, comment-skipping is NOT supported — JSON has no
// comments) but strict on the numbers (a malformed percentile is a parse
// error, never a silent 0). Returns true on success.
//
// This is NOT a general JSON parser — it handles exactly the shape
// serializeBudgetReport produces. A general parser would be overkill for
// this single-file format.
bool extractDouble(
    const std::string &text, const std::string &key, double &out
)
{
    const std::string needle = "\"" + key + "\":";
    auto pos                 = text.find(needle);
    if (pos == std::string::npos)
        return false;
    pos += needle.size();
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r'))
        ++pos;
    if (pos >= text.size())
        return false;
    try
    {
        out = std::stod(text.substr(pos));
        return true;
    } catch (...)
    {
        return false;
    }
}

bool extractUint64(
    const std::string &text, const std::string &key, std::uint64_t &out
)
{
    double d = 0.0;
    if (!extractDouble(text, key, d))
        return false;
    if (d < 0.0)
        return false;
    out = static_cast<std::uint64_t>(d);
    return true;
}

bool extractBool(
    const std::string &text, const std::string &key, bool &out
)
{
    const std::string needle = "\"" + key + "\":";
    auto pos                 = text.find(needle);
    if (pos == std::string::npos)
        return false;
    pos += needle.size();
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r'))
        ++pos;
    if (pos >= text.size())
        return false;
    if (text.compare(pos, 4, "true") == 0)
    {
        out = true;
        return true;
    }
    if (text.compare(pos, 5, "false") == 0)
    {
        out = false;
        return true;
    }
    return false;
}

bool extractString(
    const std::string &text, const std::string &key, std::string &out
)
{
    const std::string needle = "\"" + key + "\": \"";
    auto pos                 = text.find(needle);
    if (pos == std::string::npos)
        return false;
    pos += needle.size();
    const auto end = text.find('"', pos);
    if (end == std::string::npos)
        return false;
    out = text.substr(pos, end - pos);
    return true;
}

// Extract the per-row block at position `rowStart` (the substring from one
// `"name":` to the next). Returns the block and advances rowStart past it.
bool extractRowBlock(
    const std::string &text, std::size_t &cursor, std::string &outBlock
)
{
    auto start = text.find("\"name\":", cursor);
    if (start == std::string::npos)
        return false;
    // The row ends at the matching `}` after the row's fields.
    auto end = text.find('}', start);
    if (end == std::string::npos)
        return false;
    outBlock = text.substr(start, end - start + 1);
    cursor   = end + 1;
    return true;
}

} // namespace

bool parseBudgetReport(
    const std::string &text, BudgetReport &out
)
{
    out = BudgetReport{};
    if (text.empty())
        return false;

    if (!extractDouble(text, "budgetGateMs", out.budgetGateMs))
        return false;
    if (!extractBool(text, "allGatesGreen", out.allGatesGreen))
        return false;
    extractString(text, "referenceMachineTag", out.referenceMachineTag);

    // Walk the rows array.
    auto rowsPos = text.find("\"rows\":");
    if (rowsPos == std::string::npos)
        return false;
    std::size_t cursor = rowsPos;
    std::string block;
    while (extractRowBlock(text, cursor, block))
    {
        ScenarioBudgetRow row;
        std::string name;
        if (!extractString(block, "name", name))
            continue;
        row.name = name;
        // Map the name back to a ScenarioId.
        for (std::uint8_t i = 0; i < SCENARIO_COUNT; ++i)
        {
            if (name == kScenarioNames[i])
            {
                row.id = static_cast<ScenarioId>(i);
                break;
            }
        }
        if (!extractDouble(block, "p50Ms", row.p50Ms))
            return false;
        if (!extractDouble(block, "p95Ms", row.p95Ms))
            return false;
        if (!extractUint64(block, "frameSamples", row.frameSamples))
            return false;
        if (!extractBool(block, "gateAsserted", row.gateAsserted))
            return false;
        out.rows.push_back(std::move(row));
    }

    // Re-derive allGatesGreen from the rows (defense in depth: the report's
    // flag might be stale vs the parsed rows).
    bool all_green = true;
    for (const auto &r : out.rows)
    {
        if (r.gateAsserted && r.p95Ms > out.budgetGateMs)
            all_green = false;
    }
    out.allGatesGreen = all_green;
    return true;
}

// ---- the run ---------------------------------------------------------------

BudgetReport runSuite(
    const RunConfig &config
)
{
    BudgetReport report;
    report.budgetGateMs        = harrowmere::RENDER_FRAME_BUDGET_MS; // A-146
    report.referenceMachineTag = config.referenceMachineTag.empty() ? "unknown" : config.referenceMachineTag;

    const auto &scenarios = scenarioList();
    report.rows.reserve(scenarios.size());

    for (const auto &scenario : scenarios)
    {
        ScenarioBudgetRow row;
        runSingleScenario(config, scenario.setup.id, row);
        report.rows.push_back(std::move(row));
    }

    // Aggregate: allGatesGreen iff every gateAsserted row's p95 ≤ budgetGateMs.
    bool all_green = true;
    for (const auto &r : report.rows)
    {
        if (r.gateAsserted && r.p95Ms > report.budgetGateMs)
            all_green = false;
    }
    report.allGatesGreen = all_green;

    // Write the report to <outputDirectory>/report.json (the regression
    // contract's archival artifact).
    if (!config.outputDirectory.empty())
    {
        const auto reportPath = config.outputDirectory / "report.json";
        writeFile(reportPath, serializeBudgetReport(report));
    }

    return report;
}

bool runSingleScenario(
    const RunConfig &config, ScenarioId id, ScenarioBudgetRow &outRow
)
{
    outRow              = ScenarioBudgetRow{};
    outRow.id           = id;
    outRow.name         = scenarioName(id);
    outRow.gateAsserted = isGateAnchor(id);

    // The per-scenario output directory: <outputDirectory>/<scenarioName>/.
    std::filesystem::path scenarioDir;
    if (!config.outputDirectory.empty())
    {
        scenarioDir = config.outputDirectory / outRow.name;
        std::error_code ec;
        std::filesystem::create_directories(scenarioDir, ec);
    }

    // In dry mode (or no engine executable), the suite walks the scenarios
    // and writes a zeroed report. The plumbing is exercised, not the GPU —
    // TestCapture_RunProducesPngsPerScenario uses this to assert the suite's
    // structure without a real GPU.
    if (config.dry || config.engineExecutable.empty())
    {
        outRow.p50Ms        = 0.0;
        outRow.p95Ms        = 0.0;
        outRow.frameSamples = 0;
        // Write a placeholder checklist.md so the regression contract's
        // expectation file exists even in dry mode (a reviewer can read the
        // checklist without running the suite).
        if (!scenarioDir.empty())
        {
            const auto &scenarios = scenarioList();
            const auto &scenario  = scenarios[static_cast<std::size_t>(id)];
            std::ostringstream md;
            md << "# " << outRow.name << " checklist\n\n";
            md << "Seed: " << scenario.setup.seed << "\n";
            md << "Time of day (game hours): " << scenario.setup.timeOfDayGameHours << "\n";
            md << "Weather kind: " << scenario.setup.weatherKind << "\n";
            md << "Panels open: " << (scenario.setup.panelsOpen ? "true" : "false") << "\n";
            md << "Camera target (x, z): " << scenario.setup.cameraTargetX << ", " << scenario.setup.cameraTargetZ
               << "\n";
            md << "Camera yaw/pitch (deg): " << scenario.setup.cameraYawDeg << ", " << scenario.setup.cameraPitchDeg
               << "\n";
            md << "Quit after (seconds): " << scenario.setup.quitAfterSeconds << "\n\n";
            md << "## Checklist (reviewer must confirm in PNG)\n\n";
            for (const auto &line : scenario.checklist)
                md << "- [ ] " << line.feature << " (" << line.backing << ")\n";
            md << "\n## Budget\n\n";
            md << "- p95 gate: " << scenario.budgetP95Ms << " ms (A-146)\n";
            md << "- gate asserted: " << (outRow.gateAsserted ? "true" : "false") << "\n";
            writeFile(scenarioDir / "checklist.md", md.str());
        }
        return true;
    }

    // Wet mode: shell out to the engine executable per scenario. The command
    // stages --set overrides for the scenario's setup (seed, time of day,
    // weather, panels, camera, quit_after_seconds). The engine writes the
    // PNG into the per-scenario directory (the screenshot module's
    // output_directory key) and writes a percentile report the run reads back.
    //
    // The command shape mirrors the bootstrap smoke (task 1.1.1): one
    // --set per override, --set engine.quit_after_seconds=<float>.
    const auto &scenarios = scenarioList();
    const auto &scenario  = scenarios[static_cast<std::size_t>(id)];

    std::ostringstream cmd;
    cmd << "\"" << config.engineExecutable.string() << "\"";
    cmd << " --set engine.quit_after_seconds=" << std::fixed << std::setprecision(1) << scenario.setup.quitAfterSeconds;
    cmd << " --set harrowmere.seed=" << scenario.setup.seed;
    cmd << " --set harrowmere.time_of_day_game_hours=" << std::fixed << std::setprecision(2)
        << scenario.setup.timeOfDayGameHours;
    cmd << " --set harrowmere.weather_kind=" << scenario.setup.weatherKind;
    cmd << " --set harrowmere_ui.visible_on_start=" << (scenario.setup.panelsOpen ? 1 : 0);
    cmd << " --set harrowmere_ui.follow_camera=1";
    if (!scenarioDir.empty())
        cmd << " --set screenshot.output_directory=\"" << scenarioDir.string() << "\"";

    const std::string cmdStr = cmd.str();
    const int rc             = std::system(cmdStr.c_str());
    if (rc != 0)
    {
        // The scenario failed to run; record a zeroed row and return false.
        outRow.p50Ms        = 0.0;
        outRow.p95Ms        = 0.0;
        outRow.frameSamples = 0;
        return false;
    }

    // Read the percentile report the engine's profiler wrote into the
    // scenario directory. The report's shape is the same as
    // serializeBudgetReport's per-row block; the parser handles it.
    if (!scenarioDir.empty())
    {
        const auto reportText = readFile(scenarioDir / "frame_percentiles.txt");
        if (!reportText.empty())
        {
            double p50 = 0.0, p95 = 0.0;
            std::uint64_t samples = 0;
            extractDouble(reportText, "p50Ms", p50);
            extractDouble(reportText, "p95Ms", p95);
            extractUint64(reportText, "frameSamples", samples);
            outRow.p50Ms        = p50;
            outRow.p95Ms        = p95;
            outRow.frameSamples = samples;
        }
    }

    return true;
}

} // namespace harrowmere::capture
