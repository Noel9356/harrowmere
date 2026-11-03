#ifndef HARROWMERE_CORE_DATA_MOD_OVERRIDES_HPP
#define HARROWMERE_CORE_DATA_MOD_OVERRIDES_HPP

#include <filesystem>
#include <string>
#include <vector>

#include "data/json.hpp"

namespace harrowmere::data
{

struct DataModPackage
{
    std::string id;
    std::filesystem::path directory;
};

// Merge core data with active package data/<name>.json overlays in manifest-id
// order and materialize a validated-input staging directory for the existing
// strict typed loaders. The caller owns the staging directory lifetime.
bool materializeMergedData(
    const std::filesystem::path &coreDataDirectory,
    std::vector<DataModPackage> packages,
    const std::filesystem::path &outputDirectory,
    std::vector<DataError> &errors
);

// Public for tests and authoring tools: D-MOD-2 entity arrays merge by their
// stable identity field; other JSON values follow mergeData's deep-object /
// replace-array policy.
Json mergeModData(const std::string &fileName, const Json &base, const Json &overrideJson);

} // namespace harrowmere::data

#endif // HARROWMERE_CORE_DATA_MOD_OVERRIDES_HPP
