#include "localization.hpp"

#include <toml.hpp>

#include <sol/sol.hpp>

#include <filesystem>
#include <set>
#include <sstream>
#include <utility>

#include "engine/kernel/filesystem_utilities.hpp"

namespace engine
{

namespace
{

std::filesystem::path resolveAgainstExecutable(
    const std::string &path
)
{
    const std::filesystem::path candidate(path);
    return candidate.is_relative() ? (executableDirectory() / candidate) : candidate;
}

// The missing-key warning dedup set is keyed by script-controlled
// localize() arguments; past this cap new keys stop logging (the returned
// translation is unchanged - still the key), so a script cannot grow the
// set without bound.
constexpr std::size_t missingWarnedCap = 10000;

// Walks one parsed TOML table into (key, text) rows. A bare string row
// lands under its key; a nested TABLE recurses with the dotted prefix, so
// translations may use whatever TOML idiom reads best - top-level
// "menu.start = ..." rows (quoted keys, ONE flat key with a literal dot) or
// [menu] start = "..." tables (nested, the same dotted key after the walk).
// Non-string rows are skipped with one precise log: a language file is
// text by definition, and a mistyped row must not fail the whole table.
void collectTexts(
    const toml::table &table,
    const std::string &prefix,
    std::unordered_map<std::string, std::string> &rows,
    LoggerCapability *logger,
    const std::string &sourceName
)
{
    for (const auto &entry : table)
    {
        const toml::key &key   = entry.first;
        const toml::node &node = entry.second;
        const std::string name = prefix + std::string(key.str());

        if (node.is_table())
        {
            collectTexts(*node.as_table(), name + ".", rows, logger, sourceName);
            continue;
        }

        if (node.is_string())
        {
            rows[name] = node.as_string()->get();
            continue;
        }

        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning,
                "localization module: '" + sourceName + "' row '" + name + "' is not a string; row skipped"
            );
        }
    }
}

// The dedicated Lua bridge extension object for the localization module.
// Owns the sol2 wiring that installs engine.localize, engine.set_language,
// engine.active_language and engine.loaded_languages into every script
// state. Holds raw pointers to the LocalizationCapability the module
// provides and the LoggerCapability for load/switch error reports.
class LocalizationLuaBridge final : public LuaBridgeExtension
{
public:
    LocalizationLuaBridge(
        LocalizationCapability *capability, LoggerCapability *logger
    )
        : capability(capability), logger(logger)
    {}

    const char *extensionName() const override
    {
        return "localization";
    }

    void installLuaBridge(
        luaState *lua
    ) override
    {
        // The host (lua_scripting) guarantees the engine table exists when
        // this is called. The bridge functions call the LocalizationCapability
        // methods directly through the captured pointer - no capability
        // acquisition, no permission check (the extension is part of the
        // module that provides the capability).
        //
        // engine::luaState IS the Lua C API's ::lua_State (the alias in
        // lua_bridge_extension.hpp), so the state_view wraps the pointer
        // directly - no cast, exactly like every other bridge extension.
        sol::state_view luaView(lua);
        sol::table engine = luaView["engine"];

        engine.set_function("localize", [this](const std::string &key) -> std::string {
            return capability->translate(key);
        });
        engine.set_function("set_language", [this](const std::string &code) -> bool {
            std::string errorReport;
            const bool switched = capability->setLanguage(code, errorReport);
            if (!errorReport.empty() && logger != nullptr)
            {
                logger->logMessage(LogLevel::Error, errorReport);
            }
            return switched;
        });
        engine.set_function("active_language", [this]() -> std::string { return capability->activeLanguage(); });
        engine.set_function("loaded_languages", [this](sol::this_state state) -> sol::table {
            // One Lua array of the loaded language codes, in the sorted
            // order the capability reports - a script's language picker
            // iterates it directly.
            sol::state_view luaView(state);
            sol::table codes                      = luaView.create_table();
            const std::vector<std::string> loaded = capability->loadedLanguages();
            std::uint64_t index                   = 1;
            for (const std::string &code : loaded)
            {
                codes[index] = code;
                index += 1;
            }
            return codes;
        });
    }

private:
    LocalizationCapability *capability;
    LoggerCapability *logger;
};

} // namespace

LocalizationModule::LocalizationModule()
{
    componentManifest.name = "localization";
    componentManifest.kind = ComponentKind::Module;
    // The implementation-bound member of the "localization" family: a future
    // format (binary tables, a remote source) declares its own member and
    // the [providers] configuration picks between them.
    componentManifest.providedCapabilities
        = {"localization.toml",
            // The Lua bridge extension member: declares this module as a
            // provider of the "scripting.lua_bridge" family. The
            // lua_scripting host acquires every active member through
            // ComponentContext::acquireEachCapability and asks each
            // one to install its own bindings into the host's engine
            // table.
            "scripting.lua_bridge.localization"};
    componentManifest.requiredCapabilities = {"logging"};
    // Reading the language files is the module's whole file surface
    // (filesystem.read); it never writes. Deniable in configuration like
    // every grant - a run that denies the read gets a lookup that degrades
    // to keys and reports the reason once.
    componentManifest.requiredPermissions = {"capability.logging", "filesystem.read"};
    componentManifest.knownConfigKeys = {"localization.directory", "localization.language", "localization.fallback"};
}

void LocalizationModule::construct(
    ComponentContext &context
)
{
    // The one cross-component touch of the construct phase: the log sink.
    logger = &context.acquireCapability<LoggerCapability>();

    directory = context.configurationString("directory").value_or("languages");
    fallback  = context.configurationString("fallback").value_or("en");
    // The active language is intentionally NOT set here: start() activates
    // the configured one, and a script can switch any time after.
    configuredLanguage = context.configurationString("language").value_or("en");

    // Construct the Lua bridge extension object. The module IS the
    // LocalizationCapability, so `this` is the capability pointer.
    luaBridge = std::make_unique<LocalizationLuaBridge>(this, logger);
}

void LocalizationModule::start()
{
    if (logger == nullptr)
    {
        return;
    }

    // Scan the configured directory and load every *.toml file as a
    // language: the file stem IS the code (en.toml loads as "en"). A
    // missing directory is a configuration or packaging gap - the lookup
    // degrades to keys, and one warning says exactly where the tables were
    // expected. Loading every file present makes loadedLanguages() the
    // honest language picker a settings screen iterates.
    const std::filesystem::path resolvedDirectory = resolveAgainstExecutable(directory);
    std::error_code scanError;
    if (!std::filesystem::is_directory(resolvedDirectory, scanError))
    {
        logger->logMessage(
            LogLevel::Warning,
            "localization module: language directory '" + directory + "' does not exist (resolved '"
                + resolvedDirectory.string() + "'); lookups degrade to keys"
        );
    } else
    {
        // The error_code forms (constructor + increment): a directory that
        // changes under the scan, or a permission hiccup, must not throw
        // out of start() mid-scan. An iteration error breaks the loop
        // cleanly with one warning - whatever languages loaded stay usable.
        std::error_code iterationError;
        std::filesystem::directory_iterator iterator(resolvedDirectory, iterationError);
        if (iterationError)
        {
            logger->logMessage(
                LogLevel::Warning,
                "localization module: language directory '" + directory + "' cannot be scanned ("
                    + iterationError.message() + "); lookups degrade to keys"
            );
        } else
        {
            for (std::filesystem::directory_iterator end; iterator != end; iterator.increment(iterationError))
            {
                if (iterationError)
                {
                    logger->logMessage(
                        LogLevel::Warning,
                        "localization module: language scan aborted (" + iterationError.message()
                            + "); the languages loaded so far stay usable"
                    );
                    break;
                }
                const std::filesystem::directory_entry &entry = *iterator;
                std::error_code entryError;
                const bool regular = entry.is_regular_file(entryError);
                if (entryError || !regular || entry.path().extension() != ".toml")
                {
                    continue;
                }
                std::string errorReport;
                const std::string code = entry.path().stem().string();
                if (!loadLanguage(code, errorReport))
                {
                    logger->logMessage(LogLevel::Warning, errorReport);
                }
            }
        }
    }

    // Activate the configured language (already loaded by the scan when its
    // file was there; the loadLanguage inside setLanguage retries with a
    // precise message when it was not). The fallback language is loaded by
    // the same scan; a fallback whose file is missing is only a warning -
    // the lookup chain simply skips it.
    std::string errorReport;
    if (!setLanguage(configuredLanguage, errorReport))
    {
        logger->logMessage(LogLevel::Warning, errorReport);
    }
}

void LocalizationModule::stop()
{
    // Reset the Lua bridge before nulling the capability pointers it captures.
    luaBridge.reset();
    languages.clear();
    active.clear();
    // The row caches pointed into `languages`; the maps are gone now.
    activeRows   = nullptr;
    fallbackRows = nullptr;
    logger       = nullptr;
}

void *LocalizationModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    if (capabilityName == "localization.toml")
    {
        return static_cast<LocalizationCapability *>(this);
    }
    // The "scripting.lua_bridge.localization" member: returns the dedicated
    // LocalizationLuaBridge object this module owns (constructed in
    // construct()).
    if (capabilityName == "scripting.lua_bridge.localization")
    {
        return luaBridge.get();
    }
    return nullptr;
}

bool LocalizationModule::parseLanguageFile(
    const std::string &code, std::unordered_map<std::string, std::string> &rows, std::string &errorReport
)
{
    // Code hygiene first: the code names a file (directory injection guard)
    // and a capability-facing identifier - letters, digits, underscore,
    // hyphen, exactly the charset mod ids keep to.
    if (code.empty()
        || code.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")
               != std::string::npos)
    {
        errorReport = "localization module: '" + code + "' is not a valid language code"
                    + " (letters, digits, '_' and '-' only)";
        return false;
    }

    const std::filesystem::path resolved = resolveAgainstExecutable(directory) / (code + ".toml");
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(resolved, fileError))
    {
        errorReport = "localization module: language '" + code + "' has no file (expected '" + resolved.string() + "')";
        return false;
    }

    toml::parse_result parsed;
    try
    {
        parsed = toml::parse_file(resolved.string());
    } catch (const toml::parse_error &error)
    {
        std::ostringstream description;
        description << "localization module: '" << code << ".toml" << "' is not valid TOML: " << error.description()
                    << " (line " << error.source().begin.line << ")";
        errorReport = description.str();
        return false;
    }

    // Parse into a fresh map, then swap in: a re-load replaces the
    // language's rows wholesale (a hot-reloaded translation), and a parse
    // that failed above never touched the previous rows.
    std::unordered_map<std::string, std::string> fresh;
    collectTexts(parsed, std::string(), fresh, logger, code + ".toml");
    rows = std::move(fresh);
    return true;
}

bool LocalizationModule::loadLanguage(
    const std::string &code, std::string &errorReport
)
{
    std::unordered_map<std::string, std::string> rows;
    if (!parseLanguageFile(code, rows, errorReport))
    {
        return false;
    }
    languages[code] = std::move(rows);
    // A newly loaded language can change what the fallback resolves to (and
    // never invalidates a cached pointer: the operator[] re-assigns the
    // existing inner map in place).
    refreshRowCache();
    return true;
}

bool LocalizationModule::setLanguage(
    const std::string &code, std::string &errorReport
)
{
    // A not-yet-loaded language loads first; the previously active language
    // stays active when that load fails (a failed switch must never leave
    // the UI untranslated).
    if (languages.find(code) == languages.end() && !loadLanguage(code, errorReport))
    {
        return false;
    }
    active = code;
    refreshRowCache();
    if (logger != nullptr)
    {
        logger->logMessage(LogLevel::Information, "localization module: active language is '" + code + "'");
    }
    return true;
}

void LocalizationModule::refreshRowCache()
{
    const auto foundActive   = languages.find(active);
    activeRows               = foundActive != languages.end() ? &foundActive->second : nullptr;
    const auto foundFallback = languages.find(fallback);
    fallbackRows             = foundFallback != languages.end() ? &foundFallback->second : nullptr;
}

std::string LocalizationModule::activeLanguage() const
{
    return active;
}

std::vector<std::string> LocalizationModule::loadedLanguages() const
{
    std::vector<std::string> codes;
    codes.reserve(languages.size());
    for (const auto &entry : languages)
    {
        codes.push_back(entry.first);
    }
    // std::map iterates sorted: the picker's order is stable for free.
    return codes;
}

std::string LocalizationModule::translate(
    const std::string &key
) const
{
    // Active language first (the row map pointers are cached at
    // loadLanguage/setLanguage - see the member comment).
    if (activeRows != nullptr)
    {
        const auto row = activeRows->find(key);
        if (row != activeRows->end())
        {
            return row->second;
        }
    }

    // Then the configured fallback (a pointer identity covers the
    // fallback == active case: the same loaded language is the same map).
    if (fallbackRows != nullptr && fallbackRows != activeRows)
    {
        const auto row = fallbackRows->find(key);
        if (row != fallbackRows->end())
        {
            return row->second;
        }
    }

    // Missing from both: the key itself IS the message (visible in the UI),
    // and the log names it once per key so a typo is precise and cheap. The
    // size check happens before the insert: past the cap new keys stay
    // silent instead of growing the dedup set without bound.
    if (logger != nullptr && missingWarned.find(key) == missingWarned.end() && missingWarned.size() < missingWarnedCap)
    {
        missingWarned.insert(key);
        logger->logMessage(
            LogLevel::Warning,
            "localization module: key '" + key + "' is missing from '" + active + "' and '" + fallback
                + "'; returning the key"
        );
    }
    return key;
}

} // namespace engine
