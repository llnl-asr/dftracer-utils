// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_arena.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/json/dom_value.h>
#include <dftracer/utils/json/writer.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/host/fold_adapter/ext.h>
#include <dftracer/utils/plugins/host/host_status.h>
#include <dftracer/utils/yaml/document.h>
#include <dftracer/utils/yaml/escape.h>
#include <dftracer/utils/yaml/writer.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace dftracer::utils::plugins::host {

namespace {

constexpr int MAX_DEPTH = 1000;
constexpr std::size_t MAX_YAML_KEY = 1022;

// The parsed tree of one document. A dftu_value string and a key are NUL
// terminated, which the text of the document is not, so every string is copied
// once into `strings`; the other containers only hold what the tree points at.
struct Doc {
    explicit Doc(std::size_t text_size) : strings(text_size + 64) {}

    dstd::StringArena strings;
    dstd::Deque<dftu_value> values;
    dstd::Deque<dstd::Vector<dftu_value>> items;
    dstd::Deque<dstd::Vector<dftu_member>> members;
    dftu_value root{};
};

Doc* doc_of(::dftu_doc* doc) { return reinterpret_cast<Doc*>(doc); }
const Doc* doc_of(const ::dftu_doc* doc) {
    return reinterpret_cast<const Doc*>(doc);
}

class TreeBuilder {
   public:
    explicit TreeBuilder(Doc& doc) : doc_(doc) {}

    // Why the build stopped, or empty.
    dstd::StringView failure() const { return failure_; }

    bool build_json(const json::DomValue& v, dftu_value& out, int depth) {
        if (depth > MAX_DEPTH) return fail("nested too deep");
        switch (v.type()) {
            case json::DomValue::Type::Object: {
                dstd::Vector<dftu_member> members;
                v.for_each_member(
                    [&](dstd::StringView key, json::DomValue child) {
                        if (!failure_.empty()) return;
                        dftu_value& slot = doc_.values.emplace_back();
                        if (build_json(child, slot, depth + 1))
                            add_member(members, key, slot);
                    });
                if (!failure_.empty()) return false;
                return finish_members(members, out);
            }
            case json::DomValue::Type::Array: {
                dstd::Vector<dftu_value> items;
                v.for_each_element([&](json::DomValue child) {
                    if (!failure_.empty()) return;
                    dftu_value item{};
                    if (build_json(child, item, depth + 1))
                        items.push_back(item);
                });
                if (!failure_.empty()) return false;
                return finish_items(items, out);
            }
            case json::DomValue::Type::String:
                return set_string(v.as_string(), out);
            case json::DomValue::Type::Int:
                out.kind = DFTU_VAL_I64;
                out.as.i64 = v.as_int();
                return true;
            case json::DomValue::Type::Uint:
                out.kind = DFTU_VAL_F64;
                out.as.f64 = static_cast<double>(v.as_uint());
                return true;
            case json::DomValue::Type::Double:
                out.kind = DFTU_VAL_F64;
                out.as.f64 = v.as_double();
                return true;
            case json::DomValue::Type::Bool:
                out.kind = DFTU_VAL_BOOL;
                out.as.b = v.as_bool() ? 1u : 0u;
                return true;
            case json::DomValue::Type::Null:
                out.kind = DFTU_VAL_NULL;
                return true;
        }
        return fail("invalid value");
    }

    bool build_yaml(const yaml::Value& v, dftu_value& out, int depth) {
        if (depth > MAX_DEPTH) return fail("nested too deep");
        if (v.is_object()) {
            dstd::Vector<dftu_member> members;
            v.for_each_member([&](dstd::StringView key, yaml::Value child) {
                if (!failure_.empty()) return;
                dftu_value& slot = doc_.values.emplace_back();
                if (build_yaml(child, slot, depth + 1))
                    add_member(members, key, slot);
            });
            if (!failure_.empty()) return false;
            return finish_members(members, out);
        }
        if (v.is_array()) {
            dstd::Vector<dftu_value> items;
            v.for_each_element([&](yaml::Value child) {
                if (!failure_.empty()) return;
                dftu_value item{};
                if (build_yaml(child, item, depth + 1)) items.push_back(item);
            });
            if (!failure_.empty()) return false;
            return finish_items(items, out);
        }
        if (v.is_null()) {
            out.kind = DFTU_VAL_NULL;
            return true;
        }
        return set_string(v.raw(), out);
    }

   private:
    bool fail(dstd::StringView reason) {
        if (failure_.empty()) failure_ = reason;
        return false;
    }

    // The NUL terminated copy of `text` in the document, or empty on failure.
    bool copy_text(dstd::StringView text, dstd::StringView& out) {
        if (text.size() > std::numeric_limits<std::uint32_t>::max())
            return fail("a string is longer than 4 GiB");
        out = doc_.strings.push(text, dstd::StringView("\0", 1));
        return true;
    }

    bool set_string(dstd::StringView text, dftu_value& out) {
        dstd::StringView copy;
        if (!copy_text(text, copy)) return false;
        out.kind = DFTU_VAL_STR;
        out.count = static_cast<std::uint32_t>(text.size());
        out.as.str = copy.data();
        return true;
    }

    void add_member(dstd::Vector<dftu_member>& members, dstd::StringView key,
                    const dftu_value& slot) {
        dstd::StringView copy;
        if (!copy_text(key, copy)) return;
        members.push_back(
            {copy.data(), static_cast<std::uint32_t>(key.size()), &slot});
    }

    bool finish_members(dstd::Vector<dftu_member>& members, dftu_value& out) {
        if (members.size() > std::numeric_limits<std::uint32_t>::max())
            return fail("too many members");
        out.kind = DFTU_VAL_OBJECT;
        out.count = static_cast<std::uint32_t>(members.size());
        doc_.members.push_back(std::move(members));
        out.as.members = doc_.members.back().data();
        return true;
    }

    bool finish_items(dstd::Vector<dftu_value>& items, dftu_value& out) {
        if (items.size() > std::numeric_limits<std::uint32_t>::max())
            return fail("too many elements");
        out.kind = DFTU_VAL_ARRAY;
        out.count = static_cast<std::uint32_t>(items.size());
        doc_.items.push_back(std::move(items));
        out.as.items = doc_.items.back().data();
        return true;
    }

    Doc& doc_;
    dstd::StringView failure_;
};

dftu_status check_text(::dftu_ctx* ctx, bool text_missing, ::dftu_doc** out) {
    if (out == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "parse: out is NULL");
    *out = nullptr;
    if (text_missing)
        return host_fail(ctx, DFTU_E_INVALID, "parse: text is NULL");
    return DFTU_OK;
}

dftu_status json_parse(::dftu_ctx* ctx, void*, const char* text,
                       std::uint64_t len, ::dftu_doc** out) {
    if (dftu_status st = check_text(ctx, text == nullptr && len != 0, out);
        st < 0)
        return st;
    const dstd::StringView view(text == nullptr ? "" : text,
                                static_cast<std::size_t>(len));
    auto parsed = json::DomDocument::parse(view);
    if (!parsed) return host_fail(ctx, DFTU_E_PARSE, parsed.error().message());
    auto doc = dstd::make_unique<Doc>(view.size());
    TreeBuilder builder(*doc);
    if (!builder.build_json(parsed->root(), doc->root, 0))
        return host_fail(ctx, DFTU_E_PARSE, builder.failure());
    *out = reinterpret_cast<::dftu_doc*>(doc.release());
    return DFTU_OK;
}

dftu_status yaml_parse(::dftu_ctx* ctx, void*, const char* text,
                       std::uint64_t len, ::dftu_doc** out) {
    if (dftu_status st = check_text(ctx, text == nullptr && len != 0, out);
        st < 0)
        return st;
    auto parsed =
        yaml::Document::parse(dstd::String(text == nullptr ? "" : text,
                                           static_cast<std::size_t>(len)),
                              dstd::StringView());
    if (!parsed) return host_fail(ctx, DFTU_E_PARSE, parsed.error().message());
    auto doc = dstd::make_unique<Doc>(static_cast<std::size_t>(len));
    TreeBuilder builder(*doc);
    if (!builder.build_yaml(parsed->root(), doc->root, 0))
        return host_fail(ctx, DFTU_E_PARSE, builder.failure());
    *out = reinterpret_cast<::dftu_doc*>(doc.release());
    return DFTU_OK;
}

const dftu_value* format_root(const ::dftu_doc* doc) {
    return doc == nullptr ? nullptr : &doc_of(doc)->root;
}

void format_doc_free(::dftu_doc* doc) {
    dstd::UniquePtr<Doc> owned(doc_of(doc));
}

struct Frame {
    bool is_array;
    bool key_pending;
};

// A writer over one text. The grammar is checked here, so a misuse is a status
// and never reaches the format writers, which end the process on it in a debug
// build.
struct FormatWriter {
    explicit FormatWriter(bool is_yaml) {
        if (is_yaml)
            yaml_writer.emplace(out);
        else
            json_writer.emplace(out);
    }

    ~FormatWriter() {
        while (!frames.empty()) {
            if (frames.back().key_pending) null();
            end(frames.back().is_array);
            frames.pop_back();
        }
    }

    FormatWriter(const FormatWriter&) = delete;
    FormatWriter& operator=(const FormatWriter&) = delete;

    void null() {
        if (yaml_writer)
            yaml_writer->null();
        else
            json_writer->null();
    }
    void end(bool is_array) {
        if (yaml_writer) {
            if (is_array)
                yaml_writer->end_array();
            else
                yaml_writer->end_object();
        } else if (is_array) {
            json_writer->end_array();
        } else {
            json_writer->end_object();
        }
    }

    dstd::String out;
    dstd::Optional<json::Writer> json_writer;
    dstd::Optional<yaml::Writer> yaml_writer;
    dstd::Vector<Frame> frames;
    bool root_done = false;
    bool finished = false;
};

FormatWriter* writer_of(::dftu_format_writer* w) {
    return reinterpret_cast<FormatWriter*>(w);
}

dftu_status invalid(::dftu_ctx* ctx, dstd::StringView op,
                    dstd::StringView why) {
    return host_fail(ctx, DFTU_E_INVALID, dstd::fmt<"{}: {}">(op, why));
}

dftu_status writer_new(::dftu_ctx* ctx, bool is_yaml,
                       ::dftu_format_writer** out) {
    if (out == nullptr) return invalid(ctx, "writer_new", "out is NULL");
    *out = reinterpret_cast<::dftu_format_writer*>(
        dstd::make_unique<FormatWriter>(is_yaml).release());
    return DFTU_OK;
}

dftu_status json_writer_new(::dftu_ctx* ctx, void*,
                            ::dftu_format_writer** out) {
    return writer_new(ctx, false, out);
}

dftu_status yaml_writer_new(::dftu_ctx* ctx, void*,
                            ::dftu_format_writer** out) {
    return writer_new(ctx, true, out);
}

// Checks that a value may start here.
dftu_status begin_value(::dftu_ctx* ctx, dstd::StringView op, FormatWriter* w) {
    if (w == nullptr) return invalid(ctx, op, "the writer is NULL");
    if (w->finished) return invalid(ctx, op, "the writer is finished");
    if (w->frames.empty()) {
        if (w->root_done) return invalid(ctx, op, "a second root");
    } else if (!w->frames.back().is_array && !w->frames.back().key_pending) {
        return invalid(ctx, op, "a value without a key");
    }
    return DFTU_OK;
}

void end_value(FormatWriter* w) {
    if (w->frames.empty())
        w->root_done = true;
    else
        w->frames.back().key_pending = false;
}

dftu_status begin_container(::dftu_ctx* ctx, ::dftu_format_writer* handle,
                            bool is_array) {
    const char* op = is_array ? "begin_array" : "begin_object";
    FormatWriter* w = writer_of(handle);
    if (dftu_status st = begin_value(ctx, op, w); st < 0) return st;
    if (w->yaml_writer) {
        if (is_array)
            w->yaml_writer->begin_array();
        else
            w->yaml_writer->begin_object();
    } else if (is_array) {
        w->json_writer->begin_array();
    } else {
        w->json_writer->begin_object();
    }
    w->frames.push_back({is_array, false});
    return DFTU_OK;
}

dftu_status end_container(::dftu_ctx* ctx, ::dftu_format_writer* handle,
                          bool is_array) {
    const char* op = is_array ? "end_array" : "end_object";
    FormatWriter* w = writer_of(handle);
    if (w == nullptr) return invalid(ctx, op, "the writer is NULL");
    if (w->finished) return invalid(ctx, op, "the writer is finished");
    if (w->frames.empty()) return invalid(ctx, op, "nothing is open");
    if (w->frames.back().is_array != is_array)
        return invalid(ctx, op, "the open container is the other kind");
    if (w->frames.back().key_pending)
        return invalid(ctx, op, "a key without a value");
    w->end(is_array);
    w->frames.pop_back();
    end_value(w);
    return DFTU_OK;
}

dftu_status format_begin_object(::dftu_ctx* ctx, ::dftu_format_writer* w) {
    return begin_container(ctx, w, false);
}
dftu_status format_end_object(::dftu_ctx* ctx, ::dftu_format_writer* w) {
    return end_container(ctx, w, false);
}
dftu_status format_begin_array(::dftu_ctx* ctx, ::dftu_format_writer* w) {
    return begin_container(ctx, w, true);
}
dftu_status format_end_array(::dftu_ctx* ctx, ::dftu_format_writer* w) {
    return end_container(ctx, w, true);
}

dftu_status check_text_arg(::dftu_ctx* ctx, dstd::StringView op,
                           bool text_missing) {
    if (text_missing) return invalid(ctx, op, "the text is NULL");
    return DFTU_OK;
}

dftu_status format_key(::dftu_ctx* ctx, ::dftu_format_writer* handle,
                       const char* k, std::uint64_t len) {
    FormatWriter* w = writer_of(handle);
    if (w == nullptr) return invalid(ctx, "key", "the writer is NULL");
    if (w->finished) return invalid(ctx, "key", "the writer is finished");
    if (dftu_status st = check_text_arg(ctx, "key", k == nullptr && len != 0);
        st < 0)
        return st;
    if (w->frames.empty() || w->frames.back().is_array)
        return invalid(ctx, "key", "a key outside an object");
    if (w->frames.back().key_pending)
        return invalid(ctx, "key", "two keys in a row");
    const dstd::StringView name(k == nullptr ? "" : k,
                                static_cast<std::size_t>(len));
    if (w->yaml_writer) {
        if (yaml::escaped_yaml_size(name) > MAX_YAML_KEY)
            return invalid(ctx, "key", "a YAML key is over 1022 bytes");
        w->yaml_writer->key(name);
    } else {
        w->json_writer->key(name);
    }
    w->frames.back().key_pending = true;
    return DFTU_OK;
}

template <class Write>
dftu_status write_scalar(::dftu_ctx* ctx, dstd::StringView op,
                         ::dftu_format_writer* handle, Write&& write) {
    FormatWriter* w = writer_of(handle);
    if (dftu_status st = begin_value(ctx, op, w); st < 0) return st;
    if (w->yaml_writer)
        write(*w->yaml_writer);
    else
        write(*w->json_writer);
    end_value(w);
    return DFTU_OK;
}

dftu_status format_i64(::dftu_ctx* ctx, ::dftu_format_writer* w,
                       std::int64_t v) {
    return write_scalar(ctx, "i64", w, [&](auto& writer) { writer.value(v); });
}

dftu_status format_f64(::dftu_ctx* ctx, ::dftu_format_writer* w, double v) {
    FormatWriter* writer = writer_of(w);
    if (writer != nullptr && writer->json_writer && !std::isfinite(v))
        return invalid(ctx, "f64",
                       "JSON has no spelling for a double that is "
                       "not finite");
    return write_scalar(ctx, "f64", w, [&](auto& out) { out.value(v); });
}

dftu_status format_str(::dftu_ctx* ctx, ::dftu_format_writer* w, const char* s,
                       std::uint64_t len) {
    if (dftu_status st = check_text_arg(ctx, "str", s == nullptr && len != 0);
        st < 0)
        return st;
    const dstd::StringView text(s == nullptr ? "" : s,
                                static_cast<std::size_t>(len));
    return write_scalar(ctx, "str", w,
                        [&](auto& writer) { writer.value(text); });
}

dftu_status format_boolean(::dftu_ctx* ctx, ::dftu_format_writer* w,
                           std::int32_t v) {
    return write_scalar(ctx, "boolean", w,
                        [&](auto& writer) { writer.value(v != 0); });
}

dftu_status format_null(::dftu_ctx* ctx, ::dftu_format_writer* w) {
    return write_scalar(ctx, "null", w, [&](auto& writer) { writer.null(); });
}

dftu_status format_raw(::dftu_ctx* ctx, ::dftu_format_writer* w,
                       const char* token, std::uint64_t len) {
    if (dftu_status st =
            check_text_arg(ctx, "raw", token == nullptr && len != 0);
        st < 0)
        return st;
    if (len == 0) return invalid(ctx, "raw", "the token is empty");
    const dstd::StringView text(token, static_cast<std::size_t>(len));
    return write_scalar(ctx, "raw", w,
                        [&](auto& writer) { writer.raw_value(text); });
}

void free_owned_string(void*, void* ud) {
    dstd::UniquePtr<dstd::String> owned(static_cast<dstd::String*>(ud));
}

dftu_status format_finish(::dftu_ctx* ctx, ::dftu_format_writer* handle,
                          dftu_bytes* out) {
    FormatWriter* w = writer_of(handle);
    if (w == nullptr) return invalid(ctx, "finish", "the writer is NULL");
    if (out == nullptr) return invalid(ctx, "finish", "out is NULL");
    if (w->finished) return invalid(ctx, "finish", "the writer is finished");
    if (!w->frames.empty())
        return invalid(ctx, "finish", "a container is still open");
    if (!w->root_done) return invalid(ctx, "finish", "nothing was written");
    auto owned = dstd::make_unique<dstd::String>(std::move(w->out));
    w->finished = true;
    out->data = owned->data();
    out->len = owned->size();
    out->free_fn = free_owned_string;
    out->ud = owned.release();
    return DFTU_OK;
}

void format_writer_free(::dftu_format_writer* w) {
    dstd::UniquePtr<FormatWriter> owned(writer_of(w));
}

const dftu_svc_format JSON_VTABLE = {sizeof(dftu_svc_format),
                                     json_parse,
                                     format_root,
                                     format_doc_free,
                                     json_writer_new,
                                     format_begin_object,
                                     format_end_object,
                                     format_begin_array,
                                     format_end_array,
                                     format_key,
                                     format_i64,
                                     format_f64,
                                     format_str,
                                     format_boolean,
                                     format_null,
                                     format_raw,
                                     format_finish,
                                     format_writer_free};

const dftu_svc_format YAML_VTABLE = {sizeof(dftu_svc_format),
                                     yaml_parse,
                                     format_root,
                                     format_doc_free,
                                     yaml_writer_new,
                                     format_begin_object,
                                     format_end_object,
                                     format_begin_array,
                                     format_end_array,
                                     format_key,
                                     format_i64,
                                     format_f64,
                                     format_str,
                                     format_boolean,
                                     format_null,
                                     format_raw,
                                     format_finish,
                                     format_writer_free};

}  // namespace

const void* detail::json_ext_vtable() { return &JSON_VTABLE; }
const void* detail::yaml_ext_vtable() { return &YAML_VTABLE; }

}  // namespace dftracer::utils::plugins::host
