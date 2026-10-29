#ifndef ENGINE_MODULES_LOCALIZATION_HPP
#define ENGINE_MODULES_LOCALIZATION_HPP

#include "localization_capability.hpp"
#include "logging_capability.hpp"
#include "lua_bridge_extension.hpp"
#include "engine/kernel/component_interface.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

namespace engine
{

// Module "localization": provides the LocalizationCapability (family member
// "localization.toml").
//
// One lookup over a set of language tables: <directory>/<code>.toml files
// ([localization] directory, resolved against the executable directory -
// the engine ships no content, so a mod points the key at its own
// assets/languages and the existing mod packaging copies them), loaded at
// start or on demand, translated through active -> fallback -> key.
//
// The module additionally provides the "scripting.lua_bridge.localization"
// Lua bridge extension: it installs engine.localize, engine.set_language,
// engine.active_language and engine.loaded_languages into every script
// state through a dedicated LocalizationLuaBridge object owned by this
// module (see localization.cpp). The host discovers this extension through
// the multi-provider acquisition of the "scripting.lua_bridge" family, so
// adding this capability to the module never touches the lua_scripting
// source.
class LocalizationModule final : public ModuleInterface, public LocalizationCapability
{
public:
    LocalizationModule();

    const ComponentManifest &manifest() const override
    {
        return componentManifest;
    }

    void construct(ComponentContext &context) override;
    void start() override;
    void stop() override;

    void *capabilityPointerFor(const std::string &capabilityName) override;

    // LocalizationCapability.
    bool loadLanguage(const std::string &code, std::string &errorReport) override;
    bool setLanguage(const std::string &code, std::string &errorReport) override;
    std::string activeLanguage() const override;
    std::vector<std::string> loadedLanguages() const override;
    std::string translate(const std::string &key) const override;

private:
    // Parses one language file into the given row map. Shared by loadLanguage
    // and the start phase's directory scan; a parse failure reports through
    // errorReport and leaves the language's previous rows untouched.
    bool parseLanguageFile(
        const std::string &code, std::unordered_map<std::string, std::string> &rows, std::string &errorReport
    );

    ComponentManifest componentManifest;
    LoggerCapability *logger = nullptr;
    // The loaded language tables, keyed by language code (std::map so
    // loadedLanguages is sorted for free and the scan order is stable). The
    // ROWS of one language live in an unordered_map: translate() is the
    // per-frame path (a UI drawing N localized labels calls it N times per
    // frame), and a red-black tree walked the key with 1-2+ string compares
    // per label where one hash answers - the outer map keeps every
    // iteration-order property the module documents.
    std::map<std::string, std::unordered_map<std::string, std::string>> languages;
    std::string active   = std::string();
    std::string fallback = "en";
    // [localization] directory (read in construct, resolved against the
    // executable directory at use).
    std::string directory = "languages";
    // [localization] language: the language start() activates.
    std::string configuredLanguage = "en";
    // Keys already reported missing (one warning per key, not one per
    // lookup - a per-frame translate of a typo'd key must not spam). The
    // translate contract is const, so the set is mutable.
    mutable std::set<std::string> missingWarned;
    // The row maps translate() resolves against, cached between mutations:
    // every translate used to re-walk the language tree with string compares
    // (active, then fallback) before touching any rows, and returned the
    // translation by value on top - a UI drawing N localized labels paid
    // 2-4 tree walks each per frame. The maps only change on loadLanguage
    // and setLanguage, so the two pointers refresh at those points (and are
    // nulled at stop) and translate reads them directly. Null means "this
    // language is not loaded"; node stability is the same contract either
    // container keeps - rehashing an unordered_map moves its buckets, never
    // the stored elements' addresses, and a re-load move-assigns the SAME
    // inner map object, so a cached pointer stays valid across both.
    const std::unordered_map<std::string, std::string> *activeRows   = nullptr;
    const std::unordered_map<std::string, std::string> *fallbackRows = nullptr;
    // Re-derives the two cached row pointers from active/fallback. Called by
    // every mutation that can change what they point at.
    void refreshRowCache();
    // The Lua bridge extension object owned by this module (constructed in
    // construct(), returned from capabilityPointerFor for the
    // "scripting.lua_bridge.localization" member; the concrete
    // LocalizationLuaBridge type stays private to localization.cpp).
    std::unique_ptr<LuaBridgeExtension> luaBridge;
};

} // namespace engine

#endif // ENGINE_MODULES_LOCALIZATION_HPP
