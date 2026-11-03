#include "panels/options.hpp"

#include <array>

#include "audio_capability.hpp"
#include "input_capability.hpp"
#include "hot_reload_capability.hpp"
#include "settings_capability.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <filesystem>
#include <string>

namespace harrowmere::ui
{
namespace
{
struct KeyOption
{
    const char *label;
    const char *setting;
    const char *fallback;
};

constexpr std::array<KeyOption, 14> kKeyOptions{{
    {"Walk forward", "input.actions.walk_forward", "W"},
    {"Walk backward", "input.actions.walk_back", "S"},
    {"Strafe left", "input.actions.walk_left", "A"},
    {"Strafe right", "input.actions.walk_right", "D"},
    {"Sprint", "input.actions.sprint", "LeftShift"},
    {"Interact", "input.actions.interact", "E"},
    {"Crouch", "input.actions.crouch", "Z"},
    {"Light torch", "input.actions.torch", "T"},
    {"Toggle mask", "input.actions.mask", "Y"},
    {"Report crime", "input.actions.report_crime", "G"},
    {"Bribe", "input.actions.bribe", "B"},
    {"Surrender", "input.actions.surrender", "X"},
    {"Jump", "input.actions.jump", "Space"},
    {"Toggle overlay", "ui.actions.toggle_overlay", "F1"},
}};

std::map<std::string, std::array<char, 32>> keyBuffers;
std::string saveStatus;

void save(
    engine::SettingsCapability &settings
)
{
    std::string error;
    if (settings.save("", error))
        saveStatus = "Saved";
    else
        saveStatus = "Could not save settings: " + error;
}

void ensureBuffer(
    engine::SettingsCapability &settings, const KeyOption &option
)
{
    const auto [it, inserted] = keyBuffers.try_emplace(option.setting);
    if (!inserted)
        return;
    const std::string value = settings.stringValue(option.setting, option.fallback);
    std::snprintf(it->second.data(), it->second.size(), "%s", value.c_str());
}
} // namespace

bool reloadSettingsFromFileEvent(
    engine::SettingsCapability &settings,
    const engine::HotReloadEvent &event,
    const std::string &expectedAbsolutePath,
    const std::string &loadPath,
    std::string &error
)
{
    error.clear();
    if (event.kind != engine::hot_reload_kind::config)
        return false;
    const std::filesystem::path changed  = std::filesystem::path(event.relativePath).lexically_normal();
    const std::filesystem::path expected = std::filesystem::path(expectedAbsolutePath).lexically_normal();
    if (changed != expected)
        return false;
    return settings.load(loadPath, error);
}

void drawOptionsPanel(
    engine::SettingsCapability &settings, engine::AudioCapability *audio, bool *pOpen
)
{
    if (!ImGui::Begin("Options###harrowmere_options", pOpen))
    {
        ImGui::End();
        return;
    }

    float volume = static_cast<float>(std::clamp(settings.numberValue("audio.master_volume", 1.0), 0.0, 1.0));
    if (ImGui::SliderFloat("Master volume", &volume, 0.0f, 1.0f, "%.2f"))
    {
        settings.setNumber("audio.master_volume", static_cast<double>(volume));
        if (audio != nullptr)
            audio->setMasterVolume(volume);
        save(settings);
    }
    if (audio == nullptr)
        ImGui::TextDisabled("Audio device capability unavailable; volume will apply when audio is available.");

    static constexpr const char *qualityNames[] = {"Low", "Medium", "High"};
    std::string quality                         = settings.stringValue("graphics.quality", "High");
    int qualityIndex                            = quality == "Low" ? 0 : quality == "Medium" ? 1 : 2;
    if (ImGui::Combo("Graphics quality preset", &qualityIndex, qualityNames, 3))
    {
        settings.setString("graphics.quality", qualityNames[qualityIndex]);
        save(settings);
    }
    ImGui::TextDisabled(
        "The current renderer exposes no live quality setter; this preset is persisted for renderer configuration."
    );

    float uiScale = static_cast<float>(std::clamp(settings.numberValue("ui.scale", 1.0), 0.75, 1.5));
    if (ImGui::SliderFloat("UI scale", &uiScale, 0.75f, 1.5f, "%.2fx"))
    {
        settings.setNumber("ui.scale", static_cast<double>(uiScale));
        save(settings);
    }

    bool verbose = settings.booleanValue("ui.verbose", false);
    if (ImGui::Checkbox("Verbose UI details", &verbose))
    {
        settings.setBoolean("ui.verbose", verbose);
        save(settings);
    }
    if (verbose)
        ImGui::TextWrapped(
            "Verbose mode enables additional guidance in this panel. Simulation rules and campaign state are unchanged."
        );

    if (ImGui::CollapsingHeader("Key bindings", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled(
            "Enter a key name understood by the engine input mapper (for example W, F1, LeftShift, or Space), then "
            "press Enter."
        );
        for (const KeyOption &option : kKeyOptions)
        {
            ensureBuffer(settings, option);
            auto &buffer = keyBuffers[option.setting];
            ImGui::SetNextItemWidth(150.0f);
            const bool committed
                = ImGui::InputText(option.label, buffer.data(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
            if (committed)
            {
                const engine::KeyCode key = engine::keyCodeFromName(buffer.data());
                if (key == engine::KeyCode::Unknown)
                {
                    saveStatus = std::string("Unknown key name: ") + buffer.data();
                } else
                {
                    settings.setString(option.setting, buffer.data());
                    save(settings);
                }
            }
            // A hand-edited settings.toml is authoritative after the watcher
            // reloads it. Refresh inactive editors so the panel never shows
            // a key name older than the live settings store.
            if (!ImGui::IsItemActive())
            {
                const std::string current = settings.stringValue(option.setting, option.fallback);
                std::snprintf(buffer.data(), buffer.size(), "%s", current.c_str());
            }
        }
        ImGui::TextDisabled(
            "Gameplay remaps and the overlay toggle apply when the settings file watcher observes the saved file."
        );
    }

    if (!saveStatus.empty())
        ImGui::TextWrapped("%s", saveStatus.c_str());
    ImGui::End();
}

} // namespace harrowmere::ui
