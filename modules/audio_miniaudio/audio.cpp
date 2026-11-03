#include "audio.hpp"

// The miniaudio implementation, in this translation unit exactly once
// (module_plugin.cpp includes the header only, which carries declarations).
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "engine/kernel/filesystem_utilities.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sol/sol.hpp>

namespace engine
{

namespace
{

// Volume clamp for every entry that carries a volume. NaN poisons the mixer
// (std::max(NaN, 0.0f) is NaN - the comparison is always false), and any
// runaway gain above 4x just clips downstream, so non-finite resets to the
// neutral 1.0 and everything else lands in [0, 4].
float clampVolume(
    float volume
)
{
    return std::isfinite(volume) ? std::clamp(volume, 0.0f, 4.0f) : 1.0f;
}

// The dedicated Lua bridge extension object for the audio module. Owns the
// sol2 wiring that installs engine.play_sound, engine.set_master_volume,
// engine.play_sound_3d, engine.set_listener_position and
// engine.set_listener_orientation into every script state. Holds a raw
// pointer to the AudioCapability the module provides.
class AudioLuaBridge final : public LuaBridgeExtension
{
public:
    explicit AudioLuaBridge(
        AudioCapability *capability
    )
        : capability(capability)
    {}

    const char *extensionName() const override
    {
        return "audio";
    }

    void installLuaBridge(
        luaState *lua
    ) override
    {
        // The host (lua_scripting) guarantees the engine table exists when
        // this is called. The bridge functions call the AudioCapability
        // methods directly through the captured pointer - no capability
        // acquisition, no permission check.
        //
        // play_sound / play_sound_3d use loadSound (which caches by path
        // inside the audio module already - the historical soundHandlesByPath
        // cache that lived in lua_scripting was duplicating this module's
        // own cache).
        sol::state_view luaView(lua);
        sol::table engine = luaView["engine"];

        engine.set_function("play_sound", [this](const std::string &path, sol::object volume) {
            const AudioCapability::SoundHandle handle = capability->loadSound(path, false);
            if (handle == AudioCapability::invalidSound)
            {
                return;
            }
            capability->setVolume(handle, static_cast<float>(volume.valid() ? volume.as<double>() : 1.0));
            capability->play(handle);
        });
        engine.set_function("set_master_volume", [this](double volume) {
            capability->setMasterVolume(static_cast<float>(volume));
        });
        engine.set_function(
            "play_sound_3d", [this](const std::string &path, float x, float y, float z, sol::object volume) {
                capability->playSound3D(
                    path, {x, y, z}, static_cast<float>(volume.valid() ? volume.as<double>() : 1.0)
                );
            }
        );
        engine.set_function("set_listener_position", [this](float x, float y, float z) {
            capability->setListenerPosition({x, y, z});
        });
        engine.set_function(
            "set_listener_orientation", [this](float fx, float fy, float fz, float ux, float uy, float uz) {
                capability->setListenerOrientation({fx, fy, fz}, {ux, uy, uz});
            }
        );
    }

private:
    AudioCapability *capability;
};

} // namespace

AudioModule::AudioModule()
{
    componentManifest = ComponentManifest{
        .name    = "audio",
        .kind    = ComponentKind::Module,
        .version = "0.1.0",
        .providedCapabilities
        = {"audio.miniaudio",
            // The Lua bridge extension member: declares this module as a
            // provider of the "scripting.lua_bridge" family. The
            // lua_scripting host acquires every active member through
            // ComponentContext::acquireEachCapability and asks each
            // one to install its own bindings into the host's engine
            // table.
            "scripting.lua_bridge.audio"},
        // Reading content files is the module's whole job; no other capability
        // dependency - audio works in any assembly, renderer or not.
        .requiredCapabilities = {"logging"},
        .optionalCapabilities = {},
        .requiredComponents   = {},
        .requiredPermissions  = {"capability.logging", "filesystem.read"},
        .knownConfigKeys      = {
            "audio.master_volume",
            "audio.start_muted",
            "audio.device",
            "audio.decode_in_memory",
        },
    };
}

AudioModule::~AudioModule()
{
    stop();
}

void AudioModule::construct(
    ComponentContext &context
)
{
    boundContext = &context;
    logger       = &context.acquireCapability<LoggerCapability>();

    masterVolume = clampVolume(static_cast<float>(context.configurationFloating("master_volume").value_or(1.0)));
    if (context.configurationBoolean("start_muted").value_or(false))
    {
        masterVolume = 0.0f;
    }
    // Output device: empty or "default" (the default) lets miniaudio pick
    // the system default; any other value is matched case-insensitively as a
    // SUBSTRING against the playback devices' names, so "headset" finds
    // "USB-C Headset (driver)" without knowing the full name. A name that
    // matches nothing falls back to the default device with a warning -
    // a machine without the configured device must still start.
    deviceName = context.configurationString("device").value_or(std::string());
    // Loading strategy: false (the default) streams every sound from disk
    // (the historical behavior - unbounded scene audio at a small per-read
    // cost); true decodes each sound fully into memory at load, which trades
    // RAM for zero stream IO on short, frequently-retriggered SFX.
    decodeInMemory = context.configurationBoolean("decode_in_memory").value_or(false);

    // The fire-and-forget path: one subscription, any component can request.
    EventBus &bus                                           = context.eventBus();
    EventChannel<AudioPlaybackRequestEvent> &requestChannel = bus.channel<AudioPlaybackRequestEvent>();
    playbackRequestSubscription                             = EventSubscription(
        &bus,
        typeid(AudioPlaybackRequestEvent),
        requestChannel.subscribe<AudioModule, &AudioModule::handlePlaybackRequest>(this)
    );

    // Live configuration reload (kernel reloadConfiguration): re-applies the
    // master volume and the mute flag when the event names them - one mixer
    // call, no device change. The [audio].device key is deliberately NOT
    // reloadable: switching devices means re-initializing the backend, which
    // would cut every playing sound; it keeps the startup device.
    EventChannel<ConfigurationReloadedEvent> &reloadedChannel = bus.channel<ConfigurationReloadedEvent>();
    configurationReloadedSubscription                         = EventSubscription(
        &bus,
        typeid(ConfigurationReloadedEvent),
        reloadedChannel.subscribe<AudioModule, &AudioModule::handleConfigurationReloaded>(this)
    );

    // Construct the Lua bridge extension object. The module IS the
    // AudioCapability, so `this` is the capability pointer the bridge calls
    // back into.
    luaBridge = std::make_unique<AudioLuaBridge>(this);
}

void AudioModule::start()
{
    // Resource manager first (decoders, file streaming), then the engine
    // (mixer + device). ma_engine_init builds a context over every available
    // backend - including the NULL backend, so a success here means real OR
    // silent output, both fully functional API-wise. Failure means even the
    // NULL backend could not open: the degraded mode below keeps every call
    // legal (ghost handles, precise no-ops) so a headless CI run plays
    // silence instead of failing startup.
    ma_resource_manager_config resourceConfig = ma_resource_manager_config_init();
    resourceManager                           = new ma_resource_manager();
    if (ma_resource_manager_init(&resourceConfig, resourceManager) != MA_SUCCESS)
    {
        delete resourceManager;
        resourceManager = nullptr;
        publishDeviceUnavailable("miniaudio resource manager failed to initialize");
        logger->logMessage(
            LogLevel::Warning, "audio module started DEGRADED (no decoder pipeline; playback is a no-op)"
        );
        return;
    }

    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.pResourceManager = resourceManager;

    // Explicit output device ([audio].device): enumerate the playback
    // devices through a module-owned context, match the configured name as
    // a case-insensitive substring, and hand the engine a playback device
    // bound to that id. The context and device stay alive for the engine's
    // lifetime (freed in stop, after the engine uninit). No match (or a
    // failure anywhere in this path) logs a warning and falls back to the
    // default device - a named device is a preference, never a startup
    // condition.
    if (!deviceName.empty() && deviceName != "default")
    {
        audioContext = new ma_context();
        if (ma_context_init(nullptr, 0, nullptr, audioContext) == MA_SUCCESS)
        {
            ma_device_info *playbackDevices = nullptr;
            ma_uint32 playbackCount         = 0;
            if (ma_context_get_devices(audioContext, &playbackDevices, &playbackCount, nullptr, nullptr) == MA_SUCCESS
                && playbackCount > 0)
            {
                std::string wanted = deviceName;
                std::transform(wanted.begin(), wanted.end(), wanted.begin(), [](unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
                ma_uint32 matched = playbackCount;
                for (ma_uint32 index = 0; index < playbackCount; ++index)
                {
                    std::string candidate(playbackDevices[index].name);
                    std::transform(candidate.begin(), candidate.end(), candidate.begin(), [](unsigned char character) {
                        return static_cast<char>(std::tolower(character));
                    });
                    if (candidate.find(wanted) != std::string::npos)
                    {
                        matched = index;
                        break;
                    }
                }
                if (matched < playbackCount)
                {
                    playbackDevice                  = new ma_device();
                    ma_device_config deviceConfig   = ma_device_config_init(ma_device_type_playback);
                    ma_device_id deviceId           = playbackDevices[matched].id;
                    deviceConfig.playback.pDeviceID = &deviceId;
                    if (ma_device_init(audioContext, &deviceConfig, playbackDevice) == MA_SUCCESS)
                    {
                        engineConfig.pDevice = playbackDevice;
                        logger->logMessage(
                            LogLevel::Information,
                            "audio module: using output device '" + std::string(playbackDevices[matched].name) + "'"
                        );
                    } else
                    {
                        delete playbackDevice;
                        playbackDevice = nullptr;
                        logger->logMessage(
                            LogLevel::Warning, "audio module: configured device failed to open; using the default"
                        );
                    }
                } else
                {
                    logger->logMessage(
                        LogLevel::Warning,
                        "audio module: no playback device matches '" + deviceName + "'; using the default"
                    );
                }
            } else
            {
                logger->logMessage(
                    LogLevel::Warning, "audio module: device enumeration failed; using the default device"
                );
            }
        } else
        {
            delete audioContext;
            audioContext = nullptr;
            logger->logMessage(
                LogLevel::Warning, "audio module: device enumeration context failed; using the default device"
            );
        }
    }

    audioEngine            = new ma_engine();
    const ma_result result = ma_engine_init(&engineConfig, audioEngine);
    if (result != MA_SUCCESS)
    {
        delete audioEngine;
        audioEngine = nullptr;
        publishDeviceUnavailable(
            "miniaudio engine failed to initialize (error " + std::to_string(static_cast<int>(result)) + ")"
        );
        logger->logMessage(LogLevel::Warning, "audio module started DEGRADED (no device; playback is a no-op)");
        return;
    }
    ma_engine_set_volume(audioEngine, masterVolume);

    logger->logMessage(LogLevel::Information, "audio module started");
}

void AudioModule::stop()
{
    configurationReloadedSubscription.release();
    playbackRequestSubscription.release();
    for (auto &entry : sounds)
    {
        if (entry.second.loaded && audioEngine != nullptr)
        {
            ma_sound_uninit(entry.second.sound);
        }
        delete entry.second.sound;
        entry.second.sound = nullptr;
    }
    sounds.clear();
    soundsByPath.clear();
    if (audioEngine != nullptr)
    {
        ma_engine_uninit(audioEngine);
        delete audioEngine;
        audioEngine = nullptr;
    }
    // The engine released its claim on the device first: only now may the
    // device and the enumeration context die (reverse construction order).
    if (playbackDevice != nullptr)
    {
        ma_device_uninit(playbackDevice);
        delete playbackDevice;
        playbackDevice = nullptr;
    }
    if (audioContext != nullptr)
    {
        ma_context_uninit(audioContext);
        delete audioContext;
        audioContext = nullptr;
    }
    if (resourceManager != nullptr)
    {
        ma_resource_manager_uninit(resourceManager);
        delete resourceManager;
        resourceManager = nullptr;
    }
    // Reset the Lua bridge before nulling the capability pointers it captures.
    luaBridge.reset();
    boundContext = nullptr;
    logger       = nullptr;
}

void *AudioModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    if (capabilityName == "audio.miniaudio")
    {
        return static_cast<AudioCapability *>(this);
    }
    // The "scripting.lua_bridge.audio" member: returns the dedicated
    // AudioLuaBridge object this module owns (constructed in construct()).
    if (capabilityName == "scripting.lua_bridge.audio")
    {
        return luaBridge.get();
    }
    return nullptr;
}

std::filesystem::path AudioModule::resolvePath(
    const std::string &path
) const
{
    const std::filesystem::path candidate(path);
    return candidate.is_relative() ? (executableDirectory() / candidate) : candidate;
}

AudioModule::LoadedSound *AudioModule::findSound(
    SoundHandle handle
)
{
    const auto found = sounds.find(handle);
    return found == sounds.end() ? nullptr : &found->second;
}

void AudioModule::publishDeviceUnavailable(
    const std::string &reason
)
{
    if (deviceUnavailableLogged)
    {
        return;
    }
    deviceUnavailableLogged = true;
    if (boundContext != nullptr)
    {
        boundContext->eventBus().publish(AudioDeviceUnavailableEvent{.reason = reason});
    }
}

AudioCapability::SoundHandle AudioModule::loadSound(
    const std::string &path, bool looping
)
{
    // Same path + same looping = the same handle (load-once semantics); a
    // different looping mode is a different sound by contract (the flag is
    // fixed at load). The lookup runs on a borrowed view (transparent hash,
    // see audio.hpp): a play call for an already-loaded sound - the common
    // case after the first - hashes the path WITHOUT copying it into a key
    // first.
    const SoundCacheKeyView cacheKeyView{path, looping};
    const auto cached = soundsByPath.find(cacheKeyView);
    if (cached != soundsByPath.end())
    {
        return cached->second;
    }
    if (audioEngine == nullptr)
    {
        // Degraded run: hand out a stable handle so scripts keep working,
        // the sound simply never plays.
        const std::uint32_t ghostHandle = nextHandle++;
        soundsByPath.emplace(SoundCacheKey{path, looping}, ghostHandle);
        LoadedSound ghost;
        ghost.handle       = ghostHandle;
        ghost.resolvedPath = path;
        ghost.looping      = looping;
        ghost.loaded       = false;
        sounds.emplace(ghostHandle, std::move(ghost));
        return ghostHandle;
    }
    const std::filesystem::path resolved = resolvePath(path);
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(resolved, fileError))
    {
        logger->logMessage(
            LogLevel::Error,
            "audio module: '" + path + "' does not name a readable file (resolved '" + resolved.string() + "')"
        );
        return invalidSound;
    }

    LoadedSound sound;
    sound.handle       = nextHandle++;
    sound.resolvedPath = resolved.string();
    sound.looping      = looping;
    sound.sound        = new ma_sound();
    // Streaming (the default) keeps RAM flat for long files; the configured
    // in-memory decode (MA_SOUND_FLAG_DECODE) pays the full decode at load
    // and stops reading the file during playback - the short-SFX trade.
    const ma_uint32 loadFlags = decodeInMemory ? MA_SOUND_FLAG_DECODE : MA_SOUND_FLAG_STREAM;
    const ma_result result
        = ma_sound_init_from_file(audioEngine, sound.resolvedPath.c_str(), loadFlags, nullptr, nullptr, sound.sound);
    if (result != MA_SUCCESS)
    {
        delete sound.sound;
        logger->logMessage(
            LogLevel::Error,
            "audio module: failed to decode '" + path + "' (miniaudio error " + std::to_string(static_cast<int>(result))
                + "; WAV/MP3/FLAC/OGG are the supported container set)"
        );
        return invalidSound;
    }
    sound.loaded = true;
    ma_sound_set_looping(sound.sound, sound.looping ? MA_TRUE : MA_FALSE);
    const std::uint32_t handle = sound.handle;
    sounds.emplace(handle, std::move(sound));
    soundsByPath.emplace(SoundCacheKey{path, looping}, handle);
    return handle;
}

void AudioModule::play(
    SoundHandle handle
)
{
    LoadedSound *sound = findSound(handle);
    if (sound == nullptr || !sound->loaded || audioEngine == nullptr)
    {
        return; // degraded or unknown: precise no-op
    }
    ma_sound_seek_to_pcm_frame(sound->sound, 0);
    ma_sound_start(sound->sound);
}

void AudioModule::stop(
    SoundHandle handle
)
{
    LoadedSound *sound = findSound(handle);
    if (sound == nullptr || !sound->loaded || audioEngine == nullptr)
    {
        return;
    }
    ma_sound_stop(sound->sound);
}

void AudioModule::setVolume(
    SoundHandle handle, float volume
)
{
    LoadedSound *sound = findSound(handle);
    if (sound == nullptr || !sound->loaded || audioEngine == nullptr)
    {
        return;
    }
    ma_sound_set_volume(sound->sound, clampVolume(volume));
}

void AudioModule::setMasterVolume(
    float volume
)
{
    masterVolume = clampVolume(volume);
    if (audioEngine != nullptr)
    {
        ma_engine_set_volume(audioEngine, masterVolume);
    }
}

bool AudioModule::isPlaying(
    SoundHandle handle
)
{
    LoadedSound *sound = findSound(handle);
    if (sound == nullptr || !sound->loaded || audioEngine == nullptr)
    {
        return false;
    }
    return ma_sound_is_playing(sound->sound) == MA_TRUE;
}

void AudioModule::unload(
    SoundHandle handle
)
{
    LoadedSound *sound = findSound(handle);
    if (sound == nullptr)
    {
        return;
    }
    if (sound->loaded && audioEngine != nullptr)
    {
        ma_sound_stop(sound->sound);
        ma_sound_uninit(sound->sound);
    }
    for (auto iterator = soundsByPath.begin(); iterator != soundsByPath.end(); ++iterator)
    {
        if (iterator->second == handle)
        {
            soundsByPath.erase(iterator);
            break;
        }
    }
    sounds.erase(handle);
}

void AudioModule::handlePlaybackRequest(
    const AudioPlaybackRequestEvent &request
)
{
    const SoundHandle handle = loadSound(request.path, request.looping);
    if (handle == invalidSound)
    {
        return;
    }
    setVolume(handle, request.volume);
    play(handle);
}

void AudioModule::handleConfigurationReloaded(
    const ConfigurationReloadedEvent &event
)
{
    if (boundContext == nullptr)
    {
        return;
    }
    const auto touches = [&event](const char *key) {
        return std::find(event.changedKeys.begin(), event.changedKeys.end(), std::string("audio.") + key)
            != event.changedKeys.end();
    };
    // Volume is a mixer knob: re-read and re-apply, exactly what the
    // capability's setMasterVolume does. With no engine (degraded headless
    // mode) the member still updates so the next start carries the value.
    if (touches("master_volume") || touches("start_muted"))
    {
        masterVolume
            = clampVolume(static_cast<float>(boundContext->configurationFloating("master_volume").value_or(1.0)));
        if (boundContext->configurationBoolean("start_muted").value_or(false))
        {
            masterVolume = 0.0f;
        }
        if (audioEngine != nullptr)
        {
            ma_engine_set_volume(audioEngine, masterVolume);
        }
        logger->logMessage(LogLevel::Information, "audio module: reloaded master volume");
    }
    if (touches("device"))
    {
        // Deliberately not applied live: switching devices re-initializes
        // the backend and cuts every playing sound. The reload carries the
        // value for the NEXT run's start (the member is what construct
        // consumes), and the log says so.
        deviceName = boundContext->configurationString("device").value_or(std::string());
        logger->logMessage(
            LogLevel::Warning,
            "audio module: [audio].device changed to '" + (deviceName.empty() ? std::string("default") : deviceName)
                + "'; the new device takes effect at the next run"
        );
    }
}

void AudioModule::setListenerPosition(
    const std::array<float, 3> &position
)
{
    // Same finite screen as setListenerOrientation: a non-finite coordinate
    // would misplace the listener permanently (and NaN pans every 3D sound
    // to garbage); ignore and keep the previous position.
    if (!std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2]))
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning, "audio module: setListenerPosition ignored (position must be finite)"
            );
        }
        return;
    }
    listenerPosition = position;
    if (audioEngine != nullptr)
    {
        ma_engine_listener_set_position(audioEngine, 0, position[0], position[1], position[2]);
    }
}

void AudioModule::setListenerOrientation(
    const std::array<float, 3> &forward, const std::array<float, 3> &up
)
{
    // Zero-length inputs are ignored (the previous orientation stays): a
    // caller that degenerates mid-flight must not flip the world's panning.
    const float forwardLength = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    const float upLength      = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    if (forwardLength > 1e-6f)
    {
        listenerForward = {forward[0] / forwardLength, forward[1] / forwardLength, forward[2] / forwardLength};
    }
    if (upLength > 1e-6f)
    {
        listenerUp = {up[0] / upLength, up[1] / upLength, up[2] / upLength};
    }
    if (audioEngine != nullptr)
    {
        // miniaudio splits the orientation into a forward direction plus a
        // separate world-up vector (ma_engine_listener_set_direction /
        // _set_world_up) - the same two numbers the capability carries.
        ma_engine_listener_set_direction(audioEngine, 0, listenerForward[0], listenerForward[1], listenerForward[2]);
        ma_engine_listener_set_world_up(audioEngine, 0, listenerUp[0], listenerUp[1], listenerUp[2]);
    }
}

AudioCapability::SoundHandle AudioModule::playSound3D(
    const std::string &path, const std::array<float, 3> &position, float volume
)
{
    // The provider's load-once model means one concurrent instance per path:
    // a second one-shot of the same file RE-starts the first (documented).
    const SoundHandle handle = loadSound(path, false);
    if (handle == invalidSound)
    {
        return invalidSound;
    }
    setSoundPosition(handle, position);
    setVolume(handle, volume);
    play(handle);
    return handle;
}

bool AudioModule::setSoundPosition(
    SoundHandle handle, const std::array<float, 3> &position
)
{
    LoadedSound *sound = findSound(handle);
    if (sound == nullptr || !sound->loaded || audioEngine == nullptr)
    {
        return false; // degraded or unknown: precise no-op
    }
    ma_sound_set_position(sound->sound, position[0], position[1], position[2]);
    return true;
}

} // namespace engine
