// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/trace/index/record_schema_yaml.h>
#include <dftracer/utils/yaml/document.h>

#include <algorithm>
#include <initializer_list>
#include <limits>

namespace dftracer::utils::trace::index {

namespace {

using yaml::Document;
using yaml::Value;
using Fail = dstd::Unexpected<dstd::Error>;

Fail spec_error(const dstd::String& source, dstd::StringView what) {
    return common::make_error<"record schema spec {}: {}">(
        common::ErrorCode::InvalidArgument, source, what);
}

dstd::Result<void> check_keys(const Value& map,
                              std::initializer_list<const char*> keys,
                              const dstd::String& where,
                              const dstd::String& source) {
    if (!map.is_object())
        return spec_error(source, dstd::fmt<"{} must be a mapping">(where));
    dstd::Result<void> status;
    map.for_each_member([&](dstd::StringView key, Value) {
        if (!status) return;
        if (std::none_of(keys.begin(), keys.end(),
                         [&](const char* k) { return key == k; }))
            status =
                spec_error(source, dstd::fmt<"unknown key {}{}">(where, key));
    });
    return status;
}

dstd::Result<dstd::String> text_of(const Value& n, dstd::StringView key,
                                   const dstd::String& source) {
    const auto text = n.as_string();
    if (!text) return spec_error(source, dstd::fmt<"{} must be a string">(key));
    return dstd::String(*text);
}

dstd::Result<bool> flag_of(const Value& n, dstd::StringView key,
                           const dstd::String& source) {
    const auto flag = n.as_bool();
    if (!flag)
        return spec_error(source, dstd::fmt<"{} must be true or false">(key));
    return *flag;
}

template <typename E, std::size_t N>
dstd::Result<E> choice_of(
    const Value& n, const dstd::String& key,
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

dstd::Result<FieldSpec> parse_field(dstd::StringView name, const Value& n,
                                    const dstd::String& source) {
    FieldSpec f;
    f.name = dstd::String(name);
    const dstd::String where = dstd::fmt<"fields.{}">(f.name);
    if (auto ok = check_keys(
            n, {"type", "path", "optional", "role", "unit", "always_index"},
            dstd::fmt<"{}.">(where), source);
        !ok)
        return ok.error();
    const dstd::String type_key = dstd::fmt<"{}.type">(where);
    const auto type_node = n.find("type");
    if (!type_node)
        return spec_error(source, dstd::fmt<"{} is required">(type_key));
    const auto type = choice_of(*type_node, type_key, TYPE_NAMES, source);
    if (!type) return type.error();
    f.type = *type;
    if (const auto path_node = n.find("path")) {
        auto path = text_of(*path_node, dstd::fmt<"{}.path">(where), source);
        if (!path) return path.error();
        f.path = std::move(*path);
    }
    if (const auto optional_node = n.find("optional")) {
        const auto optional =
            flag_of(*optional_node, dstd::fmt<"{}.optional">(where), source);
        if (!optional.has_value()) return optional.error();
        f.optional = *optional;
    }
    if (const auto role_node = n.find("role")) {
        const auto role = choice_of(*role_node, dstd::fmt<"{}.role">(where),
                                    ROLE_NAMES, source);
        if (!role) return role.error();
        f.role = *role;
    }
    if (const auto unit_node = n.find("unit")) {
        const auto unit = choice_of(*unit_node, dstd::fmt<"{}.unit">(where),
                                    UNIT_NAMES, source);
        if (!unit) return unit.error();
        f.unit = *unit;
    }
    if (const auto always_node = n.find("always_index")) {
        const auto always =
            flag_of(*always_node, dstd::fmt<"{}.always_index">(where), source);
        if (!always.has_value()) return always.error();
        f.always_index = *always;
    }
    return f;
}

dstd::Result<SchemaSpec> parse_spec(const Value& spec,
                                    const dstd::String& source) {
    if (auto ok = check_keys(
            spec, {"id", "extends", "fields", "index", "source"}, "", source);
        !ok)
        return ok.error();
    SchemaSpec out;
    const auto id_node = spec.find("id");
    if (!id_node) return spec_error(source, "missing id");
    {
        auto id = text_of(*id_node, "id", source);
        if (!id) return id.error();
        out.id = std::move(*id);
    }
    if (const auto extends_node = spec.find("extends")) {
        auto extends = text_of(*extends_node, "extends", source);
        if (!extends) return extends.error();
        out.extends = std::move(*extends);
    }
    if (const auto fs = spec.find("fields")) {
        if (!fs->is_object())
            return spec_error(source,
                              "fields must map field names to settings");
        dstd::Result<void> status;
        fs->for_each_member([&](dstd::StringView name, Value n) {
            if (!status) return;
            auto field = parse_field(name, n, source);
            if (!field)
                status = field.error();
            else
                out.fields.push_back(std::move(*field));
        });
        if (!status) return status.error();
    }
    if (const auto ix = spec.find("index")) {
        if (auto ok =
                check_keys(*ix, {"path_budget", "stats_share", "never_index"},
                           "index.", source);
            !ok)
            return ok.error();
        if (const auto list = ix->find("never_index")) {
            if (!list->is_array())
                return spec_error(source,
                                  "index.never_index must be a list of paths");
            dstd::Result<void> status;
            list->for_each_element([&](Value item) {
                if (!status) return;
                const auto path = item.as_string();
                if (!path || path->empty())
                    status = spec_error(
                        source, "index.never_index must be a list of paths");
                else
                    out.never_index.emplace_back(*path);
            });
            if (!status) return status.error();
        }
        if (const auto budget = ix->find("path_budget")) {
            const auto count = budget->as_uint();
            if (!count || *count > std::numeric_limits<std::size_t>::max())
                return spec_error(source, "index.path_budget must be a count");
            out.path_budget = static_cast<std::size_t>(*count);
        }
        if (const auto stats = ix->find("stats_share")) {
            const auto share = stats->as_double();
            if (!share || !(*share > 0 && *share <= 1))
                return spec_error(source,
                                  "index.stats_share must be in (0, 1]");
            out.stats_share = *share;
        }
    }
    if (const auto source_node = spec.find("source")) {
        auto text = text_of(*source_node, "source", source);
        if (!text) return text.error();
        out.source = std::move(*text);
    }
    return out;
}

}  // namespace

dstd::Result<SchemaSpec> parse_schema_spec(dstd::StringView text,
                                           const dstd::String& source) {
    auto document = Document::parse(dstd::String(text), "");
    if (!document) return spec_error(source, document.error().message());
    return parse_spec(document->root(), source);
}

}  // namespace dftracer::utils::trace::index
