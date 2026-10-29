#include "persistence.hpp"

#include <toml.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sol/sol.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "engine/kernel/filesystem_utilities.hpp"

namespace engine
{

namespace
{

// One field per object row, named exactly like the [scene] section's keys so
// a save file reads as documentation and a hand-tuned save behaves like a
// hand-tuned scene (the row schema is the [scene.objects] schema plus the
// mesh name; the [scene] header comment documents the same fields).
const char *objectTypeToString(
    std::uint32_t type
)
{
    switch (type)
    {
    case scene_object_type::sphere:
        return "sphere";
    case scene_object_type::mesh:
        return "mesh";
    case scene_object_type::box:
    default:
        return "box";
    }
}

toml::array floatArray(
    const std::array<float, 3> &values
)
{
    toml::array array;
    for (const float value : values)
    {
        array.push_back(static_cast<double>(value));
    }
    return array;
}

std::array<float, 3> floatArray3(
    const toml::table &row, const char *key, const std::array<float, 3> &fallback, bool &replaced
)
{
    const toml::node *value = row.get(key);
    if (value == nullptr || !value->is_array())
    {
        return fallback;
    }
    const toml::array *array = value->as_array();
    if (array == nullptr || array->size() < 3)
    {
        return fallback;
    }
    // TOML floats may legally be nan/inf, and one non-finite component
    // poisons every downstream consumer of the descriptor (bounds, BVH,
    // physics). A component that is not finite leaves the default in place
    // and reports the surgery so the row can log once.
    std::array<float, 3> result = fallback;
    for (std::size_t index = 0; index < 3; ++index)
    {
        const float component = static_cast<float>(array->get(index)->value_or(static_cast<double>(fallback[index])));
        if (std::isfinite(component))
        {
            result[index] = component;
        } else
        {
            replaced = true;
        }
    }
    return result;
}

// One scalar field from a save row: a non-finite TOML number (nan/inf parse
// as legal floats) falls back to the descriptor default and reports it.
float finiteScalar(
    const toml::table &row, const char *key, float fallback, bool &replaced
)
{
    const toml::node *value = row.get(key);
    if (value == nullptr)
    {
        return fallback;
    }
    const float number = static_cast<float>(value->value_or(static_cast<double>(fallback)));
    if (!std::isfinite(number))
    {
        replaced = true;
        return fallback;
    }
    return number;
}

// finiteScalar plus the field's sane span: roughness/metallic live in [0, 1]
// by schema, and an out-of-span number renders as garbage as a NaN would.
float clampedScalar(
    const toml::table &row, const char *key, float fallback, float spanMin, float spanMax, bool &replaced
)
{
    const float number = finiteScalar(row, key, fallback, replaced);
    if (number < spanMin || number > spanMax)
    {
        replaced = true;
        return std::clamp(number, spanMin, spanMax);
    }
    return number;
}

std::filesystem::path resolveAgainstExecutable(
    const std::string &path
)
{
    const std::filesystem::path candidate(path);
    return candidate.is_relative() ? (executableDirectory() / candidate) : candidate;
}

} // namespace

namespace
{

// The dedicated Lua bridge extension object for the persistence module. Owns
// the sol2 wiring that installs engine.save_scene, engine.load_scene and
// engine.clear_scene into every script state. Holds raw pointers to the
// PersistenceCapability the module provides and the LoggerCapability for
// routing save/load error reports to the log.
class PersistenceLuaBridge final : public LuaBridgeExtension
{
public:
    PersistenceLuaBridge(
        PersistenceCapability *capability, LoggerCapability *logger
    )
        : capability(capability), logger(logger)
    {}

    const char *extensionName() const override
    {
        return "persistence";
    }

    void installLuaBridge(
        luaState *lua
    ) override
    {
        // The host (lua_scripting) guarantees the engine table exists when
        // this is called. The bridge functions call the PersistenceCapability
        // methods directly through the captured pointer - no capability
        // acquisition, no permission check. Errors are routed to the logging
        // capability exactly like the in-module call path: a failed save/load
        // fills errorReport, the bridge logs it and surfaces the success
        // boolean / object count to the script.
        sol::state_view luaView(lua);
        sol::table engine = luaView["engine"];

        engine.set_function("save_scene", [this](const std::string &path) -> bool {
            std::string errorReport;
            const bool saved = capability->saveScene(path, errorReport);
            if (!errorReport.empty() && logger != nullptr)
            {
                logger->logMessage(LogLevel::Error, errorReport);
            }
            return saved;
        });
        engine.set_function("load_scene", [this](const std::string &path) -> std::uint32_t {
            std::string errorReport;
            const std::uint32_t loaded = capability->loadScene(path, errorReport);
            if (!errorReport.empty() && logger != nullptr)
            {
                logger->logMessage(LogLevel::Error, errorReport);
            }
            return loaded;
        });
        engine.set_function("clear_scene", [this]() -> bool {
            std::string errorReport;
            const bool cleared = capability->clearScene(errorReport);
            if (!errorReport.empty() && logger != nullptr)
            {
                logger->logMessage(LogLevel::Error, errorReport);
            }
            return cleared;
        });
    }

private:
    PersistenceCapability *capability;
    LoggerCapability *logger;
};

} // namespace

PersistenceModule::PersistenceModule()
{
    componentManifest = ComponentManifest{
        .name    = "persistence",
        .kind    = ComponentKind::Module,
        .version = "0.1.0",
        // The implementation-bound member of the "persistence" family: a future
        // binary or streamed-world save format declares its own member and the
        // [providers] configuration picks between them.
        .providedCapabilities
        = {"persistence.toml",
            // The Lua bridge extension member: declares this module as a
            // provider of the "scripting.lua_bridge" family. The
            // lua_scripting host acquires every active member through
            // ComponentContext::acquireEachCapability and asks each
            // one to install its own bindings into the host's engine
            // table.
            "scripting.lua_bridge.persistence"},
        .requiredCapabilities = {"logging", "scene"},
        .optionalCapabilities = {},
        .requiredComponents   = {},
        // Saving writes a file (filesystem.write); loading reads one
        // (filesystem.read). Both are deniable in configuration like every grant.
        .requiredPermissions = {"capability.logging", "capability.scene", "filesystem.write", "filesystem.read"},
        .knownConfigKeys     = {},
    };
}

PersistenceModule::~PersistenceModule()
{
    stop();
}

void PersistenceModule::construct(
    ComponentContext &context
)
{
    boundContext    = &context;
    logger          = &context.acquireCapability<LoggerCapability>();
    sceneCapability = &context.acquireCapability<SceneCapability>();

    // Construct the Lua bridge extension object. The module IS the
    // PersistenceCapability, so `this` is the capability pointer; the logger
    // is passed separately so the bridge can route save/load error reports.
    luaBridge = std::make_unique<PersistenceLuaBridge>(this, logger);
}

void PersistenceModule::start()
{
    logger->logMessage(LogLevel::Information, "persistence module started");
}

void PersistenceModule::stop()
{
    // Reset the Lua bridge before nulling the capability pointers it captures.
    luaBridge.reset();
    boundContext    = nullptr;
    logger          = nullptr;
    sceneCapability = nullptr;
}

void *PersistenceModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    if (capabilityName == "persistence.toml")
    {
        return static_cast<PersistenceCapability *>(this);
    }
    // The "scripting.lua_bridge.persistence" member: returns the dedicated
    // PersistenceLuaBridge object this module owns (constructed in
    // construct()).
    if (capabilityName == "scripting.lua_bridge.persistence")
    {
        return luaBridge.get();
    }
    return nullptr;
}

bool PersistenceModule::saveScene(
    const std::string &path, std::string &errorReport
)
{
    // Same liveness guard as the rest of the capability surface: stop() has
    // nulled the pointers, and a save after that must refuse precisely.
    if (sceneCapability == nullptr || logger == nullptr)
    {
        errorReport = "persistence module: saveScene refused (the module is not live)";
        return false;
    }
    const std::vector<SceneObjectDescriptor> snapshot = sceneCapability->objectSnapshot();
    toml::table file;
    file.emplace("saved_objects", static_cast<std::int64_t>(snapshot.size()));

    toml::array objects;
    for (const SceneObjectDescriptor &object : snapshot)
    {
        toml::table row;
        row.emplace("type", objectTypeToString(object.type));
        row.emplace("position", floatArray(object.position));
        // Box: half extents; sphere: radius in x; mesh: the bbox half extents
        // (recomputed from the registered mesh at load time - the row's size
        // documents the proxy, the registry's mesh data is authoritative).
        row.emplace("size", floatArray(object.size));
        row.emplace("rotation_y", static_cast<double>(object.rotationYRadians));
        row.emplace("albedo", floatArray(object.albedo));
        row.emplace("roughness", static_cast<double>(object.roughness));
        row.emplace("metallic", static_cast<double>(object.metallic));
        row.emplace("emissive_strength", static_cast<double>(object.emissiveStrength));
        if (!object.textureSet.empty())
        {
            row.emplace("texture", object.textureSet);
        }
        row.emplace("uv_scale", static_cast<double>(object.uvScale));
        if (object.type == scene_object_type::mesh)
        {
            const std::string meshName = sceneCapability->meshName(object.meshHandle);
            if (meshName.empty())
            {
                errorReport = "persistence module: object " + std::to_string(object.meshHandle)
                            + " instances a mesh the registry does not know; save refused";
                return false;
            }
            row.emplace("mesh", meshName);
        }
        objects.push_back(std::move(row));
    }
    file.emplace("objects", std::move(objects));

    const std::filesystem::path resolved = resolveAgainstExecutable(path);
    std::error_code directoryError;
    std::filesystem::create_directories(resolved.parent_path(), directoryError);
    // Atomic save: write a sibling .tmp file, flush it fully, close it, then
    // rename over the target. rename is atomic on POSIX and on Windows (same
    // volume), so a crash mid-write leaves the previous save intact - for a
    // script-driven assembly this file IS the only copy of the scene.
    const std::filesystem::path temporary = resolved.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            errorReport = "persistence module: cannot open '" + temporary.string() + "' for writing"
                        + " (is filesystem.write granted to this caller's component?)";
            return false;
        }
        output << "# Saved by the engine's persistence module: " << snapshot.size() << " object(s).\n"
               << "# Loading spawns these as NEW registry objects (handles are runtime values).\n"
               << "# Mesh rows reference registry meshes by NAME - the mesh must be registered\n"
               << "# (or re-imported) before load. The row schema is the [scene.objects] schema.\n"
               << file << "\n";
        // Flush and check BEFORE closing: a full disk (or any write error)
        // must abort the save here, not commit a truncated temp file over
        // the good save.
        output.flush();
        if (!output)
        {
            errorReport = "persistence module: writing '" + temporary.string() + "' failed";
            return false;
        }
    } // the stream closes (and releases the file) before the rename
    std::error_code renameError;
    std::filesystem::rename(temporary, resolved, renameError);
    if (renameError)
    {
        errorReport = "persistence module: cannot commit '" + resolved.string()
                    + "' (rename failed: " + renameError.message() + ")";
        // Best effort: do not strand the temp file next to the save.
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        return false;
    }
    logger->logMessage(
        LogLevel::Information,
        "persistence module: saved " + std::to_string(snapshot.size()) + " object(s) to '" + path + "'"
    );
    return true;
}

std::uint32_t PersistenceModule::loadScene(
    const std::string &path, std::string &errorReport
)
{
    const std::filesystem::path resolved = resolveAgainstExecutable(path);
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(resolved, fileError))
    {
        errorReport
            = "persistence module: '" + path + "' does not name a readable file (resolved '" + resolved.string() + "')";
        return 0;
    }
    toml::parse_result parsed;
    try
    {
        parsed = toml::parse_file(resolved.string());
    } catch (const toml::parse_error &error)
    {
        std::ostringstream description;
        description << "persistence module: '" << path << "' is not valid TOML: " << error.description() << " (line "
                    << error.source().begin.line << ")";
        errorReport = description.str();
        return 0;
    }

    const toml::node *objectsNode = parsed.get("objects");
    if (objectsNode == nullptr || !objectsNode->is_array())
    {
        errorReport = "persistence module: '" + path + "' defines no [[objects]] array - nothing to load";
        return 0;
    }
    const toml::array *objects = objectsNode->as_array();
    if (objects == nullptr)
    {
        errorReport = "persistence module: '" + path + "'s objects array is not an array of tables";
        return 0;
    }

    std::uint32_t spawned = 0;
    std::size_t rowIndex  = 0;
    for (const toml::node &entry : *objects)
    {
        const toml::table *row = entry.as_table();
        if (row == nullptr)
        {
            continue;
        }
        ++rowIndex;
        SceneObjectDescriptor object;
        const std::string type = row->get("type") != nullptr ? row->get("type")->value_or(std::string("box")) : "box";
        if (type == "sphere")
        {
            object.type = scene_object_type::sphere;
        } else if (type == "mesh")
        {
            const std::string meshName
                = row->get("mesh") != nullptr ? row->get("mesh")->value_or(std::string()) : std::string();
            if (meshName.empty())
            {
                if (logger != nullptr)
                {
                    logger->logMessage(
                        LogLevel::Warning,
                        "persistence module: '" + path + "' has a mesh row without a mesh name; row skipped"
                    );
                }
                continue;
            }
            const SceneCapability::MeshHandle handle = sceneCapability->meshHandle(meshName);
            if (handle == SceneCapability::invalidMesh)
            {
                if (logger != nullptr)
                {
                    logger->logMessage(
                        LogLevel::Warning,
                        "persistence module: mesh '" + meshName + "' is not registered (import it first); row skipped"
                    );
                }
                continue;
            }
            object.type       = scene_object_type::mesh;
            object.meshHandle = handle;
        }
        // Per-field sanitize: a hand-edited (or corrupted) save may carry
        // nan/inf TOML floats, which are legal TOML but poison the registry
        // (one NaN position breaks bounds, BVH and physics alike). Every
        // field is either finite - and, where the schema has one, inside its
        // sane span - or replaced by the descriptor default. The row logs
        // ONCE about whatever surgery was needed.
        bool rowSanitized = false;
        object.position   = floatArray3(*row, "position", object.position, rowSanitized);
        object.size       = floatArray3(*row, "size", object.size, rowSanitized);
        for (float &extent : object.size)
        {
            // A zero or negative extent degenerates the bounds and every
            // contact against the object; the schema's floor keeps it sane.
            if (extent < 0.001f)
            {
                rowSanitized = true;
                extent       = 0.001f;
            }
        }
        object.rotationYRadians = finiteScalar(*row, "rotation_y", object.rotationYRadians, rowSanitized);
        object.albedo           = floatArray3(*row, "albedo", object.albedo, rowSanitized);
        object.roughness        = clampedScalar(*row, "roughness", object.roughness, 0.0f, 1.0f, rowSanitized);
        object.metallic         = clampedScalar(*row, "metallic", object.metallic, 0.0f, 1.0f, rowSanitized);
        object.emissiveStrength = finiteScalar(*row, "emissive_strength", object.emissiveStrength, rowSanitized);
        object.textureSet
            = row->get("texture") != nullptr ? row->get("texture")->value_or(std::string()) : std::string();
        object.uvScale = finiteScalar(*row, "uv_scale", object.uvScale, rowSanitized);
        if (rowSanitized && logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning,
                "persistence module: '" + path + "' row " + std::to_string(rowIndex)
                    + " carried non-finite or out-of-range numbers; sane values substituted"
            );
        }
        if (sceneCapability->addObject(object) == SceneCapability::invalidObject)
        {
            // The registry is full (its refusal is logged precisely there):
            // report what arrived, like the asset importer.
            return spawned;
        }
        ++spawned;
    }
    if (logger != nullptr)
    {
        logger->logMessage(
            LogLevel::Information,
            "persistence module: loaded " + std::to_string(spawned) + " object(s) from '" + path + "'"
        );
    }
    return spawned;
}

bool PersistenceModule::clearScene(
    std::string &errorReport
)
{
    // Same liveness guard as saveScene: the capability pointers are null
    // once the module has stopped.
    if (sceneCapability == nullptr)
    {
        errorReport = "persistence module: clearScene refused (the module is not live)";
        return false;
    }
    // Meshes stay: they are assets shared by instances, and a load that
    // follows typically re-references them by name. Handles are read in one
    // ascending pass and removed after (removal never reuses handles, and a
    // live iteration over a mutating map would be the wrong shape here).
    const std::vector<SceneCapability::ObjectHandle> handles = sceneCapability->objectHandles();
    for (const SceneCapability::ObjectHandle handle : handles)
    {
        sceneCapability->removeObject(handle);
    }
    if (logger != nullptr)
    {
        logger->logMessage(
            LogLevel::Information,
            "persistence module: scene registry cleared (" + std::to_string(handles.size()) + " object(s) removed)"
        );
    }
    return true;
}

} // namespace engine
