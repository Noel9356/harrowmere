#include "data/mod_overrides.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>

namespace harrowmere::data
{
namespace
{

bool isKeyedEntityFile(
    const std::string &fileName
)
{
    return fileName == "species.json" || fileName == "items.json" || fileName == "goods.json"
        || fileName == "roles.json" || fileName == "event_templates.json" || fileName == "crime_data.json";
}

std::string identityField(
    const std::string &fileName
)
{
    // crime_data.json's established schema calls its stable discriminator
    // "type"; all other D-MOD-2 entity rows call it "key".
    return fileName == "crime_data.json" ? "type" : "key";
}

bool keyedMerge(
    const std::string &fileName, const Json &base, const Json &over, Json &merged, std::string &reason
)
{
    if (!base.is_array() || !over.is_array())
    {
        reason = "D-MOD-2 entity-array override must be a JSON array";
        return false;
    }
    const std::string identity = identityField(fileName);
    std::map<std::string, Json> rows;
    const auto addRows = [&](const Json &source, bool overrideRows) -> bool {
        std::set<std::string> seenInFile;
        for (std::size_t i = 0; i < source.size(); ++i)
        {
            const Json &row = source[i];
            if (!row.is_object() || !row.contains(identity) || !row[identity].is_string())
            {
                reason = "row " + std::to_string(i) + " must be an object with string '" + identity + "'";
                return false;
            }
            const std::string key = row[identity].get<std::string>();
            if (key.empty() || !seenInFile.insert(key).second)
            {
                reason = "duplicate or empty entity identity '" + key + "' in one file";
                return false;
            }
            const auto found = rows.find(key);
            if (found == rows.end())
            {
                rows.emplace(key, row);
            } else if (overrideRows)
            {
                found->second = mergeData(found->second, row);
            } else
            {
                reason = "duplicate base entity identity '" + key + "'";
                return false;
            }
        }
        return true;
    };

    if (!addRows(base, false) || !addRows(over, true))
        return false;
    merged = Json::array();
    for (auto &[key, row] : rows)
    {
        (void)key;
        merged.push_back(std::move(row));
    }
    return true;
}

bool readJson(
    const std::filesystem::path &path, Json &out, std::vector<DataError> &errors
)
{
    auto loaded = loadJsonFile(path.string());
    if (loaded.isErr())
    {
        errors.push_back(loaded.error());
        return false;
    }
    out = loaded.value();
    return true;
}

bool writeJson(
    const std::filesystem::path &path, const Json &value, std::vector<DataError> &errors
)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        errors.push_back(DataError{path.string(), "", "cannot create merged data directory: " + ec.message()});
        return false;
    }
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            errors.push_back(DataError{tmp.string(), "", "cannot open merged data file for writing"});
            return false;
        }
        out << value.dump(2) << '\n';
        out.flush();
        if (!out)
        {
            errors.push_back(DataError{tmp.string(), "", "failed while writing merged data file"});
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec)
    {
        std::filesystem::remove(tmp);
        errors.push_back(DataError{path.string(), "", "cannot commit merged data file: " + ec.message()});
        return false;
    }
    return true;
}

} // namespace

Json mergeModData(
    const std::string &fileName, const Json &base, const Json &overrideJson
)
{
    if (!isKeyedEntityFile(fileName))
        return mergeData(base, overrideJson);
    Json merged;
    std::string reason;
    if (!keyedMerge(fileName, base, overrideJson, merged, reason))
        throw std::invalid_argument(fileName + ": " + reason);
    return merged;
}

bool materializeMergedData(
    const std::filesystem::path &coreDataDirectory,
    std::vector<DataModPackage> packages,
    const std::filesystem::path &outputDirectory,
    std::vector<DataError> &errors
)
{
    errors.clear();
    std::sort(packages.begin(), packages.end(), [](const DataModPackage &left, const DataModPackage &right) {
        return left.id < right.id;
    });

    std::error_code ec;
    std::filesystem::remove_all(outputDirectory, ec);
    ec.clear();
    std::filesystem::create_directories(outputDirectory, ec);
    if (ec)
    {
        errors.push_back(
            DataError{outputDirectory.string(), "", "cannot create data staging directory: " + ec.message()}
        );
        return false;
    }

    std::vector<std::filesystem::path> baseFiles;
    for (const auto &entry : std::filesystem::directory_iterator(coreDataDirectory, ec))
    {
        if (ec)
            break;
        if (entry.is_regular_file() && entry.path().extension() == ".json")
            baseFiles.push_back(entry.path());
    }
    if (ec)
    {
        errors.push_back(DataError{coreDataDirectory.string(), "", "cannot enumerate core data: " + ec.message()});
        return false;
    }
    std::sort(baseFiles.begin(), baseFiles.end());

    std::map<std::string, Json> mergedFiles;
    for (const auto &path : baseFiles)
    {
        Json value;
        if (!readJson(path, value, errors))
            continue;
        mergedFiles.emplace(path.filename().string(), std::move(value));
    }
    if (!errors.empty())
        return false;

    // rev 34: subdirectory content trees (data/characters/ and any future
    // content folder) ride the merge VERBATIM - the keyed .json merge above
    // stays the top-level file contract, while whole directories the game
    // reads directly (the character parts library, its recipes) copy
    // recursively, core first and each package's tree over it (the same
    // later-package-wins order the file merge uses).
    const auto copyContentTree = [&errors](const std::filesystem::path &from, const std::filesystem::path &to) {
        std::error_code copyEc;
        std::filesystem::create_directories(to, copyEc);
        std::filesystem::copy(from, to,
            std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, copyEc);
        if (copyEc)
        {
            errors.push_back(DataError{from.string(), "", "cannot copy content tree: " + copyEc.message()});
            return false;
        }
        return true;
    };
    {
        for (const auto &entry : std::filesystem::directory_iterator(coreDataDirectory, ec))
        {
            if (ec)
                break;
            if (entry.is_directory() && !copyContentTree(entry.path(), outputDirectory / entry.path().filename()))
                return false;
        }
    }

    for (const DataModPackage &package : packages)
    {
        const std::filesystem::path modData = package.directory / "data";
        if (!std::filesystem::is_directory(modData))
            continue;
        for (const auto &entry : std::filesystem::directory_iterator(modData, ec))
        {
            if (ec)
                break;
            if (entry.is_directory() && !copyContentTree(entry.path(), outputDirectory / entry.path().filename()))
                return false;
        }
        std::vector<std::filesystem::path> overrides;
        for (const auto &entry : std::filesystem::directory_iterator(modData, ec))
        {
            if (ec)
                break;
            if (entry.is_regular_file() && entry.path().extension() == ".json")
                overrides.push_back(entry.path());
        }
        if (ec)
        {
            errors.push_back(DataError{modData.string(), "", "cannot enumerate mod data: " + ec.message()});
            return false;
        }
        std::sort(overrides.begin(), overrides.end());
        for (const auto &path : overrides)
        {
            Json overrideValue;
            if (!readJson(path, overrideValue, errors))
                continue;
            const std::string name = path.filename().string();
            const auto found       = mergedFiles.find(name);
            try
            {
                // Even a newly introduced entity file must pass the D-MOD-2
                // identity/shape checks. An empty array is the base for a
                // new keyed entity table; ordinary files retain mergeData's
                // object/array policy through the same path.
                const Json emptyBase = Json::array();
                Json merged = mergeModData(name, found == mergedFiles.end() ? emptyBase : found->second, overrideValue);
                if (found == mergedFiles.end())
                    mergedFiles.emplace(name, std::move(merged));
                else
                    found->second = std::move(merged);
            } catch (const std::exception &exception)
            {
                errors.push_back(DataError{path.string(), "", exception.what()});
            }
        }
    }
    if (!errors.empty())
        return false;

    for (const auto &[name, value] : mergedFiles)
    {
        if (!writeJson(outputDirectory / name, value, errors))
            return false;
    }
    return true;
}

} // namespace harrowmere::data
