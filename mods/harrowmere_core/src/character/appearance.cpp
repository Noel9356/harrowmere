// appearance.cpp — the appearance change flow (task 2.6). See appearance.hpp
// for the pinned semantics; this file is the gate → pay → apply → arm →
// record → publish sequence, nothing more.

#include "character/appearance.hpp"

#include <algorithm>

#include "character/status.hpp" // cooldownActive / setCooldown (task 2.1)
#include "core/math.hpp"        // applyXpDelta (R4's one clamp)

namespace harrowmere
{

// ---- the history ----------------------------------------------------------------

void character::AppearanceHistory::record(
    characterId who, std::uint64_t worldTime
)
{
    changes_[who].push_back(worldTime); // append order IS time order (worldTime monotonic, R8)
}

std::size_t character::AppearanceHistory::changesWithin(
    characterId who, std::uint64_t now, std::uint64_t windowGameS
) const
{
    const auto it = changes_.find(who);
    if (it == changes_.end())
        return 0;

    const std::vector<std::uint64_t> &stamps = it->second;
    // The window (now - windowGameS, now]: stamps are ascending, so the
    // count is the suffix above the lower bound. The subtraction is safe:
    // any stamp <= now - windowGameS falls out, and stamps never exceed the
    // `now` they were recorded with (monotonic clock).
    const std::uint64_t lower = now - std::min<std::uint64_t>(windowGameS, now);
    std::size_t count         = 0;
    for (auto stamp = stamps.rbegin(); stamp != stamps.rend() && *stamp > lower; ++stamp)
        ++count;
    return count;
}

std::uint64_t character::AppearanceHistory::lastChangeTime(
    characterId who
) const
{
    const auto it = changes_.find(who);
    if (it == changes_.end() || it->second.empty())
        return 0;
    return it->second.back();
}

std::size_t character::AppearanceHistory::changeCount(
    characterId who
) const
{
    // The version counter (task 3.9): one applied change = one version bump.
    const auto it = changes_.find(who);
    return it == changes_.end() ? 0 : it->second.size();
}

std::size_t character::AppearanceHistory::changeCountUpTo(
    characterId who, std::uint64_t worldTime
) const
{
    // The version a sighting at `worldTime` saw: the change stamps are
    // ascending (append order IS time order, R8), so the count is the prefix
    // at or below the stamp — std::upper_bound, no linear scan.
    const auto it = changes_.find(who);
    if (it == changes_.end())
        return 0;
    const std::vector<std::uint64_t> &stamps = it->second;
    return static_cast<std::size_t>(std::upper_bound(stamps.begin(), stamps.end(), worldTime) - stamps.begin());
}

// ---- the change flow --------------------------------------------------------------

bool canChangeAppearance(
    const Character &c, std::uint64_t now
)
{
    // design reference verbatim, gate order as written: the cooldown slot
    // first, then the XP comparison — against availableXp (i64 widened from
    // the u64 cost; R1: never the reverse).
    if (cooldownActive(c, CooldownType::APPEARANCE, now))
        return false;
    const std::uint64_t cost = appearanceChangeCost(c);
    if (c.progression.availableXp < static_cast<std::int64_t>(cost))
        return false;
    return true;
}

AppearanceResult changeAppearance(
    Character &c, std::span<const std::uint64_t> newComponents, WorldContext &ctx
)
{
    // 1. The gate (the doc's canChangeAppearance; the refusal reasons map
    // onto the two rule errors — §3.4: return values, never exceptions).
    if (cooldownActive(c, CooldownType::APPEARANCE, ctx.now))
        return AppearanceResult::err(AppearanceError::CooldownActive);
    const std::uint64_t cost = appearanceChangeCost(c);
    if (c.progression.availableXp < static_cast<std::int64_t>(cost))
        return AppearanceResult::err(AppearanceError::InsufficientXp);

    // 2. Pay from availableXp (the doc's own comparison; applyXpDelta owns
    // the R4 floor — a player deep in XP debt simply cannot pay).
    c.progression.availableXp = applyXpDelta(c.progression.availableXp, -static_cast<std::int64_t>(cost));

    // 3. Replace the look. Appearance NEVER touches stats, skills, or
    // equipment (the doc's hard rule — no markPowerDirty here either: the
    // power formula reads none of this, and a change must not disturb the
    // cache; TestAppearance_PowerUnchangedAfterChange pins both sides).
    c.appearanceComponents.assign(newComponents.begin(), newComponents.end());

    // 4. Arm the cooldown (A-24: 1 game-day — the slot holds the expiry).
    setCooldown(c, CooldownType::APPEARANCE, ctx.now + APPEARANCE_COOLDOWN_GAME_S);

    // 5. The A-25 trail: record the stamp, then fire the suspicion seam when
    // the window's count reaches APPEARANCE_FREQUENT_COUNT (3 changes in 10
    // game-days). The trigger fires on the CHANGE THAT CROSSED the line and
    // again on every later change while the window stays full — 4.5 weighs
    // each signal (A-94); a once-only latch would be a behavior the plan
    // never pinned.
    if (ctx.appearanceHistory != nullptr)
    {
        ctx.appearanceHistory->record(c.id, ctx.now);
        if (ctx.suspicion != nullptr
            && ctx.appearanceHistory->changesWithin(c.id, ctx.now, APPEARANCE_FREQUENT_WINDOW_GAME_S)
                   >= APPEARANCE_FREQUENT_COUNT)
        {
            ctx.suspicion->onAppearanceFrequent(c.id, ctx.now);
        }
    }

    // 6. Publish the fact (post-mutation — the components are replaced, the
    // cost paid, the cooldown armed; 0.7's rule). Null sink = dropped.
    if (ctx.appearance != nullptr)
        ctx.appearance->onAppearanceChanged(c.id);

    return AppearanceResult::ok();
}

} // namespace harrowmere
