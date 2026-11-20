// SPDX-License-Identifier: MIT
#include <dftracer/utils/arrow/internal.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/shared_string.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstring>
#include <utility>

namespace dftracer::utils::arrow {

namespace {

using dataframe::DataFrame;
using dataframe::TypeId;

/// A reader over the JSON the index metadata uses: strings, arrays and
/// objects of strings, and any other value, which it skips.
class JsonReader {
   public:
    explicit JsonReader(dstd::StringView text) : text_(text) {}

    bool at(char c) {
        skip_space();
        return pos_ < text_.size() && text_[pos_] == c;
    }

    bool take(char c) {
        if (!at(c)) return false;
        ++pos_;
        return true;
    }

    bool string(dstd::String& out) {
        if (!take('"')) return false;
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) return false;
            const char e = text_[pos_++];
            switch (e) {
                case 'n':
                    out += '\n';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 &&
                        text_.substr(pos_, 2) == "\\u") {
                        pos_ += 2;
                        std::uint32_t low = 0;
                        if (!hex4(low)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    utf8(cp, out);
                    break;
                }
                default:
                    out += e;
            }
        }
        return false;
    }

    /// Skips one value of any kind.
    bool skip() {
        skip_space();
        if (pos_ >= text_.size()) return false;
        const char c = text_[pos_];
        if (c == '"') {
            dstd::String ignored;
            return string(ignored);
        }
        if (c == '[' || c == '{') {
            const char close = c == '[' ? ']' : '}';
            ++pos_;
            if (take(close)) return true;
            for (;;) {
                if (c == '{') {
                    dstd::String key;
                    if (!string(key) || !take(':')) return false;
                }
                if (!skip()) return false;
                if (take(close)) return true;
                if (!take(',')) return false;
            }
        }
        while (pos_ < text_.size() && text_[pos_] != ',' &&
               text_[pos_] != ']' && text_[pos_] != '}')
            ++pos_;
        return true;
    }

   private:
    void skip_space() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\n' ||
                text_[pos_] == '\t' || text_[pos_] == '\r'))
            ++pos_;
    }

    bool hex4(std::uint32_t& out) {
        if (pos_ + 4 > text_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            std::uint32_t d = 0;
            if (c >= '0' && c <= '9')
                d = static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                d = static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                d = static_cast<std::uint32_t>(c - 'A' + 10);
            else
                return false;
            out = out * 16 + d;
        }
        return true;
    }

    static void utf8(std::uint32_t cp, dstd::String& out) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    dstd::StringView text_;
    std::size_t pos_ = 0;
};

/// The strings of the JSON array at the reader; a non-string element (a pandas
/// RangeIndex is an object) is skipped. False for text that is no array.
bool string_array(JsonReader& r, dstd::Vector<dstd::String>& out) {
    if (!r.take('[')) return false;
    if (r.take(']')) return true;
    for (;;) {
        if (r.at('"')) {
            dstd::String s;
            if (!r.string(s)) return false;
            out.push_back(std::move(s));
        } else if (!r.skip()) {
            return false;
        }
        if (r.take(']')) return true;
        if (!r.take(',')) return false;
    }
}

/// `index_columns` of a pandas schema metadata value.
dstd::Vector<dstd::String> pandas_index_columns(dstd::StringView json) {
    dstd::Vector<dstd::String> out;
    JsonReader r(json);
    if (!r.take('{')) return {};
    if (r.take('}')) return {};
    for (;;) {
        dstd::String key;
        if (!r.string(key) || !r.take(':')) return {};
        if (key == "index_columns") {
            dstd::Vector<dstd::String> names;
            return string_array(r, names) ? names
                                          : dstd::Vector<dstd::String>{};
        }
        if (!r.skip()) return {};
        if (r.take('}')) return {};
        if (!r.take(',')) return {};
    }
}

void append_json_string(dstd::String& out, dstd::StringView s) {
    out += '"';
    for (const char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\r':
                out += "\\r";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static constexpr char HEX[] = "0123456789abcdef";
                    out += "\\u00";
                    out += HEX[(c >> 4) & 0xF];
                    out += HEX[c & 0xF];
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

}  // namespace

namespace detail {

void set_frame_metadata(const DataFrame& frame, ArrowSchema* schema) {
    const dstd::Span<const dstd::SharedString> names = frame.index_names();
    if (names.empty()) return;
    dstd::String json = "[";
    dstd::fmt_append_join<",">(
        json, names, [](dstd::String& o, const dstd::SharedString& name) {
            append_json_string(o, name);
        });
    json += ']';
    ArrowBuffer meta;
    ArrowMetadataBuilderInit(&meta, schema->metadata);
    ArrowMetadataBuilderAppend(
        &meta, ArrowCharView(INDEX_METADATA_KEY),
        ArrowStringView{json.data(), static_cast<std::int64_t>(json.size())});
    ArrowSchemaSetMetadata(schema, reinterpret_cast<const char*>(meta.data));
    ArrowBufferReset(&meta);
}

void import_frame_metadata(const ArrowSchema* schema, DataFrame& frame) {
    if (schema->metadata == nullptr) return;
    ArrowMetadataReader reader;
    if (ArrowMetadataReaderInit(&reader, schema->metadata) != NANOARROW_OK)
        return;
    dstd::Vector<dstd::String> names;
    bool found = false;
    ArrowStringView key;
    ArrowStringView value;
    while (ArrowMetadataReaderRead(&reader, &key, &value) == NANOARROW_OK) {
        const dstd::StringView k(key.data,
                                 static_cast<std::size_t>(key.size_bytes));
        const dstd::StringView v(value.data,
                                 static_cast<std::size_t>(value.size_bytes));
        if (k == INDEX_METADATA_KEY) {
            JsonReader r(v);
            names.clear();
            found = string_array(r, names);
            break;
        }
        if (k == "pandas" && !found) names = pandas_index_columns(v);
    }
    dstd::Vector<dstd::StringView> usable;
    for (const dstd::String& n : names) {
        const std::int64_t at = frame.column_index(n);
        if (at < 0) continue;
        const TypeId type = frame.columns[static_cast<std::size_t>(at)].type();
        if (type == TypeId::List || type == TypeId::Struct) continue;
        bool repeats = false;
        for (const dstd::StringView u : usable) repeats = repeats || u == n;
        if (!repeats) usable.push_back(dstd::StringView(n));
    }
    if (usable.empty()) return;
    auto indexed = frame.with_index(usable);
    if (indexed) frame = std::move(*indexed);
}

}  // namespace detail

}  // namespace dftracer::utils::arrow
