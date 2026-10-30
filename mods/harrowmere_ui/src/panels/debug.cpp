// debug.cpp -- Phase-1 debug overlay panel (Phase-1 slice of task 7.3).
//
// Draws the single debug panel this phase ships: region / territory / law set
// / safe-zone flag underfoot, season, weather, current tick number, fps. This
// is the plumbing Phase-2's real panels (HUD, character sheet, inventory,
// world inspector, time controls -- card 7.3 full) ride on.
//
// Reads the world exclusively through SimCapability views (D-ARCH-1, common.md
// §4.2). The view types are published by harrowmere_core in
// harrowmere_capability.hpp -- the ONLY header from harrowmere_core this file
// includes. No sim internals, no store handles, no Character* -- ids and
// copies only (R10, common.md §4.5). The 7.3P1 Done-when review grep confirms
// this: "No file in mods/harrowmere_ui/ includes harrowmere_core internals."
//
// Compile-time view binding (test TestUi_DebugPanelBindsWorldView):
//   The function signature takes the WorldView + other view types by const
//   reference. Including harrowmere_capability.hpp and calling drawDebugPanel
//   with the view types is itself the binding proof -- a future change to a
//   view shape that drops a field this panel reads fails to compile this TU,
//   surfacing the breakage at build time rather than mid-sim.
//
// View field names: the fields used below are the ones Phase-1's SimCapability
// publishes. If the as-built harrowmere_capability.hpp uses different names
// (the card's v0.1 stubs are filled by Phase-2's task 2.x and 3.x), adapt the
// field accesses -- the panel structure stays. The fields read here are the
// ones the 7.3P1 Done-when enumerates: region, territory, law set, safe-zone
// flag, season, weather, tick number.

#include "../ui_harrowmere.hpp"  // pulls in harrowmere_capability.hpp + forward decl

#include <imgui.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace harrowmere::ui {

namespace {

// Small helper -- formats a Position for the debug panel. Position is f64 x/y/z
// per design reference §Conventions (common.md F.1); the overlay is
// presentation code so f64 here is fine (R5 forbids f64 only in *game rules*,
// not presentation).
void formatPosition(char* buf, std::size_t bufSize, const harrowmere::views::Position& p) {
    std::snprintf(buf, bufSize, "(%.1f, %.1f, %.1f)", p.x, p.y, p.z);
}

// Helper -- formats a season id to the doc name. The SeasonId enum is closed
// (spring/summer/autumn/winter per A-51 / task 3.8); a missing case is a
// compile-time gap to fix, not a runtime branch.
const char* seasonName(harrowmere::views::SeasonId s) {
    switch (s) {
        case harrowmere::views::SeasonId::Spring: return "Spring";
        case harrowmere::views::SeasonId::Summer: return "Summer";
        case harrowmere::views::SeasonId::Autumn: return "Autumn";
        case harrowmere::views::SeasonId::Winter: return "Winter";
    }
    return "?";
}

// Helper -- formats a weather kind. WeatherKind is closed (task 3.8): clear,
// rain, storm, fog, snow.
const char* weatherName(harrowmere::views::WeatherKind w) {
    switch (w) {
        case harrowmere::views::WeatherKind::Clear: return "Clear";
        case harrowmere::views::WeatherKind::Rain:  return "Rain";
        case harrowmere::views::WeatherKind::Storm: return "Storm";
        case harrowmere::views::WeatherKind::Fog:   return "Fog";
        case harrowmere::views::WeatherKind::Snow:  return "Snow";
    }
    return "?";
}

// Helper -- formats a territory type. TerritoryType is closed (task 1.1):
// VILLAGE / CITY / KINGDOM / EMPIRE.
const char* territoryTypeName(harrowmere::views::TerritoryType t) {
    switch (t) {
        case harrowmere::views::TerritoryType::Village: return "Village";
        case harrowmere::views::TerritoryType::City:     return "City";
        case harrowmere::views::TerritoryType::Kingdom:  return "Kingdom";
        case harrowmere::views::TerritoryType::Empire:    return "Empire";
    }
    return "?";
}

}  // namespace

void drawDebugPanel(const harrowmere::views::WorldView&    world,
                    const harrowmere::views::PlayerView&  player,
                    const harrowmere::views::JusticeView& justice,
                    const harrowmere::views::EventsView&  events,
                    const harrowmere::views::UiSignals&   signals,
                    int                             fps,
                    bool*                           pOpen) {
    // The window's collapse/close flags. The close button (ImGuiWindowFlags_
    // NoClose would suppress it) lets the player hide the overlay with the
    // mouse equivalent of F1 -- the caller treats *pOpen=false as "hide".
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_AlwaysAutoResize;

    // Top-left anchored: the debug overlay sits where it does not fight the
    // Phase-2 HUD (top-center, when it lands). Phase-2 panels will set their
    // own positions; this default is fine for Phase 1.
    ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Harrowmere Debug [F1]", pOpen, flags)) {
        // Collapsed -- skip drawing the body but still End.
        ImGui::End();
        return;
    }

    // --- World underfoot (region / territory / law / safe zone) ---
    if (ImGui::CollapsingHeader("World", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextUnformatted("Region:    ");
        ImGui::SameLine();
        ImGui::Text("id=%u  mana=%u/10000",
                     static_cast<unsigned>(world.currentRegion.id),
                     static_cast<unsigned>(world.currentRegion.manaRichness));

        ImGui::TextUnformatted("Territory: ");
        ImGui::SameLine();
        ImGui::Text("id=%llu  type=%s",
                     static_cast<unsigned long long>(world.currentTerritory.id),
                     territoryTypeName(world.currentTerritory.type));

        ImGui::TextUnformatted("Law set:   ");
        ImGui::SameLine();
        ImGui::TextUnformatted(world.currentTerritory.lawSetKey.c_str());

        ImGui::TextUnformatted("Safe zone: ");
        ImGui::SameLine();
        if (world.currentTerritory.safe) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "YES (spawn-safe)");
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "no");
        }

        ImGui::TextUnformatted("Lawless:   ");
        ImGui::SameLine();
        ImGui::TextUnformatted(world.currentTerritory.lawless ? "yes (garrison absent)" : "no");
    }

    // --- Climate (season / weather) ---
    if (ImGui::CollapsingHeader("Climate", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Season:  %s  (day %llu of %d)",
                     seasonName(world.season.season),
                     static_cast<unsigned long long>(world.season.dayOfSeason),
                     30 /* A-51 SEASON_LENGTH_GAME_DAYS */);
        ImGui::Text("Weather: %s", weatherName(world.weather.kind));
    }

    // --- Clocks / loop ---
    if (ImGui::CollapsingHeader("Loop", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Tick:    %llu",
                     static_cast<unsigned long long>(world.tickNumber));
        ImGui::Text("FPS:     %d", fps);
        // worldTime and realTime come from the WorldClock (task 0.3). They are
        // not in WorldView (which is the world *state*, not the clock); the
        // sim exposes them through the player's view for now. If the
        // capability does not surface them in Phase 1, leave the lines as
        // placeholders -- the 7.3P1 Done-when enumerates "tick number, fps"
        // and not the clocks; they arrive with the time-controls panel (7.3
        // full in Phase 2).
        if (player.worldTime > 0 || player.realTime > 0) {
            ImGui::Text("World:   %llu s  (day %llu)",
                         static_cast<unsigned long long>(player.worldTime),
                         static_cast<unsigned long long>(player.worldTime / 86400ull));
            ImGui::Text("Real:    %llu s",
                         static_cast<unsigned long long>(player.realTime));
        }
    }

    // --- Player (presence + power) ---
    if (ImGui::CollapsingHeader("Player", ImGuiTreeNodeFlags_DefaultOpen)) {
        char posBuf[64];
        formatPosition(posBuf, sizeof(posBuf), player.position);
        ImGui::Text("Position: %s", posBuf);
        ImGui::Text("Power:    cached=%llu  peak=%llu",
                     static_cast<unsigned long long>(player.cachedPower),
                     static_cast<unsigned long long>(player.peakPower));
        ImGui::Text("Level:    %u  (xp avail=%lld)",
                     static_cast<unsigned>(player.level),
                     static_cast<long long>(player.availableXp));
    }

    // --- Justice (fog-of-war whitelist -- only what the doc lets the player
    //     see, common.md §4.2 rule 3: reputation, notoriety, warrants against
    //     you, jail status -- never caseScore or hidden evidence) ---
    if (ImGui::CollapsingHeader("Justice")) {
        ImGui::Text("Reputation: %lld  (max ±%d)",
                     static_cast<long long>(justice.reputation),
                     10000 /* A-87 MAX_REPUTATION */);
        ImGui::Text("Notoriety:  %llu",
                     static_cast<unsigned long long>(justice.notoriety));
        ImGui::Text("Warrants:   %u active against you",
                     static_cast<unsigned>(justice.activeWarrantsAgainstYou));
        ImGui::Text("Jail:       %s",
                     justice.isJailed ? "jailed (sentence running)" : "free");
        if (justice.isJailed) {
            ImGui::Text("  sentence ends at worldTime=%llu",
                         static_cast<unsigned long long>(justice.jailUntil));
        }
    }

    // --- Signals (the doc's PlayerSignals queue, common.md §4.2) ---
    // Phase 1 has no signals yet (no justice / events systems active); the
    // queue is empty. Phase 3 fills it: guards shadowing (INVESTIGATING),
    // posters (WANTED), cold greetings (suspicion). Drawn here so the seam is
    // real from day one.
    if (ImGui::CollapsingHeader("Signals")) {
        if (signals.entries.empty()) {
            ImGui::TextDisabled("(no signals -- Phase 1)");
        } else {
            for (const auto& s : signals.entries) {
                const char* kindStr = "?";
                switch (s.kind) {
                    case harrowmere::views::PlayerSignalKind::Investigated: kindStr = "INVESTIGATED"; break;
                    case harrowmere::views::PlayerSignalKind::Wanted:        kindStr = "WANTED"; break;
                    case harrowmere::views::PlayerSignalKind::Poster:        kindStr = "POSTER"; break;
                    case harrowmere::views::PlayerSignalKind::Greeting:      kindStr = "GREETING"; break;
                }
                ImGui::Text("[%s] %s", kindStr, s.text.c_str());
            }
        }
    }

    // --- Events (joinable / active) ---
    if (ImGui::CollapsingHeader("Events")) {
        if (events.active.empty()) {
            ImGui::TextDisabled("(no active events -- Phase 1)");
        } else {
            for (const auto& e : events.active) {
                ImGui::Text("evt id=%llu  template=%s  state=%llu",
                             static_cast<unsigned long long>(e.eventId),
                             e.templateKey.c_str(),
                             static_cast<unsigned long long>(e.stateId));
            }
        }
    }

    ImGui::End();
}

}  // namespace harrowmere::ui
