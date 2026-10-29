#ifndef ENGINE_MODULES_AUDIO_HPP
#define ENGINE_MODULES_AUDIO_HPP

#include "audio_capability.hpp"
#include "logging_capability.hpp"
#include "lua_bridge_extension.hpp"
#include "engine/kernel/component_interface.hpp"
#include "engine/kernel/event_bus.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

// miniaudio: vendored, pinned single header (third_party/miniaudio). The
// implementation lives in audio.cpp (MINIAUDIO_IMPLEMENTATION); this header
// only needs the opaque struct types - the audio backend never leaks into
// any consumer of the capability.
struct ma_engine;
struct ma_resource_manager;
struct ma_sound;
struct ma_context;
struct ma_device;

namespace engine
{

// Module "audio_miniaudio": the miniaudio-backed provider (family member
// "audio.miniaudio").
//
// Two surfaces:
// 1. the AudioCapability (fine control: load, play, stop, volumes),
// 2. the event-driven path: any component may publish an
//    AudioPlaybackRequestEvent on the bus - no capability acquisition, no
//    handle bookkeeping - and the module plays the file fire-and-forget.
//    That is the SFX path for modules that just want a click when something
//    happens.
//
// Headless degradation: without a device (CI containers, servers) the engine
// initializes a NULL device - the whole pipeline runs, everything is legal,
// nothing is audible - and the module logs once. A run never fails because
// the machine has no sound hardware.
//
// The module additionally provides the "scripting.lua_bridge.audio" Lua
// bridge extension: it installs engine.play_sound, engine.set_master_volume,
// engine.play_sound_3d, engine.set_listener_position and
// engine.set_listener_orientation into every script state through a dedicated
// AudioLuaBridge object owned by this module (see audio.cpp). The host
// discovers this extension through the multi-provider acquisition of the
// "scripting.lua_bridge" family, so adding this capability to the module never
// touches the lua_scripting source.
class AudioModule final : public ModuleInterface, public AudioCapability
{
public:
    AudioModule();
    ~AudioModule() override;

    const ComponentManifest &manifest() const override
    {
        return componentManifest;
    }

    void construct(ComponentContext &context) override;
    void start() override;
    void stop() override;

    void *capabilityPointerFor(const std::string &capabilityName) override;

    // AudioCapability.
    SoundHandle loadSound(const std::string &path, bool looping) override;
    void play(SoundHandle handle) override;
    void stop(SoundHandle handle) override;
    void setVolume(SoundHandle handle, float volume) override;
    void setMasterVolume(float volume) override;
    bool isPlaying(SoundHandle handle) override;
    void unload(SoundHandle handle) override;
    void setListenerPosition(const std::array<float, 3> &position) override;
    void setListenerOrientation(const std::array<float, 3> &forward, const std::array<float, 3> &up) override;
    SoundHandle playSound3D(const std::string &path, const std::array<float, 3> &position, float volume) override;
    bool setSoundPosition(SoundHandle handle, const std::array<float, 3> &position) override;

private:
    struct LoadedSound
    {
        std::uint32_t handle = invalidSound;
        std::string resolvedPath;
        bool looping = false;
        bool loaded  = false; // the heap ma_sound below is initialized
        // Heap anchored (opaque here): the module's translation unit owns the
        // full miniaudio types; the header stays backend-free.
        struct ma_sound *sound = nullptr;
    };

    // Resolves and validates one path (relative to the executable directory,
    // the engine's path convention).
    std::filesystem::path resolvePath(const std::string &path) const;
    LoadedSound *findSound(SoundHandle handle);
    void publishDeviceUnavailable(const std::string &reason);

    ComponentManifest componentManifest;
    ComponentContext *boundContext = nullptr;
    LoggerCapability *logger       = nullptr;

    // The miniaudio engine (resource manager + mixer + device). Null when
    // initialization failed: every call then degrades to a logged no-op.
    ma_engine *audioEngine               = nullptr;
    ma_resource_manager *resourceManager = nullptr;
    // [audio].device: the module-owned enumeration context and playback
    // device when a named device was requested and opened (null = the
    // engine's default device). Freed in stop, after the engine uninit.
    ma_context *audioContext  = nullptr;
    ma_device *playbackDevice = nullptr;
    // Configuration: the requested device name (empty = default) and the
    // loading strategy (false = stream, the historical behavior).
    std::string deviceName;
    bool decodeInMemory          = false;
    bool deviceUnavailableLogged = false;
    float masterVolume           = 1.0f;

    // Listener state (the capability's spatialization): mirrored into
    // miniaudio on every call; kept here so the orientation setter can ignore
    // zero-length inputs and so diagnostics can report the last state.
    std::array<float, 3> listenerPosition{0.0f, 0.0f, 0.0f};
    std::array<float, 3> listenerForward{0.0f, 0.0f, -1.0f};
    std::array<float, 3> listenerUp{0.0f, 1.0f, 0.0f};

    std::unordered_map<std::uint32_t, LoadedSound> sounds;
    // Load-once cache key: (path, looping). A struct pair with the standard
    // hash bundle avoids the per-call "path + suffix" heap key the lookup
    // used to build (audio.hpp is module-internal; the capability surface
    // is untouched).
    struct SoundCacheKey
    {
        std::string path;
        bool looping = false;

        bool operator==(const SoundCacheKey &) const = default;
    };
    // A borrowed view of the same key: play calls hit the cache after the
    // first load (that is what load-once means), and building a real key
    // copied the path string every time just to answer "already loaded".
    // The transparent hash/equal below let find() run on the view without
    // ever materializing a std::string (C++20 heterogeneous lookup).
    struct SoundCacheKeyView
    {
        std::string_view path;
        bool looping = false;
    };
    struct SoundCacheKeyHash
    {
        using is_transparent = void;

        std::size_t operator()(
            const SoundCacheKey &key
        ) const
        {
            return hashOf(key.path, key.looping);
        }

        std::size_t operator()(
            const SoundCacheKeyView &key
        ) const
        {
            return hashOf(key.path, key.looping);
        }

    private:
        static std::size_t hashOf(
            std::string_view path, bool looping
        )
        {
            const std::size_t pathHash = std::hash<std::string_view>{}(path);
            return pathHash ^ (std::hash<bool>{}(looping) + 0x9e3779b97f4a7c15ull + (pathHash << 6) + (pathHash >> 2));
        }
    };
    struct SoundCacheKeyEqual
    {
        using is_transparent = void;

        bool operator()(
            const SoundCacheKey &left, const SoundCacheKey &right
        ) const
        {
            return left == right;
        }

        bool operator()(
            const SoundCacheKey &left, const SoundCacheKeyView &right
        ) const
        {
            return left.looping == right.looping && left.path == right.path;
        }

        bool operator()(
            const SoundCacheKeyView &left, const SoundCacheKey &right
        ) const
        {
            return left.looping == right.looping && left.path == right.path;
        }
    };
    std::unordered_map<SoundCacheKey, std::uint32_t, SoundCacheKeyHash, SoundCacheKeyEqual> soundsByPath;
    std::uint32_t nextHandle = invalidSound + 1;

    EventSubscription playbackRequestSubscription;
    // Live configuration reload (kernel reloadConfiguration): master volume
    // and mute re-apply live (one mixer call); the device key is carried for
    // the next run (a live device switch would cut every playing sound).
    EventSubscription configurationReloadedSubscription;

    // The fire-and-forget path's consumer (event channel subscription above).
    void handlePlaybackRequest(const AudioPlaybackRequestEvent &request);
    // The reload consumer (see the subscription above).
    void handleConfigurationReloaded(const ConfigurationReloadedEvent &event);
    // The Lua bridge extension object owned by this module. Constructed in
    // construct() and returned from capabilityPointerFor for the
    // "scripting.lua_bridge.audio" member. Held as the base LuaBridgeExtension
    // pointer: the concrete AudioLuaBridge type (defined in audio.cpp) stays
    // private to this module's translation unit.
    std::unique_ptr<LuaBridgeExtension> luaBridge;
};

} // namespace engine

#endif // ENGINE_MODULES_AUDIO_HPP
