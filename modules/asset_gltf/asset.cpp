#include "asset.hpp"

#include "gltf_import.hpp"
#include "engine/kernel/filesystem_utilities.hpp"

#include <cstring>
#include <fstream>
#include <sol/sol.hpp>
#include <sstream>
#include <string>

namespace engine
{

namespace
{

// The glTF 2.0 binary container (GLB): a 12-byte header - the 'glTF' magic,
// the container version, the total length - followed by chunks, each with an
// 8-byte header (length, type). The JSON chunk (type 'JSON') is required and
// comes first; the optional binary chunk (type 'BIN') follows, padded to a
// 4-byte boundary with zeros. buffer 0 of the JSON document refers to the
// BIN chunk when it declares no URI.
constexpr char glbMagic[4]                 = {'g', 'l', 'T', 'F'};
constexpr std::uint32_t glbVersionExpected = 2u;
constexpr std::size_t glbHeaderSize        = 12;
constexpr std::size_t glbChunkHeaderSize   = 8;
constexpr std::uint32_t glbJsonChunkType   = 0x4E4F534Au; // 'JSON' little endian
constexpr std::uint32_t glbBinChunkType    = 0x004E4942u; // 'BIN\0' little endian

// Parses one GLB container into its JSON text and its optional BIN chunk.
// Every deviation from the container layout is a precise refusal: a garbled
// binary section must never reach the JSON parser as content.
bool parseGlbContainer(
    const std::string &fileData,
    const std::string &sourceName,
    std::string &jsonText,
    std::vector<std::uint8_t> &binaryChunk,
    std::string &errorReport
)
{
    if (fileData.size() < glbHeaderSize + glbChunkHeaderSize)
    {
        errorReport = "asset module: '" + sourceName + "' is too small to be a .glb container";
        return false;
    }
    if (std::memcmp(fileData.data(), glbMagic, sizeof(glbMagic)) != 0)
    {
        errorReport = "asset module: '" + sourceName + "' has the .glb extension but not the 'glTF' magic";
        return false;
    }
    std::uint32_t version     = 0;
    std::uint32_t totalLength = 0;
    std::memcpy(&version, fileData.data() + 4, sizeof(version));
    std::memcpy(&totalLength, fileData.data() + 8, sizeof(totalLength));
    if (version != glbVersionExpected)
    {
        errorReport = "asset module: '" + sourceName + "' is glTF container version " + std::to_string(version)
                    + "; this importer reads version 2";
        return false;
    }
    if (totalLength < glbHeaderSize || totalLength > fileData.size())
    {
        errorReport = "asset module: '" + sourceName + "' declares a " + std::to_string(totalLength)
                    + " byte container in a " + std::to_string(fileData.size()) + " byte file";
        return false;
    }

    std::size_t position = glbHeaderSize;
    bool jsonRead        = false;
    while (position + glbChunkHeaderSize <= totalLength)
    {
        std::uint32_t chunkLength = 0;
        std::uint32_t chunkType   = 0;
        std::memcpy(&chunkLength, fileData.data() + position, sizeof(chunkLength));
        std::memcpy(&chunkType, fileData.data() + position + 4, sizeof(chunkType));
        position += glbChunkHeaderSize;
        if (chunkLength > totalLength - position)
        {
            errorReport = "asset module: '" + sourceName + "' has a chunk that overruns the container";
            return false;
        }
        if (chunkType == glbJsonChunkType && !jsonRead)
        {
            jsonText.assign(fileData.data() + position, chunkLength);
            jsonRead = true;
        } else if (chunkType == glbBinChunkType && binaryChunk.empty())
        {
            binaryChunk.assign(fileData.data() + position, fileData.data() + position + chunkLength);
        }
        // Unknown chunk types are skipped per the spec (forward
        // compatibility); a second JSON or BIN chunk would be malformed and
        // is ignored by the same rule - the first of each wins.
        position += chunkLength;
    }
    if (!jsonRead)
    {
        errorReport = "asset module: '" + sourceName + "' has no JSON chunk";
        return false;
    }
    return true;
}

} // namespace

namespace
{

// The dedicated Lua bridge extension object for the asset module. Owns the
// sol2 wiring that installs engine.import_gltf into every script state.
// Holds raw pointers to the AssetCapability the module provides and the
// LoggerCapability for routing import error reports to the log.
class AssetLuaBridge final : public LuaBridgeExtension
{
public:
    AssetLuaBridge(
        AssetCapability *capability, LoggerCapability *logger
    )
        : capability(capability), logger(logger)
    {}

    const char *extensionName() const override
    {
        return "asset";
    }

    void installLuaBridge(
        luaState *lua
    ) override
    {
        // The host (lua_scripting) guarantees the engine table exists when
        // this is called. The bridge function calls the AssetCapability's
        // importGltfIntoScene directly through the captured pointer - no
        // capability acquisition, no permission check.
        //
        // Returns the number of spawned objects; a failed import returns 0
        // and logs the precise reason (the script sees the count, not an
        // exception - import failures are content problems, not script bugs).
        // Fidelity is "mesh" (the default) or "bbox".
        sol::state_view luaView(lua);
        sol::table engine = luaView["engine"];

        engine.set_function("import_gltf", [this](const std::string &path, sol::object fidelity) -> std::uint32_t {
            const std::string fidelityName = fidelity.valid() ? fidelity.as<std::string>() : std::string("mesh");
            std::uint32_t fidelityValue    = asset_fidelity::full_mesh;
            if (fidelityName == "bbox")
            {
                fidelityValue = asset_fidelity::bounding_proxies;
            } else if (fidelityName != "mesh")
            {
                if (logger != nullptr)
                {
                    logger->logMessage(
                        LogLevel::Error,
                        "import_gltf: unknown fidelity '" + fidelityName + "' (use \"mesh\" or \"bbox\")"
                    );
                }
                return 0;
            }
            std::string errorReport;
            const std::uint32_t spawned = capability->importGltfIntoScene(path, fidelityValue, errorReport);
            if (!errorReport.empty() && logger != nullptr)
            {
                logger->logMessage(LogLevel::Error, errorReport);
            }
            return spawned;
        });
    }

private:
    AssetCapability *capability;
    LoggerCapability *logger;
};

} // namespace

AssetModule::AssetModule()
{
    componentManifest = ComponentManifest{
        .name    = "asset",
        .kind    = ComponentKind::Module,
        .version = "0.1.0",
        .providedCapabilities
        = {"asset.gltf",
            // The Lua bridge extension member: declares this module as a
            // provider of the "scripting.lua_bridge" family. The
            // lua_scripting host acquires every active member through
            // ComponentContext::acquireEachCapability and asks each
            // one to install its own bindings into the host's engine
            // table.
            "scripting.lua_bridge.asset"},
        // The scene registry is the import target (hard edge); reading content
        // files is the importer's whole job, so the read permission is declared.
        .requiredCapabilities = {"logging", "scene"},
        .optionalCapabilities = {},
        .requiredComponents   = {},
        .requiredPermissions  = {"capability.logging", "capability.scene", "filesystem.read"},
        .knownConfigKeys      = {},
    };
}

AssetModule::~AssetModule()
{
    stop();
}

void AssetModule::construct(
    ComponentContext &context
)
{
    boundContext    = &context;
    logger          = &context.acquireCapability<LoggerCapability>();
    sceneCapability = &context.acquireCapability<SceneCapability>();

    // Construct the Lua bridge extension object. The module IS the
    // AssetCapability, so `this` is the capability pointer; the logger is
    // passed separately so the bridge can route import error reports.
    luaBridge = std::make_unique<AssetLuaBridge>(this, logger);
}

void AssetModule::start()
{
    logger->logMessage(LogLevel::Information, "asset module started");
}

void AssetModule::stop()
{
    // Reset the Lua bridge before nulling the capability pointers it captures.
    luaBridge.reset();
    boundContext    = nullptr;
    logger          = nullptr;
    sceneCapability = nullptr;
}

void *AssetModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    if (capabilityName == "asset.gltf")
    {
        return static_cast<AssetCapability *>(this);
    }
    // The "scripting.lua_bridge.asset" member: returns the dedicated
    // AssetLuaBridge object this module owns (constructed in construct()).
    if (capabilityName == "scripting.lua_bridge.asset")
    {
        return luaBridge.get();
    }
    return nullptr;
}

std::uint32_t AssetModule::importGltfIntoScene(
    const std::string &path, std::uint32_t fidelity, std::string &errorReport
)
{
    // Read the file. Relative paths resolve against the executable directory
    // (the engine's path convention), which is where the runtime layout puts
    // mod packages - so "mods/my_mod/assets/level.gltf" just works from a
    // script.
    const std::filesystem::path resolved
        = std::filesystem::path(path).is_relative() ? (executableDirectory() / path) : std::filesystem::path(path);
    std::error_code fileError;
    const bool exists = std::filesystem::is_regular_file(resolved, fileError);
    if (!exists)
    {
        errorReport
            = "asset module: '" + path + "' does not name a readable file (resolved '" + resolved.string() + "')";
        return 0;
    }
    // Read the file in ONE buffer: size the string from the file size, then
    // read straight into it. The ostringstream + str() round trip used to
    // hold two copies of the file transiently (the stream's internal buffer
    // plus the returned string) and copied the whole document once more into
    // jsonText below; for a megabyte-scale glTF that halved the load's peak
    // memory and dropped a full-document copy.
    std::ifstream file(resolved, std::ios::binary);
    if (!file)
    {
        errorReport = "asset module: failed to open '" + resolved.string() + "'";
        return 0;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    std::string fileData;
    if (fileSize > 0)
    {
        fileData.resize(static_cast<std::size_t>(fileSize));
        file.read(fileData.data(), fileSize);
        fileData.resize(file.gcount() > 0 ? static_cast<std::size_t>(file.gcount()) : 0);
    }

    std::string jsonText;
    std::vector<std::uint8_t> glbBinaryChunk;
    const bool isGlb = resolved.extension() == ".glb";

    if (isGlb)
    {
        if (!parseGlbContainer(fileData, path, jsonText, glbBinaryChunk, errorReport))
        {
            return 0;
        }
        // parseGlbContainer copied the JSON chunk into jsonText and the BIN
        // chunk into glbBinaryChunk; the raw file buffer is dead weight from
        // here on (for a .glb it holds a THIRD copy of the same bytes) and
        // used to stay resident through the whole import - file + JSON
        // chunk + BIN chunk + parsed tree all coexisted at peak. Free it
        // now (shrink so the heap block is actually returned, not just
        // marked empty).
        std::string().swap(fileData);
    } else
    {
        // The whole file IS the JSON document; hand over the buffer instead
        // of copying it.
        jsonText = std::move(fileData);
    }

    std::string parseError;
    std::unique_ptr<JsonValue> document = jsonParse(jsonText, parseError);
    if (document == nullptr)
    {
        errorReport = "asset module: '" + path + "' is not valid JSON: " + parseError;
        return 0;
    }
    // The parsed tree no longer references jsonText (every string was copied
    // into the tree), so the JSON text buffer is freed before the scene walk
    // - the import's largest transient, coexisting with the tree and (for a
    // .glb) the BIN chunk until now.
    std::string().swap(jsonText);

    if (fidelity != asset_fidelity::bounding_proxies && fidelity != asset_fidelity::full_mesh)
    {
        errorReport = "asset module: unknown import fidelity " + std::to_string(fidelity)
                    + " (0 = bounding proxies, 1 = full mesh)";
        return 0;
    }

    std::uint32_t spawned = gltfImportIntoScene(
        *document,
        *sceneCapability,
        resolved.filename().string(),
        fidelity,
        errorReport,
        isGlb && !glbBinaryChunk.empty() ? &glbBinaryChunk : nullptr
    );
    if (!errorReport.empty())
    {
        errorReport = "asset module: " + errorReport;
        return 0;
    }
    logger->logMessage(
        LogLevel::Information, "asset module: imported " + std::to_string(spawned) + " object(s) from '" + path + "'"
    );
    return spawned;
}

} // namespace engine
