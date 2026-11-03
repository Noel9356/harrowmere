// justice.cpp - the Justice panel (card 7.3, panel 4 of 8).
//
// THE fog-of-war panel. Reads: JusticeView - whose member set IS the
// whitelist (reputation per territory, notoriety, the live warrants against
// the player with their accused-side facts, jail status) + UiSignals. It
// NEVER shows a case score, an evidence list or a report weight: "Nothing is
// visible at the act... give observable signals" (design reference) - what a
// wanted player can see is exactly what 4.6's signals + the poster text say.
// TestUi_NoHiddenEvidenceExposed pins the whitelist shape; this panel is the
// consumer that would rat out a violation.
//
// Submits: ServeSentence (D-JAIL-1's fast-forward), AttemptEscape (4.3's
// skill check), Surrender (4.6 - ends a manhunt). All through the shared
// helpers; every one is a logged rejection until Phase 3 lands its owner.

#include "panels/panels.hpp"

#include <cstdio>

namespace harrowmere::ui
{

void drawJusticePanel(
    PanelContext &ctx, bool *pOpen
)
{
    if (ctx.justice == nullptr)
        return;
    const harrowmere::views::JusticeView &j = *ctx.justice;

    ImGui::SetNextWindowSize(ImVec2(460.0f, 380.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(kPanelBindings[static_cast<std::size_t>(PanelId::Justice)].title, pOpen))
    {
        ImGui::End();
        return;
    }

    // --- the record (what anyone could know) ----------------------------------
    ImGui::Text("Notoriety: %llu", static_cast<unsigned long long>(j.notoriety));

    if (ImGui::CollapsingHeader("Reputation by territory", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (j.perTerritory.empty())
        {
            ImGui::TextDisabled("(no reputation recorded yet)");
        } else
        {
            constexpr ImGuiTableFlags flags
                = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("reputation", 2, flags))
            {
                ImGui::TableSetupColumn("territory");
                ImGui::TableSetupColumn("standing");
                ImGui::TableHeadersRow();
                for (const auto &row : j.perTerritory)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("territory #%llu", static_cast<unsigned long long>(row.territoryId));
                    ImGui::TableNextColumn();
                    if (row.reputation < 0)
                    {
                        ImGui::TextColored(
                            ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "%lld", static_cast<long long>(row.reputation)
                        );
                    } else
                    {
                        ImGui::Text("%lld", static_cast<long long>(row.reputation));
                    }
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled("The walk is flattened: origin first, root last (4.4's propagation order).");
        }
    }

    // --- warrants against me (the accused-side view) ---------------------------
    if (ImGui::CollapsingHeader("Warrants against you", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("%u live warrant(s)", static_cast<unsigned>(j.activeWarrantsAgainstYou));
        for (const auto &w : j.warrants)
        {
            ImGui::Bullet();
            ImGui::Text(
                "issued in territory #%llu - bounty %llu - %s",
                static_cast<unsigned long long>(w.territoryId),
                static_cast<unsigned long long>(w.bounty),
                w.lethal ? "LETHAL" : "arrest"
            );
            char expiry[48];
            formatGameClock(w.expiresWorldTime, expiry, sizeof(expiry));
            ImGui::SameLine();
            ImGui::TextDisabled("expires: %s", expiry);
        }
    }

    // --- jail (4.3) -------------------------------------------------------------
    if (ImGui::CollapsingHeader("Jail", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (!j.isJailed)
        {
            ImGui::TextUnformatted("Free.");
        } else
        {
            char end[48];
            formatGameClock(j.jailUntil, end, sizeof(end));
            ImGui::Text("Serving a sentence until %s.", end);
            // D-JAIL-1: the fast-forward runs the pinned pipeline at max
            // speed until jailUntil (the sim advances worldTime in a
            // catch-up loop; the panel only submits the request).
            if (ImGui::Button("Serve sentence (fast-forward)"))
            {
                submitServeSentence(ctx);
            }
            ImGui::SameLine();
            if (ImGui::Button("Attempt escape"))
            {
                submitAttemptEscape(ctx);
            }
            ImGui::TextDisabled("Escape is a skill check; failure extends the sentence and records a fresh crime.");
        }
        // Surrender: to any guard of the issuing territory (4.6) - ends any
        // manhunt. Available while wanted even outside jail.
        if (j.activeWarrantsAgainstYou > 0)
        {
            if (ImGui::Button("Surrender to the next guard"))
            {
                submitSurrender(ctx);
            }
        }
    }

    // --- the signals mirror (the observable surface) ------------------------------
    if (ctx.signals != nullptr && !ctx.signals->entries.empty())
    {
        if (ImGui::CollapsingHeader("Recent signals"))
        {
            for (const auto &s : ctx.signals->entries)
            {
                ImGui::BulletText("%s", s.text.c_str());
            }
        }
    }

    ImGui::End();
}

} // namespace harrowmere::ui
