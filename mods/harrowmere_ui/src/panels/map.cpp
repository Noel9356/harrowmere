// map.cpp - the Map panel (card 7.3, panel 7 of 8).
//
// Reads: WorldView.territories - the territory tree as a scrollable LIST
// ("not a graphic map in v1" - the card's own scope line), parents indented
// over children, safe zones and lawless territories flagged.
//
// "War overlays, my claims" are Phase 4 surfaces (6.2/6.3): the panel shows
// the tree + the safe/lawless facets today and carries the overlay rows as
// documented placeholders - a placeholder with a name and a phase is a plan
// seam, not a silent gap.

#include "panels/panels.hpp"

namespace harrowmere::ui
{

namespace
{

const char *typeName(
    harrowmere::views::TerritoryType t
)
{
    switch (t)
    {
    case harrowmere::views::TerritoryType::Village:
        return "village";
    case harrowmere::views::TerritoryType::City:
        return "city";
    case harrowmere::views::TerritoryType::Kingdom:
        return "kingdom";
    case harrowmere::views::TerritoryType::Empire:
        return "empire";
    }
    return "?";
}

// Tree depth of one node: a parent-walk over the id-ascending list, bounded
// like WorldStore's walks (the tree is shallow at v1 worldgen counts).
std::uint32_t territoryDepth(
    const harrowmere::views::WorldView &w, const harrowmere::views::TerritorySummary &t
)
{
    std::uint32_t depth = 0;
    std::uint64_t p     = t.parent;
    while (p != 0 && depth < 16)
    {
        ++depth;
        bool found = false;
        for (const auto &candidate : w.territories)
        {
            if (candidate.id == p)
            {
                p     = candidate.parent;
                found = true;
                break;
            }
        }
        if (!found)
        {
            break; // a malformed chain ends the walk (deterministic, R7-shaped)
        }
    }
    return depth;
}

// One territory row: type + id + the observable facets. The facets come
// from mapRowFacets (the pinned classification the test reads back) — the
// draw renders exactly what the classifier answers.
void territoryRow(
    const harrowmere::views::TerritorySummary &t, std::uint32_t depth, std::uint64_t underfoot
)
{
    ImGui::PushID(static_cast<int>(t.id % 0x7fffffff));
    for (std::uint32_t i = 0; i < depth; ++i)
    {
        ImGui::Indent(16.0f);
    }
    if (t.id == underfoot)
    {
        ImGui::TextColored(
            ImVec4(0.55f, 0.85f, 0.95f, 1.0f), "> %s #%llu", typeName(t.type), static_cast<unsigned long long>(t.id)
        );
    } else
    {
        ImGui::Text("%s #%llu", typeName(t.type), static_cast<unsigned long long>(t.id));
    }
    ImGui::SameLine(220.0f);
    const MapRowFacets facets = mapRowFacets(t);
    if ((facets & MapRowFacets::Safe) != MapRowFacets::None)
    {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "SAFE");
        ImGui::SameLine();
    }
    if ((facets & MapRowFacets::Lawless) != MapRowFacets::None)
    {
        ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.2f, 1.0f), "LAWLESS");
        ImGui::SameLine();
    }
    if ((facets & MapRowFacets::ClaimedByMe) != MapRowFacets::None)
    {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.3f, 1.0f), "CLAIM");
        ImGui::SameLine();
    }
    ImGui::TextDisabled("law: %s", t.lawSetKey.c_str());
    for (std::uint32_t i = 0; i < depth; ++i)
    {
        ImGui::Unindent(16.0f);
    }
    ImGui::PopID();
}

} // namespace

// The pinned classification (panels.hpp's contract): the row's own facets
// from the view data alone — safe zones, lawless territories, the player's
// own claims (6.3 fills the view; the facet renders from day one). The
// underfoot marker is the caller's comparison (the currentTerritory read),
// not the row's facet.
MapRowFacets mapRowFacets(
    const harrowmere::views::TerritorySummary &t
)
{
    std::uint32_t facets = 0;
    if (t.safe)
        facets |= static_cast<std::uint32_t>(MapRowFacets::Safe);
    if (t.lawless)
        facets |= static_cast<std::uint32_t>(MapRowFacets::Lawless);
    if (t.claimedByMe)
        facets |= static_cast<std::uint32_t>(MapRowFacets::ClaimedByMe);
    return static_cast<MapRowFacets>(facets);
}

void drawMapPanel(
    PanelContext &ctx, bool *pOpen
)
{
    if (ctx.world == nullptr)
        return;
    const harrowmere::views::WorldView &w = *ctx.world;

    ImGui::SetNextWindowSize(ImVec2(520.0f, 460.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(kPanelBindings[static_cast<std::size_t>(PanelId::Map)].title, pOpen))
    {
        ImGui::End();
        return;
    }

    ImGui::Text("The territory tree (%zu nodes)", w.territories.size());
    ImGui::TextDisabled("Scrolling list, not a graphic map (v1 scope). Wars and claims overlay with 6.2/6.3.");

    if (w.territories.empty())
    {
        ImGui::TextDisabled("(the world is unclaimed frontier)");
        ImGui::End();
        return;
    }

    ImGui::BeginChild("territory_tree", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    for (const auto &t : w.territories)
    {
        territoryRow(t, territoryDepth(w, t), w.currentTerritory.id);
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace harrowmere::ui
