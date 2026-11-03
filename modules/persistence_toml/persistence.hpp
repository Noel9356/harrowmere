#ifndef ENGINE_MODULES_PERSISTENCE_HPP
#define ENGINE_MODULES_PERSISTENCE_HPP

#include "logging_capability.hpp"
#include "lua_bridge_extension.hpp"
#include "persistence_capability.hpp"
#include "scene_capability.hpp"
#include "engine/kernel/component_interface.hpp"
#include "engine/kernel/event_bus.hpp"

#include <memory>
#include <string>

namespace engine
{

// Module "persistence_toml": the scene registry's save/load (family member
// "persistence.toml").
//
// The module is a thin, honest bridge: it snapshots the registry (the
// registry's snapshot order is stable - ascending handles) into one TOML
// file and spawns rows back as new registry objects. It owns no world state
// of its own; the registry stays the single source of truth, and a save
// captures exactly what the renderer renders, physics steps and scripts
// address at the moment of the call.
//
// File format (documented in the module README): one [[objects]] array of
// tables, one row per object, the same field names the [scene] section uses
// (type, position, size, rotation_y, albedo, roughness, metallic,
// emissive_strength, texture, uv_scale, mesh). A saved file is READABLE as
// documentation and EDITABLE by hand: load accepts every file a human tuned.
//
// The module additionally provides the "scripting.lua_bridge.persistence"
// Lua bridge extension: it installs engine.save_scene, engine.load_scene and
// engine.clear_scene into every script state through a dedicated
// PersistenceLuaBridge object owned by this module (see persistence.cpp).
// The host discovers this extension through the multi-provider acquisition of
// the "scripting.lua_bridge" family, so adding this capability to the module
// never touches the lua_scripting source.
class PersistenceModule final : public ModuleInterface, public PersistenceCapability
{
public:
    PersistenceModule();
    ~PersistenceModule() override;

    const ComponentManifest &manifest() const override
    {
        return componentManifest;
    }

    void construct(ComponentContext &context) override;
    void start() override;
    void stop() override;

    void *capabilityPointerFor(const std::string &capabilityName) override;

    // PersistenceCapability.
    bool saveScene(const std::string &path, std::string &errorReport) override;
    std::uint32_t loadScene(const std::string &path, std::string &errorReport) override;
    bool clearScene(std::string &errorReport) override;

private:
    ComponentManifest componentManifest;
    ComponentContext *boundContext   = nullptr;
    LoggerCapability *logger         = nullptr;
    SceneCapability *sceneCapability = nullptr;
    // The Lua bridge extension object owned by this module. Constructed in
    // construct() and returned from capabilityPointerFor for the
    // "scripting.lua_bridge.persistence" member. Held as the base
    // LuaBridgeExtension pointer: the concrete PersistenceLuaBridge type
    // (defined in persistence.cpp) stays private to this module's translation
    // unit.
    std::unique_ptr<LuaBridgeExtension> luaBridge;
};

} // namespace engine

#endif // ENGINE_MODULES_PERSISTENCE_HPP
