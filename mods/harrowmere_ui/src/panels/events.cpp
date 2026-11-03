// events.cpp - the Events panel (card 7.3, panel 5 of 8).
//
// Reads: EventsView (active events with my contribution + estimated share;
// read-only for running events). Submits: JoinEvent / LeaveEvent for the
// rows the core marked joinable / participated. Contribution math is the
// core's (5.3's normalized, capped, weighted amounts; the share is the
// getRewardShare estimate) - the panel displays, never re-derives.

#include "panels/panels.hpp"

#include <cstdio>

namespace harrowmere::ui
{

void drawEventsPanel(
    PanelContext &ctx, bool *pOpen
)
{
    if (ctx.events == nullptr)
        return;
    const harrowmere::views::EventsView &e = *ctx.events;

    ImGui::SetNextWindowSize(ImVec2(520.0f, 360.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(kPanelBindings[static_cast<std::size_t>(PanelId::Events)].title, pOpen))
    {
        ImGui::End();
        return;
    }

    if (e.active.empty())
    {
        ImGui::TextDisabled("(no active events - the world is quiet)");
        ImGui::End();
        return;
    }

    constexpr ImGuiTableFlags flags
        = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("events", 5, flags))
    {
        ImGui::TableSetupColumn("event");
        ImGui::TableSetupColumn("state");
        ImGui::TableSetupColumn("my contribution");
        ImGui::TableSetupColumn("my share");
        ImGui::TableSetupColumn("action");
        ImGui::TableHeadersRow();
        for (const auto &ev : e.active)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(ev.templateKey.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("#%llu", static_cast<unsigned long long>(ev.stateId));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(ev.myContribution));
            ImGui::TableNextColumn();
            ImGui::Text(
                "%u.%u%%",
                static_cast<unsigned>(ev.mySharePermille / 10),
                static_cast<unsigned>(ev.mySharePermille % 10)
            );
            ImGui::TableNextColumn();
            if (ev.amParticipant)
            {
                if (ImGui::SmallButton(("leave##" + std::to_string(ev.eventId)).c_str()))
                {
                    submitLeaveEvent(ctx, ev.eventId);
                }
            } else if (ev.joinable)
            {
                if (ImGui::SmallButton(("join##" + std::to_string(ev.eventId)).c_str()))
                {
                    submitJoinEvent(ctx, ev.eventId);
                }
            } else
            {
                ImGui::TextDisabled("-");
            }
        }
        ImGui::EndTable();
    }

    ImGui::TextDisabled(
        "Difficulty %u is display-only; contributions cap and weight per template (5.3).",
        e.active.empty() ? 0u : static_cast<unsigned>(e.active.front().difficulty)
    );

    ImGui::End();
}

} // namespace harrowmere::ui
