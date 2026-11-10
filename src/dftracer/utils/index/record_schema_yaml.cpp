// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/index/record_schema_yaml.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <initializer_list>
#include <string>

namespace dftracer::utils::index {

namespace {

using Fail = dstd::Unexpected<dstd::Error>;

Fail spec_error(const dstd::String& source, dstd::StringView what) {
    return common::make_error<"record schema spec {}: {}">(
        common::ErrorCode::InvalidArgument, source, what);
}

dstd::Result<void> check_keys(const YAML::Node& map,
                              std::initializer_list<const char*> keys,
                              const dstd::String& where,
                              const dstd::String& source) {
    if (!map.IsMap())
        return spec_error(source, dstd::fmt<"{} must be a mapping">(where));
    for (const auto& kv : map) {
        const auto text = kv.first.as<std::string>();
        const dstd::String key(text.data(), text.size());
        if (std::none_of(keys.begin(), keys.end(),
                         [&](const char* k) { return key == k; }))
            return spec_error(source,
                              dstd::fmt<"unknown key {}{}">(where, key));
    }
    return {};
}

dstd::Result<dstd::String> text_of(const YAML::Node& n, const dstd::String& key,
                                   const dstd::String& source) {
    if (!n.IsScalar())
        return spec_error(source, dstd::fmt<"{} must be a string">(key));
    const auto text = n.as<std::string>();
    return dstd::String(text.data(), text.size());
}

dstd::Result<bool> flag_of(const YAML::Node& n, const dstd::String& key,
                           const dstd::String& source) {
    try {
        return n.as<bool>();
    } catch (const YAML::Exception&) {
        return spec_error(source, dstd::fmt<"{} must be true or false">(key));
    }
}

template <typename E, std::size_t N>
dstd::Result<E> choice_of(
    const YAML::Node& n, const dstd::String& key,
    const std::array<dstd::Pair<const char*, E>, N>& names,
    const dstd::String& source) {
    const auto v = text_of(n, key, source);
    if (!v) return v.error();
    for (const auto& [name, value] : names)
        if (*v == name) return value;
    dstd::String all;
    for (const auto& [name, value] : names) {
        if (!all.empty()) all += ", ";
        all += name;
    }
    return spec_error(source,
                      dstd::fmt<"{} is {}; expected one of {}">(key, *v, all));
}

dstd::Result<SchemaSpec> parse_spec(const YAML::Node& spec,
                                    const dstd::String& source) {
    if (auto ok = check_keys(
            spec, {"id", "extends", "fields", "index", "source"}, "", source);
        !ok)
        return ok.error();
    SchemaSpec out;
    if (!spec["id"]) return spec_error(source, "missing id");
    {
        auto id = text_of(spec["id"], "id", source);
        if (!id) return id.error();
        out.id = std::move(*id);
    }
    if (spec["extends"]) {
        auto extends = text_of(spec["extends"], "extends", source);
        if (!extends) return extends.error();
        out.extends = std::move(*extends);
    }
    if (const auto fs = spec["fields"]) {
        if (!fs.IsMap())
            return spec_error(source,
                              "fields must map field names to settings");
        for (const auto& kv : fs) {
            FieldSpec f;
            auto name = text_of(kv.first, "fields", source);
            if (!name) return name.error();
            f.name = std::move(*name);
            const dstd::String where = dstd::fmt<"fields.{}">(f.name);
            const YAML::Node n = kv.second;
            if (auto ok = check_keys(n,
                                     {"type", "path", "optional", "role",
                                      "unit", "always_index"},
                                     dstd::fmt<"{}.">(where), source);
                !ok)
                return ok.error();
            const dstd::String type_key = dstd::fmt<"{}.type">(where);
            if (!n["type"])
                return spec_error(source,
                                  dstd::fmt<"{} is required">(type_key));
            const auto type =
                choice_of(n["type"], type_key, TYPE_NAMES, source);
            if (!type) return type.error();
            f.type = *type;
            if (n["path"]) {
                auto path =
                    text_of(n["path"], dstd::fmt<"{}.path">(where), source);
                if (!path) return path.error();
                f.path = std::move(*path);
            }
            if (n["optional"]) {
                const auto optional = flag_of(
                    n["optional"], dstd::fmt<"{}.optional">(where), source);
                if (!optional.has_value()) return optional.error();
                f.optional = *optional;
            }
            if (n["role"]) {
                const auto role = choice_of(
                    n["role"], dstd::fmt<"{}.role">(where), ROLE_NAMES, source);
                if (!role) return role.error();
                f.role = *role;
            }
            if (n["unit"]) {
                const auto unit = choice_of(
                    n["unit"], dstd::fmt<"{}.unit">(where), UNIT_NAMES, source);
                if (!unit) return unit.error();
                f.unit = *unit;
            }
            if (n["always_index"]) {
                const auto always =
                    flag_of(n["always_index"],
                            dstd::fmt<"{}.always_index">(where), source);
                if (!always.has_value()) return always.error();
                f.always_index = *always;
            }
            out.fields.push_back(std::move(f));
        }
    }
    if (const auto ix = spec["index"]) {
        if (auto ok =
                check_keys(ix, {"path_budget", "stats_share", "never_index"},
                           "index.", source);
            !ok)
            return ok.error();
        if (ix["never_index"]) {
            const auto list = ix["never_index"];
            if (!list.IsSequence())
                return spec_error(source,
                                  "index.never_index must be a list of paths");
            for (const auto& item : list) {
                if (!item.IsScalar() || item.Scalar().empty())
                    return spec_error(
                        source, "index.never_index must be a list of paths");
                const auto& path = item.Scalar();
                out.never_index.emplace_back(path.data(), path.size());
            }
        }
        if (ix["path_budget"]) {
            try {
                out.path_budget = ix["path_budget"].as<std::size_t>();
            } catch (const YAML::Exception&) {
                return spec_error(source, "index.path_budget must be a count");
            }
        }
        if (ix["stats_share"]) {
            double share = 0;
            try {
                share = ix["stats_share"].as<double>();
            } catch (const YAML::Exception&) {
                return spec_error(source,
                                  "index.stats_share must be in (0, 1]");
            }
            if (!(share > 0 && share <= 1))
                return spec_error(source,
                                  "index.stats_share must be in (0, 1]");
            out.stats_share = share;
        }
    }
    if (spec["source"]) {
        auto text = text_of(spec["source"], "source", source);
        if (!text) return text.error();
        out.source = std::move(*text);
    }
    return out;
}

}  // namespace

dstd::Result<SchemaSpec> parse_schema_spec(dstd::StringView text,
                                           const dstd::String& source) {
    try {
        const YAML::Node spec = YAML::Load(std::string(text));
        return parse_spec(spec, source);
    } catch (const YAML::Exception& e) {
        return spec_error(source, e.what());
    }
}

}  // namespace dftracer::utils::index
