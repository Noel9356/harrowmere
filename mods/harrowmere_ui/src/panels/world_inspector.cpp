// world_inspector.cpp - the World Inspector panel (card 7.3, panel 6 of 8).
//
// Reads: CharactersView (the present set - the same window the 7.2 bridge
// renders, so the list and the world's markers agree) + PlayerView (the
// framing row).
//
// Card 7.3's "click an NPC marker -> role, species, age, needs bars, LOD
// tier, trust tier toward me": v1's picker is the selection list (nearest
// first) - a true 3D marker pick needs the camera's view-projection, which
// the published capability surface does not expose; the list is the §5
// "panel layout is yours" reading, and the selection id is the same handle a
// future marker-pick would set (PanelContext::selectedCharacterId).
//
// The trust tier is 4.5's OBSERVER-side read: what THEY think of ME
// (TrustTierId; Phase 3 fills it - v1 reads STRANGER everywhere).

#include "panels/panels.hpp"

#include <cstdio>
#include <cmath>

namespace harrowmere::ui
{

namespace
{

const char *tierName(
    harrowmere::views::LodTierId t
)
{
    switch (t)
    {
    case harrowmere::views::LodTierId::Near:
        return "NEAR";
    case harrowmere::views::LodTierId::Mid:
        return "MID";
    case harrowmere::views::LodTierId::Far:
        return "FAR";
    }
    return "?";
}

const char *trustName(
    harrowmere::views::TrustTierId t
)
{
    switch (t)
    {
    case harrowmere::views::TrustTierId::Stranger:
        return "STRANGER";
    case harrowmere::views::TrustTierId::Watched:
        return "WATCHED";
    case harrowmere::views::TrustTierId::Distrusted:
        return "DISTRUSTED";
    case harrowmere::views::TrustTierId::Shunned:
        return "SHUNNED";
    }
    return "?";
}

// One need bar (0..SCALE; A-37's decay drives the value, the panel renders).
void needBar(
    const char *label, std::uint64_t value
)
{
    const float frac = static_cast<float>(static_cast<double>(value) / 10000.0);
    ImGui::TextUnformatted(label);
    ImGui::SameLine(90.0f);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.45f, 0.65f, 0.35f, 1.0f));
    ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), "");
    ImGui::PopStyleColor();
}

} // namespace

void drawWorldInspectorPanel(
    PanelContext &ctx, bool *pOpen
)
{
    if (ctx.characters == nullptr)
        return;
    const harrowmere::views::CharactersView &c = *ctx.characters;

    ImGui::SetNextWindowSize(ImVec2(480.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(kPanelBindings[static_cast<std::size_t>(PanelId::WorldInspector)].title, pOpen))
    {
        ImGui::End();
        return;
    }

    ImGui::Text("%zu present character(s) within 300 m", c.present.size());
    ImGui::TextDisabled("Pick a row to inspect (the list mirrors the rendered markers).");
    ImGui::Separator();

    // --- the selection list ----------------------------------------------------
    for (const auto &ch : c.present)
    {
        ImGui::PushID(static_cast<int>(ch.id % 0x7fffffff));
        const bool selected = ctx.selectedCharacterId == ch.id;
        char label[128];
        std::snprintf(
            label,
            sizeof(label),
            "%s%s  %s  Lv %u  (%.0f m)",
            ch.isPlayer ? "[you] " : "",
            ch.name.empty() ? "(unnamed)" : ch.name.c_str(),
            ch.roleKey.empty() ? "-" : ch.roleKey.c_str(),
            static_cast<unsigned>(ch.level),
            ctx.player != nullptr
                ? std::sqrt(
                      (ch.position.x - ctx.player->position.x) * (ch.position.x - ctx.player->position.x)
                      + (ch.position.z - ctx.player->position.z) * (ch.position.z - ctx.player->position.z)
                  )
                : 0.0
        );
        if (ImGui::Selectable(label, selected))
        {
            ctx.selectedCharacterId = ch.id; // UI state; not a sim write
        }
        ImGui::PopID();
    }

    // --- the detail pane ---------------------------------------------------------
    const harrowmere::views::CharacterSummary *sel = nullptr;
    for (const auto &ch : c.present)
    {
        if (ch.id == ctx.selectedCharacterId)
        {
            sel = &ch;
            break;
        }
    }

    ImGui::Separator();
    if (sel == nullptr)
    {
        ImGui::TextDisabled("(select a character above)");
        ImGui::End();
        return;
    }

    ImGui::Text(
        "id #%llu  -  %s (%s)",
        static_cast<unsigned long long>(sel->id),
        sel->speciesKey.empty() ? "unknown species" : sel->speciesKey.c_str(),
        sel->sapient ? "sapient" : "non-sapient"
    );
    ImGui::Text(
        "Role: %s    Age: %u years",
        sel->roleKey.empty() ? "-" : sel->roleKey.c_str(),
        static_cast<unsigned>(sel->ageYears)
    );
    ImGui::Text("LOD tier: %s    Power: %llu", tierName(sel->tier), static_cast<unsigned long long>(sel->cachedPower));
    ImGui::Text(
        "HP %llu / %llu", static_cast<unsigned long long>(sel->currentHp), static_cast<unsigned long long>(sel->maxHp)
    );
    ImGui::Text("Trust tier toward you: %s", trustName(sel->trustTowardMe));
    if (sel->inCombat)
    {
        ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.2f, 1.0f), "IN COMBAT");
    }

    if (ImGui::CollapsingHeader("Needs", ImGuiTreeNodeFlags_DefaultOpen))
    {
        needBar("hunger", sel->needs[0]);
        needBar("rest", sel->needs[1]);
        needBar("wealth", sel->needs[2]);
        needBar("social", sel->needs[3]);
    }

    // --- targeted actions on the selection (card 7.3's command set) -----------
    // Duel (2.7's consent), report crime + bribe (4.1's paths - logged
    // rejections until Phase 3). Only sapient targets make sense; the sim
    // re-gates everything (range, consent, existence).
    if (!sel->isPlayer && sel->sapient)
    {
        if (ImGui::Button("Request duel"))
        {
            submitRequestDuel(ctx, sel->id);
        }
        ImGui::SameLine();
        if (ImGui::Button("Report crime"))
        {
            submitReportCrime(ctx, sel->id);
        }
        ImGui::SameLine();
        if (ImGui::Button("Bribe"))
        {
            submitBribe(ctx, sel->id);
        }
        ImGui::TextDisabled("Actions queue as commands; step 9 applies (or rejects with a signal).");
    }

    ImGui::End();
}

} // namespace harrowmere::ui
