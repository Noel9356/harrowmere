// character.cpp - the Character panel (card 7.3, panel 2 of 8).
//
// Reads: PlayerView (stats/skills + effectiveness + caps + cooldowns).
// Submits: BuyStat / BuySkill / BuyLevel / RespecStat / RespecSkill /
// ChangeAppearance - all through the shared submit helpers (the panels'
// single write path). Respec buttons render the pinned cooldown countdown
// (A-3 day math against PlayerView.worldTime); the appearance change offers
// the v1 preset looks (2.6: never touches stats - the doc's hard rule the
// panel cannot violate by construction).

#include "panels/panels.hpp"

#include <cstdio>

namespace harrowmere::ui
{

namespace
{

// A stat/skill row: name, points, effectiveness, cap, buy + respec buttons.
void progressionRow(
    PanelContext &ctx,
    const char *label,
    std::uint32_t ordinal,
    std::uint64_t points,
    std::uint64_t effectiveness,
    std::uint64_t cap,
    bool isStat
)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::Text("%llu", static_cast<unsigned long long>(points));
    ImGui::TableNextColumn();
    ImGui::Text("%llu", static_cast<unsigned long long>(effectiveness));
    ImGui::TableNextColumn();
    ImGui::Text("/ %llu", static_cast<unsigned long long>(cap));
    ImGui::TableNextColumn();
    // Buy: at/over the cap the button greys (the sim would refuse - the
    // local gate only hides a doomed request; the sim stays authoritative).
    const bool canBuy = points < cap;
    char buyLabel[32];
    std::snprintf(buyLabel, sizeof(buyLabel), "+##%s%u", isStat ? "s" : "k", ordinal);
    if (!canBuy)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::SmallButton(buyLabel))
    {
        if (isStat)
        {
            submitBuyStat(ctx, ordinal);
        } else
        {
            submitBuySkill(ctx, ordinal);
        }
    }
    if (!canBuy)
    {
        ImGui::EndDisabled();
    }
    ImGui::TableNextColumn();
    // Respec with the cooldown countdown (canRespec's cooldown slot read
    // against worldTime; the peaceful gate is the sim's business).
    char respecLabel[32];
    std::snprintf(respecLabel, sizeof(respecLabel), "respec##%s%u", isStat ? "s" : "k", ordinal);
    if (points == 0)
    {
        ImGui::TextDisabled("-");
    } else if (ImGui::SmallButton(respecLabel))
    {
        if (isStat)
        {
            submitRespecStat(ctx, ordinal);
        } else
        {
            submitRespecSkill(ctx, ordinal);
        }
    }
}

// The respec cooldown countdown text (0 = ready). The cooldowns array is
// indexed by the published kCooldownCount ordinals: APPEARANCE, RESPEC_STAT,
// RESPEC_SKILL (core/enums.hpp order).
std::uint64_t cooldownRemaining(
    const harrowmere::views::PlayerView &p, std::uint32_t slot
)
{
    if (slot >= harrowmere::views::kCooldownCount)
        return 0;
    const std::uint64_t expiry = p.cooldowns[slot];
    if (expiry <= p.worldTime)
        return 0;
    return expiry - p.worldTime;
}

} // namespace

void drawCharacterPanel(
    PanelContext &ctx, bool *pOpen
)
{
    if (ctx.player == nullptr)
        return;
    const harrowmere::views::PlayerView &p = *ctx.player;

    ImGui::SetNextWindowSize(ImVec2(560.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(kPanelBindings[static_cast<std::size_t>(PanelId::Character)].title, pOpen))
    {
        ImGui::End();
        return;
    }

    // --- summary -------------------------------------------------------------
    ImGui::Text("%s  -  level %u%s", p.name.c_str(), static_cast<unsigned>(p.level), p.inDuel ? "  (dueling)" : "");
    ImGui::Text(
        "HP %llu/%llu    XP avail %lld    lifetime %llu    coin %lld",
        static_cast<unsigned long long>(p.currentHp),
        static_cast<unsigned long long>(p.maxHp),
        static_cast<long long>(p.availableXp),
        static_cast<unsigned long long>(p.totalXp),
        static_cast<long long>(p.currency)
    );
    ImGui::Text(
        "Power %llu (peak %llu)",
        static_cast<unsigned long long>(p.cachedPower),
        static_cast<unsigned long long>(p.peakPower)
    );

    // Dueling (2.7): the yield button while a duel runs (the opponent's id
    // is on the view; the sim validates the pair).
    if (p.inDuel)
    {
        if (ImGui::Button("Yield duel"))
        {
            submitYieldDuel(ctx);
        }
        ImGui::SameLine();
        ImGui::Text("vs character #%llu", static_cast<unsigned long long>(p.duelOpponent));
    }

    // Level purchase (players cap at 75 - A-6; the button greys at the cap,
    // where nextLevelCost reads 0).
    if (p.nextLevelCost == 0)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Buy level"))
    {
        submitBuyLevel(ctx);
    }
    if (p.nextLevelCost == 0)
    {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (p.nextLevelCost > 0)
    {
        ImGui::Text("cost %llu XP", static_cast<unsigned long long>(p.nextLevelCost));
    } else
    {
        ImGui::TextDisabled("at the level cap");
    }

    // --- stats -------------------------------------------------------------------
    if (ImGui::CollapsingHeader("Stats", ImGuiTreeNodeFlags_DefaultOpen))
    {
        constexpr ImGuiTableFlags flags
            = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("stats", 6, flags))
        {
            ImGui::TableSetupColumn("stat");
            ImGui::TableSetupColumn("points");
            ImGui::TableSetupColumn("effect");
            ImGui::TableSetupColumn("cap");
            ImGui::TableSetupColumn("buy");
            ImGui::TableSetupColumn("respec");
            ImGui::TableHeadersRow();
            for (std::uint32_t i = 0; i < harrowmere::views::kStatCount; ++i)
            {
                progressionRow(
                    ctx,
                    harrowmere::views::kStatNames[i],
                    i,
                    p.stats[i],
                    p.statEffectiveness[i],
                    p.statCap,
                    /*isStat=*/true
                );
            }
            ImGui::EndTable();
        }
        const std::uint64_t statCd = cooldownRemaining(p, 1 /* RESPEC_STAT */);
        if (statCd > 0)
        {
            char cd[48];
            formatGameClock(p.worldTime + statCd, cd, sizeof(cd));
            ImGui::TextDisabled(
                "stat respec cooldown: %llu game-s left (until %s)", static_cast<unsigned long long>(statCd), cd
            );
        }
    }

    // --- skills -----------------------------------------------------------------
    if (ImGui::CollapsingHeader("Skills", ImGuiTreeNodeFlags_DefaultOpen))
    {
        constexpr ImGuiTableFlags flags
            = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("skills", 6, flags))
        {
            ImGui::TableSetupColumn("skill");
            ImGui::TableSetupColumn("points");
            ImGui::TableSetupColumn("effect");
            ImGui::TableSetupColumn("cap");
            ImGui::TableSetupColumn("buy");
            ImGui::TableSetupColumn("respec");
            ImGui::TableHeadersRow();
            for (std::uint32_t i = 0; i < harrowmere::views::kSkillCount; ++i)
            {
                progressionRow(
                    ctx,
                    harrowmere::views::kSkillNames[i],
                    i,
                    p.skills[i],
                    p.skillEffectiveness[i],
                    p.skillCap,
                    /*isStat=*/false
                );
            }
            ImGui::EndTable();
        }
        const std::uint64_t skillCd = cooldownRemaining(p, 2 /* RESPEC_SKILL */);
        if (skillCd > 0)
        {
            char cd[48];
            formatGameClock(p.worldTime + skillCd, cd, sizeof(cd));
            ImGui::TextDisabled(
                "skill respec cooldown: %llu game-s left (until %s)", static_cast<unsigned long long>(skillCd), cd
            );
        }
    }

    // --- appearance ---------------------------------------------------------------
    // 2.6's gate: paid from availableXp (the doc's own comparison), APPEARANCE
    // cooldown; NEVER touches stats/skills/equipment. v1 offers the two
    // preset looks (the default set and the hooded set) - full customization
    // is a later-phase surface.
    if (ImGui::CollapsingHeader("Appearance", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const std::uint64_t appearCd = cooldownRemaining(p, 0 /* APPEARANCE */);
        const bool ready             = appearCd == 0;
        ImGui::Text(
            "Next change costs %llu XP (paid from available XP)", static_cast<unsigned long long>(p.appearanceCostNext)
        );
        if (!ready)
        {
            ImGui::BeginDisabled(); // the cooldown gate (the sim re-checks authoritatively)
        }
        if (ImGui::Button("Apply preset look: wanderer"))
        {
            const std::uint64_t wanderer[3] = {1, 2, 3}; // the default component set (2.0's creation default)
            submitChangeAppearance(ctx, wanderer, 3);
        }
        ImGui::SameLine();
        if (ImGui::Button("Apply preset look: hooded"))
        {
            const std::uint64_t hooded[3] = {1, 4, 5}; // the alternate preset
            submitChangeAppearance(ctx, hooded, 3);
        }
        if (!ready)
        {
            ImGui::EndDisabled();
        }
        if (!ready)
        {
            ImGui::TextDisabled("cooldown: %llu game-s left", static_cast<unsigned long long>(appearCd));
        }
    }

    ImGui::End();
}

} // namespace harrowmere::ui
