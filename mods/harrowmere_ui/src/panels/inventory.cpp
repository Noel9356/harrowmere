// inventory.cpp - the Inventory panel (card 7.3, panel 3 of 8).
//
// Reads: PlayerView (the bag, the equipped instances, the known recipes with
// their craft-gate verdicts). Submits: Consume / Equip / Unequip / Craft -
// the shared submit helpers. v1 trade (buy/sell from a vendor) lives with
// the Events/interaction loop; the panel's buy/sell column is reserved.

#include "panels/panels.hpp"

#include <cstdio>

namespace harrowmere::ui
{

void drawInventoryPanel(
    PanelContext &ctx, bool *pOpen
)
{
    if (ctx.player == nullptr)
        return;
    const harrowmere::views::PlayerView &p = *ctx.player;

    ImGui::SetNextWindowSize(ImVec2(520.0f, 460.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(kPanelBindings[static_cast<std::size_t>(PanelId::Inventory)].title, pOpen))
    {
        ImGui::End();
        return;
    }

    ImGui::Text("Coin: %lld", static_cast<long long>(p.currency));

    // --- the bag -----------------------------------------------------------------
    if (ImGui::CollapsingHeader("Bag", ImGuiTreeNodeFlags_DefaultOpen))
    {
        constexpr ImGuiTableFlags flags
            = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("bag", 4, flags))
        {
            ImGui::TableSetupColumn("item");
            ImGui::TableSetupColumn("count");
            ImGui::TableSetupColumn("consume");
            ImGui::TableSetupColumn("equip");
            ImGui::TableHeadersRow();
            if (p.bag.empty())
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("(empty)");
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
            }
            for (const auto &entry : p.bag)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entry.name.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%u", static_cast<unsigned>(entry.count));
                ImGui::TableNextColumn();
                if (!entry.consumable || entry.count == 0)
                {
                    ImGui::TextDisabled("-");
                } else if (ImGui::SmallButton(("consume##" + entry.itemKey).c_str()))
                {
                    submitConsume(ctx, entry.itemKey);
                }
                ImGui::TableNextColumn();
                // Equippable bag rows are stack-side (materials); equippables
                // reach the body as INSTANCES (2.4) - the loose list shows
                // under Equipped's "loose" section below.
                ImGui::TextDisabled(entry.equippable ? "(instance)" : "-");
            }
            ImGui::EndTable();
        }
    }

    // --- equipped + the loose instances (the equip/unequip pairs) ---------------
    if (ImGui::CollapsingHeader("Equipped", ImGuiTreeNodeFlags_DefaultOpen))
    {
        constexpr ImGuiTableFlags flags
            = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("equipped", 3, flags))
        {
            ImGui::TableSetupColumn("worn");
            ImGui::TableSetupColumn("instance");
            ImGui::TableSetupColumn("unequip");
            ImGui::TableHeadersRow();
            if (p.equipped.empty())
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("(nothing equipped)");
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
            }
            for (const auto &worn : p.equipped)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(worn.name.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("#%llu", static_cast<unsigned long long>(worn.itemInstanceId));
                ImGui::TableNextColumn();
                if (ImGui::SmallButton(("unequip##" + std::to_string(worn.itemInstanceId)).c_str()))
                {
                    submitUnequip(ctx, worn.itemInstanceId);
                }
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Equipping a loose instance happens from the loot/interact flow (E / pick-up).");
    }

    // --- crafting -----------------------------------------------------------------
    // The recipes list is "by known recipes" (card 7.3): the loaded
    // recipes.json rows, each with its craft gate verdict (inputs in the bag
    // AND the CRAFTING skill requirement met - the core's verdict, never a
    // UI re-derivation).
    if (ImGui::CollapsingHeader("Crafting", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (p.recipes.empty())
        {
            ImGui::TextDisabled("(no known recipes)");
        }
        for (const auto &recipe : p.recipes)
        {
            ImGui::PushID(recipe.recipeKey.c_str());
            ImGui::TextUnformatted(recipe.outputName.c_str());
            ImGui::SameLine(180.0f);
            std::string inputs;
            for (const auto &in : recipe.inputs)
            {
                if (!inputs.empty())
                    inputs += ", ";
                inputs += in.itemKey + " " + std::to_string(in.count) + "/" + std::to_string(in.inBag);
            }
            ImGui::TextDisabled("%s", inputs.c_str());
            if (!recipe.reqSkillName.empty())
            {
                ImGui::SameLine(420.0f);
                ImGui::TextDisabled("%s %u", recipe.reqSkillName.c_str(), static_cast<unsigned>(recipe.reqSkillValue));
            }
            ImGui::SameLine(520.0f);
            if (!recipe.craftableNow)
            {
                ImGui::BeginDisabled();
            }
            if (ImGui::SmallButton("craft"))
            {
                submitCraft(ctx, recipe.recipeKey);
            }
            if (!recipe.craftableNow)
            {
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
    }

    ImGui::End();
}

} // namespace harrowmere::ui
