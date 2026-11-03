#ifndef HARROWMERE_CORE_BRIDGE_STORE_ADAPTERS_HPP
#define HARROWMERE_CORE_BRIDGE_STORE_ADAPTERS_HPP

// Runtime glue for task 7.2's full bridge: the production
// CharactersViewForBridge over the sim stores, plus the engine-side notes the
// plugin's sink adapter needs. This header is CORE-INTERNAL (it includes
// store headers) - the published contract stays harrowmere_capability.hpp
// (§4.2 rule 5); only harrowmere_plugin.cpp (the 3.1 runtime-wiring home)
// includes this file.
//
// Wiring (the 3.1 integrator's lines, after the stores exist):
//
//     storeCharacterView_ = std::make_unique<bridge::StoreCharacterView>(
//         characters_, registry_, player_.position);
//     storeCharacterView_->setCorpseProvider([this] {
//         return bridge::corpseDescsFromStore(corpses_);   // helper below
//     });
//     storeCharacterView_->setNpcTierProvider(
//         [this](characterId id) {                         // the LOD tier read
//             return npcs_.contains(id) ? static_cast<std::uint8_t>(npcs_.get(id).tier)
//                                       : static_cast<std::uint8_t>(bridge::kTierFar);
//         },
//         [this](characterId id) {                         // the movement flag read
//             return npcs_.contains(id)
//                 && (npcs_.get(id).migrating || npcs_.get(id).currentIntent.kind != NpcIntentKind::None);
//         });
//     bridge_ = std::make_unique<bridge::SceneBridge>(
//         *bridgeWorld_, *bridgeClock_, *bridgeSink_, storeCharacterView_.get());
//
// WHY PROVIDERS (not store references) for the NPC and corpse halves: those
// records' headers (npc/state.hpp, combat/death.hpp) are completed by the 3.x
// integration (the batch's own headers carry open items - the Inventory
// alias, the goodId alias, the ItemStack loot type). Provider functions keep
// THIS header compilable today, with safe defaults (all NPCs statistical -
// invisible, exactly the Phase-1 presentation) - and the plugin swaps in the
// three lambdas above when the NPC/corpse stores join the runtime. The
// character half (species, faction, position) rides the built CharacterStore
// directly.

#include <algorithm>
#include <functional>
#include <vector>

#include "bridge/scene_bridge.hpp" // the interfaces (CharactersViewForBridge)

#include "character/store.hpp"   // CharacterStore (task 2.0; built)
#include "character/species.hpp" // isSapient (task 2.2; built)
#include "core/constants.hpp"    // PLAYER_SPECIES_ID
#include "core/types.hpp"
#include "data/registry.hpp" // DataRegistry (species rows; built)

namespace harrowmere::bridge
{

class StoreCharacterView final : public CharactersViewForBridge
{
public:
    // All references are caller-owned and must outlive this view (the plugin
    // keeps the stores and the player transform as members).
    StoreCharacterView(
        const character::CharacterStore &characters, const data::DataRegistry &data, const Position &anchor
    )
        : characters_(characters), data_(data), anchor_(anchor)
    {}

    // The corpse provider: returns the present corpse records, id-ascending
    // (the plugin reads CorpseStore inside the lambda). Unset = no corpses.
    using CorpseProvider = std::function<std::vector<CorpseDesc>()>;
    void setCorpseProvider(
        CorpseProvider provider
    )
    {
        corpses_ = std::move(provider);
    }

    // The NPC tier read (npc/state.hpp's LodTier cast to the kTier* ordinals)
    // and the movement flag (an in-progress migration or an active NEAR
    // intent). Unset = every NPC reads as static FAR: the statistical tier,
    // which the bridge renders nothing for (the safe pre-integration default).
    using TierProvider   = std::function<std::uint8_t(characterId)>;
    using MovingProvider = std::function<bool(characterId)>;
    void setNpcTierProvider(
        TierProvider tier, MovingProvider moving
    )
    {
        npcTier_   = std::move(tier);
        npcMoving_ = std::move(moving);
    }

    Position anchor() const override
    {
        return anchor_;
    }

    std::vector<CharacterDesc> presentCharacters() const override
    {
        std::vector<CharacterDesc> out;
        out.reserve(characters_.population());
        for (const auto &[id, c] : characters_.all())
        {
            const double dx = c.body.position.x - anchor_.x;
            const double dz = c.body.position.z - anchor_.z;
            if (dx * dx + dz * dz > PRESENTATION_RADIUS_M * PRESENTATION_RADIUS_M)
            {
                continue; // beyond A-118: the renderer never sees it
            }

            CharacterDesc d;
            d.id        = id;
            d.species   = c.species;
            d.faction   = c.faction;
            d.position  = c.body.position;
            d.sapient   = isSapient(data_, c.species);
            d.tier      = kTierFar;
            bool moving = false;
            if (characters_.isPlayer(id))
            {
                d.tier = kTierNear; // the anchor itself is NEAR by definition
            } else
            {
                if (npcTier_ != nullptr)
                {
                    d.tier = npcTier_(id);
                }
                if (npcMoving_ != nullptr)
                {
                    moving = npcMoving_(id);
                }
            }
            d.moving = moving;
            out.push_back(d);
        }
        // characters_.all() iterates id-ascending (R7); the radius cut keeps
        // the order - the output contract holds without a re-sort.
        return out;
    }

    std::vector<CorpseDesc> presentCorpses() const override
    {
        if (!corpses_)
        {
            return {};
        }
        return corpses_();
    }

private:
    const character::CharacterStore &characters_;
    const data::DataRegistry &data_;
    const Position &anchor_;
    CorpseProvider corpses_;
    TierProvider npcTier_;
    MovingProvider npcMoving_;
};

// The plugin-side corpse helper (see the header comment). Compiled in the
// plugin TU once combat/death.hpp joins the build; kept here as the single
// documented integration point.
//
//     std::vector<bridge::CharactersViewForBridge::CorpseDesc>
//     corpseDescsFromStore(const CorpseStore &store, const Position &anchor)
//     {
//         std::vector<...> out;
//         for (const auto &[victim, corpse] : store.all()) {
//             if (planar distance > PRESENTATION_RADIUS_M) continue;
//             out.push_back({victim, corpse.pos, corpse.discovered});
//         }
//         return out; // all() iterates id-ascending (R7)
//     }

} // namespace harrowmere::bridge

#endif // HARROWMERE_CORE_BRIDGE_STORE_ADAPTERS_HPP
