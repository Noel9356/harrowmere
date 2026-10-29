#ifndef ENGINE_KERNEL_MODULE_PLUGIN_CACHE_HPP
#define ENGINE_KERNEL_MODULE_PLUGIN_CACHE_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine
{

// The runtime descriptor cache file's name, resolved against the executable
// directory (the same directory the modules/ deployment layout lives in).
inline constexpr const char *modulePluginCacheFileName = "module_plugins.cache";

// One plugin descriptor as the cache remembers it. The fields mirror exactly
// what reading the real descriptor (dlopen + engineQueryModulePlugin, see
// engine/kernel/module_api.hpp) produces - the cache is a REMEMBERED ANSWER,
// never a second source of truth.
struct ModulePluginCacheEntry
{
    // The identity check. A record is trusted only while the binary's size
    // and modification stamp (the raw file_time_type tick count of the last
    // write, compared within one platform and one toolchain - the cache is
    // written and read by the same engine build) both match what the
    // filesystem reports right now. Any mismatch - a rebuild, a redeploy, a
    // touched file - is a cache miss and the real descriptor is read.
    std::uintmax_t sizeBytes         = 0;
    std::uintmax_t modificationStamp = 0;

    std::string moduleName;
    std::vector<std::string> providedCapabilities;
    std::vector<std::string> requiredCapabilities;
    std::vector<std::string> optionalCapabilities;
    // The environment-scope permission requests the descriptor declares (see
    // engine/kernel/module_api.hpp, declaredPermissions). Remembered like the
    // capability lists so a cache-hit plugin is risk-assessed without loading
    // its binary.
    std::vector<std::string> declaredPermissions;
};

// The module plugin descriptor cache: startup latency for the demand-driven
// plugin scan, in the shape of a derived state file.
//
// The problem it addresses: the kernel computes the capability demand closure
// from the plugin DESCRIPTORS - one static record per shared library in the
// modules directory. Reading a descriptor means loading the binary (dlopen /
// LoadLibrary), resolving its entry point and calling it - for every plugin
// on disk, including the plugins this run will never instantiate (a plugin
// whose capabilities no active mod demands stays dormant by design). The
// descriptor is the only part of that work the demand closure actually needs;
// the load itself is pure startup latency for the dormant majority.
//
// The fix, without touching the flexibility the discovery seam guarantees:
// after a scan, remember each binary's descriptor in one text file next to
// the executable. The NEXT startup stats each binary (size + modification
// stamp, one syscall pair), reads remembered descriptors for the unchanged
// binaries and never loads them; only binaries that are new, changed or
// actually demanded touch the loader. A dropped-in module, a rebuilt module
// or a deleted module changes size or stamp and is re-read on the spot - the
// cache accelerates discovery, it never freezes it. Deleting the file is
// always safe: the next run does the full scan and writes a fresh one.
//
// Correctness contract: a cached descriptor decides which plugins become
// ACTIVE; every plugin that is actually instantiated has its library loaded
// and its real descriptor re-read at that moment (instantiateActiveModules),
// and the instance manifest is verified against what the binary really
// declares. A binary that changed underneath a same-size, same-stamp stat
// pair is the only way to slip past - a rebuilt artifact changes at least
// one of the two, which makes the residual risk the same class as the
// toolchain's own incremental-build staleness, not a new one.
//
// The file format is line-based plain text, one record per plugin:
//
//   format 2
//   record
//   size 184320
//   mtime 63874182123456789
//   path /runtime/modules/libengine_module_logging.so
//   name logging
//   provided logging
//   required
//   optional
//   permission filesystem.write
//   record
//   ...
//
// "path" and every capability/permission value extend to the end of the line
// (paths may contain spaces); "size" and "mtime" are decimal integers.
// Format 2 added the "permission" lines (the ABI 7 descriptor's
// declaredPermissions); a format 1 file reads as empty and triggers the safe
// full rescan. Unknown keys and malformed records end the scan of the file -
// the cache then reads as empty, which is the safe direction: everything is
// re-read from the binaries and rewritten.
class ModulePluginCache final
{
public:
    ModulePluginCache() = default;

    ModulePluginCache(const ModulePluginCache &)            = delete;
    ModulePluginCache &operator=(const ModulePluginCache &) = delete;

    // Where the cache file lives. Set once before load/save.
    void setCacheFile(
        const std::filesystem::path &cacheFilePath
    )
    {
        cacheFile = cacheFilePath;
    }

    // Reads the cache file. Any failure (missing file, unreadable, malformed)
    // leaves the cache empty - a full descriptor rescan is the recovery, so
    // no failure here is ever reported to the user.
    void load();

    // The remembered descriptor for one library path, or nullptr when the
    // path is unknown or its size / modification stamp no longer match. The
    // pointer stays valid until the next record() call.
    const ModulePluginCacheEntry *
    find(const std::filesystem::path &libraryPath, std::uintmax_t sizeBytes, std::uintmax_t modificationStamp) const;

    // Adds or replaces one record. Call for every descriptor read from a
    // binary during the scan; the save() below persists the merged set so a
    // partial rescan (one changed plugin) rewrites only what it learned.
    void record(const std::filesystem::path &libraryPath, const ModulePluginCacheEntry &entry);

    // Writes the merged record set back. Best-effort by contract: a read-only
    // deployment or a full disk loses the acceleration, never correctness -
    // the next scan re-reads everything and tries again. A write lands
    // atomically enough for the purpose: the content is written to a
    // temporary file in the same directory and renamed over the target.
    void save() const;

private:
    std::filesystem::path cacheFile;
    std::unordered_map<std::string, ModulePluginCacheEntry> entries;
};

} // namespace engine

#endif // ENGINE_KERNEL_MODULE_PLUGIN_CACHE_HPP
