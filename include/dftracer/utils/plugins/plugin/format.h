// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_FORMAT_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_FORMAT_H

#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugin/map.h>
#include <dftracer/utils/plugins/plugin/types.h>

#include <cstdint>
#include <type_traits>

namespace dftracer::utils::plugins {

/// A parsed JSON or YAML document. Move only; frees the host document, and so
/// every value below @ref root, when destroyed.
class Doc {
   public:
    Doc(Doc&& other) noexcept : e_(other.e_), doc_(other.doc_) {
        other.doc_ = nullptr;
    }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
    Doc& operator=(Doc&&) = delete;
    ~Doc() {
        if (doc_) e_->doc_free(doc_);
    }

    /// The root of the tree, valid until this document is destroyed. Read it
    /// with the dftu_obj_get and dftu_as_* helpers of abi/value.h.
    const dftu_value* root() const noexcept { return e_->root(doc_); }

   private:
    friend class Format;
    Doc(const dftu_svc_format* e, dftu_doc* doc) noexcept : e_(e), doc_(doc) {}

    const dftu_svc_format* e_;
    dftu_doc* doc_;
};

/// A streaming writer of one document. Move only. A call that breaks the
/// grammar returns an error with DFTU_E_INVALID and leaves the writer as it
/// was; see abi/format.h.
class FormatWriter {
   public:
    FormatWriter(FormatWriter&& other) noexcept
        : e_(other.e_), ctx_(other.ctx_), w_(other.w_) {
        other.w_ = nullptr;
    }
    FormatWriter(const FormatWriter&) = delete;
    FormatWriter& operator=(const FormatWriter&) = delete;
    FormatWriter& operator=(FormatWriter&&) = delete;
    ~FormatWriter() {
        if (w_) e_->writer_free(w_);
    }

    dstd::Result<void> begin_object() const {
        return detail::reported(e_->begin_object(ctx_, w_));
    }
    dstd::Result<void> end_object() const {
        return detail::reported(e_->end_object(ctx_, w_));
    }
    dstd::Result<void> begin_array() const {
        return detail::reported(e_->begin_array(ctx_, w_));
    }
    dstd::Result<void> end_array() const {
        return detail::reported(e_->end_array(ctx_, w_));
    }
    dstd::Result<void> key(dstd::StringView k) const {
        return detail::reported(e_->key(ctx_, w_, k.data(), k.size()));
    }
    dstd::Result<void> value(std::int64_t v) const {
        return detail::reported(e_->i64(ctx_, w_, v));
    }
    dstd::Result<void> value(double v) const {
        return detail::reported(e_->f64(ctx_, w_, v));
    }
    dstd::Result<void> value(dstd::StringView v) const {
        return detail::reported(e_->str(ctx_, w_, v.data(), v.size()));
    }
    dstd::Result<void> value(const char* v) const {
        return value(dstd::StringView(v));
    }
    template <class T>
        requires(std::is_pointer_v<T> && !std::is_same_v<T, const char*>)
    dstd::Result<void> value(T) const = delete;
    dstd::Result<void> value(bool v) const {
        return detail::reported(e_->boolean(ctx_, w_, v ? 1 : 0));
    }
    dstd::Result<void> null() const {
        return detail::reported(e_->null(ctx_, w_));
    }
    /// One scalar token written as is; the caller guarantees it is valid.
    dstd::Result<void> raw(dstd::StringView token) const {
        return detail::reported(e_->raw(ctx_, w_, token.data(), token.size()));
    }

    /// The text of the finished document. The writer takes no more calls.
    dstd::Result<dstd::String> finish() const {
        dftu_bytes out{};
        DFTRACER_UTILS_TRY_VOID(detail::reported(e_->finish(ctx_, w_, &out)));
        dstd::String text(static_cast<const char*>(out.data),
                          static_cast<std::size_t>(out.len));
        if (out.free_fn) out.free_fn(const_cast<void*>(out.data), out.ud);
        return text;
    }

   private:
    friend class Format;
    FormatWriter(const dftu_svc_format* e, dftu_ctx* ctx,
                 dftu_format_writer* w) noexcept
        : e_(e), ctx_(ctx), w_(w) {}

    const dftu_svc_format* e_;
    dftu_ctx* ctx_;
    dftu_format_writer* w_;
};

/// Typed wrapper over one dftu_svc_format (see abi/format.h). A call the host
/// refuses, or a host without the service, returns its error; the message of
/// a refused call is on the call's dftu_ctx.
class Format {
   public:
    /// DFTU_E_PARSE with the message `line:column: reason` for a text that does
    /// not parse.
    dstd::Result<Doc> parse(dstd::StringView text) const {
        if (!e_) return unsupported();
        dftu_doc* doc = nullptr;
        DFTRACER_UTILS_TRY_VOID(detail::reported(
            e_->parse(ctx_, h_->h, text.data(), text.size(), &doc)));
        return Doc(e_, doc);
    }

    dstd::Result<FormatWriter> writer() const {
        if (!e_) return unsupported();
        dftu_format_writer* w = nullptr;
        DFTRACER_UTILS_TRY_VOID(
            detail::reported(e_->writer_new(ctx_, h_->h, &w)));
        return FormatWriter(e_, ctx_, w);
    }

   protected:
    Format(const dftu_plugin_host* h, dftu_ctx* ctx, const char* id)
        : h_(h),
          ctx_(ctx),
          e_(h && h->get_service
                 ? static_cast<const dftu_svc_format*>(h->get_service(h->h, id))
                 : nullptr) {}

   private:
    static dstd::Error unsupported() noexcept {
        return make_error(DFTU_E_UNSUPPORTED, "host lacks the format service");
    }

    const dftu_plugin_host* h_;
    dftu_ctx* ctx_;
    const dftu_svc_format* e_;
};

/// The `dftu.svc.json` service.
class Json : public Format {
   public:
    Json(const dftu_plugin_host* h, dftu_ctx* ctx)
        : Format(h, ctx, DFTU_SVC_JSON) {}
};

/// The `dftu.svc.yaml` service.
class Yaml : public Format {
   public:
    Yaml(const dftu_plugin_host* h, dftu_ctx* ctx)
        : Format(h, ctx, DFTU_SVC_YAML) {}
};

inline Json Host::json() const { return Json{h_, ctx_}; }
inline Yaml Host::yaml() const { return Yaml{h_, ctx_}; }

}  // namespace dftracer::utils::plugins

#endif /* DFTRACER_UTILS_PLUGINS_PLUGIN_FORMAT_H */
