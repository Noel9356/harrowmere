// time_controls.cpp - the Time Controls panel (card 7.3, panel 8 of 8).
//
// Reads: the capability's TimeControls pacing state + PlayerView.worldTime.
// Writes: setTimeControls (the presentation pacing surface - NOT a
// SimCommand; see harrowmere_capability.hpp's TimeControls decision note)
// plus ServeSentence (D-JAIL-1) - the card's "serve-sentence here" row.
//
// pause / 1x / 8x / 60x - "scales accumulator only - worldTime mapping
// unchanged": the fixed-update handler consumes ticksPerDispatch; the
// per-tick worldTime math never moves.

#include "panels/panels.hpp"

#include <cstdio>

namespace harrowmere::ui
{

void drawTimeControlsPanel(
    PanelContext &ctx, bool *pOpen
)
{
    if (ctx.player == nullptr)
        return;

    ImGui::SetNextWindowSize(ImVec2(360.0f, 190.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(kPanelBindings[static_cast<std::size_t>(PanelId::TimeControls)].title, pOpen))
    {
        ImGui::End();
        return;
    }

    char clockBuf[48];
    formatGameClock(ctx.player->worldTime, clockBuf, sizeof(clockBuf));
    ImGui::Text("World time: %s", clockBuf);

    const harrowmere::views::TimeControls &t = ctx.timeControls;
    ImGui::Text("Pacing: %s", t.paused ? "PAUSED" : "running");

    // The pinned stops. Active button carries the "pressed" look.
    if (ImGui::Button(t.paused ? "Resume" : "Pause"))
    {
        applyTimeControls(ctx, !t.paused, t.ticksPerDispatch == 0 ? 1u : t.ticksPerDispatch);
    }
    ImGui::SameLine();
    for (const std::uint32_t speed : {1u, 8u, 60u})
    {
        char label[16];
        std::snprintf(label, sizeof(label), "%ux", speed);
        if (t.ticksPerDispatch == speed && !t.paused)
        {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(label))
        {
            applyTimeControls(ctx, /*paused=*/false, speed);
        }
        if (t.ticksPerDispatch == speed && !t.paused)
        {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::TextDisabled("Scales the fixed-update accumulator only; the per-tick worldTime mapping never moves.");

    // Serve-sentence (D-JAIL-1): the same fast-forward the Justice panel
    // offers, here because time is this panel's business.
    if (ImGui::Button("Serve sentence (fast-forward)"))
    {
        submitServeSentence(ctx);
    }

    ImGui::End();
}

} // namespace harrowmere::ui
