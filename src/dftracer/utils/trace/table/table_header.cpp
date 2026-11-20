// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/trace/table/table_header.h>

#include <utility>

namespace dftracer::utils::trace::table::detail {

namespace {

using common::ErrorCode;

enum class Labels : std::uint8_t { Str, Int, Float, Other };

struct RawField {
    dstd::String name;
    dstd::String dtype;
    dstd::String elem;
    Labels labels = Labels::Str;
};

Labels labels_of(json::Value& categories) {
    bool strs = false;
    bool ints = false;
    bool floats = false;
    bool other = false;
    categories.for_each_element([&](json::Value v) {
        switch (v.type()) {
            case json::Value::Type::String:
                strs = true;
                break;
            case json::Value::Type::Int:
                ints = true;
                break;
            case json::Value::Type::Double:
                floats = true;
                break;
            default:
                other = true;
                break;
        }
    });
    const bool nums = ints || floats;
    if (other || (strs && nums)) return Labels::Other;
    if (floats) return Labels::Float;
    if (ints) return Labels::Int;
    return Labels::Str;
}

bool read_width(dstd::StringView digits, unsigned& bits) {
    if (digits == "8")
        bits = 8;
    else if (digits == "16")
        bits = 16;
    else if (digits == "32")
        bits = 32;
    else if (digits == "64")
        bits = 64;
    else
        return false;
    return true;
}

dstd::Optional<Field> resolve(const RawField& raw) {
    Field f;
    f.name = raw.name;
    dstd::StringView base(raw.dtype);
    bool suffixed = false;
    if (const auto open = base.find('['); open != dstd::StringView::npos) {
        base = base.substr(0, open);
        suffixed = true;
    }
    if (base == "category") {
        switch (raw.labels) {
            case Labels::Str:
                f.kind = Kind::String;
                break;
            case Labels::Int:
                f.kind = Kind::Int;
                break;
            case Labels::Float:
                f.kind = Kind::Float;
                break;
            case Labels::Other:
                f.kind = Kind::Raw;
                break;
        }
        return f;
    }
    if (base == "object") {
        f.kind = raw.elem.empty() ? Kind::String : Kind::Raw;
        return f;
    }
    if (base == "string" || base == "str" || base == "large_string") {
        f.kind = Kind::String;
        return f;
    }
    if (base == "bool" || base == "boolean") {
        f.kind = Kind::Bool;
        f.nulls_ok = suffixed || base == "boolean";
        return f;
    }
    if (base == "double") {
        f.kind = Kind::Float;
        return f;
    }
    if (base == "float") {
        f.kind = Kind::Float;
        return f;
    }
    const auto numeric = [&](dstd::StringView prefix, Kind kind) {
        if (base.size() <= prefix.size() ||
            base.substr(0, prefix.size()) != prefix)
            return false;
        if (!read_width(base.substr(prefix.size()), f.bits)) return false;
        f.kind = kind;
        return true;
    };
    if (numeric("int", Kind::Int) || numeric("uint", Kind::UInt)) {
        f.nulls_ok = suffixed;
        return f;
    }
    if (numeric("Int", Kind::Int) || numeric("UInt", Kind::UInt)) return f;
    if (base == "float16" || base == "float32" || base == "float64" ||
        base == "Float32" || base == "Float64") {
        f.kind = Kind::Float;
        return f;
    }
    return dstd::nullopt;
}

}  // namespace

dstd::Result<Header> parse_header(json::Value& args) {
    Header h;
    dstd::Optional<dstd::Error> err;
    bool format_ok = false;
    bool version_ok = false;
    bool has_nrows = false;
    bool has_fields = false;
    dstd::Vector<RawField> raws;

    args.for_each_member([&](dstd::StringView key, json::Value v) {
        if (err) return;
        if (key == "format") {
            auto s = v.as_string();
            format_ok = s && *s == FORMAT;
        } else if (key == "format_version") {
            auto n = v.as_int();
            version_ok = n && *n == FORMAT_VERSION;
        } else if (key == "table") {
            if (auto s = v.as_string()) h.table = dstd::String(*s);
        } else if (key == "nrows") {
            auto n = v.as_int();
            if (!n || *n < 0) {
                err.emplace(ErrorCode::Parse,
                            "table header nrows is not a count");
                return;
            }
            h.nrows = *n;
            has_nrows = true;
        } else if (key == "fields") {
            has_fields = true;
            v.for_each_element([&](json::Value fv) {
                if (err) return;
                RawField raw;
                bool named = false;
                bool typed = false;
                fv.for_each_member([&](dstd::StringView fk, json::Value x) {
                    if (fk == "name") {
                        if (auto s = x.as_string()) {
                            raw.name = dstd::String(*s);
                            named = true;
                        }
                    } else if (fk == "dtype") {
                        if (auto s = x.as_string()) {
                            raw.dtype = dstd::String(*s);
                            typed = true;
                        }
                    } else if (fk == "elem") {
                        if (auto s = x.as_string()) raw.elem = dstd::String(*s);
                    } else if (fk == "categories") {
                        raw.labels = labels_of(x);
                    }
                });
                if (!named || !typed) {
                    err.emplace(ErrorCode::Parse,
                                "a table field has no name or dtype");
                    return;
                }
                raws.push_back(std::move(raw));
            });
        }
    });
    if (err) return dstd::Unexpected(*err);
    if (!format_ok)
        return common::make_error(ErrorCode::Parse, "not a dftu.table header");
    if (!version_ok)
        return common::make_error(ErrorCode::Parse,
                                  "unknown dftu.table format_version");
    if (!has_nrows || !has_fields)
        return common::make_error(ErrorCode::Parse,
                                  "table header lacks nrows or fields");
    for (const RawField& raw : raws) {
        auto field = resolve(raw);
        if (!field)
            return common::make_error<"field {} has the unsupported dtype {}">(
                ErrorCode::Parse, dstd::StringView(raw.name),
                dstd::StringView(raw.dtype));
        h.fields.push_back(std::move(*field));
    }
    return h;
}

}  // namespace dftracer::utils::trace::table::detail
