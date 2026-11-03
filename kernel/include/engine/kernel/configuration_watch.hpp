#ifndef ENGINE_KERNEL_CONFIGURATION_WATCH_HPP
#define ENGINE_KERNEL_CONFIGURATION_WATCH_HPP

// Per-key callbacks on the live configuration reload, as a kernel utility a
// component instantiates for itself - no kernel state, no ABI surface, the
// same header-only discipline as AsyncTaskDispatcher.
//
// The kernel publishes one ConfigurationReloadedEvent per reload carrying the
// CHANGED dotted key paths. A component that cares currently hand-writes the
// same shape every time: subscribe in construct, then a handler that scans
// event.changedKeys for each of its own keys (the "touches" lambda pattern)
// and re-reads the ones that changed. This utility is that shape, written
// once:
//
//   ConfigurationWatchHub watches;              // a member of the component
//   watches.bind(context.eventBus());           // in construct
//   formatWatch = watches.watch("screenshot.format",
//       [this](const std::string &) { reloadFormat(); });
//
// The hub subscribes to the reload event exactly once; a watch fires its
// callback only when its key is in the changed set (a REMOVED key counts as
// changed - the re-read falls back to the component's built-in default, which
// is a behavior difference the component must see). The callback runs inside
// the event dispatch on the main thread, so it may do anything a reload
// handler may: re-read configuration, apply the value, publish follow-up
// events - and it must not throw (the same contract every live-bus handler
// already has).
//
// Cost model: nothing while no reload happens (the utility only ever runs
// inside the already opt-in reload path), and a reload costs one pass over
// the watch list - the same scan the hand-written handler was doing, minus
// the boilerplate. Keys are matched exactly, in the same "<section>.<dotted
// path>" spelling the changed set carries and the typed context accessors
// read; world content ([scene] paths) never appears in the set, so a watch on
// one never fires (the kernel reports scene edits as restart-required).
//
// RAII discipline matches EventSubscription: each watch() call returns a
// ConfigurationWatch guard that releases the watch when it dies, and the
// hub's own destruction releases the reload subscription plus every watch -
// a component holding the hub (and the guards, or not - unguarded watches
// live until the hub dies) as members cannot leave callbacks pointing at its
// destructed state.

#include "engine/kernel/component_interface.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace engine
{

class ConfigurationWatchHub;

// RAII guard for one watch, in the EventSubscription discipline: destruction
// (or release) removes the watch from its hub. A default-constructed guard
// owns nothing. The guard must not outlive the hub it came from.
class ConfigurationWatch final
{
public:
    ConfigurationWatch() = default;

    ConfigurationWatch(
        ConfigurationWatchHub *ownerHub, std::uint64_t watchId
    )
        : owner(ownerHub), watchId(watchId)
    {}

    ~ConfigurationWatch()
    {
        release();
    }

    ConfigurationWatch(const ConfigurationWatch &)            = delete;
    ConfigurationWatch &operator=(const ConfigurationWatch &) = delete;

    ConfigurationWatch(
        ConfigurationWatch &&other
    ) noexcept
        : owner(other.owner), watchId(other.watchId)
    {
        other.owner   = nullptr;
        other.watchId = invalidWatchId;
    }

    ConfigurationWatch &operator=(
        ConfigurationWatch &&other
    ) noexcept
    {
        if (this != &other)
        {
            release();
            owner         = other.owner;
            watchId       = other.watchId;
            other.owner   = nullptr;
            other.watchId = invalidWatchId;
        }
        return *this;
    }

    // Removes the watch this guard owns and empties the guard.
    void release();

    bool valid() const
    {
        return owner != nullptr && watchId != invalidWatchId;
    }

private:
    static constexpr std::uint64_t invalidWatchId = 0;

    ConfigurationWatchHub *owner = nullptr;
    std::uint64_t watchId        = invalidWatchId;
};

// One component's collection of key watches plus the single reload
// subscription that drives them. Instantiate as a member, bind in construct,
// add watches; the destructor (or an explicit reset through re-assignment)
// releases everything. Move-only, like the subscription it holds.
class ConfigurationWatchHub final
{
public:
    // The unbound form: usable as a default-constructed member that construct
    // later binds to the component's event bus.
    ConfigurationWatchHub() = default;

    // The bound form of the same. Binding subscribes the hub's one reload
    // handler; an unbound hub is inert (watches can be added but never fire).
    explicit ConfigurationWatchHub(
        EventBus &eventBus
    )
    {
        bind(eventBus);
    }

    ~ConfigurationWatchHub();

    ConfigurationWatchHub(const ConfigurationWatchHub &)            = delete;
    ConfigurationWatchHub &operator=(const ConfigurationWatchHub &) = delete;

    ConfigurationWatchHub(ConfigurationWatchHub &&other) noexcept;

    ConfigurationWatchHub &operator=(ConfigurationWatchHub &&other) noexcept;

    // Subscribes the hub's reload handler. Main thread, construct time by
    // convention. Re-binding releases the previous subscription first, so a
    // re-bind is a replace, never a double subscription.
    void bind(EventBus &eventBus);

    // Registers one key watch: callback is invoked with the changed key's
    // path (the same string the watch was registered with) at every reload
    // that names the key - changed or removed, never scene content. Returns
    // the RAII guard that owns the watch; a hub that ignores the guard keeps
    // the watch for its own lifetime (component-lifetime watches).
    [[nodiscard]] ConfigurationWatch
    watch(const std::string &dottedKeyPath, std::function<void(const std::string &)> callback);

private:
    friend class ConfigurationWatch;

    struct WatchEntry
    {
        std::uint64_t id = 0;
        std::string key;
        std::function<void(const std::string &)> callback;
    };

    void releaseWatch(std::uint64_t watchId);

    // The one reload handler behind the subscription: fires exactly the
    // watches whose key is in the changed set, in registration order.
    void handleReloaded(const ConfigurationReloadedEvent &event);

    EventBus *bus = nullptr;
    EventSubscription reloadSubscription;
    std::vector<WatchEntry> watches;
    std::uint64_t nextWatchId = 1;
};

inline ConfigurationWatchHub::~ConfigurationWatchHub()
{
    watches.clear();
    // The subscription releases itself; the destructor body exists so the
    // type stays non-trivially destructible in a readable, greppable way.
}

inline ConfigurationWatchHub::ConfigurationWatchHub(
    ConfigurationWatchHub &&other
) noexcept
    : bus(other.bus), reloadSubscription(std::move(other.reloadSubscription)), watches(std::move(other.watches)),
      nextWatchId(other.nextWatchId)
{
    other.bus         = nullptr;
    other.nextWatchId = 1;
}

inline ConfigurationWatchHub &ConfigurationWatchHub::operator=(
    ConfigurationWatchHub &&other
) noexcept
{
    if (this != &other)
    {
        watches.clear();
        reloadSubscription.release();
        bus                = other.bus;
        reloadSubscription = std::move(other.reloadSubscription);
        watches            = std::move(other.watches);
        nextWatchId        = other.nextWatchId;
        other.bus          = nullptr;
        other.nextWatchId  = 1;
    }
    return *this;
}

inline void ConfigurationWatchHub::bind(
    EventBus &eventBus
)
{
    reloadSubscription.release();
    bus                = &eventBus;
    reloadSubscription = EventSubscription(
        &eventBus,
        typeid(ConfigurationReloadedEvent),
        eventBus.channel<ConfigurationReloadedEvent>()
            .subscribe<ConfigurationWatchHub, &ConfigurationWatchHub::handleReloaded>(this)
    );
}

inline ConfigurationWatch ConfigurationWatchHub::watch(
    const std::string &dottedKeyPath, std::function<void(const std::string &)> callback
)
{
    const std::uint64_t watchIdentifier = nextWatchId++;
    WatchEntry entry;
    entry.id       = watchIdentifier;
    entry.key      = dottedKeyPath;
    entry.callback = std::move(callback);
    watches.push_back(std::move(entry));
    return ConfigurationWatch(this, watchIdentifier);
}

inline void ConfigurationWatchHub::releaseWatch(
    std::uint64_t watchId
)
{
    for (std::size_t index = 0; index < watches.size(); ++index)
    {
        if (watches[index].id == watchId)
        {
            watches.erase(watches.begin() + static_cast<std::ptrdiff_t>(index));
            return;
        }
    }
}

inline void ConfigurationWatchHub::handleReloaded(
    const ConfigurationReloadedEvent &event
)
{
    // Snapshot the watch ids, then re-resolve each one before firing: a
    // callback may release another watch (or add one) through its own
    // ConfigurationWatch, and that would erase from / grow `watches` while
    // this loop is iterating it. Re-resolving by id keeps every live watch
    // firing exactly once and drops the ones a callback retired mid-flight.
    std::vector<std::uint64_t> ids;
    ids.reserve(watches.size());
    for (const WatchEntry &entry : watches)
    {
        ids.push_back(entry.id);
    }
    for (const std::uint64_t id : ids)
    {
        const auto stillHeld
            = std::find_if(watches.begin(), watches.end(), [id](const WatchEntry &entry) { return entry.id == id; });
        if (stillHeld == watches.end())
        {
            continue; // an earlier callback released this watch
        }
        if (std::find(event.changedKeys.begin(), event.changedKeys.end(), stillHeld->key) != event.changedKeys.end())
        {
            stillHeld->callback(stillHeld->key);
        }
    }
}

inline void ConfigurationWatch::release()
{
    if (owner != nullptr && watchId != invalidWatchId)
    {
        owner->releaseWatch(watchId);
    }
    owner   = nullptr;
    watchId = invalidWatchId;
}

} // namespace engine

#endif // ENGINE_KERNEL_CONFIGURATION_WATCH_HPP
