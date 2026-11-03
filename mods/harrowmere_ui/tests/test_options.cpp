#include "input_mapper.hpp"
#include "panels/options.hpp"

#include "hot_reload_capability.hpp"
#include "settings.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace
{
std::filesystem::path testSettingsPath()
{
    return std::filesystem::temp_directory_path() / "harrowmere_options_persistence_test.toml";
}

void writeSettings(
    const std::filesystem::path &path, const std::string &text
)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output << text;
    REQUIRE(output.good());
}
} // namespace

TEST_CASE(
    "TestOptions_PersistAcrossRestart"
)
{
    const std::filesystem::path path = testSettingsPath();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    engine::SettingsModule first;
    first.setNumber("audio.master_volume", 0.42);
    first.setString("graphics.quality", "Medium");
    first.setNumber("ui.scale", 1.25);
    first.setBoolean("ui.verbose", true);
    first.setString("input.actions.walk_forward", "Up");
    std::string error;
    REQUIRE(first.save(path.string(), error));

    engine::SettingsModule restarted;
    REQUIRE(restarted.load(path.string(), error));
    CHECK(restarted.numberValue("audio.master_volume", 1.0) == doctest::Approx(0.42));
    CHECK(restarted.stringValue("graphics.quality", "") == "Medium");
    CHECK(restarted.numberValue("ui.scale", 1.0) == doctest::Approx(1.25));
    CHECK(restarted.booleanValue("ui.verbose", false));
    CHECK(restarted.stringValue("input.actions.walk_forward", "") == "Up");
    CHECK(harrowmere::ui::loadInputBindings(restarted).walk_forward == engine::KeyCode::Up);
    std::filesystem::remove(path, ignored);
}

TEST_CASE(
    "TestOptions_KeybindRemapReachesMapper"
)
{
    engine::SettingsModule settings;
    settings.setString("input.actions.walk_forward", "Up");
    settings.setString("input.actions.interact", "F6");
    const harrowmere::ui::InputBindings bindings = harrowmere::ui::loadInputBindings(settings);
    CHECK(bindings.walk_forward == engine::KeyCode::Up);
    CHECK(bindings.interact == engine::KeyCode::F6);
    CHECK(bindings.walk_back == engine::KeyCode::S);
}

TEST_CASE(
    "TestOptions_HandEditTriggersWatch"
)
{
    const std::filesystem::path path = testSettingsPath();
    writeSettings(path, "[audio]\nmaster_volume = 0.25\n[graphics]\nquality = \"Low\"\n");

    engine::SettingsModule settings;
    std::string error;
    engine::HotReloadEvent event{
        .kind = engine::hot_reload_kind::config, .relativePath = path.lexically_normal().generic_string(), .modId = ""
    };
    CHECK(
        harrowmere::ui::reloadSettingsFromFileEvent(
            settings, event, path.lexically_normal().generic_string(), path.string(), error
        )
    );
    CHECK(settings.numberValue("audio.master_volume", 1.0) == doctest::Approx(0.25));
    CHECK(settings.stringValue("graphics.quality", "") == "Low");

    engine::HotReloadEvent unrelated = event;
    unrelated.relativePath           = (path.parent_path() / "unrelated.toml").generic_string();
    CHECK_FALSE(
        harrowmere::ui::reloadSettingsFromFileEvent(
            settings, unrelated, path.lexically_normal().generic_string(), path.string(), error
        )
    );
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}
