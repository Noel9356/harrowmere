#include "native_consumer.hpp"

namespace harrowmere_fixture_mod
{

Readback readCoreViews(
    const harrowmere::SimCapability &core
)
{
    const harrowmere::views::WorldView &world   = core.world();
    const harrowmere::views::PlayerView &player = core.player();
    return {world.tickNumber, world.currentTerritory.id, player.level};
}

} // namespace harrowmere_fixture_mod
