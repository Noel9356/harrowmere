#ifndef ENGINE_MODULES_LOCALIZATION_LOCALIZATION_CAPABILITY_HPP
#define ENGINE_MODULES_LOCALIZATION_LOCALIZATION_CAPABILITY_HPP

// The published capability surface of the localization module. Every consumer -
// other modules, native mods, the app - includes THIS header, from the
// module that provides the capability: the provider owns and publishes its
// own capability vocabulary, so there is no central contract directory and
// no mirrored copy that could drift. Like every published capability header
// this stays implementation-, platform- and vendor-agnostic; the per-module
// purity guard in the module's CMakeLists enforces that mechanically.
// See mods/README.md, section "Portability".

#include <string>
#include <vector>

namespace engine
{

// Capability "localization": string tables for UI text.
//
// The engine had no place to put user-facing text: every UI string was a
// literal in whoever drew it - the ui module's overlay, a script's log
// lines, a mod's dialogs - so nothing was translatable and nothing was
// consistent. This capability is the one lookup: language tables ship as
// TOML files (the engine's configuration format, so translations carry
// comments and stay hand-editable), a mod ships its own under assets/, and
// every consumer addresses text by KEY through the active language - or the
// fallback when the active language is missing a row, or the key itself
// when both are (a missing translation degrades visibly, it never blanks a
// screen).
//
// The surface deals in keys and plain UTF-8 strings only. Formatting,
// pluralization and font selection are the consumer's business: a table row
// is a string, and a replacement implementation that swaps the file format
// (or adds a remote source) declares its own member of the "localization"
// family and wins through [providers].
class LocalizationCapability
{
public:
    // The implementation-agnostic family name of this capability. Consumers
    // acquire through it; the module's plugin descriptor carries the
    // implementation-bound member name ("localization.toml").
    static constexpr const char *capabilityName = "localization";

    virtual ~LocalizationCapability() = default;

    // Loads one language file (<configured directory>/<code>.toml) into the
    // set of loaded languages, without activating it. Loading the SAME code
    // again replaces that language's rows (a hot-reloaded translation);
    // other languages are untouched. Returns false with a precise message
    // when the file cannot be read or parsed; rows of a non-string type are
    // skipped with one log each and the load continues.
    virtual bool loadLanguage(const std::string &code, std::string &errorReport) = 0;

    // Activates a language, loading it first when it is not loaded yet.
    // Returns false when the language cannot be loaded - the previously
    // active language stays active (a failed switch must never leave the
    // UI untranslated).
    virtual bool setLanguage(const std::string &code, std::string &errorReport) = 0;

    // The currently active language code (empty when none was activated
    // yet - every translate then degrades to the keys).
    virtual std::string activeLanguage() const = 0;

    // The language codes currently loaded, sorted (a settings screen's
    // language picker is the expected consumer).
    virtual std::vector<std::string> loadedLanguages() const = 0;

    // Translates one key: the active language first, then the configured
    // fallback language, then the key itself. A key missing from both
    // languages is logged once per key (a per-frame lookup of a typo'd key
    // must not spam) and degrades to the key, so the failure is visible in
    // the UI and precise in the log.
    virtual std::string translate(const std::string &key) const = 0;
};

} // namespace engine

#endif // ENGINE_MODULES_LOCALIZATION_LOCALIZATION_CAPABILITY_HPP
