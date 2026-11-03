#include <doctest.h>

#include "fixtures/native_consumer.hpp"

namespace
{

class FixtureCore final : public harrowmere::SimCapability
{
public:
    const harrowmere::views::WorldView &world() const override
    {
        return worldView;
    }
    const harrowmere::views::PlayerView &player() const override
    {
        return playerView;
    }

    harrowmere::views::WorldView worldView{};
    harrowmere::views::PlayerView playerView{};
};

} // namespace

TEST_CASE(
    "TestMod_NativeConsumerCanReadViews"
)
{
    FixtureCore core;
    core.worldView.tickNumber          = 123;
    core.worldView.currentTerritory.id = 456;
    core.playerView.level              = 17;

    const auto read = harrowmere_fixture_mod::readCoreViews(core);
    CHECK(read.tick == 123);
    CHECK(read.territory == 456);
    CHECK(read.level == 17);
}
