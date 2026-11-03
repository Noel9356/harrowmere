#include "engine/kernel/module_plugin_cache.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <utility>

namespace engine
{

namespace
{

// One parsed line of the cache file: the key word up to the first space and
// the value (which may itself contain spaces - paths do). A line without a
// space is a key with an empty value, which is how the "record" separator
// reads.
bool splitCacheLine(
    const std::string &line, std::string &key, std::string &value
)
{
    const std::size_t split = line.find(' ');
    if (split == std::string::npos)
    {
        key   = line;
        value = std::string();
        return !key.empty();
    }
    key   = line.substr(0, split);
    value = line.substr(split + 1);
    return true;
}

// Parses a decimal integer without exceptions or locale: the cache file is
// written by this same class, so a malformed number means a hand-edited or
// corrupted file - the caller treats the record as unreadable and drops it.
bool parseDecimal(
    const std::string &text, std::uintmax_t &out
)
{
    if (text.empty())
    {
        return false;
    }
    std::uintmax_t parsed = 0;
    for (const char digit : text)
    {
        if (digit < '0' || digit > '9')
        {
            return false;
        }
        const std::uintmax_t digitValue = static_cast<std::uintmax_t>(digit - '0');
        const std::uintmax_t next       = parsed * 10 + digitValue;
        if (next < parsed)
        {
            return false; // overflow: the stamp is nonsense, the record is dropped
        }
        parsed = next;
    }
    out = parsed;
    return true;
}

} // namespace

void ModulePluginCache::load()
{
    entries.clear();
    if (cacheFile.empty())
    {
        return;
    }
    std::ifstream cacheStream(cacheFile, std::ios::in | std::ios::binary);
    if (!cacheStream.is_open())
    {
        return; // no cache yet: the first run after a clean checkout scans fully
    }

    // The format gate: a file that does not open with "format 2" (a future,
    // older or foreign layout) reads as empty - the full rescan rewrites it
    // in the current shape.
    std::string line;
    if (!std::getline(cacheStream, line) || line != "format 2")
    {
        return;
    }

    ModulePluginCacheEntry pending;
    std::string pendingPath;
    bool recordOpen = false;
    while (std::getline(cacheStream, line))
    {
        std::string key;
        std::string value;
        if (!splitCacheLine(line, key, value))
        {
            continue; // blank line: ignored between and inside records alike
        }
        if (key == "record")
        {
            // A new record begins. If the previous one was complete, keep it;
            // an incomplete one (no path, no name) is dropped silently - the
            // scan that follows re-reads that binary either way.
            if (recordOpen && !pendingPath.empty() && !pending.moduleName.empty())
            {
                entries.emplace(pendingPath, pending);
            }
            pending     = ModulePluginCacheEntry{};
            pendingPath = std::string();
            recordOpen  = true;
            continue;
        }
        if (!recordOpen)
        {
            continue; // stray key before the first record: tolerated, ignored
        }
        if (key == "size")
        {
            (void)parseDecimal(value, pending.sizeBytes);
        } else if (key == "mtime")
        {
            (void)parseDecimal(value, pending.modificationStamp);
        } else if (key == "path")
        {
            pendingPath = value;
        } else if (key == "name")
        {
            pending.moduleName = value;
        } else if (key == "provided")
        {
            if (!value.empty())
            {
                pending.providedCapabilities.push_back(value);
            }
        } else if (key == "required")
        {
            if (!value.empty())
            {
                pending.requiredCapabilities.push_back(value);
            }
        } else if (key == "optional")
        {
            if (!value.empty())
            {
                pending.optionalCapabilities.push_back(value);
            }
        } else if (key == "permission")
        {
            if (!value.empty())
            {
                pending.declaredPermissions.push_back(value);
            }
        }
        // Unknown keys are skipped: a cache written by a slightly newer build
        // still parses to the extent this build understands it, and the
        // identity check (size + stamp) keeps any misreading from mattering.
    }
    if (recordOpen && !pendingPath.empty() && !pending.moduleName.empty())
    {
        entries.emplace(pendingPath, pending);
    }
}

const ModulePluginCacheEntry *ModulePluginCache::find(
    const std::filesystem::path &libraryPath, std::uintmax_t sizeBytes, std::uintmax_t modificationStamp
) const
{
    const auto found = entries.find(libraryPath.string());
    if (found == entries.end())
    {
        return nullptr;
    }
    const ModulePluginCacheEntry &entry = found->second;
    // The identity check IS the invalidation: a rebuild changes the size
    // (almost always), the modification stamp (always, on any file system a
    // developer uses) or the path (a rename, a redeploy). Only an exact
    // match on all three remembered values is a hit.
    if (entry.sizeBytes != sizeBytes || entry.modificationStamp != modificationStamp)
    {
        return nullptr;
    }
    return &entry;
}

void ModulePluginCache::record(
    const std::filesystem::path &libraryPath, const ModulePluginCacheEntry &entry
)
{
    entries.insert_or_assign(libraryPath.string(), entry);
}

void ModulePluginCache::save() const
{
    if (cacheFile.empty() || entries.empty())
    {
        return;
    }

    // Deterministic order: the file is a deployment artifact a human may
    // diff between runs, so the records keep the sorted-path order the scan
    // used, not the hash map's iteration order.
    std::vector<const std::pair<const std::string, ModulePluginCacheEntry> *> ordered;
    ordered.reserve(entries.size());
    for (const auto &entry : entries)
    {
        ordered.push_back(&entry);
    }
    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const std::pair<const std::string, ModulePluginCacheEntry> *left,
            const std::pair<const std::string, ModulePluginCacheEntry> *right) { return left->first < right->first; }
    );

    // Write-then-rename: a reader (another engine starting concurrently with
    // a shared runtime directory) sees either the old file or the new one,
    // never a half-written record set. The rename is best-effort on every
    // platform's C++ runtime; a failure abandons the refresh silently.
    const std::filesystem::path temporaryFile = cacheFile.string() + ".tmp";
    {
        std::ofstream cacheStream(temporaryFile, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!cacheStream.is_open())
        {
            return;
        }
        cacheStream << "format 2\n";
        for (const auto *entry : ordered)
        {
            cacheStream << "record\n";
            cacheStream << "size " << entry->second.sizeBytes << "\n";
            cacheStream << "mtime " << entry->second.modificationStamp << "\n";
            cacheStream << "path " << entry->first << "\n";
            cacheStream << "name " << entry->second.moduleName << "\n";
            for (const std::string &capability : entry->second.providedCapabilities)
            {
                cacheStream << "provided " << capability << "\n";
            }
            for (const std::string &capability : entry->second.requiredCapabilities)
            {
                cacheStream << "required " << capability << "\n";
            }
            for (const std::string &capability : entry->second.optionalCapabilities)
            {
                cacheStream << "optional " << capability << "\n";
            }
            for (const std::string &permission : entry->second.declaredPermissions)
            {
                cacheStream << "permission " << permission << "\n";
            }
        }
        if (!cacheStream.good())
        {
            cacheStream.close();
            std::error_code discardError;
            (void)std::filesystem::remove(temporaryFile, discardError);
            return;
        }
    }
    std::error_code renameError;
    std::filesystem::rename(temporaryFile, cacheFile, renameError);
    if (renameError)
    {
        // The rename can fail across odd file systems (and on Windows when a
        // reader still holds the target open); remove the temp file and let
        // the next run try again - the cache is an accelerator, never a
        // requirement.
        std::error_code discardError;
        (void)std::filesystem::remove(temporaryFile, discardError);
    }
}

} // namespace engine
