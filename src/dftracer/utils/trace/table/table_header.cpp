// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/fmt.h>
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
    dstd::String arrow_type;
    bool ordered = false;
    Labels labels = Labels::Str;
    dstd::Vector<dstd::String> categories;
    dstd::String categories_dtype;
};

Labels labels_of(json::Value& categories, dstd::Vector<dstd::String>& texts) {
    bool strs = false;
    bool ints = false;
    bool floats = false;
    bool other = false;
    categories.for_each_element([&](json::Value v) {
        texts.emplace_back(v.raw());
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
    f.dtype = raw.dtype;
    f.elem = raw.elem;
    f.arrow_type = raw.arrow_type;
    f.ordered = raw.ordered;
    f.categories = raw.categories;
    f.categories_dtype = raw.categories_dtype;
    dstd::StringView base(raw.dtype);
    bool suffixed = false;
    if (const auto open = base.find('['); open != dstd::StringView::npos) {
        base = base.substr(0, open);
        suffixed = true;
    }
    if (base == "category") {
        // Float labels may hold the text of an infinite float, as cells do.
        const dstd::StringView cats(raw.categories_dtype);
        if (cats.size() >= 5 &&
            dstd::ascii_iequals(cats.substr(0, 5), "float")) {
            f.kind = Kind::Float;
            return f;
        }
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
        if (suffixed) f.bits = 32;
        return f;
    }
    const auto numeric = [&](dstd::StringView prefix, Kind kind) {
        if (base.size() <= prefix.size() || !base.starts_with(prefix))
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
        if (base == "float32" || base == "Float32") f.bits = 32;
        return f;
    }
    return dstd::nullopt;
}

// "MAJOR.MINOR" with the engine's major and a minor not above its own. The
// integer 1 that DFAnalyzer's writer stamps reads as "1.0".
bool version_readable(json::Value& v) {
    std::int64_t major = 0;
    std::int64_t minor = 0;
    if (auto n = v.as_int()) {
        major = *n;
    } else if (auto s = v.as_string()) {
        const dstd::StringView text = *s;
        const auto dot = text.find('.');
        if (dot == dstd::StringView::npos) return false;
        const auto number = [](dstd::StringView t, std::int64_t& out) {
            if (t.empty() || t.size() > 6) return false;
            out = 0;
            for (const char c : t) {
                if (c < '0' || c > '9') return false;
                out = out * 10 + (c - '0');
            }
            return true;
        };
        if (!number(text.substr(0, dot), major) ||
            !number(text.substr(dot + 1), minor))
            return false;
    } else {
        return false;
    }
    return major == FORMAT_VERSION_MAJOR && minor <= FORMAT_VERSION_MINOR;
}

}  // namespace

dstd::Result<Header> parse_header(json::Value& args) {
    Header h;
    dstd::Optional<dstd::Error> err;
    bool format_ok = false;
    bool version_ok = false;
    dstd::String version_text;
    bool has_nrows = false;
    bool has_fields = false;
    dstd::Vector<RawField> raws;

    args.for_each_member([&](dstd::StringView key, json::Value v) {
        if (err) return;
        if (key == "format") {
            auto s = v.as_string();
            format_ok = s && *s == FORMAT;
        } else if (key == "format_version") {
            version_ok = version_readable(v);
            if (!version_ok) version_text = dstd::String(v.raw());
        } else if (key == "table") {
            if (auto s = v.as_string()) h.table = dstd::String(*s);
        } else if (key == "nrows") {
            auto n = v.as_int();
            if (n && *n == -1) {
                err.emplace(ErrorCode::Parse,
                            "table header nrows is -1: the writer did not "
                            "finish the file");
                return;
            }
            if (!n || *n < 0) {
                err.emplace(ErrorCode::Parse,
                            "table header nrows is not a count");
                return;
            }
            h.nrows = *n;
            has_nrows = true;
        } else if (key == "index") {
            v.for_each_element([&](json::Value iv) {
                if (auto s = iv.as_string()) h.index.emplace_back(*s);
            });
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
                    } else if (fk == "arrow_type") {
                        if (auto s = x.as_string())
                            raw.arrow_type = dstd::String(*s);
                    } else if (fk == "ordered") {
                        if (auto b = x.as_bool()) raw.ordered = *b;
                    } else if (fk == "categories") {
                        raw.labels = labels_of(x, raw.categories);
                    } else if (fk == "categories_dtype") {
                        if (auto s = x.as_string())
                            raw.categories_dtype = dstd::String(*s);
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
        return common::make_error<
            "unknown dftu.table format_version {}: this engine reads "
            "\"{}\"">(ErrorCode::Parse, dstd::StringView(version_text),
                      FORMAT_VERSION);
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

namespace dftracer::utils::trace::table::detail {

dstd::Optional<dstd::String> disagreement(const Header& a, const Header& b) {
    if (a.fields.size() != b.fields.size())
        return dstd::fmt<"{} fields against {}">(a.fields.size(),
                                                 b.fields.size());
    for (std::size_t i = 0; i < a.fields.size(); ++i) {
        const Field& x = a.fields[i];
        const Field& y = b.fields[i];
        if (x.name != y.name)
            return dstd::fmt<"field {} is named {} against {}">(
                i, dstd::StringView(x.name), dstd::StringView(y.name));
        const dstd::StringView name(x.name);
        if (x.dtype != y.dtype)
            return dstd::fmt<"field {} has dtype {} against {}">(
                name, dstd::StringView(x.dtype), dstd::StringView(y.dtype));
        if (x.ordered != y.ordered)
            return dstd::fmt<"field {} is ordered in one and not in the other">(
                name);
        if (x.ordered && x.categories != y.categories)
            return dstd::fmt<"field {} is ordered with other categories">(name);
        if (x.ordered && x.categories_dtype != y.categories_dtype)
            return dstd::fmt<
                "field {} is ordered with another categories "
                "dtype">(name);
        if (x.elem != y.elem)
            return dstd::fmt<"field {} has elem '{}' against '{}'">(
                name, dstd::StringView(x.elem), dstd::StringView(y.elem));
        if (x.arrow_type != y.arrow_type)
            return dstd::fmt<"field {} has arrow_type '{}' against '{}'">(
                name, dstd::StringView(x.arrow_type),
                dstd::StringView(y.arrow_type));
        if (x.kind != y.kind || x.bits != y.bits)
            return dstd::fmt<"field {} holds labels of another kind">(name);
    }
    if (a.index != b.index) return dstd::String("the index names differ");
    return dstd::nullopt;
}

}  // namespace dftracer::utils::trace::table::detail
