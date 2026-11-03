#include "settings.hpp"

#include <toml.hpp>

#include <sol/sol.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
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

// One row as the file walker produces it - an intermediate shape local to
// this translation unit (the module's own SettingValue stays private to the
// class; the published capability deals in the typed reads only).
struct FileRow
{
    SettingKind kind = SettingKind::Missing;
    double number    = 0.0;
    bool boolean     = false;
    std::string string;
};

// The kind-mismatch warning dedup table (mismatchWarned) is keyed by
// script-controlled names; past this cap new names stay silent (the reads
// still return the fallback), so a script cannot grow the map without
// bound. 10000 distinct mistyped keys is far past any real options file.
constexpr std::size_t mismatchWarnedCap = 10000;

// Walks one parsed TOML table into (name, row) pairs. A bare
// string/double/integer/boolean row lands under its key; a nested TABLE
// recurses with the dotted prefix - both spellings of one key are the same
// stored value ("volume = 0.8" at the top level and [audio] volume = 0.8
// address "volume" / "audio.volume" respectively), because a hand-edited
// options file should be allowed the TOML idioms it was written with.
// Quoted keys keep their literal dots ("audio.volume" = 0.8 is ONE flat
// key), matching toml++'s round-trip of a dotted name written back.
// Anything else (arrays, dates, arrays-of-tables) is skipped with one
// precise log - a row the store cannot represent must not fail the whole
// file.
void collectRows(
    const toml::table &table,
    const std::string &prefix,
    std::map<std::string, FileRow> &rows,
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
            collectRows(*node.as_table(), name + ".", rows, logger, sourceName);
            continue;
        }

        FileRow row;
        if (node.is_boolean())
        {
            row.kind    = SettingKind::Boolean;
            row.boolean = node.as_boolean()->get();
        } else if (node.is_number())
        {
            // TOML integers and floats land in ONE number kind (integers
            // widened to double): an options file rarely means anything by
            // the int/float distinction, and one kind keeps every read
            // site honest.
            row.kind = SettingKind::Number;
            row.number
                = node.is_integer() ? static_cast<double>(node.as_integer()->get()) : node.as_floating_point()->get();
            // nan/inf are legal TOML floats but poison every consumer that
            // trusts the store (volumes, time spans, counters); the row is
            // skipped exactly like the unrepresentable kinds below.
            if (!std::isfinite(row.number))
            {
                if (logger != nullptr)
                {
                    logger->logMessage(
                        LogLevel::Warning,
                        "settings module: '" + sourceName + "' row '" + name + "' is not a finite number; row skipped"
                    );
                }
                continue;
            }
        } else if (node.is_string())
        {
            row.kind   = SettingKind::String;
            row.string = node.as_string()->get();
        } else
        {
            if (logger != nullptr)
            {
                logger->logMessage(
                    LogLevel::Warning,
                    "settings module: '" + sourceName + "' row '" + name
                        + "' is not a number, boolean or string; row skipped"
                );
            }
            continue;
        }
        rows.emplace(name, std::move(row));
    }
}

// The dedicated Lua bridge extension object for the settings module. Owns
// the sol2 wiring that installs engine.set_setting, engine.get_setting,
// engine.remove_setting, engine.save_settings and engine.load_settings into
// every script state. Holds raw pointers to the SettingsCapability the
// module provides and the LoggerCapability for save/load error reports.
class SettingsLuaBridge final : public LuaBridgeExtension
{
public:
    SettingsLuaBridge(
        SettingsCapability *capability, LoggerCapability *logger
    )
        : capability(capability), logger(logger)
    {}

    const char *extensionName() const override
    {
        return "settings";
    }

    void installLuaBridge(
        luaState *lua
    ) override
    {
        // The host (lua_scripting) guarantees the engine table exists when
        // this is called. The bridge functions call the SettingsCapability
        // methods directly through the captured pointer - no capability
        // acquisition, no permission check (the extension is part of the
        // module that provides the capability).
        //
        // engine::luaState IS the Lua C API's ::lua_State (the alias in
        // lua_bridge_extension.hpp), so the state_view wraps the pointer
        // directly - no cast, exactly like every other bridge extension.
        sol::state_view luaView(lua);
        sol::table engine = luaView["engine"];

        engine.set_function("set_setting", [this](const std::string &name, sol::object value) {
            // Dispatch on the Lua value's type; anything else (tables,
            // functions) is refused with one log line - the store keeps
            // to the scalars its file format can hold.
            if (value.is<bool>())
            {
                capability->setBoolean(name, value.as<bool>());
            } else if (value.is<double>())
            {
                capability->setNumber(name, value.as<double>());
            } else if (value.is<std::string>())
            {
                capability->setString(name, value.as<std::string>());
            } else if (logger != nullptr)
            {
                logger->logMessage(
                    LogLevel::Warning,
                    "settings module: set_setting('" + name
                        + "') refused - only numbers, booleans and strings are stored"
                );
            }
        });
        engine.set_function(
            "get_setting", [this](const std::string &name, sol::object fallback, sol::this_state state) -> sol::object {
                // The stored kind wins; the fallback answers only when the
                // key is unset (and is returned as-is, nil included). One
                // store lookup serves both the kind dispatch and the value
                // read: the two public capability calls this used to make
                // (kindOf, then the typed read) walked the same tree twice
                // per script call. The module IS the capability, so the cast
                // is the documented identity, and findValue is the module's
                // own single-lookup read.
                const auto *stored = static_cast<const SettingsModule *>(capability)->findValue(name);
                if (stored == nullptr)
                {
                    return fallback;
                }
                switch (stored->kind)
                {
                case SettingKind::Number:
                    return sol::make_object(state, stored->number);
                case SettingKind::Boolean:
                    return sol::make_object(state, stored->boolean);
                case SettingKind::String:
                    return sol::make_object(state, stored->string);
                case SettingKind::Missing:
                default:
                    return fallback;
                }
            }
        );
        engine.set_function("remove_setting", [this](const std::string &name) -> bool {
            return capability->removeValue(name);
        });
        engine.set_function("save_settings", [this](sol::optional<std::string> path) -> bool {
            std::string errorReport;
            const bool saved = capability->save(path.value_or(std::string()), errorReport);
            if (!errorReport.empty() && logger != nullptr)
            {
                logger->logMessage(LogLevel::Error, errorReport);
            }
            return saved;
        });
        engine.set_function("load_settings", [this](sol::optional<std::string> path) -> bool {
            std::string errorReport;
            const bool loaded = capability->load(path.value_or(std::string()), errorReport);
            if (!errorReport.empty() && logger != nullptr)
            {
                logger->logMessage(LogLevel::Error, errorReport);
            }
            return loaded;
        });
    }

private:
    SettingsCapability *capability;
    LoggerCapability *logger;
};

} // namespace

SettingsModule::SettingsModule()
{
    componentManifest.name = "settings";
    componentManifest.kind = ComponentKind::Module;
    // The implementation-bound member of the "settings" family: a future
    // binary or streamed options format declares its own member and the
    // [providers] configuration picks between them.
    componentManifest.providedCapabilities
        = {"settings.toml",
            // The Lua bridge extension member: declares this module as a
            // provider of the "scripting.lua_bridge" family. The
            // lua_scripting host acquires every active member through
            // ComponentContext::acquireEachCapability and asks each
            // one to install its own bindings into the host's engine
            // table.
            "scripting.lua_bridge.settings"};
    componentManifest.requiredCapabilities = {"logging"};
    // Loading reads a file (filesystem.read); saving writes one
    // (filesystem.write). Both are deniable in configuration like every
    // grant - a run that denies the write gets a settings store that works
    // in memory and reports every save precisely.
    componentManifest.requiredPermissions = {"capability.logging", "filesystem.read", "filesystem.write"};
    componentManifest.knownConfigKeys     = {"settings.file", "settings.autosave"};
}

void SettingsModule::construct(
    ComponentContext &context
)
{
    // The one cross-component touch of the construct phase: the log sink.
    logger = &context.acquireCapability<LoggerCapability>();

    defaultFile = context.configurationString("file").value_or("settings.toml");
    autosave    = context.configurationBoolean("autosave").value_or(false);

    // Construct the Lua bridge extension object. The module IS the
    // SettingsCapability, so `this` is the capability pointer.
    luaBridge = std::make_unique<SettingsLuaBridge>(this, logger);
}

void SettingsModule::start()
{
    // Auto-load the configured default file when it exists: a fresh checkout
    // (no file yet) starts empty and logs the fact, an existing file loads
    // with the same merge semantics an explicit load keeps. A broken file
    // degrades to the empty store with one precise warning - a settings
    // file a user half-edited must not fail the run.
    if (logger != nullptr)
    {
        std::string errorReport;
        const std::size_t before = values.size();
        if (load(std::string(), errorReport))
        {
            logger->logMessage(
                LogLevel::Information,
                "settings module: loaded " + std::to_string(values.size() - before) + " value(s) from '" + defaultFile
                    + "'"
            );
        } else if (errorReport.find("does not name a readable file") != std::string::npos)
        {
            logger->logMessage(
                LogLevel::Information, "settings module: no settings file yet ('" + defaultFile + "') - starting empty"
            );
        } else
        {
            logger->logMessage(LogLevel::Warning, errorReport);
        }
    }
}

void SettingsModule::stop()
{
    // Reset the Lua bridge before nulling the capability pointers it captures.
    luaBridge.reset();
    logger = nullptr;
}

void *SettingsModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    if (capabilityName == "settings.toml")
    {
        return static_cast<SettingsCapability *>(this);
    }
    // The "scripting.lua_bridge.settings" member: returns the dedicated
    // SettingsLuaBridge object this module owns (constructed in construct()).
    if (capabilityName == "scripting.lua_bridge.settings")
    {
        return luaBridge.get();
    }
    return nullptr;
}

std::string SettingsModule::resolvePath(
    const std::string &path
) const
{
    return path.empty() ? defaultFile : path;
}

void SettingsModule::autosaveIfEnabled()
{
    if (!autosave)
    {
        return;
    }
    std::string errorReport;
    if (!save(std::string(), errorReport) && logger != nullptr)
    {
        logger->logMessage(LogLevel::Warning, errorReport);
    }
}

bool SettingsModule::load(
    const std::string &path, std::string &errorReport
)
{
    const std::string sourceName         = resolvePath(path);
    const std::filesystem::path resolved = resolveAgainstExecutable(sourceName);
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(resolved, fileError))
    {
        errorReport = "settings module: '" + sourceName + "' does not name a readable file (resolved '"
                    + resolved.string() + "')";
        return false;
    }

    toml::parse_result parsed;
    try
    {
        parsed = toml::parse_file(resolved.string());
    } catch (const toml::parse_error &error)
    {
        std::ostringstream description;
        description << "settings module: '" << sourceName << "' is not valid TOML: " << error.description() << " (line "
                    << error.source().begin.line << ")";
        errorReport = description.str();
        return false;
    }

    // Walk the file into rows first, then merge: a structurally broken file
    // (the parse above) never touches the store, and a half-typed row only
    // skips itself.
    std::map<std::string, FileRow> rows;
    collectRows(parsed, std::string(), rows, logger, sourceName);

    for (const auto &entry : rows)
    {
        SettingValue &value = values[entry.first];
        value.kind          = entry.second.kind;
        value.number        = entry.second.number;
        value.boolean       = entry.second.boolean;
        value.string        = entry.second.string;
    }
    return true;
}

bool SettingsModule::save(
    const std::string &path, std::string &errorReport
)
{
    const std::string sourceName         = resolvePath(path);
    const std::filesystem::path resolved = resolveAgainstExecutable(sourceName);
    std::error_code directoryError;
    std::filesystem::create_directories(resolved.parent_path(), directoryError);

    toml::table file;
    for (const auto &entry : values)
    {
        const SettingValue &value = entry.second;
        switch (value.kind)
        {
        case SettingKind::Number:
            file.emplace(entry.first, value.number);
            break;
        case SettingKind::Boolean:
            file.emplace(entry.first, value.boolean);
            break;
        case SettingKind::String:
            file.emplace(entry.first, value.string);
            break;
        case SettingKind::Missing:
        default:
            break;
        }
    }

    // Atomic save (the persistence module's shape): write a sibling .tmp,
    // flush and check it, close, then rename over the target. rename is
    // atomic on POSIX and on Windows (same volume); with autosave firing on
    // EVERY mutation, a crash mid-write must not destroy the one on-disk
    // options file.
    const std::filesystem::path temporary = resolved.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            errorReport = "settings module: cannot open '" + temporary.string() + "' for writing"
                        + " (is filesystem.write granted to this component?)";
            return false;
        }
        output << "# Saved by the engine's settings module: " << values.size() << " value(s).\n"
               << "# Hand-editable: rows are scalar keys (nested [tables] load as dotted names).\n"
               << file << "\n";
        // Flush and check BEFORE closing: a full disk (or any write error)
        // must abort the save instead of committing a truncated temp file
        // over the good one.
        output.flush();
        if (!output)
        {
            errorReport = "settings module: writing '" + temporary.string() + "' failed";
            return false;
        }
    } // the stream closes (and releases the file) before the rename
    std::error_code renameError;
    std::filesystem::rename(temporary, resolved, renameError);
    if (renameError)
    {
        errorReport = "settings module: cannot commit '" + resolved.string()
                    + "' (rename failed: " + renameError.message() + ")";
        // Best effort: do not strand the temp file next to the settings.
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        return false;
    }
    return true;
}

void SettingsModule::setNumber(
    const std::string &name, double value
)
{
    SettingValue &stored = values[name];
    stored.kind          = SettingKind::Number;
    stored.number        = value;
    autosaveIfEnabled();
}

void SettingsModule::setBoolean(
    const std::string &name, bool value
)
{
    SettingValue &stored = values[name];
    stored.kind          = SettingKind::Boolean;
    stored.boolean       = value;
    autosaveIfEnabled();
}

void SettingsModule::setString(
    const std::string &name, const std::string &value
)
{
    SettingValue &stored = values[name];
    stored.kind          = SettingKind::String;
    stored.string        = value;
    autosaveIfEnabled();
}

double SettingsModule::numberValue(
    const std::string &name, double fallback
) const
{
    const auto found = values.find(name);
    if (found == values.end())
    {
        return fallback;
    }
    if (found->second.kind != SettingKind::Number)
    {
        // contains() before the size check: an already-warned name must
        // stay silent even after the cap, and operator[] would insert.
        if (logger != nullptr && !mismatchWarned.contains(name) && mismatchWarned.size() < mismatchWarnedCap)
        {
            mismatchWarned[name] = true;
            logger->logMessage(
                LogLevel::Warning,
                "settings module: '" + name + "' holds a different kind of value; number read returned the fallback"
            );
        }
        return fallback;
    }
    return found->second.number;
}

bool SettingsModule::booleanValue(
    const std::string &name, bool fallback
) const
{
    const auto found = values.find(name);
    if (found == values.end())
    {
        return fallback;
    }
    if (found->second.kind != SettingKind::Boolean)
    {
        // contains() before the size check: an already-warned name must
        // stay silent even after the cap, and operator[] would insert.
        if (logger != nullptr && !mismatchWarned.contains(name) && mismatchWarned.size() < mismatchWarnedCap)
        {
            mismatchWarned[name] = true;
            logger->logMessage(
                LogLevel::Warning,
                "settings module: '" + name + "' holds a different kind of value; boolean read returned the fallback"
            );
        }
        return fallback;
    }
    return found->second.boolean;
}

std::string SettingsModule::stringValue(
    const std::string &name, const std::string &fallback
) const
{
    const auto found = values.find(name);
    if (found == values.end())
    {
        return fallback;
    }
    if (found->second.kind != SettingKind::String)
    {
        // contains() before the size check: an already-warned name must
        // stay silent even after the cap, and operator[] would insert.
        if (logger != nullptr && !mismatchWarned.contains(name) && mismatchWarned.size() < mismatchWarnedCap)
        {
            mismatchWarned[name] = true;
            logger->logMessage(
                LogLevel::Warning,
                "settings module: '" + name + "' holds a different kind of value; string read returned the fallback"
            );
        }
        return fallback;
    }
    return found->second.string;
}

SettingKind SettingsModule::kindOf(
    const std::string &name
) const
{
    const auto found = values.find(name);
    return found != values.end() ? found->second.kind : SettingKind::Missing;
}

bool SettingsModule::contains(
    const std::string &name
) const
{
    return values.find(name) != values.end();
}

bool SettingsModule::removeValue(
    const std::string &name
)
{
    const auto found = values.find(name);
    if (found == values.end())
    {
        return false;
    }
    values.erase(found);
    mismatchWarned.erase(name);
    autosaveIfEnabled();
    return true;
}

std::size_t SettingsModule::valueCount() const
{
    return values.size();
}

} // namespace engine
