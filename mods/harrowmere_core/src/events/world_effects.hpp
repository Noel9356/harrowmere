#ifndef HARROWMERE_CORE_EVENTS_WORLD_EFFECTS_HPP
#define HARROWMERE_CORE_EVENTS_WORLD_EFFECTS_HPP

// world_effects.hpp — task 5.5: the action interpreter's consumer wiring and
// the transient-vs-persistent split the docs draw (D-FLAGS-1).
//
// THE SPLIT (the card's pinned reading — the "flags may remain" nuance is
// the doc's least obvious rule, so it lives here AND in the module README):
//
//   TRANSIENT (state-scoped — all *return to default* when the event ends;
//   the doc: "Events eventually return the world to its default state"):
//     * spawned targets — tracked per event (the engine's spawnedTargets_
//       book) and DESPAWNED AT ENDEVENT (5.5's finishEvent wiring) — the
//       template's explicit DespawnTargets action (B.6's raid) stays
//       available and idempotent. Their deaths generated evidence/hpLost
//       while alive; removal at endEvent does not retroactively strip
//       contributions (the amounts were already validated) — only the
//       ACTORS despawn.
//     * price biases (PriceBias) — timed records; they expire by
//       themselves (the until-timestamp) and priceBiasFor answers SCALE
//       again.
//     * weather biases (WeatherBias) — timed; the step-2 slice below pins
//       the region's weather kind for the window and hands back to the
//       markov chain at expiry.
//     * evacuations (EvacuateTerritory) — the fleeing intent rides an
//       untilWorldTime (the manhunt's duration); 4.6's mechanic owns the
//       expiry.
//     * MANHUNT enforcement — the event's exits end the hunt (5.4).
//
//   PERSISTENT (world-scoped): SetRegionFlag writes Region.flags and is
//   NEVER reverted by endEvent (the doc: "while flags may remain changed")
//   — only later events, war outcomes (6.2), or explicit template actions
//   change the bits. Region flags drive environment flavor (3.8), NPC
//   behavior masks (3.4's intents), event branching (5.1's FlagCondition)
//   and settlement/war outcomes (6.2).
//
//   GarrisonTerritory (the D-ACTION-1++ extension, DEVIATIONS.md row) is
//   the reconquest exit: the event's NPC participants take the data-named
//   guard role (3.6's assignRole) in the event's territory and the
//   territory's lawless flag clears — the doc's NPC-reconquest path out of
//   lawlessness. The interpreter branch lives in the engine's runAction
//   (the interpreter is 5.1's); the shipped GARRISON_RETURN template is
//   pure data.
//
// Nested event spawning walks depth + 1 <= MAX_EVENT_DEPTH (5.1's rule)
// and inherits the parent's regionId unless overridden (the engine's
// SpawnNestedEvent); GrantRewardTier is a no-op bookkeeping hook for 5.4's
// table (the payout reads the granted tiers at endEvent).

#include "core/scheduler.hpp" // IStepSystem (the step-2 slice)
#include "events/engine.hpp"  // EventEngine (the bias records)

namespace harrowmere
{
struct WorldContext; // core/world_context.hpp — reference only here
}

namespace harrowmere::events
{

// WorldEffectsTickSystem — step 2's weather-bias slice (registered AFTER
// the weather machine): every active WeatherBias pins its region's weather
// kind for the bias window (a re-pin only stamps when the kind actually
// changes — the same stamping rule the machine itself follows); at expiry
// the markov chain's roll stands again. Deterministic: biases iterate in
// region order (R7), the kind resolution is the closed Weather list.
class WorldEffectsTickSystem final : public IStepSystem
{
public:
    EventEngine *engine = nullptr;

    void run(WorldContext &ctx) override;
};

} // namespace harrowmere::events

#endif // HARROWMERE_CORE_EVENTS_WORLD_EFFECTS_HPP
