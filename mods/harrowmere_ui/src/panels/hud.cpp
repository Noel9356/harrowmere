// hud.cpp - the HUD (card 7.3, panel 1 of 8).
//
// Reads: WorldView (season/weather), PlayerView (hp/xp/currency/status),
// UiSignals (WANTED / INVESTIGATED - the design reference's observable
// signals end here; §4.2 rule 3's fog-of-war line for the HUD).
//
// Always-on (kPanelBindings[Hud].alwaysOn): drawn whenever the overlay is
// visible, top-left-anchored under the debug panel's default position is
// taken, so the HUD sits top-right. No buttons, no submissions - the HUD is
// read-only by design.

#include "panels/panels.hpp"

namespace harrowmere::ui
{

namespace
{

const char *seasonName(
    harrowmere::views::SeasonId s
)
{
    switch (s)
    {
    case harrowmere::views::SeasonId::Spring:
        return "Spring";
    case harrowmere::views::SeasonId::Summer:
        return "Summer";
    case harrowmere::views::SeasonId::Autumn:
        return "Autumn";
    case harrowmere::views::SeasonId::Winter:
        return "Winter";
    }
    return "?";
}

const char *weatherName(
    harrowmere::views::WeatherKind w
)
{
    switch (w)
    {
    case harrowmere::views::WeatherKind::Clear:
        return "Clear";
    case harrowmere::views::WeatherKind::Rain:
        return "Rain";
    case harrowmere::views::WeatherKind::Storm:
        return "Storm";
    case harrowmere::views::WeatherKind::Fog:
        return "Fog";
    case harrowmere::views::WeatherKind::Snow:
        return "Snow";
    }
    return "?";
}

// One status icon row entry: derived statuses first (D-CHAR-1 - the core
// computed them for the snapshot), then the justice signals.
void statusIcon(
    bool active, const char *label, const ImVec4 &color
)
{
    if (!active)
        return;
    ImGui::TextColored(color, "[%s]", label);
    ImGui::SameLine();
}

} // namespace

void drawHudPanel(
    PanelContext &ctx, bool *pOpen
)
{
    if (ctx.world == nullptr || ctx.player == nullptr)
        return;

    ImGui::SetNextWindowPos(ImVec2(260.0f, 8.0f), ImGuiCond_FirstUseEver);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize
                                 | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar;
    if (!ImGui::Begin("Harrowmere HUD###harrowmere_hud", pOpen, flags))
    {
        ImGui::End();
        return;
    }

    const harrowmere::views::PlayerView &p = *ctx.player;

    // --- vitals ---------------------------------------------------------------
    const float hpFrac
        = p.maxHp > 0 ? static_cast<float>(static_cast<double>(p.currentHp) / static_cast<double>(p.maxHp)) : 0.0f;
    ImGui::Text("%s  Lv %u", p.name.c_str(), static_cast<unsigned>(p.level));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.75f, 0.20f, 0.20f, 1.0f));
    ImGui::ProgressBar(hpFrac, ImVec2(220.0f, 0.0f), "");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Text("%llu/%llu HP", static_cast<unsigned long long>(p.currentHp), static_cast<unsigned long long>(p.maxHp));

    // XP: progress toward the next level's cost (A-7/A-8; 0 at the cap).
    const float xpFrac
        = p.nextLevelCost > 0
            ? static_cast<float>(
                  static_cast<double>(p.availableXp < 0 ? 0 : p.availableXp) / static_cast<double>(p.nextLevelCost)
              )
            : 1.0f;
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.30f, 0.55f, 0.85f, 1.0f));
    ImGui::ProgressBar(xpFrac, ImVec2(220.0f, 0.0f), "");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Text(
        "XP %lld / %llu", static_cast<long long>(p.availableXp), static_cast<unsigned long long>(p.nextLevelCost)
    );

    // --- purse, clock, climate ----------------------------------------------
    ImGui::Text("Coin: %lld", static_cast<long long>(p.currency));
    char clockBuf[48];
    formatGameClock(p.worldTime, clockBuf, sizeof(clockBuf));
    ImGui::TextUnformatted(clockBuf);
    if (ctx.world != nullptr)
    {
        ImGui::Text("%s  /  %s", seasonName(ctx.world->season.season), weatherName(ctx.world->weather.kind));
    }

    // --- status icons ----------------------------------------------------------
    // Derived statuses (the core computed them; D-CHAR-1). Colors are
    // presentation defaults. The icon row is a SameLine chain; a line break
    // follows via the next full-width widget (no window internals needed).
    statusIcon(p.inCombat, "COMBAT", ImVec4(0.90f, 0.45f, 0.20f, 1.0f));
    statusIcon(p.inEvent, "EVENT", ImVec4(0.60f, 0.55f, 0.95f, 1.0f));
    statusIcon(p.inWar, "WAR", ImVec4(0.95f, 0.30f, 0.30f, 1.0f));
    statusIcon(p.arrested, "ARRESTED", ImVec4(0.55f, 0.55f, 0.60f, 1.0f));
    statusIcon(p.inDuel, "DUEL", ImVec4(0.90f, 0.80f, 0.30f, 1.0f));
    ImGui::NewLine(); // end the SameLine chain

    // Justice signals: the ONLY wanted/investigated surface (design
    // reference: "give observable signals" - guards shadowing = INVESTIGATED,
    // a poster = WANTED; never a case score, §4.2 rule 3).
    if (ctx.signals != nullptr)
    {
        for (const auto &s : ctx.signals->entries)
        {
            switch (s.kind)
            {
            case harrowmere::views::PlayerSignalKind::Investigated:
                ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f), "[WATCHED] %s", s.text.c_str());
                break;
            case harrowmere::views::PlayerSignalKind::Wanted:
                ImGui::TextColored(ImVec4(0.95f, 0.30f, 0.30f, 1.0f), "[WANTED] %s", s.text.c_str());
                break;
            case harrowmere::views::PlayerSignalKind::Poster:
                ImGui::TextColored(ImVec4(0.95f, 0.30f, 0.30f, 1.0f), "[POSTER] %s", s.text.c_str());
                break;
            case harrowmere::views::PlayerSignalKind::Greeting:
                ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.70f, 1.0f), "%s", s.text.c_str());
                break;
            case harrowmere::views::PlayerSignalKind::War:
                ImGui::TextColored(ImVec4(1.00f, 0.35f, 0.10f, 1.0f), "[WAR] %s", s.text.c_str());
                break;
            }
        }
    }

    ImGui::End();
}

} // namespace harrowmere::ui
