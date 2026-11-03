#ifndef HARROWMERE_UI_SRC_PANELS_OPTIONS_HPP
#define HARROWMERE_UI_SRC_PANELS_OPTIONS_HPP

#include <string>

namespace engine
{
class SettingsCapability;
class AudioCapability;
struct HotReloadEvent;
} // namespace engine

namespace harrowmere::ui
{

void drawOptionsPanel(engine::SettingsCapability &settings, engine::AudioCapability *audio, bool *pOpen);
bool reloadSettingsFromFileEvent(
    engine::SettingsCapability &settings,
    const engine::HotReloadEvent &event,
    const std::string &expectedAbsolutePath,
    const std::string &loadPath,
    std::string &error
);

} // namespace harrowmere::ui

#endif // HARROWMERE_UI_SRC_PANELS_OPTIONS_HPP
