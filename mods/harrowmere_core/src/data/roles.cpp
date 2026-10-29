// roles.cpp — NPC role loader (Task 1.2).
#include "data/roles.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <fstream>
#include <sstream>

namespace harrowmere::data
{

namespace
{

std::optional<std::string> readFile(
    const std::string &path
)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// The closed v1 role set — kept here (not in enums.hpp) because roles are
// data-driven (the design rule "species, classes, items, quests, and
// recipes are data-driven" extends to roles — they live in JSON). The
// list is used only by the `hasPinnedV1Set` validation check.
constexpr const char *kPinnedV1Roles[]
    = {"guard", "captain", "vendor", "farmer", "laborer", "noble", "militia_veteran", "healer", "child"};

} // namespace

bool RoleRegistry::has(
    std::string_view key
) const noexcept
{
    return roles_.find(std::string(key)) != roles_.end();
}

const RoleData *RoleRegistry::byKey(
    std::string_view key
) const noexcept
{
    auto it = roles_.find(std::string(key));
    return it == roles_.end() ? nullptr : &it->second;
}
RoleData *RoleRegistry::byKey(
    std::string_view key
) noexcept
{
    auto it = roles_.find(std::string(key));
    return it == roles_.end() ? nullptr : &it->second;
}

bool RoleRegistry::hasPinnedV1Set() const noexcept
{
    for (const char *k : kPinnedV1Roles)
    {
        if (!has(k))
            return false;
    }
    return true;
}

bool loadRoles(
    const std::string &path, RoleRegistry &out, std::vector<RoleDataError> &errors
)
{
    const auto content = readFile(path);
    if (!content)
    {
        errors.emplace_back(path, "", "could not open file");
        return false;
    }
    nlohmann::json root;
    try
    {
        root = nlohmann::json::parse(*content, nullptr, true, true);
    } catch (const nlohmann::json::parse_error &e)
    {
        errors.emplace_back(path, "", std::string("parse error: ") + e.what());
        return false;
    }
    if (!root.is_array())
    {
        errors.emplace_back(path, "", "roles.json must be a JSON array");
        return false;
    }

    bool ok         = true;
    std::size_t idx = 0;
    for (auto it = root.begin(); it != root.end(); ++it, ++idx)
    {
        const std::string pointer = "/" + std::to_string(idx);
        const auto &e             = *it;
        if (!e.is_object())
        {
            errors.emplace_back(path, pointer, "role entry must be a JSON object");
            ok = false;
            continue;
        }

        RoleData r;

        // `key` (required, unique, string).
        auto k = e.find("key");
        if (k == e.end() || !k->is_string())
        {
            errors.emplace_back(path, pointer + "/key", "missing or non-string 'key'");
            ok = false;
            continue;
        }
        r.key = k->get<std::string>();
        if (r.key.empty())
        {
            errors.emplace_back(path, pointer + "/key", "'key' must not be empty");
            ok = false;
            continue;
        }
        if (out.has(r.key))
        {
            errors.emplace_back(path, pointer, "duplicate role key: " + r.key);
            ok = false;
            continue;
        }

        // `baseReportWeight` (required, integer 0..kScale).
        auto bw = e.find("baseReportWeight");
        if (bw == e.end() || (!bw->is_number_integer() && !bw->is_number_unsigned()))
        {
            errors.emplace_back(path, pointer + "/baseReportWeight", "missing or non-integer 'baseReportWeight'");
            ok = false;
            continue;
        }
        const auto bwv = bw->get<std::int64_t>();
        if (bwv < 0 || bwv > static_cast<std::int64_t>(kScale))
        {
            errors.emplace_back(
                path,
                pointer + "/baseReportWeight",
                std::to_string(bwv) + " out of range [0, " + std::to_string(kScale) + "]"
            );
            ok = false;
            continue;
        }
        r.baseReportWeight = static_cast<std::uint32_t>(bwv);

        // `integrityRange` (required, [lo, hi] both 0..kScale).
        auto ir = e.find("integrityRange");
        if (ir == e.end() || !ir->is_array() || ir->size() != 2)
        {
            errors.emplace_back(path, pointer + "/integrityRange", "missing 'integrityRange' or not a 2-element array");
            ok = false;
            continue;
        }
        const auto &lo = (*ir)[0];
        const auto &hi = (*ir)[1];
        if (!lo.is_number_integer() && !lo.is_number_unsigned())
        {
            errors.emplace_back(path, pointer + "/integrityRange/0", "integrityRange[0] must be an integer");
            ok = false;
            continue;
        }
        if (!hi.is_number_integer() && !hi.is_number_unsigned())
        {
            errors.emplace_back(path, pointer + "/integrityRange/1", "integrityRange[1] must be an integer");
            ok = false;
            continue;
        }
        const auto lov = lo.get<std::int64_t>();
        const auto hiv = hi.get<std::int64_t>();
        if (lov < 0 || lov > static_cast<std::int64_t>(kScale))
        {
            errors.emplace_back(
                path,
                pointer + "/integrityRange/0",
                std::to_string(lov) + " out of range [0, " + std::to_string(kScale) + "]"
            );
            ok = false;
            continue;
        }
        if (hiv < 0 || hiv > static_cast<std::int64_t>(kScale))
        {
            errors.emplace_back(
                path,
                pointer + "/integrityRange/1",
                std::to_string(hiv) + " out of range [0, " + std::to_string(kScale) + "]"
            );
            ok = false;
            continue;
        }
        if (lov > hiv)
        {
            errors.emplace_back(path, pointer + "/integrityRange", "integrityRange[0] > integrityRange[1]");
            ok = false;
            continue;
        }
        r.integrityMin = static_cast<std::uint32_t>(lov);
        r.integrityMax = static_cast<std::uint32_t>(hiv);

        // `critical` (required, boolean).
        auto c = e.find("critical");
        if (c == e.end() || !c->is_boolean())
        {
            errors.emplace_back(path, pointer + "/critical", "missing or non-boolean 'critical'");
            ok = false;
            continue;
        }
        r.critical = c->get<bool>();

        // `priority` (optional, integer 0..255). Default 0.
        if (e.contains("priority"))
        {
            const auto &p = e["priority"];
            if (!p.is_number_integer() && !p.is_number_unsigned())
            {
                errors.emplace_back(path, pointer + "/priority", "'priority' must be an integer");
                ok = false;
                continue;
            }
            const auto pv = p.get<std::int64_t>();
            if (pv < 0 || pv > 255)
            {
                errors.emplace_back(path, pointer + "/priority", std::to_string(pv) + " out of range [0, 255]");
                ok = false;
                continue;
            }
            r.priority = static_cast<std::uint32_t>(pv);
        }

        // `duties` (optional, array of strings).
        if (e.contains("duties"))
        {
            const auto &d = e["duties"];
            if (!d.is_array())
            {
                errors.emplace_back(path, pointer + "/duties", "'duties' must be an array");
                ok = false;
                continue;
            }
            for (auto dit = d.begin(); dit != d.end(); ++dit)
            {
                if (!dit->is_string())
                {
                    errors.emplace_back(path, pointer + "/duties", "duties entries must be strings");
                    ok = false;
                    continue;
                }
                r.duties.push_back(dit->get<std::string>());
            }
        }

        // `cannotTestify` (optional, boolean). Default false.
        if (e.contains("cannotTestify"))
        {
            if (!e["cannotTestify"].is_boolean())
            {
                errors.emplace_back(path, pointer + "/cannotTestify", "'cannotTestify' must be boolean");
                ok = false;
                continue;
            }
            r.cannotTestify = e["cannotTestify"].get<bool>();
        }

        out.roles_[r.key] = std::move(r);
    }

    // The pinned v1 set must be present. Missing pinned roles are an error
    // — worldgen (task 1.5) seeds captains and guards; the absence breaks
    // the simulation.
    if (!out.hasPinnedV1Set())
    {
        for (const char *k : kPinnedV1Roles)
        {
            if (!out.has(k))
            {
                errors.emplace_back(path, "", std::string("missing pinned v1 role: ") + k);
            }
        }
        ok = false;
    }

    return ok;
}

} // namespace harrowmere::data
