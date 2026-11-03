// world_effects.cpp — task 5.5's implementation (see world_effects.hpp for
// the D-FLAGS-1 split). The transient/persistent split is enforced where
// each half lives: the despawn-at-end rides the engine's finishEvent (the
// lifecycle tail), the flag persistence rides the engine's SetRegionFlag
// branch (a write to Region.flags with no endEvent counterpart), and this
// file carries the step-2 weather-bias slice — the one world effect that
// needs a per-tick consumer to hold the window.

#include "events/world_effects.hpp"

#include <cstring>

#include "core/world_context.hpp"
#include "world/region.hpp"
#include "world/seasons.hpp" // weatherKey / the closed Weather list
#include "world/world.hpp"

namespace harrowmere::events
{

namespace
{

// The bias key ("clear"/"rain"/"storm"/"fog"/"snow") resolves against the
// closed Weather list — an unknown key holds nothing (deterministic no-op,
// the same rule the SetRegionFlag branch applies to undeclared flags).
world::Weather weatherForKey(
    const std::string &key
) noexcept
{
    for (std::uint32_t w = 0; w < static_cast<std::uint32_t>(world::Weather::COUNT); ++w)
    {
        if (key == world::weatherKey(world::weatherFromKind(w)))
        {
            return world::weatherFromKind(w);
        }
    }
    return world::Weather::COUNT; // out-of-range sentinel: never a real kind
}

} // namespace

void WorldEffectsTickSystem::run(
    WorldContext &ctx
)
{
    if (engine == nullptr || ctx.worldStore == nullptr)
    {
        return;
    }
    // The active biases, region-ascending (the engine's map, R7). The pin
    // is a kind-write only: sinceWorldTime stamps when the kind actually
    // changes (the machine's own rule), so a held bias churns nothing.
    const std::vector<regionId> biased = engine->activeWeatherBiasRegions(ctx.now);
    for (const regionId region : biased)
    {
        const std::string *key = engine->weatherBiasFor(region, ctx.now);
        if (key == nullptr)
        {
            continue; // expired between the listing and the read (same tick — defensive)
        }
        const world::Weather pinned = weatherForKey(*key);
        if (static_cast<std::uint32_t>(pinned) >= static_cast<std::uint32_t>(world::Weather::COUNT))
        {
            continue; // an unknown key holds nothing (deterministic)
        }
        world::Region *regionRow = ctx.worldStore->region(region);
        if (regionRow == nullptr)
        {
            continue;
        }
        if (regionRow->weather.kind != static_cast<std::uint32_t>(pinned))
        {
            regionRow->weather.kind           = static_cast<std::uint32_t>(pinned);
            regionRow->weather.sinceWorldTime = ctx.now; // "when this weather began"
        }
    }
}

} // namespace harrowmere::events
