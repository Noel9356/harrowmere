// suspicion.cpp — task 4.5's implementation (see suspicion.hpp for the
// pinned reading). Everything here is a READ over the world and a WRITE to
// the observer's own TrustMemory — nothing in this file may create evidence,
// authorize violence, or damage anyone (the doc's hard rule; the tests pin
// it from the outside).

#include "justice/suspicion.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "character/character.hpp"
#include "core/constants_phase2.hpp" // WITNESS_RADIUS_M, MID_DISTANCE_M (A-60/A-46)
#include "core/world_context.hpp"
#include "npc/lod.hpp"           // LodActiveSet (the near-territory budget)
#include "data/laws.hpp"         // SuspicionTrigger + the mask bits
#include "player/controller.hpp" // PlayerActions (the act surface)
#include "world/region.hpp"      // D-WORLD-1 distance helpers + regionCenter
#include "world/world.hpp"       // WorldStore (the territory region anchor)

namespace harrowmere::justice
{

namespace
{

// The night band, integer game-second math on clock.hpp's documented anchors
// (the scorePhysical night marker's same rule): night is [18:00, 06:00).
bool isNight(
    std::uint64_t worldTime
)
{
    const std::uint64_t dayTime = worldTime % core::DAY_LENGTH_SECONDS;
    return dayTime >= NIGHT_START_GAME_S || dayTime < NIGHT_END_GAME_S;
}

double distanceM(
    const Position &a, const Position &b
)
{
    const double dx = a.x - b.x;
    const double dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

// The trigger rows in force at the observed character's territory: the law
// set's own rows REPLACE the global table when non-empty (laws.hpp's pinned
// semantics); the DEFAULT set's empty rows inherit suspicion.json.
const std::vector<data::SuspicionTrigger> &triggersFor(
    const WorldContext &ctx, territoryId territory
)
{
    assert(ctx.worldStore != nullptr && ctx.data != nullptr && "suspicion: unbound context");
    static const std::vector<data::SuspicionTrigger> kEmpty;
    if (ctx.worldStore == nullptr || ctx.data == nullptr)
    {
        return kEmpty;
    }
    const data::LawSet &laws = ctx.worldStore->lawsAt(territory);
    return laws.suspicion.empty() ? ctx.data->suspicionTriggers() : laws.suspicion;
}

// The observed character's species row (darkvision gate; unknown id = no row
// — UNLIT then applies, the deterministic recovery).
bool speciesHasDarkvision(
    const WorldContext &ctx, speciesId species
)
{
    if (ctx.data == nullptr || ctx.data->speciesCount() <= species)
    {
        return false;
    }
    return ctx.data->species(species).darkvision;
}

} // namespace

// ---- the ladder + the memory API -----------------------------------------------

TrustTier tierFor(
    std::uint32_t score
)
{
    return score >= TRUST_TIER_SHUNNED    ? TrustTier::SHUNNED
         : score >= TRUST_TIER_DISTRUSTED ? TrustTier::DISTRUSTED
         : score >= TRUST_TIER_WATCHED    ? TrustTier::WATCHED
                                          : TrustTier::STRANGER;
}

bool isAcquaintance(
    const NpcState &observer, characterId observed
)
{
    const auto it = observer.affinity.find(observed);
    return it != observer.affinity.end() && it->second >= ACQUAINTANCE_AFFINITY;
}

SuspicionMemoryEntry memoryEntry(
    const NpcState &observer, characterId observed
)
{
    SuspicionMemoryEntry entry;
    entry.who        = observed;
    const auto score = observer.trust.perCharacterScore.find(observed);
    entry.score      = score == observer.trust.perCharacterScore.end() ? 0u : static_cast<std::uint32_t>(score->second);
    const auto seen  = observer.trust.lastSeen.find(observed);
    entry.lastSeen   = seen == observer.trust.lastSeen.end() ? 0ull : seen->second;
    entry.acquaintance = isAcquaintance(observer, observed);
    return entry;
}

TrustTier tierOf(
    const NpcState &observer, characterId observed
)
{
    const auto score = observer.trust.perCharacterScore.find(observed);
    return tierFor(score == observer.trust.perCharacterScore.end() ? 0u : static_cast<std::uint32_t>(score->second));
}

namespace
{

// The tier rank eviction sorts by: strangers evict before acquaintances, then
// the lowest score, then the oldest lastSeen, then the lower id (R7).
struct EvictionKey
{
    bool operator()(
        const std::pair<characterId, SuspicionMemoryEntry> &a, const std::pair<characterId, SuspicionMemoryEntry> &b
    ) const
    {
        // false < true: an acquaintance (true) ranks AFTER a stranger (false)
        // — a stranger is the eviction candidate while any exists.
        if (a.second.acquaintance != b.second.acquaintance)
            return a.second.acquaintance < b.second.acquaintance;
        const int tierA = static_cast<int>(tierFor(a.second.score));
        const int tierB = static_cast<int>(tierFor(b.second.score));
        if (tierA != tierB)
            return tierA < tierB;
        if (a.second.score != b.second.score)
            return a.second.score < b.second.score;
        if (a.second.lastSeen != b.second.lastSeen)
            return a.second.lastSeen < b.second.lastSeen;
        return a.first < b.first;
    }
};

// Enforce MEMORY_CAP: evict the weakest PRIOR entries until the map fits —
// the just-updated `protectedId` keeps its slot (the doc's "lowest tier
// first" never forgets the memory it just made; TestSuspicion_
// MemoryCapEvictsLowestTierStrangerFirst pins it). Acquaintance rows only go
// when the whole memory is acquainted (the doc rule — acquaintances
// outrank strangers).
void enforceMemoryCap(
    NpcState &observer, characterId protectedId
)
{
    while (observer.trust.perCharacterScore.size() > MEMORY_CAP)
    {
        std::vector<std::pair<characterId, SuspicionMemoryEntry>> rows;
        rows.reserve(observer.trust.perCharacterScore.size());
        for (const auto &[id, score] : observer.trust.perCharacterScore)
        {
            if (id == protectedId)
            {
                continue; // the entry this call just wrote is never the victim
            }
            rows.emplace_back(id, memoryEntry(observer, id));
        }
        const auto victim = std::min_element(rows.begin(), rows.end(), EvictionKey{});
        if (victim == rows.end())
        {
            break; // the memory holds nothing but the protected row
        }
        observer.trust.perCharacterScore.erase(victim->first);
        observer.trust.lastSeen.erase(victim->first);
    }
}

} // namespace

void addSuspicion(
    NpcState &observer, characterId observed, std::uint32_t weight, std::uint64_t now
)
{
    if (observed == 0 || weight == 0)
    {
        return;
    }
    // The observer never remembers themselves; a pair with no entry starts at
    // zero (the honest zero-value rule).
    std::uint64_t &score = observer.trust.perCharacterScore[observed];
    score                = score + weight; // u64 saturates far beyond any realistic sum
    if (score > 0xFFFFFFFFull)
    {
        score = 0xFFFFFFFFull;
    }
    observer.trust.lastSeen[observed] = now;
    enforceMemoryCap(observer, observed);
}

void relieveSuspicion(
    NpcState &observer, characterId observed, std::uint32_t amount
)
{
    auto it = observer.trust.perCharacterScore.find(observed);
    if (it == observer.trust.perCharacterScore.end())
    {
        return; // no memory, nothing to relieve
    }
    it->second = it->second > amount ? it->second - amount : 0ull;
}

void decaySuspicion(
    NpcState &observer, std::uint64_t now
)
{
    // The lazy quantum (the 4.4 pattern): whole game-day steps since the
    // last decay apply at once; a rewind applies zero steps and keeps the
    // stamp (R1 — no underflow).
    if (now <= observer.trust.lastSuspicionDecay)
    {
        return;
    }
    const std::uint64_t steps = (now - observer.trust.lastSuspicionDecay) / core::DAY_LENGTH_SECONDS;
    if (steps == 0)
    {
        return;
    }
    const std::uint64_t decay = SUSPICION_DECAY * steps;
    for (auto it = observer.trust.perCharacterScore.begin(); it != observer.trust.perCharacterScore.end();)
    {
        const bool acquainted = isAcquaintance(observer, it->first);
        it->second            = it->second > decay ? it->second - decay : 0ull;
        // A stranger's memory lapses once it has FADED TO ZERO and sight was
        // lost for STRANGER_MEMORY_S (A-56) — the decay brings the score
        // down, the lost sight lets the entry go (the doc rule read against
        // the lazy-decay API: lastSeen holds the last sight stamp, the
        // evaluation refreshes it while co-present). An acquaintance's
        // memory is permanent: it decays to zero and stays.
        const auto seen       = observer.trust.lastSeen.find(it->first);
        const bool outOfSight = seen != observer.trust.lastSeen.end() && now - seen->second > STRANGER_MEMORY_S;
        const bool lapsed     = !acquainted && it->second == 0 && (outOfSight || seen == observer.trust.lastSeen.end());
        if (lapsed)
        {
            observer.trust.lastSeen.erase(it->first);
            it = observer.trust.perCharacterScore.erase(it);
        } else
        {
            ++it;
        }
    }
    observer.trust.lastSuspicionDecay += steps * core::DAY_LENGTH_SECONDS;
}

// ---- the evaluation ------------------------------------------------------------

std::uint32_t contextMaskFor(
    const WorldContext &ctx, const Character &observed
)
{
    std::uint32_t mask = 0;
    if (ctx.worldStore == nullptr)
    {
        return 0;
    }
    const territoryId terr = ctx.worldStore->territoryAt(observed.body.position);
    if (terr != 0)
    {
        // SETTLEMENT + MARKET ride D-SHELTER-1's settlement radius in v1 (the
        // settlement center doubles as the market — no separate market
        // geometry ships; the DEVIATIONS row notes it). The center derives
        // from the territory's REGION anchor — the same derivation the
        // runtime WorldQueries adapter makes (harrowmere_plugin.cpp), read
        // through the WorldStore so headless tests share the exact rule.
        const world::Territory *node = ctx.worldStore->territory(terr);
        if (node != nullptr && node->region != world::kNoRegion)
        {
            const Position center = world::regionCenter(node->region);
            if (distanceM(center, observed.body.position) <= SHELTER_RADIUS_M)
            {
                mask |= toBits(data::SuspicionMask::SETTLEMENT) | toBits(data::SuspicionMask::MARKET);
            }
        }
    }
    if (isNight(ctx.now))
    {
        mask |= toBits(data::SuspicionMask::NIGHT);
    }
    // PRIVATE_PROPERTY: the territory data flag (4.5/6.3 data) — no v1 data
    // ships it, so the context reads false (the dormant-marker rule, 4.1's
    // DEVIATIONS row covers the same shape for scorePhysical).
    return mask;
}

void appendExposedActs(
    const WorldContext &ctx, const Character &observed, std::vector<SuspicionAct> &out
)
{
    // v1 act state exists only for the player (3.10's act surface); an NPC
    // observed exposes no act (the honest zero-value — NPC acts have no
    // writer this phase).
    if (ctx.playerActions == nullptr || !ctx.characters->isPlayer(observed.id))
    {
        return;
    }
    const player::PlayerActions &acts = *ctx.playerActions;
    // UNLIT applies only to species WITHOUT darkvision (species.json — the
    // card's own pitfall note).
    const bool dark = speciesHasDarkvision(ctx, observed.species);
    for (const SuspicionAct act :
        {SuspicionAct::CROUCH,
            SuspicionAct::UNLIT,
            SuspicionAct::MASKED,
            SuspicionAct::TRESPASS,
            SuspicionAct::BLOODSTAINED,
            SuspicionAct::FLEE})
    {
        if (!acts.exposes(act))
        {
            continue;
        }
        if (act == SuspicionAct::UNLIT && dark)
        {
            continue; // the darkvision gate
        }
        out.push_back(act);
    }
}

void runSuspicionTicks(
    WorldContext &ctx
)
{
    assert(
        ctx.npcs != nullptr && ctx.characters != nullptr && ctx.lod != nullptr && "runSuspicionTicks: unbound context"
    );
    if (ctx.npcs == nullptr || ctx.characters == nullptr || ctx.lod == nullptr || ctx.perception == nullptr)
    {
        return;
    }

    std::vector<SuspicionAct> acts;
    // NEAR observers scan their witness radius; MID observers the abstract
    // MID radius (their sim is abstract, but the suspicion loop is the
    // near-territory budget's hourly pass — the active set bounds it).
    for (const characterId observerId : ctx.lod->activeNear)
    {
        evaluateObserver(ctx, observerId, WITNESS_RADIUS_M, acts);
    }
    for (const characterId observerId : ctx.lod->activeMid)
    {
        evaluateObserver(ctx, observerId, MID_DISTANCE_M, acts);
    }
}

void SuspicionTickSystem::run(
    WorldContext &ctx
)
{
    // A-76-style hourly cadence: one evaluation pass per game-hour (the
    // worldTime hour boundary; R8 — the sim clock, never the wall clock).
    const std::uint64_t hour = ctx.now / GAME_HOUR_SECONDS;
    if (hour == lastRunHour_)
    {
        return;
    }
    lastRunHour_ = hour;
    runSuspicionTicks(ctx);
}

void evaluateObserver(
    WorldContext &ctx, characterId observerId, double radiusMeters, std::vector<SuspicionAct> &actsScratch
)
{
    if (!ctx.npcs->contains(observerId))
    {
        return; // the player is never an observer (no NpcState, D-STORE-1)
    }
    NpcState &observer = ctx.npcs->get(observerId);
    if (!ctx.characters->exists(observerId))
    {
        return;
    }
    decaySuspicion(observer, ctx.now);

    const Character &observerChar = ctx.characters->get(observerId);
    for (const characterId observedId : ctx.perception->perceiveWithin(observerChar.body.position, radiusMeters))
    {
        if (observedId == observerId || !ctx.characters->exists(observedId))
        {
            continue;
        }
        const Character &observed = ctx.characters->get(observedId);

        // The AFK exemption: the observed (player) whose acts are AFK
        // artifacts (crouch held while AFK) draws nothing (D-AFK-1).
        const bool afkArtifact
            = ctx.playerActions != nullptr && ctx.characters->isPlayer(observedId) && ctx.playerActions->afk;

        const std::uint32_t mask                        = contextMaskFor(ctx, observed);
        const territoryId observedTerr                  = ctx.worldStore->territoryAt(observed.body.position);
        const std::vector<data::SuspicionTrigger> &rows = triggersFor(ctx, observedTerr);

        actsScratch.clear();
        appendExposedActs(ctx, observed, actsScratch);
        for (const SuspicionAct act : actsScratch)
        {
            if (afkArtifact && act == SuspicionAct::CROUCH)
            {
                continue; // the AFK artifact exemption
            }
            for (const data::SuspicionTrigger &row : rows)
            {
                // A row fires when the act is exposed AND EVERY context bit
                // of its mask is true (see suspicion.hpp's pinned reading).
                if (row.act == act && row.mask != 0 && (row.mask & mask) == row.mask)
                {
                    addSuspicion(observer, observedId, row.weight, ctx.now);
                }
            }
        }
    }
}

// ---- the effects ----------------------------------------------------------------

bool serviceRefused(
    const NpcState &provider, characterId customer
)
{
    return tierOf(provider, customer) == TrustTier::SHUNNED;
}

std::uint64_t sellReceiptModifierFor(
    const WorldContext &ctx, characterId seller, territoryId territory
)
{
    // The territory's first vendor-role resident (id-ascending, R7) judges
    // the seller's tier; nobody judging = SCALE (the honest neutral read).
    if (ctx.npcs == nullptr || ctx.characters == nullptr)
    {
        return SCALE;
    }
    for (const auto &[id, npc] : ctx.npcs->all())
    {
        if (npc.roleKey != "vendor" || npc.homeTerritory != territory || !ctx.characters->exists(id))
        {
            continue;
        }
        switch (tierOf(npc, seller))
        {
        case TrustTier::WATCHED:
            return TIER_RECEIPT_MODIFIER_WATCHED;
        case TrustTier::DISTRUSTED:
            return TIER_RECEIPT_MODIFIER_DISTRUSTED;
        case TrustTier::SHUNNED:
            return TIER_RECEIPT_MODIFIER_SHUNNED;
        case TrustTier::STRANGER:
            return SCALE;
        }
    }
    return SCALE;
}

std::uint64_t SuspicionPriceModifier::sellReceiptModifier(
    characterId seller, territoryId territory
) const
{
    if (ctx_ == nullptr)
    {
        return SCALE;
    }
    return sellReceiptModifierFor(*ctx_, seller, territory);
}

TrustTier greetingTone(
    const NpcState &greeter, characterId observed
)
{
    return tierOf(greeter, observed);
}

void SuspicionSignalSink::onAppearanceFrequent(
    characterId who, std::uint64_t at
)
{
    // The A-25 signal (2.6) lands as APPEARANCE_SUSPICION_WEIGHT (A-94) on
    // every NEAR/MID observer's memory of `who`, once per window — the sink
    // fires only at threshold crossings, so each call is one window's grant.
    if (ctx_ == nullptr || ctx_->npcs == nullptr || ctx_->lod == nullptr)
    {
        return;
    }
    std::vector<SuspicionAct> scratch;
    for (const characterId observerId : ctx_->lod->activeNear)
    {
        applyAppearanceSignal(*ctx_, observerId, who, at);
    }
    for (const characterId observerId : ctx_->lod->activeMid)
    {
        applyAppearanceSignal(*ctx_, observerId, who, at);
    }
}

void applyAppearanceSignal(
    WorldContext &ctx, characterId observerId, characterId who, std::uint64_t at
)
{
    if (observerId == who || !ctx.npcs->contains(observerId))
    {
        return;
    }
    NpcState &observer = ctx.npcs->get(observerId);
    addSuspicion(observer, who, APPEARANCE_SUSPICION_WEIGHT, at);
}

} // namespace harrowmere::justice
